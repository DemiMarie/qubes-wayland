use std::{
    cell::RefCell, collections::BTreeMap, convert::TryInto, num::NonZeroU32, os::unix::io::AsRawFd,
    rc::Rc, sync::atomic::Ordering, task::Poll, time::Duration,
};

use qubes_castable::Castable;
use smithay::{
    reexports::{
        calloop::{self, generic::Generic, EventLoop, Interest},
        wayland_server::Display,
    },
    wayland::{
        compositor::{with_surface_tree_upward, SurfaceAttributes, TraversalAction},
        shell::xdg::ToplevelSurface,
    },
};

use slog::Logger;

use crate::shell::SurfaceData;
use crate::state::{AnvilState, Backend};

pub const OUTPUT_NAME: &str = "qubes";

pub struct QubesData {
    pub agent: qubes_gui_client::agent::Agent,
    wid: u32,
    pub map: BTreeMap<NonZeroU32, QubesBackendData>,
    pub log: slog::Logger,
    buf: qubes_gui_client::agent::Buffer,
}

pub struct QubesBackendData {
    /// Surface ID
    //pub id: NonZeroU32,
    /// Toplevel surface
    pub surface: ToplevelSurface,
    /// Whether the surface has been configured
    pub has_configured: bool,
}

impl Drop for QubesData {
    fn drop(&mut self) {
        eprintln!("Dropping connection data!")
    }
}

impl Backend for QubesData {
    fn seat_name(&self) -> String {
        String::from("qubes")
    }
}

impl QubesData {
    pub fn id(&mut self) -> NonZeroU32 {
        let id = self.wid;
        self.wid = id
            .checked_add(1)
            .expect("not yet implemented: wid wrapping");
        id.try_into()
            .expect("IDs start at 2 and do not wrap, so they cannot be zero; qed")
    }
    pub fn data(qubes: Rc<RefCell<Self>>) -> SurfaceData {
        let window = qubes.borrow_mut().id();
        eprintln!("SurfaceData created!");
        SurfaceData {
            buffer: None,
            geometry: None,
            buffer_dimensions: None,
            buffer_scale: 0,
            window,
            qubes,
        }
    }

    fn process_configure(&mut self, m: qubes_gui::Configure, window: u32) -> std::io::Result<()> {
        self.agent
            .client()
            .send(&m, window.try_into().unwrap())
            .unwrap();
        let window = window.try_into().expect("Configure event for window 0?");
        if window == 1.try_into().unwrap() {
            self.process_self_configure(m, window)
        } else {
            self.process_client_configure(m, window)
        }
    }

    fn process_client_configure(
        &mut self,
        m: qubes_gui::Configure,
        window: NonZeroU32,
    ) -> std::io::Result<()> {
        let qubes_gui::WindowSize { width, height } = m.rectangle.size;
        self.agent.client().send(
            &qubes_gui::ShmImage {
                rectangle: m.rectangle,
            },
            window,
        )?;
        match self.map.get_mut(&window.try_into().unwrap()) {
            None => panic!("Configure event for unknown window"),
            Some(QubesBackendData {
                surface,
                ref mut has_configured,
            }) => {
                match surface.with_pending_state(|state| {
                    let new_size = Some((width as _, height as _).into());
                    let do_send = new_size == state.size;
                    state.size = new_size;
                    do_send
                }) {
                    Ok(false) if *has_configured => {
                        trace!(self.log, "Ignoring configure event that didn’t change size")
                    }
                    Ok(_) => {
                        info!(self.log, "Sending configure event to application");
                        surface.send_configure();
                        *has_configured = true;
                    }
                    Err(_) => warn!(self.log, "Ignoring MSG_CONFIGURE on dead window"),
                }
            }
        }
        Ok(())
    }

