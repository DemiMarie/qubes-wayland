use std::{
    cell::RefCell, collections::BTreeMap, convert::TryInto, num::NonZeroU32, os::unix::io::AsRawFd,
    rc::Rc, sync::atomic::AtomicBool, sync::atomic::Ordering, sync::Mutex, task::Poll,
    time::Duration,
};

use qubes_gui_agent_proto::DaemonToAgentEvent;
use smithay::{
    backend::input::KeyState,
    reexports::{
        calloop::{self, generic::Generic, EventLoop, Interest},
        wayland_protocols::xdg_shell::server::xdg_toplevel,
        wayland_server::{
            protocol::{
                wl_pointer::{Axis, AxisSource, ButtonState},
                wl_surface::WlSurface,
            },
            Display,
        },
    },
    utils::{Logical, Point},
    wayland::{
        compositor::{with_states, SurfaceAttributes},
        seat::{AxisFrame, FilterResult, KeyboardHandle, PointerHandle},
        shell::xdg,
        SERIAL_COUNTER,
    },
};

use slog::Logger;

use crate::shell::SurfaceData;
use crate::state::AnvilState;

pub const OUTPUT_NAME: &str = "qubes";

pub struct QubesData {
    pub agent: qubes_gui_client::Client,
    pub connection: qubes_gui_gntalloc::Agent,
    wid: u32,
    last_width: u32,
    last_height: u32,
    pub map: BTreeMap<NonZeroU32, QubesBackendData>,
    pub log: slog::Logger,
    buf: qubes_gui_gntalloc::Buffer,
}

/// Surface kinds
pub enum Kind {
    /// xdg-toplevel
    Toplevel(xdg::ToplevelSurface),
    /// Popup
    Popup(xdg::PopupSurface),
}

impl Kind {
    pub fn send_configure(&self) {
        match self {
            Self::Toplevel(t) => t.send_configure(),
            Self::Popup(t) => drop(t.send_configure()),
        }
    }

    fn send_close(&self) {
        match self {
            Self::Toplevel(t) => t.send_close(),
            Self::Popup(t) => t.send_popup_done(),
        }
    }

    fn get_surface(&self) -> Option<&WlSurface> {
        match self {
            Self::Toplevel(t) => t.get_surface(),
            Self::Popup(t) => t.get_surface(),
        }
    }
}

