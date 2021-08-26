#![allow(dead_code, unused_imports)]
use std::{
    cell::RefCell,
    convert::{TryFrom, TryInto},
    num::NonZeroU32,
    rc::Rc,
    sync::{Arc, Mutex},
};

use smithay::{
    backend::renderer::buffer_dimensions,
    reexports::wayland_server::{
        protocol::{wl_buffer, wl_shm, wl_surface},
        Display,
    },
    utils::{Logical, Physical, Point, Rectangle, Size},
    wayland::{
        compositor::{
            compositor_init, is_sync_subsurface, with_states, with_surface_tree_upward,
            BufferAssignment, Damage, SurfaceAttributes, TraversalAction,
        },
        shell::xdg::{self, xdg_shell_init, ShellState as XdgShellState, XdgRequest},
        shm,
    },
};

use crate::{
    qubes::QubesData,
    state::AnvilState,
    window_map::{Kind as SurfaceKind, PopupKind, WindowMap},
};
use qubes_gui::Message as _;

#[derive(Clone)]
pub struct ShellHandles {
    pub xdg_state: Arc<Mutex<XdgShellState>>,
    pub window_map: Rc<RefCell<WindowMap>>,
}

pub fn init_shell(display: Rc<RefCell<Display>>, log: ::slog::Logger) -> ShellHandles {
    // Create the compositor
    compositor_init(
        &mut *display.borrow_mut(),
        move |surface, mut ddata| {
            let anvil_state = ddata.get::<AnvilState>().unwrap();
            let window_map = anvil_state.window_map.as_ref();
            surface_commit(&surface, &*window_map, &anvil_state.backend_data)
        },
        log.clone(),
    );

    // Init a window map, to track the location of our windows
    let window_map = Rc::new(RefCell::new(WindowMap::default()));

    // init the xdg_shell
    let xdg_window_map = window_map.clone();
    let log_ = log.clone();
    let inner_log = log.clone();
    let (xdg_shell_state, _, _) = xdg_shell_init(
        &mut *display.borrow_mut(),
        move |shell_event, mut _dispatch_data| match shell_event {
            XdgRequest::NewToplevel { surface } => {
                let raw_surface = match surface.get_surface() {
                    Some(s) => s,
                    // If there is no underlying surface just ignore the request
                    None => {
                        debug!(log, "Ignoring request to create window with no surface");
                        return;
                    }
                };
                let anvil_state = _dispatch_data.get::<AnvilState>().unwrap();
                let id = with_states(raw_surface, |data| {
                    data.data_map
                        .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                            RefCell::new(QubesData::data(anvil_state.backend_data.clone()))
                        });
                    let id = data
                        .data_map
                        .get::<RefCell<SurfaceData>>()
                        .unwrap()
                        .borrow()
                        .window;
                    assert!(anvil_state
                        .backend_data
                        .borrow_mut()
                        .map
                        .insert(
                            id,
                            super::qubes::QubesBackendData {
                                surface: surface.clone(),
                                has_configured: false,
                            }
                        )
                        .is_none());
                    id
                })
                .expect("TODO: handling dead clients");
                let ref mut agent = anvil_state.backend_data.borrow_mut().agent;
                let msg = qubes_gui::Create {
                    rectangle: qubes_gui::Rectangle {
                        top_left: qubes_gui::Coordinates { x: 0, y: 0 },
                        size: qubes_gui::WindowSize {
                            width: 256,
                            height: 256,
                        },
                    },
                    parent: None,
                    override_redirect: 0,
                };
                debug!(log, "Creating window {}", id);
                agent.client().send(&msg, id).expect("TODO: send errors");
                let msg = qubes_gui::Configure {
                    rectangle: msg.rectangle,
                    override_redirect: msg.override_redirect,
                };
                agent.client().send(&msg, id).expect("TODO: send errors");
                agent
                    .client()
                    .send(
                        &qubes_gui::MapInfo {
                            override_redirect: 0,
                            transient_for: 0,
                        },
                        id,
                    )
                    .unwrap();
            }
            XdgRequest::NewPopup {
                surface,
                positioner: _,
            } => {
                xdg_window_map
                    .borrow_mut()
                    .insert_popup(PopupKind::Xdg(surface));
                todo!()
            }
            XdgRequest::AckConfigure { surface, configure } => {
                let configure = match configure {
                    xdg::Configure::Toplevel(configure) => configure,
                    xdg::Configure::Popup(_) => todo!("Popup configures"),
                };
                with_states(&surface, |data| {
                    let mut anvil_state = _dispatch_data
                        .get::<AnvilState>()
                        .unwrap()
                        .backend_data
                        .borrow_mut();
                    let state = data
                        .data_map
                        .get::<RefCell<SurfaceData>>()
                        .unwrap()
                        .borrow();
                    debug!(
                        anvil_state.log,
                        "A configure event was acknowledged!  Params: surface {:?}, configure {:?}",
                        surface,
                        configure
                    );
                    let size = configure.state.size.unwrap_or_else(|| (1, 1).into());
                    let msg = &qubes_gui::Configure {
                        rectangle: qubes_gui::Rectangle {
                            top_left: qubes_gui::Coordinates::default(),
                            size: qubes_gui::WindowSize {
                                width: size.w.max(1) as _,
                                height: size.h.max(1) as _,
                            },
                        },
                        override_redirect: 0,
                    };
                    anvil_state.agent.client().send(msg, state.window).unwrap()
                })
                .unwrap()
            }
            XdgRequest::RePosition {
                surface,
                positioner,
                token,
            } => {
                let result = surface.with_pending_state(|state| {
                    // NOTE: This is again a simplification, a proper compositor would
                    // calculate the geometry of the popup here. For simplicity we just
                    // use the default implementation here that does not take the
                    // window position and output constraints into account.
                    let geometry = positioner.get_geometry();
                    state.geometry = geometry;
                    state.positioner = positioner;
                });

                if result.is_ok() {
                    surface.send_repositioned(token);
                }
                // QUBES HOOK: notify daemon about window movement
            }
            XdgRequest::Fullscreen {
                surface, output: _, ..
            } => {
                // QUBES HOOK: ask daemon to make surface fullscreen
                // NOTE: This is only one part of the solution. We can set the
                // location and configure size here, but the surface should be rendered fullscreen
                // independently from its buffer size
                let _wl_surface = if let Some(surface) = surface.get_surface() {
                    surface
                } else {
                    // If there is no underlying surface just ignore the request
                    return;
                };
                let _msg = qubes_gui::WindowFlags { set: 1, unset: 0 };
                todo!()
            }
            XdgRequest::UnMaximize { surface } => {
                let _anvil_state = _dispatch_data.get::<AnvilState>().unwrap();
                let _wl_surface = if let Some(surface) = surface.get_surface() {
                    surface
                } else {
                    // If there is no underlying surface just ignore the request
                    return;
                };
                let _msg = qubes_gui::WindowFlags { set: 0, unset: 1 };
                todo!()
            }
            other => println!("Got an unhandled event: {:?}", other),
        },
        log_,
    );

    ShellHandles {
        xdg_state: xdg_shell_state,
        window_map,
    }
}