    fn process_self_configure(
        &mut self,
        m: qubes_gui::Configure,
        window: NonZeroU32,
    ) -> std::io::Result<()> {
        let qubes_gui::WindowSize { width, height } = m.rectangle.size;
        drop(std::mem::replace(
            &mut self.buf,
            self.agent.alloc_buffer(width, height).unwrap(),
        ));
        let shade = vec![0xFF00u32; (width * height / 2).try_into().unwrap()];
        self.buf.dump(self.agent.client(), window.into())?;
        self.buf.write(
            qubes_castable::as_bytes(&shade[..]),
            (width * height / 4 * 4).try_into().unwrap(),
        );
        self.agent.client().send(&m, window)
    }
}

pub fn run_qubes(log: Logger) {
    let mut event_loop = EventLoop::try_new().unwrap();
    let display = Rc::new(RefCell::new(Display::new()));

    /*
     * Initialize the globals
     */

    let mut agent = qubes_gui_client::agent::new(0).unwrap();
    // we now have a agent 🙂
    info!(log, "🙂 Somebody connected to us, yay!");
    debug!(log, "Configuration parameters: {:?}", agent.conf());
    debug!(log, "Creating window");
    let (width, height) = (0x200, 0x100);
    let my_window = 1.try_into().unwrap();
    agent
        .client()
        .send(
            &qubes_gui::Create {
                rectangle: qubes_gui::Rectangle {
                    top_left: qubes_gui::Coordinates { x: 50, y: 400 },
                    size: qubes_gui::WindowSize { width, height },
                },
                parent: None,
                override_redirect: 0,
            },
            my_window,
        )
        .unwrap();
    let buf = agent.alloc_buffer(width, height).unwrap();
    let shade = vec![0xFF00u32; (width * height / 2).try_into().unwrap()];
    buf.dump(agent.client(), 1.try_into().unwrap()).unwrap();
    buf.write(
        qubes_castable::as_bytes(&shade[..]),
        (width * height).try_into().unwrap(),
    );
    agent
        .client()
        .send(
            &qubes_gui::MapInfo {
                override_redirect: 0,
                transient_for: 0,
            },
            my_window,
        )
        .unwrap();
    let raw_fd = agent.as_raw_fd();
    let data = QubesData {
        agent: agent,
        wid: 2,
        map: BTreeMap::default(),
        log: log.clone(),
        buf,
    };
    let mut state = AnvilState::init(
        display.clone(),
        event_loop.handle(),
        data,
        log.clone(),
        true,
    );
    let handle = event_loop.handle();
    let log_ = log.clone();
    handle
        .insert_source(
            Generic::from_fd(
                raw_fd,
                Interest {
                    readable: true,
                    writable: true,
                },
                calloop::Mode::Edge,
            ),
            move |readiness: calloop::Readiness, _fd, agent_full| {
                if !readiness.readable && !readiness.writable {
                    panic!("No readiness?");
                    // return Ok(calloop::PostAction::Continue);
                }
                let ref mut qubes = agent_full.backend_data.borrow_mut();
                qubes.agent.client().wait();
                loop {
                    let (e, body) = match qubes.agent.client().read_header() {
                        Poll::Pending => break Ok(calloop::PostAction::Continue),
                        Poll::Ready(Ok((e, body))) => (e, body),
                        Poll::Ready(Err(e)) => {
                            error!(log_, "Critical Qubes Error: {}", e);
                            break Err(e);
                        }
                    };
                    match e.ty {
                        qubes_gui::MSG_MOTION => {
                            let mut m = qubes_gui::Motion::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Motion event: {:?}", m);
                        }
                        qubes_gui::MSG_CROSSING => {
                            let mut m = qubes_gui::Crossing::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Crossing event: {:?}", m)
                        }
                        qubes_gui::MSG_CLOSE => {
                            assert!(body.is_empty());
                            trace!(log_, "Got a close event!");
                            if e.window == 1 {
                                trace!(log_, "Got close event for our window, exiting!");
                                agent_full.running.store(false, Ordering::SeqCst);
                                break Ok(calloop::PostAction::Continue);
                            }
                            match qubes.map.get(&e.window.try_into().unwrap()) {
                                None => error!(log_, "Close event for unknown window {}", e.window),
                                Some(w) => w.surface.send_close(),
                            };
                        }
                        qubes_gui::MSG_KEYPRESS => {
                            let mut m = qubes_gui::Button::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Key pressed: {:?}", m);
                        }
                        qubes_gui::MSG_BUTTON => {
                            let mut m = qubes_gui::Button::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Button event: {:?}", m);
                        }
                        qubes_gui::MSG_CLIPBOARD_REQ => trace!(log_, "clipboard data requested!"),
                        qubes_gui::MSG_CLIPBOARD_DATA => trace!(log_, "clipboard data reply!"),
                        qubes_gui::MSG_KEYMAP_NOTIFY => {
                            let mut m = qubes_gui::KeymapNotify::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Keymap notification: {:?}", m);
                        }
                        qubes_gui::MSG_MAP => {
                            let mut m = qubes_gui::MapInfo::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Map event: {:?}", m);
                        }
                        qubes_gui::MSG_CONFIGURE => {
                            let mut m = qubes_gui::Configure::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Configure event {:?} for window {}", m, e.window);
                            qubes.process_configure(m, e.window)?
                        }
                        qubes_gui::MSG_FOCUS => {
                            let mut m = qubes_gui::Focus::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Focus event: {:?}", m);
                        }
                        qubes_gui::MSG_WINDOW_FLAGS => {
                            let mut m = qubes_gui::WindowFlags::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            trace!(log_, "Window manager flags have changed: {:?}", m);
                        }
                        _ => trace!(log_, "Ignoring unknown event!"),
                    }
                }
            },
        )
        .unwrap();
    {
        let log = log.clone();
        let redraw_timer =
            calloop::timer::Timer::new().expect("not worth handling can’t create timer");
        let timer_handle = redraw_timer.handle();
        timer_handle.add_timeout(std::time::Duration::from_millis(16), ());
        let mut time_spent = 0;
        handle
            .insert_source(redraw_timer, move |(), timer_handle, agent_full| {
                // trace!(log, "Timer callback fired, reregistering!");
                time_spent += 16;
                timer_handle.add_timeout(std::time::Duration::from_millis(16), ());
                let ref mut qubes = agent_full.backend_data.borrow_mut();
                let mut dead_surfaces = vec![];
                for (key, value) in qubes.map.iter_mut() {
                    let surface = match value.surface.get_surface() {
                        None => {
                            trace!(log, "Pushing toplevel with no surface onto dead list");
                            dead_surfaces.push(*key);
                            continue;
                        }
                        Some(s) if !s.as_ref().is_alive() => {
                            trace!(log, "Pushing toplevel with dead surface onto dead list");
                            dead_surfaces.push(*key);
                            continue;
                        }
                        Some(s) => s,
                    };
                    with_surface_tree_upward(
                        &surface,
                        (),
                        |_surface, _surface_data, ()| TraversalAction::DoChildren(()),
                        |_surface, states, ()| {
                            SurfaceData::send_frame(
                                &mut *states.cached_state.current::<SurfaceAttributes>(),
                                time_spent,
                            );
                        },
                        |_surface, _surface_data, ()| true,
                    );
                }
                for i in dead_surfaces.iter() {
                    qubes
                        .agent
                        .client()
                        .send(&qubes_gui::Destroy {}, *i)
                        .unwrap();
                    let _: QubesBackendData = qubes
                        .map
                        .remove(i)
                        .expect("these were keys in the map; qed");
                }
            })
            .expect("FIXME: handle initialization failed");
    }

    #[cfg(feature = "xwayland")]
    state.start_xwayland();

    info!(log, "Initialization completed, starting the main loop.");

    while state.running.load(Ordering::SeqCst) {
        // Send frame events so that client start drawing their next frame
        display.borrow_mut().flush_clients(&mut state);

        if event_loop
            .dispatch(Some(Duration::from_millis(16)), &mut state)
            .is_err()
        {
            state.running.store(false, Ordering::SeqCst);
        } else {
            display.borrow_mut().flush_clients(&mut state);
        }

        #[cfg(feature = "debug")]
        state.backend_data.fps.tick();
    }
}