pub struct QubesBackendData {
    /// Toplevel surface
    pub surface: Kind,
    /// Whether the surface has been configured
    pub has_configured: bool,
    /// The coordinates of the surface
    pub coordinates: Point<i32, Logical>,
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
        let window = (*qubes).borrow_mut().id();
        SurfaceData {
            buffer: None,
            geometry: None,
            buffer_dimensions: None,
            buffer_scale: 0,
            window,
            buffer_swapped: false,
            coordinates: Default::default(),
        }
    }

    fn process_configure(&mut self, m: qubes_gui::Configure, window: u32) {
        let window = window.try_into().expect("Configure event for window 0?");
        if window == 1.try_into().unwrap() {
            self.agent.send(&m, window).unwrap();
            self.process_self_configure(m, window)
        } else {
            self.process_client_configure(m, window)
        }
    }

    fn process_client_configure(&mut self, m: qubes_gui::Configure, window: NonZeroU32) {
        let qubes_gui::WindowSize { width, height } = m.rectangle.size;
        let window = window
            .try_into()
            .expect("The GUI daemon never sends a zero window; qed");
        let data: Option<&mut QubesBackendData> = self.map.get_mut(&window);
        let QubesBackendData {
            surface,
            ref mut has_configured,
            ref mut coordinates,
        } = match data {
            None => return,
            Some(data) => data,
        };
        self.agent
            .send(
                &qubes_gui::ShmImage {
                    rectangle: m.rectangle,
                },
                window,
            )
            .unwrap();
        self.agent.send(&m, window).unwrap();
        let qubes_gui::Coordinates { x, y } = m.rectangle.top_left;
        *coordinates = (x as i32, y as i32).into();
        surface.get_surface().map(|surface| {
            with_states(surface, |data| {
                if let Some(state) = data.data_map.get::<RefCell<SurfaceData>>() {
                    state.borrow_mut().coordinates = m.rectangle.top_left
                }
            })
        });
        match match surface {
            Kind::Toplevel(surface) => surface.with_pending_state(|state| {
                let new_size = Some(
                    if *has_configured {
                        (width as _, height as _)
                    } else {
                        (0, 0)
                    }
                    .into(),
                );
                let do_send = new_size == state.size;
                state.size = new_size;
                do_send
            }),
            Kind::Popup(surface) => surface.with_pending_state(|state| {
                let new_size = if *has_configured {
                    (width as _, height as _)
                } else {
                    (0, 0)
                }
                .into();
                let do_send = new_size == state.geometry.size;
                state.geometry.size = new_size;
                do_send
            }),
        } {
            Ok(true) if *has_configured => {
                debug!(self.log, "Ignoring configure event that didn’t change size")
            }
            Ok(_) => {
                trace!(
                    self.log,
                    "Sending configure event to application: width {}, height {}, has_configured {}",
                    width,
                    height,
                    *has_configured,
                );
                surface.send_configure();
                *has_configured = true;
            }
            Err(_) => warn!(self.log, "Ignoring MSG_CONFIGURE on dead window"),
        }
    }

    fn process_self_configure(&mut self, m: qubes_gui::Configure, window: NonZeroU32) {
        let qubes_gui::WindowSize { width, height } = m.rectangle.size;
        if width == self.last_width && height == self.last_height {
            // no redraw needed
            return;
        }
        let mut need_dump = false;
        if self.last_width * self.last_height != width * height {
            drop(std::mem::replace(
                &mut self.buf,
                self.connection.alloc_buffer(width, height).unwrap(),
            ));
            need_dump = true;
        }
        self.last_width = width;
        self.last_height = height;
        let lines = height / 2;
        let line_width = width * 4;
        let shade = vec![0xFF00u32; (lines * width).try_into().unwrap()];
        self.buf.write(
            qubes_castable::as_bytes(&shade[..]),
            ((height / 4) * line_width).try_into().unwrap(),
        );
        if need_dump {
            trace!(self.log, "Dumping"; "window" => u32::from(window));
            self.agent
                .send_raw(self.buf.msg(), window, qubes_gui::MSG_WINDOW_DUMP)
                .unwrap();
        }
        self.agent.send(&m, window).unwrap();
        let output_message = qubes_gui::ShmImage {
            rectangle: m.rectangle,
        };
        self.agent.send(&output_message, window).unwrap();
    }
}

fn process_motion(
    window: u32,
    mut event: qubes_gui::Motion,
    time_spent: u32,
    qubes: &mut core::cell::RefMut<'_, QubesData>,
    log: &slog::Logger,
    pointer: &mut PointerHandle,
) {
    debug!(log, "Motion event: {:?}", event);
    let surface = window.try_into().ok().and_then(|s| qubes.map.get(&s));
    let focus = surface.and_then(|surface| {
        surface.surface.get_surface().and_then(|s| {
            with_states(s, |data| {
                let geometry = data
                    .data_map
                    .get::<RefCell<SurfaceData>>()
                    .expect("this data was just inserted above, so it will be present; qed")
                    .borrow()
                    .geometry;
                if let Some(geometry) = geometry {
                    event.coordinates.x = event
                        .coordinates
                        .x
                        .saturating_add(surface.coordinates.x.max(0) as _)
                        .saturating_add(geometry.loc.x as u32);
                    event.coordinates.y = event
                        .coordinates
                        .y
                        .saturating_add(surface.coordinates.y.max(0) as _)
                        .saturating_add(geometry.loc.y as u32);
                } else {
                    event.coordinates.x = event
                        .coordinates
                        .x
                        .saturating_add(surface.coordinates.x.max(0) as _);
                    event.coordinates.y = event
                        .coordinates
                        .y
                        .saturating_add(surface.coordinates.y.max(0) as _);
                }
            })
            .ok()
            .map(|()| (s.clone(), surface.coordinates))
        })
    });
    let location = (event.coordinates.x.into(), event.coordinates.y.into()).into();
    trace!(
        log,
        "Motion event";
        "location" => ?location,
        "window" => window,
    );
    pointer.motion(location, focus, SERIAL_COUNTER.next_serial(), time_spent)
}