pub struct SurfaceData {
    pub buffer: Option<(wl_buffer::WlBuffer, qubes_gui_client::agent::Buffer)>,
    pub geometry: Option<Rectangle<i32, Logical>>,
    pub buffer_dimensions: Option<Size<i32, Physical>>,
    pub buffer_scale: i32,
    pub window: std::num::NonZeroU32,
    pub qubes: Rc<RefCell<QubesData>>,
}

impl Drop for SurfaceData {
    fn drop(&mut self) {
        self.qubes.borrow_mut().map.remove(&self.window);
        let _ = self
            .qubes
            .borrow_mut()
            .agent
            .client()
            .send(&qubes_gui::Destroy {}, self.window);
    }
}

impl SurfaceData {
    pub fn update_buffer(&mut self, attrs: &mut SurfaceAttributes, data: &mut QubesData) {
        debug!(data.log, "Updating buffer!");
        const BYTES_PER_PIXEL: i32 = qubes_gui::DUMMY_DRV_FB_BPP as i32 / 8;
        let ref mut agent = data.agent;
        let mut force = false;
        match attrs.buffer.take() {
            Some(BufferAssignment::NewBuffer { buffer, .. }) => {
                debug!(data.log, "New buffer");
                // new contents
                self.buffer_dimensions = buffer_dimensions(&buffer);
                self.buffer_scale = attrs.buffer_scale;
                let Size { w, h, .. } = buffer_dimensions(&buffer).unwrap();
                assert!(w > 0 && h > 0, "NYI: posting an error to the client");
                let qbuf = agent.alloc_buffer(w as _, h as _).expect("TODO");
                qbuf.dump(agent.client(), self.window.into()).expect("TODO");
                if let Some(old_buffer) = std::mem::replace(&mut self.buffer, Some((buffer, qbuf)))
                {
                    old_buffer.0.release();
                    drop(old_buffer.1);
                }
                force = true;
            }
            Some(BufferAssignment::Removed) => {
                // remove the contents
                self.buffer = None;
                self.buffer_dimensions = None;
            }
            None => {}
        }
        debug!(data.log, "Damage: {:?}!", &attrs.damage);
        let client = agent.client();
        if !attrs.damage.is_empty() {
            if let Some((buffer, qbuf)) = self.buffer.as_ref() {
                debug!(data.log, "Performing damage calculation");
                let log = data.log.clone();
                match shm::with_buffer_contents(
                    buffer,
                    |untrusted_slice: &[u8],
                     shm::BufferData {
                         offset: untrusted_offset,
                         width: untrusted_width,
                         height: untrusted_height,
                         stride: untrusted_stride,
                         format: _, // already validated
                     }| {
                        debug!(log, "Updating buffer with damaged areas");
                        // SANTIIZE START
                        let low_len: i32 = match i32::try_from(untrusted_slice.len()) {
                            Err(_) => {
                                buffer.as_ref().post_error(
                                    wl_shm::Error::InvalidFd as u32,
                                    "Pool size not valid".into(),
                                );
                                return;
                            }
                            Ok(l) => l,
                        };
                        if untrusted_offset < 0
                            || untrusted_height <= 0
                            || untrusted_width <= 0
                            || untrusted_stride / BYTES_PER_PIXEL < untrusted_width
                            || i32::checked_mul(untrusted_stride, untrusted_height)
                                .map(|product| {
                                    low_len < product || untrusted_offset > low_len - product
                                })
                                .unwrap_or(true)
                        {
                            buffer.as_ref().post_error(
                                wl_shm::Error::InvalidStride as u32,
                                "Parameters not valid".into(),
                            );
                            return;
                        }
                        // SANITIZE END
                        let offset = untrusted_offset;
                        let width = untrusted_width;
                        let height = untrusted_height;
                        let stride = untrusted_stride;
                        if true {
                            attrs.damage.clear();
                            attrs.damage.push(Damage::Buffer(Rectangle {
                                loc: (0, 0).into(),
                                size: (width, height).into(),
                            }));
                        }
                        for i in attrs.damage.drain(..) {
                            let (untrusted_loc, untrusted_size) = match i {
                                Damage::Surface(Rectangle {
                                    loc: Point { x, y, .. },
                                    size: Size { w, h, .. },
                                }) => ((x, y).into(), (w, h).into()),
                                Damage::Buffer(Rectangle {
                                    loc: untrusted_loc,
                                    size: untrusted_size,
                                }) => (untrusted_loc, untrusted_size),
                            };
                            // SANITIZE START
                            if untrusted_size.w <= 0
                                || untrusted_size.h <= 0
                                || untrusted_loc.x < 0
                                || untrusted_loc.y < 0
                                || untrusted_size.w > width
                                || untrusted_size.h > height
                                || width - untrusted_size.w < untrusted_loc.x
                                || height - untrusted_size.h < untrusted_loc.y
                            {
                                buffer.as_ref().post_error(
                                    wl_shm::Error::InvalidStride as u32,
                                    "Invalid damage region".to_owned(),
                                );
                                return;
                            }
                            // SANITIZE END
                            let (x, y, w, h) = (
                                untrusted_loc.x,
                                untrusted_loc.y,
                                untrusted_size.w,
                                untrusted_size.h,
                            );
                            let subslice =
                                &untrusted_slice[(offset + BYTES_PER_PIXEL * x + y * stride)
                                    .try_into()
                                    .expect("checked above")..];
                            // trace!(data.log, "Copying data!");
                            let bytes_to_write: usize = (BYTES_PER_PIXEL * w).try_into().unwrap();
                            for i in 0..h {
                                let start_offset = (i * stride).try_into().unwrap();
                                let offset_in_dest_buffer = (BYTES_PER_PIXEL
                                    * (x + (i + y) * width))
                                    .try_into()
                                    .unwrap();
                                qbuf.write(
                                    &subslice[start_offset..bytes_to_write + start_offset],
                                    offset_in_dest_buffer,
                                );
                                let output_message = qubes_gui::ShmImage {
                                    rectangle: qubes_gui::Rectangle {
                                        top_left: qubes_gui::Coordinates {
                                            x: x as u32,
                                            y: y as u32,
                                        },
                                        size: qubes_gui::WindowSize {
                                            width: w as u32,
                                            height: w as u32,
                                        },
                                    },
                                };
                                qbuf.dump(client, self.window.into()).expect("TODO");
                                client
                                    .send(&output_message, self.window.into())
                                    .expect("TODO");
                            }
                        }
                    },
                ) {
                    Ok(()) => attrs.damage.clear(),
                    Err(shm::BufferAccessError::NotManaged) => panic!("strange shm buffer"),
                    Err(shm::BufferAccessError::BadMap) => return,
                }
            }
        }
        Self::send_frame(attrs, 16000000)
    }

