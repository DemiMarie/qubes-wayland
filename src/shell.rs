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
    utils::{Buffer, Logical, Physical, Point, Rectangle, Size},
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

mod geometry;

#[derive(Clone)]
pub struct ShellHandles {
    pub xdg_state: Arc<Mutex<XdgShellState>>,
}

struct QubesClient(Rc<RefCell<BTreeMap<u32, ()>>>);

fn send_window_flags(
    anvil_state: &mut AnvilState,
    surface: xdg::ToplevelSurface,
    msg: qubes_gui::WindowFlags,
) {
    let surface = if let Some(surface) = surface.get_surface() {
        surface
    } else {
        // If there is no underlying surface just ignore the request
        return;
    };
    with_states(&surface, |data| {
        let mut anvil_state = anvil_state.backend_data.borrow_mut();
        let state = data
            .data_map
            .get::<RefCell<SurfaceData>>()
            .unwrap()
            .borrow();
        anvil_state.agent.send(&msg, state.window).unwrap()
    })
    .expect("get_surface() only returns live resources")
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
                    anvil_state.output.enter(raw_surface);
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
                    trace!(anvil_state.log, "Creating window"; "id" => u32::from(id));
                    agent.send(&msg, id).expect("TODO: send errors");
                    let msg = qubes_gui::Configure {
                        rectangle: msg.rectangle,
                        override_redirect: msg.override_redirect,
                    };
                    agent.send(&msg, id).expect("TODO: send errors");
                    agent
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
                    positioner,
                } => {
                    info!(
                        anvil_state.log,
                        "Creating popup";
                        "surface" => ?surface,
                        "positioner" => ?positioner,
                    );
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
                    anvil_state.output.enter(raw_surface);
                    let (id, geometry) = with_states(raw_surface, |data| {
                        let popup_data = data
                            .data_map
                            .get::<Mutex<xdg::XdgPopupSurfaceRoleAttributes>>()
                            .expect("Smithay always creates this data; qed")
                            .lock()
                            .expect("Poisoned?");
                        let geometry = (*popup_data).current.geometry;
                        drop(popup_data);
                        data.data_map
                            .insert_if_missing::<RefCell<SurfaceData>, _>(|| {
                                RefCell::new(QubesData::data(anvil_state.backend_data.clone()))
                            });
                        let mut mut_data = data
                            .data_map
                            .get::<RefCell<SurfaceData>>()
                            .expect("this data was just inserted above, so it will be present; qed")
                            .borrow_mut();
                        mut_data.coordinates.x = geometry.loc.x as _;
                        mut_data.coordinates.y = geometry.loc.y as _;
                        assert!(anvil_state
                            .backend_data
                            .borrow_mut()
                            .map
                            .insert(
                                mut_data.window,
                                super::qubes::QubesBackendData {
                                    surface: Kind::Popup(surface.clone()),
                                    has_configured: false,
                                    coordinates: geometry.loc,
                                }
                            )
                            .is_none());
                        (mut_data.window, geometry)
                    })
                    .expect("TODO: handling dead clients");
                    let ref mut agent = anvil_state.backend_data.borrow_mut().agent;
                    let msg = qubes_gui::Create {
                        rectangle: qubes_gui::Rectangle {
                            top_left: qubes_gui::Coordinates {
                                x: geometry.loc.x as _,
                                y: geometry.loc.y as _,
                            },
                            size: qubes_gui::WindowSize {
                                width: geometry.size.w.max(1) as _,
                                height: geometry.size.h.max(1) as _,
                            },
                        },
                        parent: None,
                        override_redirect: 1,
                    };
                    trace!(anvil_state.log, "Creating window"; "id" => u32::from(id));
                    agent.send(&msg, id).expect("TODO: send errors");
                    let msg = qubes_gui::Configure {
                        rectangle: msg.rectangle,
                        override_redirect: msg.override_redirect,
                    };
                    agent.send(&msg, id).expect("TODO: send errors");
                    agent
                        .send(
                            &qubes_gui::MapInfo {
                                override_redirect: 1,
                                transient_for: 0,
                            },
                            id,
                        )
                        .unwrap();
                    let _ = surface.send_configure();
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
                        anvil_state.agent.send(msg, state.window).unwrap()
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
                } => send_window_flags(
                    anvil_state,
                    surface,
                    qubes_gui::WindowFlags {
                        set: qubes_gui::WindowFlag::Fullscreen as _,
                        unset: 0,
                    },
                ),
                XdgRequest::UnMaximize { surface: _ } => { /* not implemented */ }
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
                XdgRequest::UnFullscreen { surface } => send_window_flags(
                    anvil_state,
                    surface,
                    qubes_gui::WindowFlags {
                        set: 0,
                        unset: qubes_gui::WindowFlag::Fullscreen as _,
                    },
                ),
                XdgRequest::Minimize { surface } => send_window_flags(
                    anvil_state,
                    surface,
                    qubes_gui::WindowFlags {
                        set: qubes_gui::WindowFlag::Minimize as _,
                        unset: 0,
                    },
                ),
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
    pub buffer: Option<(wl_buffer::WlBuffer, qubes_gui_gntalloc::Buffer)>,
    pub geometry: Option<Rectangle<i32, Buffer>>,
    pub buffer_dimensions: Option<Size<i32, Physical>>,
    pub coordinates: qubes_gui::Coordinates,
    pub buffer_swapped: bool,
    pub buffer_scale: i32,
    pub window: std::num::NonZeroU32,
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
        let connection = &mut data.connection;
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
                if let Some((old_buffer, qbuf)) = &mut self.buffer {
                    if qbuf.width() as i32 != w || qbuf.height() as i32 != h {
                        *qbuf = match connection.alloc_buffer(w as _, h as _) {
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
                    }
                    std::mem::replace(old_buffer, buffer).release();
                } else {
                    let qbuf = match connection.alloc_buffer(w as _, h as _) {
                        Err(_) => {
                            // 2 is no_memory
                            buffer
                                .as_ref()
                                .post_error(2, "Failed to allocate Xen shared memory".into());
                            return;
                        }
                        Ok(qbuf) => qbuf,
                    };
                    self.buffer = Some((buffer, qbuf));
                    self.buffer_swapped = true;
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
        if attrs.damage.is_empty() {
            return;
        }
        let (buffer, qbuf) = match self.buffer.as_ref() {
            Some(s) => s,
            None => return,
        };
        if let Some(geometry) = geometry {
            let mut geometry = geometry.to_buffer(self.buffer_scale);
            if geometry.size.w <= 0 || geometry.size.h <= 0 {
                buffer
                    .as_ref()
                    .post_error(3, "TODO: find a better error for negative geometry".into());
                return;
            }
            if geometry.loc.x < 0i32 {
                geometry.size.w = (geometry.size.w + geometry.loc.x).max(0i32);
                geometry.loc.x = 0;
            }
            if geometry.loc.y < 0i32 {
                geometry.size.h = (geometry.size.h + geometry.loc.y).max(0i32);
                geometry.loc.y = 0;
            }
            self.geometry = Some(geometry);
        } else {
            self.geometry = None;
        }
        debug!(data.log, "Performing damage calculation");
        let log = data.log.clone();
        let damage = match shm::with_buffer_contents(
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
                debug_assert!(
                    height * stride + offset <= untrusted_slice.len().try_into().unwrap()
                );
                if self.buffer_swapped {
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

                    let damage = Rectangle {
                        loc: untrusted_loc,
                        size: (
                            untrusted_size.w.min(width - untrusted_loc.x),
                            untrusted_size.h.min(height - untrusted_loc.y),
                        )
                            .into(),
                    };
                    // SANITIZE END
                    debug_assert!(damage.loc.y + damage.size.h <= height);
                    debug_assert!(damage.loc.x + damage.size.w <= width);

                    let (damage, dest_loc) = if let Some(geometry) = self.geometry {
                        debug_assert!(geometry.loc.x >= 0);
                        debug_assert!(geometry.loc.y >= 0);
                        debug_assert!(geometry.size.w >= 0);
                        debug_assert!(geometry.size.h >= 0);
                        debug_assert!(damage.loc.x >= 0);
                        debug_assert!(damage.loc.y >= 0);
                        debug_assert!(damage.size.w >= 0);
                        debug_assert!(damage.size.h >= 0);
                        match geometry::adjust_damage_source(damage, geometry) {
                            None => continue,
                            Some(damage) => (damage, damage.loc - geometry.loc),
                        }
                    } else {
                        (damage, damage.loc)
                    };
                    debug_assert!(damage.loc.y + damage.size.h <= height);
                    debug_assert!(damage.loc.x + damage.size.w <= width);
                    debug_assert!(
                        height * stride + offset <= untrusted_slice.len().try_into().unwrap()
                    );
                    debug_assert!(
                        (damage.loc.y + damage.size.h) * stride
                            <= untrusted_slice.len().try_into().unwrap()
                    );
                    let copy_start: usize = (offset + BYTES_PER_PIXEL * damage.loc.x + stride * damage.loc.y)
                        .try_into()
                        .expect("this was validated to be within range above, so the conversion will succeed; qed");
                    debug_assert!(copy_start < untrusted_slice.len());
                    let bytes_to_write: usize = (BYTES_PER_PIXEL * damage.size.w)
                        .try_into()
                        .expect("this was validated to be within range above, so the conversion will succeed; qed");
                    let subslice = &untrusted_slice[copy_start..];
                    // trace!(data.log, "Copying data!");
                    for i in 0..damage.size.h {
                        let start_offset = (i * stride).try_into().expect("bounds already checked");
                        let offset_in_dest_buffer = (BYTES_PER_PIXEL
                            * (dest_loc.x + (i + dest_loc.y) * width))
                            .try_into()
                            .expect("bounds already checked");
                        qbuf.write(
                            &subslice[start_offset..bytes_to_write + start_offset],
                            offset_in_dest_buffer,
                        );
                    }
                }
                return Some(std::mem::replace(&mut attrs.damage, vec![]));
            },
        ) {
            Err(shm::BufferAccessError::NotManaged) => panic!("strange shm buffer"),
            Err(shm::BufferAccessError::BadMap) | Ok(None) => return,
            Ok(Some(d)) => d,
        };
        if self.buffer_swapped {
            let msg = self
                .buffer
                .as_ref()
                .expect("buffer_swapped never set unless a buffer is present; qed")
                .1
                .msg();
            agent
                .send_raw(msg, self.window, qubes_gui::MSG_WINDOW_DUMP)
                .unwrap();
        }
        if let Some(Size { w, h, .. }) = geometry
            .map(|g| g.size.to_physical(self.buffer_scale))
            .or(self.buffer_dimensions)
        {
            let msg = qubes_gui::Configure {
                rectangle: qubes_gui::Rectangle {
                    top_left: self.coordinates,
                    size: qubes_gui::WindowSize {
                        width: w.try_into().expect("negative sizes rejected earlier; qed"),
                        height: h.try_into().expect("negative sizes rejected earlier; qed"),
                    },
                },
                override_redirect: 0,
            };
            agent.send(&msg, self.window.into()).expect("TODO");
        }
        self.buffer_swapped = false;
        for i in damage {
            let (loc, size) = match i {
                Damage::Surface(r) => {
                    let r = r.to_buffer(self.buffer_scale);
                    (r.loc, r.size)
                }
                Damage::Buffer(Rectangle { loc, size }) => (loc, size),
            };
            let Rectangle {
                loc: Point { x, y, .. },
                size: Size { w, h, .. },
            } = if let Some(geometry) = self.geometry {
                match geometry::adjust_damage_destination(Rectangle { loc, size }, geometry) {
                    Some(damage) => damage,
                    None => continue,
                }
            } else {
                Rectangle { loc, size }
            };
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
                .send(&output_message, self.window.into())
                .expect("TODO");
        }
    }

    /// Returns the size of the surface.
    pub fn size(&self) -> Option<Size<i32, Logical>> {
        self.buffer_dimensions
            .map(|dims| dims.to_logical(self.buffer_scale))
    }
}

fn surface_commit(surface: &WlSurface, backend_data: &Rc<RefCell<QubesData>>) {
    debug!(
        backend_data.borrow().log,
        "Got a commit for surface {:?}", surface
    );
    #[cfg(feature = "xwayland")]
    super::xwayland::commit_hook(surface);

    if is_sync_subsurface(surface) {
        return;
    }
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
        |_surface: &WlSurface, states: &compositor::SurfaceData, &_parent: &Option<NonZeroU32>| {
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