fn process_focus(
    window: u32,
    event: qubes_gui::Focus,
    log: &slog::Logger,
    keyboard: &mut KeyboardHandle,
    qubes: &mut core::cell::RefMut<'_, QubesData>,
) {
    if event.mode != 0 {
        error!(log, #"daemon-bug", "mode not zero"; "window" => window, "mode" => event.mode)
    }
    let has_focus = match event.ty {
        9 /* FocusIn */ => true,
        10 /* FocusOut */ => false,
        bad_focus => {
            error!(log, #"daemon-bug", "bad Focus event"; "window" => window, "type" => bad_focus);
            return
        }
    };
    if event.detail > 7 {
        error!(log, #"daemon-bug", "bad X11 Detail"; "detail" => event.detail);
        return;
    }
    trace!(log, "Focus event"; "event" => ?event, "window" => window);
    let window = qubes.map.get(&window.try_into().unwrap());
    let serial = SERIAL_COUNTER.next_serial();
    // pointer.set_focus(window, serial);
    let surface = window.and_then(|QubesBackendData { surface, .. }| {
        if let Ok(true) = match surface {
            Kind::Toplevel(surface) => surface.with_pending_state(|state| {
                if has_focus {
                    state.states.set(xdg_toplevel::State::Activated)
                } else {
                    state.states.unset(xdg_toplevel::State::Activated)
                }
            }),
            Kind::Popup(surface) => {
                if !has_focus {
                    info!(log, "Dismissing popup because it no longer has focus");
                    surface.send_popup_done()
                }
                Ok(true)
            }
        } {
            surface.send_configure();
        }
        surface.get_surface()
    });
    keyboard.set_focus(if has_focus { surface } else { None }, serial);
}

fn process_button(
    event: qubes_gui::Button,
    instant: std::time::Instant,
    log: &slog::Logger,
    pointer: &mut PointerHandle,
) {
    let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
    let state = match event.ty {
        4 => ButtonState::Pressed,
        5 => ButtonState::Released,
        strange => {
            error!(log, "GUI daemon bug: strange button event"; "type" => strange);
            return;
        }
    };
    // info!(log, "Sending button event: {:?}", event);
    match event.button {
        4 | 5 | 6 | 7 => {
            let frame = AxisFrame::new(time_spent).source(AxisSource::Wheel);
            let frame = match event.button {
                4 => frame.value(Axis::VerticalScroll, -10f64),
                5 => frame.value(Axis::VerticalScroll, 10f64),
                6 => frame.value(Axis::HorizontalScroll, -10f64),
                7 => frame.value(Axis::HorizontalScroll, 10f64),
                _ => unreachable!(),
            };
            pointer.axis(frame)
        }
        1 | 2 | 3 => {
            let button = match event.button {
                1 => 0x110,
                2 => 0x112,
                3 => 0x111,
                _ => unreachable!(),
            };
            pointer.button(button, state, SERIAL_COUNTER.next_serial(), time_spent);
        }
        _ => {}
    }
}

fn process_keyboard(
    event: qubes_gui::Keypress,
    instant: std::time::Instant,
    log: &slog::Logger,
    keyboard: &mut KeyboardHandle,
) {
    let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
    if event.keycode < 8 || event.keycode >= 0x108 {
        error!(log,
           "Bad keycode from X11";
           "code" => event.keycode);
        return;
    }
    // NOT A GOOD IDEA: this is sensitive information
    // info!(log, "Key pressed: {:?}", event);
    let state = match event.ty {
        2 => KeyState::Pressed,
        3 => KeyState::Released,
        strange => {
            error!(log,
               "GUI daemon bug: strange key event";
               "type" => strange);
            return;
        }
    };
    keyboard.input::<(), _>(
        event.keycode - 8,
        state,
        SERIAL_COUNTER.next_serial(),
        time_spent,
        |_, _| FilterResult::Forward,
    );
}

fn process_events(
    instant: std::time::Instant,
    qubes: &mut core::cell::RefMut<'_, QubesData>,
    log: &slog::Logger,
    keyboard: &mut KeyboardHandle,
    pointer: &mut PointerHandle,
    running: &AtomicBool,
) {
    loop {
        let (window, ev) = loop {
            match qubes.agent.read_header().map(Result::unwrap) {
                Poll::Pending => return,
                Poll::Ready((hdr, body)) => match DaemonToAgentEvent::parse(hdr, body).unwrap() {
                    None => {}
                    Some(ev) => break ev,
                },
            }
        };
        match ev {
            DaemonToAgentEvent::Motion { event } => {
                let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                process_motion(window, event, time_spent, qubes, log, pointer)
            }
            DaemonToAgentEvent::Crossing { event } => {
                trace!(log,
                   "Crossing event";
                   "window" => window,
                   "event" => ?event)
            }
            DaemonToAgentEvent::Close => {
                trace!(log, "Got a close event!");
                if window == 1 {
                    info!(log, "Got close event for our window, exiting!");
                    running.store(false, Ordering::SeqCst);
                    return;
                }
                match qubes.map.get(&window.try_into().unwrap()) {
                    None => warn!(log, "Close event for unknown window {}", window),
                    Some(w) => w.surface.send_close(),
                };
            }
            DaemonToAgentEvent::Keypress { event } => {
                process_keyboard(event, instant, log, keyboard)
            }
            DaemonToAgentEvent::Button { event } => process_button(event, instant, log, pointer),
            DaemonToAgentEvent::Copy => {
                trace!(log, "clipboard data requested!")
            }
            DaemonToAgentEvent::Paste { untrusted_data: _ } => {
                trace!(log, "clipboard data reply!")
            }
            DaemonToAgentEvent::Keymap { new_keymap } => {
                trace!(log, "Keymap notification: {:?}", new_keymap);
                let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                let serial = SERIAL_COUNTER.next_serial();
                for i in 0x0..0x100 {
                    let state = match (new_keymap.keys[i / 32] >> (i % 8)) & 0x1 {
                        1 => KeyState::Pressed,
                        0 => KeyState::Released,
                        _ => unreachable!(),
                    };
                    keyboard.input::<(), _>(i as _, state, serial, time_spent, |_, _| {
                        FilterResult::Forward
                    });
                }
            }
            DaemonToAgentEvent::Redraw { portion_to_redraw } => {
                trace!(
                    log,
                    #"qubes-event",
                    "Map event";
                    "portion_to_redraw" => ?portion_to_redraw,
                    "window" => ?window,
                );
            }
            DaemonToAgentEvent::Configure {
                new_size_and_position,
            } => {
                trace!(log,
                    #"qubes-event",
                    "Configure";
                    "event" => ?new_size_and_position,
                    "window" => window,
                );
                qubes.process_configure(new_size_and_position, window)
            }
            DaemonToAgentEvent::Focus { event } => {
                process_focus(window, event, log, keyboard, qubes)
            }
            DaemonToAgentEvent::WindowFlags { flags } => {
                trace!(log,
                   "Window manager flags changed";
                   "window" => window,
                   "new_flags" => ?flags);
            }
            _ => warn!(log, "Ignoring unknown event!"),
        }
    }
}

pub fn run_qubes(log: Logger, args: std::env::ArgsOs) {
    let mut event_loop = EventLoop::try_new().unwrap();
    let display = Rc::new(RefCell::new(Display::new()));
    let instant = std::time::Instant::now();

    /*
     * Initialize the globals
     */

    let mut connection = qubes_gui_gntalloc::new(0).unwrap();
    let (mut agent, conf) = qubes_gui_client::Client::agent(0).unwrap();
    // we now have a agent 🙂
    info!(log, "🙂 Somebody connected to us, yay!");
    info!(log, "Configuration parameters: {:?}", conf);
    let (width, height) = (0x200, 0x100);
    let my_window = 1.try_into().unwrap();
    agent
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
    let title = b"Qubes Demo Rust GUI Agent";
    let mut title_buf = [0u8; 128];
    title_buf[..title.len()].copy_from_slice(title);
    agent
        .send_raw(&mut title_buf, my_window, qubes_gui::MSG_SET_TITLE)
        .unwrap();
    let buf = connection.alloc_buffer(width, height).unwrap();
    let shade = vec![0xFF00u32; (width * height / 2).try_into().unwrap()];
    agent
        .send_raw(buf.msg(), my_window, qubes_gui::MSG_WINDOW_DUMP)
        .unwrap();
    buf.write(
        qubes_castable::as_bytes(&shade[..]),
        (width * height).try_into().unwrap(),
    );
    agent
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
        agent,
        connection,
        wid: 2,
        map: BTreeMap::default(),
        log: log.clone(),
        last_width: 0,
        last_height: 0,
        buf,
    };
    let mut state = AnvilState::init(
        display.clone(),
        event_loop.handle(),
        data,
        log.clone(),
        conf,
        true,
    );
    let handle = event_loop.handle();
    handle
        .insert_source(
            Generic::from_fd(
                raw_fd,
                Interest {
                    readable: true,
                    writable: false,
                },
                calloop::Mode::Edge,
            ),
            move |_: calloop::Readiness, _fd, agent_full| {
                let AnvilState {
                    ref log,
                    ref mut keyboard,
                    ref mut pointer,
                    ref backend_data,
                    ref running,
                    ..
                } = agent_full;
                let ref mut qubes = backend_data.borrow_mut();
                qubes.agent.wait();
                process_events(instant, qubes, log, keyboard, pointer, running);
                Ok(calloop::PostAction::Continue)
            },
        )
        .unwrap();
    {
        let log = log.clone();
        let redraw_timer =
            calloop::timer::Timer::new().expect("not worth handling can’t create timer");
        let timer_handle = redraw_timer.handle();
        let time: u32 = 16;
        timer_handle.add_timeout(std::time::Duration::from_millis(time.into()), ());
        handle
            .insert_source(redraw_timer, move |(), timer_handle, agent_full| {
                // trace!(log, "Timer callback fired, reregistering!");
                let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                timer_handle.add_timeout(std::time::Duration::from_millis(time.into()), ());
                let ref mut qubes = agent_full.backend_data.borrow_mut();
                let mut dead_surfaces = vec![];
                let QubesData {
                    ref mut agent,
                    ref mut map,
                    ..
                } = &mut **qubes;
                for (key, value) in map.iter_mut() {
                    match value.surface.get_surface() {
                        None => {
                            info!(log, "Pushing toplevel with no surface onto dead list");
                            dead_surfaces.push(*key);
                            continue;
                        }
                        Some(s) => with_states(s, |states| {
                            if let Some(title) = states
                                .data_map
                                .get::<Mutex<xdg::XdgToplevelSurfaceRoleAttributes>>()
                                .and_then(|d| d.lock().expect("Poisoned?").title.clone())
                            {
                                let title: &[u8] = title.as_bytes();
                                let mut title_buf = [0u8; 128];
                                title_buf[..title.len().min(128)].copy_from_slice(title);
                                agent
                                    .send_raw(&mut title_buf, *key, qubes_gui::MSG_SET_TITLE)
                                    .unwrap();
                            }
                            let attrs = &mut *states.cached_state.current::<SurfaceAttributes>();
                            for callback in attrs.frame_callbacks.drain(..) {
                                callback.done(time_spent);
                            }
                        })
                        .expect("get_surface() only returns live resources; qed"),
                    }
                }
                for i in dead_surfaces.iter() {
                    trace!(log, "Destroying window"; "window" => u32::from(*i));
                    qubes.agent.send(&qubes_gui::Destroy {}, *i).unwrap();
                    let _: QubesBackendData = qubes
                        .map
                        .remove(i)
                        .expect("these were keys in the map; qed");
                    trace!(log, "Destruct successful"; "window" => u32::from(*i));
                }
            })
            .expect("FIXME: handle initialization failed");
    }

    #[cfg(feature = "xwayland")]
    state.start_xwayland();

    info!(log, "Initialization completed, starting the main loop.");
    let mut args = args.skip(1);
    if let Some(arg) = args.next() {
        let mut v = vec![arg.clone()];
        let mut cmd = std::process::Command::new(arg);
        for i in args {
            v.push(i.clone());
            cmd.arg(i);
        }
        let child = cmd.spawn().expect("Failed to execute subcommand");
        println!("Spawned child process {:?} with args {:?}", child, v);
    }

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