    /// Returns the size of the surface.
    pub fn size(&self) -> Option<Size<i32, Logical>> {
        self.buffer_dimensions
            .map(|dims| dims.to_logical(self.buffer_scale))
    }

    /// Send the frame callback if it had been requested
    pub fn send_frame(attrs: &mut SurfaceAttributes, time: u32) {
        for callback in attrs.frame_callbacks.drain(..) {
            callback.done(time);
        }
    }
}

fn surface_commit(
    surface: &wl_surface::WlSurface,
    _window_map: &RefCell<WindowMap>,
    backend_data: &Rc<RefCell<QubesData>>,
) {
    debug!(
        backend_data.borrow().log,
        "Got a commit for surface {:?}", surface
    );
    #[cfg(feature = "xwayland")]
    super::xwayland::commit_hook(surface);

    if !is_sync_subsurface(surface) {
        // Update the buffer of all child surfaces
        with_surface_tree_upward(
            surface,
            (),
            |_, _, _| TraversalAction::DoChildren(()),
            |_, states, _| {
                states
                    .data_map
                    .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                        RefCell::new(QubesData::data(backend_data.clone()))
                    });
                let mut data = states
                    .data_map
                    .get::<RefCell<SurfaceData>>()
                    .unwrap()
                    .borrow_mut();
                data.update_buffer(
                    &mut *states.cached_state.current::<SurfaceAttributes>(),
                    &mut *backend_data.borrow_mut(),
                );
            },
            |_, _, _| true,
        );
    }
}
