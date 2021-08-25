use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    convert::TryInto,
    num::NonZeroU32,
    os::unix::io::AsRawFd,
    rc::Rc,
    sync::atomic::Ordering,
    task::Poll,
    time::Duration,
};

use qubes_castable::Castable;
use smithay::{
    reexports::{
        calloop::{self, generic::Generic, EventLoop, Interest},
        wayland_server::Display,
    },
    wayland::shell::xdg::ToplevelSurface,
};

use slog::Logger;

use crate::state::{AnvilState, Backend};

pub const OUTPUT_NAME: &str = "qubes";

pub struct QubesData {
    pub agent: RefCell<qubes_gui_client::agent::Agent>,
    wid: Cell<u32>,
    pub map: BTreeMap<NonZeroU32, QubesBackendData>,
    pub log: slog::Logger,
}

pub struct QubesBackendData {
    pub surface: ToplevelSurface,
    /// Whether the surface has been configured
    pub has_configured: bool,
}

impl Backend for QubesData {
    fn seat_name(&self) -> String {
        String::from("qubes")
    }
}

impl QubesData {
    pub fn id(&self) -> NonZeroU32 {
        let id = self.wid.get();
        self.wid.set(
            id.checked_add(1)
                .expect("not yet implemented: wid wrapping"),
        );
        id.try_into()
            .expect("IDs start at 2 and do not wrap, so they cannot be zero; qed")
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
    let mut buf = agent.alloc_buffer(width, height).unwrap();
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
        agent: RefCell::new(agent),
        wid: Cell::new(2),
        map: BTreeMap::default(),
        log: log.clone(),
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
            Generic::new(
                raw_fd,
                Interest {
                    readable: true,
                    writable: true,
                },
                calloop::Mode::Edge,
            ),
            move |_, _, agent_full| {
                let mut agent = agent_full.backend_data.agent.borrow_mut();
                match agent.client().read_header() {
                    Poll::Pending => {}
                    Poll::Ready(Ok((e, body))) => match e.ty {
                        qubes_gui::MSG_MOTION => {
                            let mut m = qubes_gui::Motion::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Motion event: {:?}", m);
                        }
                        qubes_gui::MSG_CROSSING => {
                            let mut m = qubes_gui::Crossing::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Crossing event: {:?}", m)
                        }
                        qubes_gui::MSG_CLOSE => {
                            assert!(body.is_empty());
                            debug!(log_, "Got a close event!");
                            if e.window == 1 {
                                debug!(log_, "Got close event for our window, exiting!");
                                std::process::exit(0);
                            }
                        }
                        qubes_gui::MSG_KEYPRESS => {
                            let mut m = qubes_gui::Button::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Key pressed: {:?}", m);
                        }
                        qubes_gui::MSG_BUTTON => {
                            let mut m = qubes_gui::Button::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Button event: {:?}", m);
                        }
                        qubes_gui::MSG_CLIPBOARD_REQ => debug!(log_, "clipboard data requested!"),
                        qubes_gui::MSG_CLIPBOARD_DATA => debug!(log_, "clipboard data reply!"),
                        qubes_gui::MSG_KEYMAP_NOTIFY => {
                            let mut m = qubes_gui::KeymapNotify::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Keymap notification: {:?}", m);
                        }
                        qubes_gui::MSG_MAP => {
                            let mut m = qubes_gui::MapInfo::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Map event: {:?}", m);
                        }
                        qubes_gui::MSG_CONFIGURE => {
                            let mut m = qubes_gui::Configure::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Configure event {:?} for window {}", m, e.window);
                            let qubes_gui::WindowSize { width, height } = m.rectangle.size;
                            match e.window {
                                0 => panic!("Configure event for window 0?"),
                                1 => {}
                                window => {
                                    match agent_full
                                        .backend_data
                                        .map
                                        .get(&window.try_into().unwrap())
                                    {
                                        None => panic!("Configure event for unknown window"),
                                        Some(QubesBackendData {
                                            surface,
                                            has_configured,
                                        }) => {
                                            if let Ok(()) = surface.with_pending_state(|state| {
                                                state.size = if *has_configured {
                                                    Some((width as _, height as _).into())
                                                } else {
                                                    None
                                                }
                                            }) {
                                                surface.send_configure();
                                            }
                                        }
                                    };
                                    return Ok(calloop::PostAction::Reregister);
                                }
                            }
                            drop(std::mem::replace(
                                &mut buf,
                                agent.alloc_buffer(width, height).unwrap(),
                            ));
                            let shade = vec![0xFF0000u32; (width * height / 2).try_into().unwrap()];
                            buf.dump(agent.client(), e.window.try_into().unwrap())
                                .unwrap();
                            buf.write(
                                qubes_castable::as_bytes(&shade[..]),
                                (width * height / 4 * 4).try_into().unwrap(),
                            );
                            agent.client().send(&m, e.window.try_into().unwrap())?
                        }
                        qubes_gui::MSG_FOCUS => {
                            let mut m = qubes_gui::Focus::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Focus event: {:?}", m);
                        }
                        qubes_gui::MSG_WINDOW_FLAGS => {
                            let mut m = qubes_gui::WindowFlags::default();
                            m.as_mut_bytes().copy_from_slice(body);
                            debug!(log_, "Window manager flags have changed: {:?}", m);
                        }
                        _ => debug!(log_, "Ignoring unknown event!"),
                    },
                    Poll::Ready(Err(e)) => {
                        error!(log_, "Critical Qubes Error: {}", e);
                        return Err(e);
                    }
                };
                Ok(calloop::PostAction::Reregister)
            },
        )
        .unwrap();
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
            state.window_map.borrow_mut().refresh();
        }

        #[cfg(feature = "debug")]
        state.backend_data.fps.tick();
    }
}
