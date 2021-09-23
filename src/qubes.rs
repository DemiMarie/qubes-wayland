use std::{
    cell::RefCell, collections::BTreeMap, convert::TryInto, num::NonZeroU32, os::unix::io::AsRawFd,
    rc::Rc, sync::atomic::Ordering, task::Poll, time::Duration,
};

use qubes_gui_agent_proto::DaemonToAgentEvent;
use smithay::{
    backend::input::KeyState,
    reexports::{
        calloop::{self, generic::Generic, EventLoop, Interest},
        wayland_protocols::xdg_shell::server::xdg_toplevel,
        wayland_server::{
            protocol::{wl_pointer::ButtonState, wl_surface::WlSurface},
            Display,
        },
    },
    utils::{Logical, Point},
    wayland::{
        compositor::{with_states, with_surface_tree_upward, SurfaceAttributes, TraversalAction},
        seat::FilterResult,
        shell::xdg::{PopupSurface, ShellClient, ToplevelSurface},
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
    Toplevel(ToplevelSurface),
    /// Popup
    Popup(PopupSurface),
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

    fn client(&self) -> Option<ShellClient> {
        match self {
            Self::Toplevel(t) => t.client(),
            Self::Popup(t) => t.client(),
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
        eprintln!("SurfaceData created!");
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

    fn process_configure(&mut self, m: qubes_gui::Configure, window: u32) -> std::io::Result<()> {
        self.agent.send(&m, window.try_into().unwrap()).unwrap();
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
        self.agent.send(
            &qubes_gui::ShmImage {
                rectangle: m.rectangle,
            },
            window,
        )?;
        let window = window
            .try_into()
            .expect("The GUI daemon never sends a zero window; qed");
        let data: Option<&mut QubesBackendData> = self.map.get_mut(&window);
        match data {
            None => panic!("Configure event for unknown window"),
            Some(QubesBackendData {
                surface,
                ref mut has_configured,
                ref mut coordinates,
            }) => {
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
                        debug!(
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
        }
        Ok(())
    }

    fn process_self_configure(
        &mut self,
        m: qubes_gui::Configure,
        window: NonZeroU32,
    ) -> std::io::Result<()> {
        let qubes_gui::WindowSize { width, height } = m.rectangle.size;
        if width == self.last_width && height == self.last_height {
            // no redraw needed
            return Ok(());
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
        let client = &mut self.agent;
        client.send(&m, window)?;
        let output_message = qubes_gui::ShmImage {
            rectangle: m.rectangle,
        };
        client.send(&output_message, window)
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
        true,
    );
    let handle = event_loop.handle();
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
                qubes.agent.wait();
                loop {
                    match loop {
                        match qubes.agent.read_header().map(Result::unwrap) {
                            Poll::Pending => return Ok(calloop::PostAction::Continue),
                            Poll::Ready((hdr, body)) => match DaemonToAgentEvent::parse(hdr, body).unwrap() {
                                None => {}
                                Some(ev) => break ev,
                            },
                        }
                    } {
                        DaemonToAgentEvent::Motion { window, mut event } => {
                            let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                            debug!(agent_full.log, "Motion event: {:?}", event);
                            let surface = window.try_into().ok().and_then(|s| qubes.map.get(&s));
                            if surface.is_none() && false {
                                warn!(
                                    agent_full.log,
                                    "Motion event for unknown window {}", window
                                )
                            }
                            let focus = surface.and_then(|surface| {
                                surface
                                    .surface
                                    .get_surface()
                                    .and_then(|s| {
                                        with_states(s, |data| {
                                            let geometry = data
                                                .data_map
                                                .get::<RefCell<SurfaceData>>()
                                                .expect("this data was just inserted above, so it will be present; qed")
                                                .borrow()
                                                .geometry;
                                            if let Some(geometry) = geometry {
                                                event.coordinates.x = event.coordinates.x
                                                    .saturating_add(surface.coordinates.x.max(0) as _)
                                                    .saturating_add(geometry.loc.x as u32);
                                                event.coordinates.y = event.coordinates.y
                                                    .saturating_add(surface.coordinates.y.max(0) as _)
                                                    .saturating_add(geometry.loc.y as u32);
                                            } else {
                                                event.coordinates.x = event.coordinates.x
                                                    .saturating_add(surface.coordinates.x.max(0) as _);
                                                event.coordinates.y = event.coordinates.y
                                                    .saturating_add(surface.coordinates.y.max(0) as _);
                                            }
                                        }).ok().map(|()| (s.clone(), surface.coordinates))
                                    })
                            });
                            let location = (event.coordinates.x.into(), event.coordinates.y.into()).into();
                            trace!(
                                agent_full.log,
                                "Sending motion event {:?} to window {}",
                                location,
                                window
                            );
                            agent_full.pointer.motion(
                                location,
                                focus,
                                SERIAL_COUNTER.next_serial(),
                                time_spent,
                            )
                        }
                        DaemonToAgentEvent::Crossing { window, event } => {
                            trace!(agent_full.log, "Crossing event for window {}: {:?}", window, event)
                        }
                        DaemonToAgentEvent::Close { window } => {
                            trace!(agent_full.log, "Got a close event!");
                            if window == 1 {
                                info!(agent_full.log, "Got close event for our window, exiting!");
                                agent_full.running.store(false, Ordering::SeqCst);
                                return Ok(calloop::PostAction::Continue);
                            }
                            match qubes.map.get(&window.try_into().unwrap()) {
                                None => warn!(
                                    agent_full.log,
                                    "Close event for unknown window {}", window
                                ),
                                Some(w) => w.surface.send_close(),
                            };
                        }
                        DaemonToAgentEvent::Keypress { window, event } => {
                            let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                            if event.keycode < 8 || event.keycode >= 0x108 {
                                error!(agent_full.log, "Bad keycode from X11: {}", event.keycode);
                                return Ok(calloop::PostAction::Continue);
                            }
                            // NOT A GOOD IDEA: this is sensitive information
                            // info!(agent_full.log, "Key pressed: {:?}", event);
                            let state = match event.ty {
                                2 => KeyState::Pressed,
                                3 => KeyState::Released,
                                strange => {
                                    error!(agent_full.log, "GUI daemon bug: strange key event type {}", strange);
                                    continue
                                }
                            };
                            agent_full.keyboard.input::<(), _>(
                                event.keycode - 8,
                                state,
                                SERIAL_COUNTER.next_serial(),
                                time_spent,
                                |_, _| FilterResult::Forward,
                            );
                            trace!(agent_full.log, "Keypress sent to client");
                            if let Some(surface) = window
                                .try_into()
                                .ok()
                                .and_then(|s| qubes.map.get(&s))
                                .and_then(|s| s.surface.client())
                            {
                                if false && surface.send_ping(SERIAL_COUNTER.next_serial()).is_err()
                                {
                                    // ignore for now
                                }
                            }
                        }
                        DaemonToAgentEvent::Button { window: _, event } => {
                            let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                            let state = match event.ty {
                                4 => ButtonState::Pressed,
                                5 => ButtonState::Released,
                                strange => {
                                    error!(agent_full.log, "GUI daemon bug: strange button event type {}", strange);
                                    continue
                                }
                            };
                            // info!(agent_full.log, "Sending button event: {:?}", event);
                            agent_full.pointer.button(
                                event.button,
                                state,
                                SERIAL_COUNTER.next_serial(),
                                time_spent,
                            )
                        }
                        DaemonToAgentEvent::Copy => {
                            trace!(agent_full.log, "clipboard data requested!")
                        }
                        DaemonToAgentEvent::Paste { untrusted_data: _ } => {
                            trace!(agent_full.log, "clipboard data reply!")
                        }
                        DaemonToAgentEvent::Keymap { new_keymap } => {
                            let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                            trace!(agent_full.log, "Keymap notification: {:?}", new_keymap);
                            let serial = SERIAL_COUNTER.next_serial();
                            for i in 0x0..0x100 {
                                let state = match (new_keymap.keys[i / 32] >> (i % 8)) & 0x1 {
                                    1 => KeyState::Pressed,
                                    0 => KeyState::Released,
                                    _ => unreachable!(),
                                };
                                agent_full.keyboard.input::<(), _>(
                                    i as _,
                                    state,
                                    serial,
                                    time_spent,
                                    |_, _| FilterResult::Forward,
                                );
                            }
                        }
                        DaemonToAgentEvent::Redraw { window, portion_to_redraw } => {
                            trace!(
                                agent_full.log,
                                #"qubes-event",
                                "Map event";
                                "portion_to_redraw" => ?portion_to_redraw,
                                "window" => ?window,
                            );
                        }
                        DaemonToAgentEvent::Configure { window, new_size_and_position } => {
                            trace!(
                                agent_full.log,
                                #"qubes-event",
                                "Configure";
                                "event" => ?new_size_and_position,
                                "window" => window,
                            );
                            qubes.process_configure(new_size_and_position, window)?
                        }
                        DaemonToAgentEvent::Focus { window, event } => {
                            if event.mode != 0 {
                                error!(
                                    agent_full.log,
                                    #"daemon-bug",
                                    "mode not zero";
                                    "window" => window,
                                    "mode" => event.mode,
                                )
                            }
                            let has_focus = match event.ty {
                                9 /* FocusIn */ => true,
                                10 /* FocusOut */ => false,
                                bad_focus => {
                                    error!(
                                        agent_full.log,
                                        #"daemon-bug",
                                        "bad Focus event type";
                                        "type" => bad_focus,
                                    );
                                    continue
                                }
                            };
                            if event.detail > 7 {
                                error!(agent_full.log, #"daemon-bug", "bad X11 Detail"; "detail" => event.detail);
                                continue
                            }
                            trace!(agent_full.log, "Focus event"; "event" => ?event, "window" => window);
                            let window = qubes.map.get(&window.try_into().unwrap());
                            let serial = SERIAL_COUNTER.next_serial();
                            // agent_full.pointer.set_focus(window, serial);
                            let surface =
                                window.and_then(|QubesBackendData { surface, .. }| {
                                    if let Ok(true) = match surface {
                                        Kind::Toplevel(surface) => surface.with_pending_state(|state| {
                                        if has_focus {
                                            state.states.set(xdg_toplevel::State::Activated)
                                        } else {
                                            state.states.unset(xdg_toplevel::State::Activated)
                                        }}),
                                        Kind::Popup(surface) => {
                                            if !has_focus {
                                                info!(agent_full.log, "Dismissing popup because it no longer has focus");
                                                surface.send_popup_done()
                                            }
                                            Ok(true)
                                        }
                                    } {
                                        surface.send_configure();
                                    }
                                    surface.get_surface()
                                });
                            agent_full.keyboard.set_focus(if has_focus { surface } else { None }, serial);
                        }
                        DaemonToAgentEvent::WindowFlags { window, flags } => {
                            trace!(agent_full.log, "Window manager flags for {} are now {:?}", window, flags);
                        }
                        _ => warn!(agent_full.log, "Ignoring unknown event!"),
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
        let time: u32 = 16;
        timer_handle.add_timeout(std::time::Duration::from_millis(time.into()), ());
        handle
            .insert_source(redraw_timer, move |(), timer_handle, agent_full| {
                // trace!(log, "Timer callback fired, reregistering!");
                let time_spent = (std::time::Instant::now() - instant).as_millis() as _;
                timer_handle.add_timeout(std::time::Duration::from_millis(time.into()), ());
                let ref mut qubes = agent_full.backend_data.borrow_mut();
                let mut dead_surfaces = vec![];
                for (key, value) in qubes.map.iter_mut() {
                    let surface = match value.surface.get_surface() {
                        None => {
                            info!(log, "Pushing toplevel with no surface onto dead list");
                            dead_surfaces.push(*key);
                            continue;
                        }
                        Some(s) if !s.as_ref().is_alive() => {
                            info!(log, "Pushing toplevel with dead surface onto dead list");
                            dead_surfaces.push(*key);
                            match with_states(s, |data| {
                                data.data_map
                                    .get::<RefCell<SurfaceData>>()
                                    .map(RefCell::borrow_mut)
                                    .map(|mut surface_data| {
                                        surface_data.buffer = None;
                                        surface_data.geometry = None;
                                        surface_data.buffer_dimensions = None;
                                    })
                            }) {
                                Ok(Some(())) => s,
                                Ok(None) => s,
                                Err(e) => {
                                    info!(log, "Got an error: {}", e);
                                    s
                                }
                            }
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
