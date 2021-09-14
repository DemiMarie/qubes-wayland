use std::{
    cell::RefCell,
    collections::BTreeMap,
    convert::{TryFrom, TryInto},
    num::NonZeroU32,
    rc::Rc,
    sync::{Arc, Mutex},
};

use smithay::{
    reexports::wayland_server::{
        protocol::{wl_buffer, wl_shm, wl_surface::WlSurface},
        Display,
    },
    utils::{Logical, Physical, Rectangle, Size},
    wayland::{
        compositor::{
            self, compositor_init, is_sync_subsurface, with_states, with_surface_tree_downward,
            BufferAssignment, Damage, SurfaceAttributes, TraversalAction,
        },
        shell::xdg::{self, xdg_shell_init, ShellState as XdgShellState, XdgRequest},
        shm,
    },
};

use crate::{
    qubes::{Kind, QubesData},
    state::AnvilState,
};

#[derive(Clone)]
pub struct ShellHandles {
    pub xdg_state: Arc<Mutex<XdgShellState>>,
}

struct QubesClient(Rc<RefCell<BTreeMap<u32, ()>>>);
impl Drop for QubesClient {
    fn drop(&mut self) {
        eprintln!("Dropped client")
    }
}

pub fn init_shell(display: Rc<RefCell<Display>>, log: ::slog::Logger) -> ShellHandles {
    // Create the compositor
    compositor_init(
        &mut *display.borrow_mut(),
        move |surface, mut ddata| {
            let anvil_state = ddata.get::<AnvilState>().unwrap();
            surface_commit(&surface, &anvil_state.backend_data)
        },
        log.clone(),
    );
    let (xdg_shell_state, _, _) = xdg_shell_init(
        &mut *display.borrow_mut(),
        move |shell_event, mut dispatch_data| {
            let anvil_state = dispatch_data.get::<AnvilState>().unwrap();
            match shell_event {
                XdgRequest::NewToplevel { surface } => {
                    let raw_surface = match surface.get_surface() {
                        Some(s) => s,
                        // If there is no underlying surface just ignore the request
                        None => {
                            debug!(
                                anvil_state.log,
                                "Ignoring request to create window with no surface"
                            );
                            return;
                        }
                    };
                    let (id, size) = with_states(raw_surface, |data| {
                        let toplevel_data = data
                            .data_map
                            .get::<Mutex<xdg::XdgToplevelSurfaceRoleAttributes>>()
                            .expect("Smithay always creates this data; qed")
                            .lock()
                            .expect("Poisoned?");
                        let size = (*toplevel_data)
                            .current
                            .size
                            .unwrap_or_else(|| (256, 256).into());
                        drop(toplevel_data);
                        data.data_map
                            .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                                RefCell::new(QubesData::data(anvil_state.backend_data.clone()))
                            });
                        let id = data
                            .data_map
                            .get::<RefCell<SurfaceData>>()
                            .expect("this data was just inserted above, so it will be present; qed")
                            .borrow()
                            .window;
                        assert!(anvil_state
                            .backend_data
                            .borrow_mut()
                            .map
                            .insert(
                                id,
                                super::qubes::QubesBackendData {
                                    surface: Kind::Toplevel(surface.clone()),
                                    has_configured: false,
                                    coordinates: Default::default(),
                                }
                            )
                            .is_none());
                        (id, size)
                    })
                    .expect("TODO: handling dead clients");
                    let ref mut agent = anvil_state.backend_data.borrow_mut().agent;
                    let msg = qubes_gui::Create {
                        rectangle: qubes_gui::Rectangle {
                            top_left: qubes_gui::Coordinates { x: 0, y: 0 },
                            size: qubes_gui::WindowSize {
                                width: size.w.max(1) as _,
                                height: size.h.max(1) as _,
                            },
                        },
                        parent: None,
                        override_redirect: 0,
                    };
                    debug!(anvil_state.log, "Creating window {}", id);
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
                    let raw_surface = match surface.get_surface() {
                        Some(s) => s,
                        // If there is no underlying surface just ignore the request
                        None => {
                            debug!(
                                anvil_state.log,
                                "Ignoring request to create window with no surface"
                            );
                            return;
                        }
                    };
                    let (id, size) = with_states(raw_surface, |data| {
                        let popup_data = data
                            .data_map
                            .get::<Mutex<xdg::XdgPopupSurfaceRoleAttributes>>()
                            .expect("Smithay always creates this data; qed")
                            .lock()
                            .expect("Poisoned?");
                        let size = (*popup_data).current.geometry.size;
                        drop(popup_data);
                        data.data_map
                            .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                                RefCell::new(QubesData::data(anvil_state.backend_data.clone()))
                            });
                        let id = data
                            .data_map
                            .get::<RefCell<SurfaceData>>()
                            .expect("this data was just inserted above, so it will be present; qed")
                            .borrow()
                            .window;
                        assert!(anvil_state
                            .backend_data
                            .borrow_mut()
                            .map
                            .insert(
                                id,
                                super::qubes::QubesBackendData {
                                    surface: Kind::Popup(surface.clone()),
                                    has_configured: false,
                                    coordinates: Default::default(),
                                }
                            )
                            .is_none());
                        (id, size)
                    })
                    .expect("TODO: handling dead clients");
                    let ref mut agent = anvil_state.backend_data.borrow_mut().agent;
                    let msg = qubes_gui::Create {
                        rectangle: qubes_gui::Rectangle {
                            top_left: qubes_gui::Coordinates { x: 0, y: 0 },
                            size: qubes_gui::WindowSize {
                                width: size.w.max(1) as _,
                                height: size.h.max(1) as _,
                            },
                        },
                        parent: None,
                        override_redirect: 0,
                    };
                    debug!(anvil_state.log, "Creating window {}", id);
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
                XdgRequest::AckConfigure { surface, configure } => {
                    let size = match configure {
                        xdg::Configure::Toplevel(configure) => {
                            configure.state.size.unwrap_or_else(|| (256, 256).into())
                        }
                        xdg::Configure::Popup(configure) => configure.state.geometry.size,
                    };
                    with_states(&surface, |data| {
                        let mut anvil_state = anvil_state.backend_data.borrow_mut();
                        let state = data
                            .data_map
                            .get::<RefCell<SurfaceData>>()
                            .unwrap()
                            .borrow();
                        debug!(
                            anvil_state.log,
                            "A configure event was acknowledged!  Params: surface {:?}", surface,
                        );
                        let msg = &qubes_gui::Configure {
                            rectangle: qubes_gui::Rectangle {
                                top_left: state.coordinates,
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
                    return;
                }
                XdgRequest::UnMaximize { surface } => {
                    let _wl_surface = if let Some(surface) = surface.get_surface() {
                        surface
                    } else {
                        // If there is no underlying surface just ignore the request
                        return;
                    };
                    let _msg = qubes_gui::WindowFlags { set: 0, unset: 1 };
                    todo!()
                }
                XdgRequest::NewClient { client } => {
                    info!(anvil_state.log, "New client connected!");
                    client
                        .with_data(|data| {
                            data.insert_if_missing(|| {
                                QubesClient(Rc::new(RefCell::new(BTreeMap::new())))
                            })
                        })
                        .expect("New clients are not dead");
                }
                XdgRequest::ClientPong { client: _ } => {}
                XdgRequest::Grab {
                    surface: _,
                    seat: _,
                    serial: _,
                } => {
                    // do nothing - pointer events are under the control of the
                    // trunk compositor and there is nothing this compositor can
                    // do about them.
                }
                XdgRequest::Move {
                    surface: _,
                    seat: _,
                    serial: _,
                } => {
                    // do nothing - these are handled by the trunk compositor
                    // this will change when subsurface support is added
                }
                XdgRequest::Resize {
                    surface: _,
                    seat: _,
                    serial: _,
                    edges: _,
                } => {
                    // do nothing - these are handled by the trunk compositor
                    // this will change when subsurface support is added
                }
                XdgRequest::Maximize { surface: _ } => {
                    // not yet implemented
                }
                XdgRequest::UnFullscreen { surface: _ } => {
                    // not yet implemented
                }
                XdgRequest::Minimize { surface: _ } => {
                    // not yet implemented
                }
                XdgRequest::ShowWindowMenu {
                    surface: _,
                    seat: _,
                    serial: _,
                    location: _,
                } => {
                    // do nothing
                }
            }
        },
        log,
    );

    ShellHandles {
        xdg_state: xdg_shell_state,
    }
}
const BYTES_PER_PIXEL: i32 = qubes_gui::DUMMY_DRV_FB_BPP as i32 / 8;

pub struct SurfaceData {
    pub buffer: Option<(wl_buffer::WlBuffer, qubes_gui_client::agent::Buffer)>,
    pub geometry: Option<Rectangle<i32, Logical>>,
    pub buffer_dimensions: Option<Size<i32, Physical>>,
    pub coordinates: qubes_gui::Coordinates,
    pub buffer_swapped: bool,
    pub buffer_scale: i32,
    pub window: std::num::NonZeroU32,
    pub qubes: Rc<RefCell<QubesData>>,
}

impl SurfaceData {
    fn process_new_buffer(
        &mut self,
        untrusted_slice: &[u8],
        shm::BufferData {
            offset: untrusted_offset,
            width: untrusted_width,
            height: untrusted_height,
            stride: untrusted_stride,
            format: _, // already validated
        }: shm::BufferData,
        buffer: &wl_buffer::WlBuffer,
    ) {
        let buffer_length: i32 =
            i32::try_from(untrusted_slice.len()).expect("Smithay rejects bad pool sizes");
        // SANTIIZE START
        if untrusted_width <= 0
            || untrusted_height <= 0
            || untrusted_width > qubes_gui::MAX_WINDOW_WIDTH as _
            || untrusted_height > qubes_gui::MAX_WINDOW_HEIGHT as _
        {
            buffer.as_ref().post_error(
                wl_shm::Error::InvalidStride as u32,
                format!(
                    "Dimensions {}x{} not valid: must be between 1x1 and {}x{} inclusive",
                    untrusted_width,
                    untrusted_height,
                    qubes_gui::MAX_WINDOW_WIDTH,
                    qubes_gui::MAX_WINDOW_HEIGHT,
                ),
            );
            return;
        }
        if untrusted_stride / BYTES_PER_PIXEL < untrusted_width {
            buffer.as_ref().post_error(
                wl_shm::Error::InvalidStride as u32,
                format!(
                    "Stride {} too small for width {}",
                    untrusted_stride, untrusted_width
                ),
            );
            return;
        }
        if untrusted_offset < 0
            || untrusted_offset >= buffer_length
            || i32::checked_mul(untrusted_stride, untrusted_height)
                .map(|untrusted_length| buffer_length - untrusted_offset < untrusted_length)
                .unwrap_or(true)
        {
            let msg = format!(
                "bad pool: offset {} stride {} width {} height {} buffer length {}",
                untrusted_offset,
                untrusted_stride,
                untrusted_width,
                untrusted_height,
                buffer_length
            );
            buffer
                .as_ref()
                .post_error(wl_shm::Error::InvalidStride as u32, msg);
            return;
        }
        // SANITIZE END
        self.buffer_dimensions = Some((untrusted_width, untrusted_height).into());
    }
    fn process_new_buffers(&mut self, attrs: BufferAssignment, data: &mut QubesData, scale: i32) {
        let agent = &mut data.agent;
        self.buffer_dimensions = None;
        match attrs {
            BufferAssignment::NewBuffer { buffer, .. } => {
                debug!(data.log, "New buffer");
                // new contents
                if shm::with_buffer_contents(&buffer, |a, b| self.process_new_buffer(a, b, &buffer))
                    .is_err()
                {
                    let err = "internal error: bad buffer not rejected by Smithay".into();
                    buffer.as_ref().post_error(3, err);
                    return;
                }

                let Size { w, h, .. } = match self.buffer_dimensions {
                    None => return, // invalid data
                    Some(data) => data,
                };
                self.buffer_scale = scale;
                let qbuf = match agent.alloc_buffer(w as _, h as _) {
                    Err(_) => {
                        // 2 is no_memory
                        buffer
                            .as_ref()
                            .post_error(2, "Failed to allocate Xen shared memory".into());
                        return;
                    }
                    Ok(qbuf) => qbuf,
                };
                self.buffer_swapped = true;
                if let Some(old_buffer) = std::mem::replace(&mut self.buffer, Some((buffer, qbuf)))
                {
                    old_buffer.0.release();
                    drop(old_buffer.1);
                }
            }
            BufferAssignment::Removed => {
                // remove the contents
                debug!(data.log, "Removing buffer");
                self.buffer = None;
            }
        }
    }

    pub fn update_buffer(
        &mut self,
        attrs: &mut SurfaceAttributes,
        data: &mut QubesData,
        geometry: Option<Rectangle<i32, Logical>>,
    ) {
        debug!(data.log, "Updating buffer with {:?}", attrs);
        if let Some(assignment) = attrs.buffer.take() {
            self.process_new_buffers(assignment, data, attrs.buffer_scale)
        }
        if self.buffer_dimensions.is_none() {
            return;
        }
        let ref mut agent = data.agent;
        debug!(data.log, "Damage: {:?}!", &attrs.damage);
        if !attrs.damage.is_empty() {
            if let Some((buffer, qbuf)) = self.buffer.as_ref() {
                debug!(data.log, "Performing damage calculation");
                let log = data.log.clone();
                match shm::with_buffer_contents(
                    buffer,
                    |untrusted_slice: &[u8],
                     shm::BufferData {
                         offset,
                         width,
                         height,
                         stride,
                         format: _,
                     }| {
                        debug!(log, "Updating buffer with damaged areas");
                        if !attrs.damage.is_empty() && true {
                            attrs.damage.clear();
                            attrs.damage.push(Damage::Buffer(Rectangle {
                                loc: (0, 0).into(),
                                size: (width, height).into(),
                            }));
                        }
                        for i in &attrs.damage {
                            let (untrusted_loc, untrusted_size) = match *i {
                                Damage::Surface(r) => {
                                    let r = r.to_buffer(self.buffer_scale);
                                    (r.loc, r.size)
                                }
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
                                || untrusted_loc.x > width
                                || untrusted_loc.y > height
                            {
                                buffer.as_ref().post_error(
                                    wl_shm::Error::InvalidStride as u32,
                                    "Invalid damage region".to_owned(),
                                );
                                return None;
                            }
                            let mut w = untrusted_size.w.min(width - untrusted_loc.x);
                            let mut h = untrusted_size.w.min(height - untrusted_loc.y);
                            let (x, y) = (untrusted_loc.x, untrusted_loc.y);
                            // MEGA-HACK FOR QUBES
                            //
                            // Qubes CANNOT render outside of the bounding box.  Move the window!
                            let (source_x, source_y) = if let Some(geometry) = geometry {
                                let x = if geometry.loc.x > 0 && w > geometry.loc.x {
                                    w -= geometry.loc.x;
                                    x + geometry.loc.x
                                } else {
                                    x
                                };
                                let y = if geometry.loc.y > 0 && h > geometry.loc.y {
                                    h -= geometry.loc.y;
                                    y + geometry.loc.y
                                } else {
                                    y
                                };
                                (x, y)
                            } else {
                                (x, y)
                            };

                            let subslice = &untrusted_slice[(offset
                                + BYTES_PER_PIXEL * source_x
                                + source_y * stride)
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
                            }
                        }
                        return Some((width, height, std::mem::replace(&mut attrs.damage, vec![])));
                    },
                ) {
                    Ok(Some((width, height, damage))) => {
                        if self.buffer_swapped {
                            self.buffer
                                .as_ref()
                                .expect("buffer_swapped never set unless a buffer is present; qed")
                                .1
                                .dump(agent.client(), self.window.into())
                                .expect("TODO");
                            let Size { w, h, .. } = geometry
                                .map(|g| g.size.to_physical(self.buffer_scale))
                                .unwrap_or_else(|| {
                                    self.buffer_dimensions.expect(
                                        "buffer_dimensions are Some if a buffer is present; qed",
                                    )
                                });
                            let msg = qubes_gui::Configure {
                                rectangle: qubes_gui::Rectangle {
                                    top_left: self.coordinates,
                                    size: qubes_gui::WindowSize {
                                        width: w
                                            .try_into()
                                            .expect("negative sizes rejected earlier; qed"),
                                        height: h
                                            .try_into()
                                            .expect("negative sizes rejected earlier; qed"),
                                    },
                                },
                                override_redirect: 0,
                            };
                            agent.client().send(&msg, self.window.into()).expect("TODO");
                        }
                        for i in damage {
                            let (loc, size) = match i {
                                Damage::Surface(r) => {
                                    let r = r.to_buffer(self.buffer_scale);
                                    (r.loc, r.size)
                                }
                                Damage::Buffer(Rectangle { loc, size }) => (loc, size),
                            };
                            let w = size.w.min(width - loc.x);
                            let h = size.w.min(height - loc.y);
                            let (x, y) = (loc.x, loc.y);
                            let output_message = qubes_gui::ShmImage {
                                rectangle: qubes_gui::Rectangle {
                                    top_left: qubes_gui::Coordinates {
                                        x: x as u32,
                                        y: y as u32,
                                    },
                                    size: qubes_gui::WindowSize {
                                        width: w as u32,
                                        height: h as u32,
                                    },
                                },
                            };
                            agent
                                .client()
                                .send(&output_message, self.window.into())
                                .expect("TODO");
                        }
                    }
                    Err(shm::BufferAccessError::NotManaged) => panic!("strange shm buffer"),
                    Err(shm::BufferAccessError::BadMap) | Ok(None) => return,
                }
            }
        }
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

fn surface_commit(surface: &WlSurface, backend_data: &Rc<RefCell<QubesData>>) {
    debug!(
        backend_data.borrow().log,
        "Got a commit for surface {:?}", surface
    );
    #[cfg(feature = "xwayland")]
    super::xwayland::commit_hook(surface);

    if !is_sync_subsurface(surface) {
        // Update the buffer of all child surfaces
        with_surface_tree_downward(
            surface,
            None,
            |_surface: &WlSurface,
             surface_data: &compositor::SurfaceData,
             &parent: &Option<NonZeroU32>| {
                surface_data
                    .data_map
                    .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                        assert!(parent.is_none());
                        let data = RefCell::new(QubesData::data(backend_data.clone()));
                        let msg = qubes_gui::Create {
                            rectangle: qubes_gui::Rectangle {
                                top_left: qubes_gui::Coordinates { x: 0, y: 0 },
                                size: qubes_gui::WindowSize {
                                    width: 256,
                                    height: 256,
                                },
                            },
                            parent,
                            override_redirect: 0,
                        };
                        backend_data
                            .borrow_mut()
                            .agent
                            .client()
                            .send(&msg, data.borrow().window)
                            .expect("TODO: send errors");
                        data
                    });
                let res: Option<NonZeroU32> = surface_data
                    .data_map
                    .get::<RefCell<SurfaceData>>()
                    .unwrap()
                    .borrow()
                    .window
                    .into();
                TraversalAction::DoChildren(res)
            },
            |_surface: &WlSurface,
             states: &compositor::SurfaceData,
             &_parent: &Option<NonZeroU32>| {
                let geometry = states
                    .cached_state
                    .current::<xdg::SurfaceCachedState>()
                    .geometry;
                states
                    .data_map
                    .get::<RefCell<SurfaceData>>()
                    .unwrap()
                    .borrow_mut()
                    .update_buffer(
                        &mut *states.cached_state.current::<SurfaceAttributes>(),
                        &mut *backend_data.borrow_mut(),
                        geometry,
                    );
            },
            |_surface: &WlSurface, _surface_data, _| true,
        );
    }
}
