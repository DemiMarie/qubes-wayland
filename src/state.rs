use std::{
    cell::RefCell,
    rc::Rc,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};

use smithay::{
    reexports::{
        calloop::{generic::Generic, Interest, LoopHandle, Mode, PostAction},
        wayland_server::{protocol::wl_data_device_manager::DndAction, Display},
    },
    wayland::{
        data_device::{init_data_device, set_data_device_focus},
        output::xdg::init_xdg_output_manager,
        seat::{CursorImageStatus, KeyboardHandle, PointerHandle, Seat, XkbConfig},
        shm::init_shm_global,
        tablet_manager::{init_tablet_manager_global, TabletSeatTrait},
    },
};

#[cfg(feature = "xwayland")]
use smithay::xwayland::{XWayland, XWaylandEvent};

use crate::qubes::QubesData;
use crate::shell::init_shell;

pub struct AnvilState {
    pub backend_data: Rc<RefCell<QubesData>>,
    pub socket_name: Option<String>,
    pub running: Arc<AtomicBool>,
    pub display: Rc<RefCell<Display>>,
    pub handle: LoopHandle<'static, AnvilState>,
    pub log: slog::Logger,
    // input-related fields
    pub pointer: PointerHandle,
    pub keyboard: KeyboardHandle,
    pub cursor_status: Arc<Mutex<CursorImageStatus>>,
    pub seat_name: String,
    pub seat: Seat,
    // things we must keep alive
    #[cfg(feature = "xwayland")]
    pub xwayland: XWayland<AnvilState>,
}

impl AnvilState {
    pub fn init(
        display: Rc<RefCell<Display>>,
        handle: LoopHandle<'static, AnvilState>,
        backend_data: QubesData,
        log: slog::Logger,
        listen_on_socket: bool,
    ) -> AnvilState {
        // init the wayland connection
        handle
            .insert_source(
                Generic::from_fd(display.borrow().get_poll_fd(), Interest::READ, Mode::Level),
                move |_, _, state: &mut AnvilState| {
                    let display = state.display.clone();
                    let mut display = display.borrow_mut();
                    match display.dispatch(std::time::Duration::from_millis(0), state) {
                        Ok(_) => Ok(PostAction::Continue),
                        Err(e) => {
                            error!(state.log, "I/O error on the Wayland display: {}", e);
                            state.running.store(false, Ordering::SeqCst);
                            Err(e)
                        }
                    }
                },
            )
            .expect("Failed to init the wayland event source.");

        // Init the basic compositor globals

        init_shm_global(&mut (*display).borrow_mut(), vec![], log.clone());

        let _shell_handles = init_shell(display.clone(), log.clone());

        init_xdg_output_manager(&mut display.borrow_mut(), log.clone());
        #[cfg(any())]
        init_xdg_activation_global(
            &mut display.borrow_mut(),
            |state, req, mut ddata| {
                let anvil_state = ddata.get::<AnvilState>().unwrap();
                match req {
                    XdgActivationEvent::RequestActivation {
                        token,
                        token_data,
                        surface,
                    } => {
                        if token_data.timestamp.elapsed().as_secs() < 10 {
                            // Just grant the wish
                            anvil_state
                                .window_map
                                .borrow_mut()
                                .bring_surface_to_top(&surface);
                        } else {
                            // Discard the request
                            state.lock().unwrap().remove_request(&token);
                        }
                    }
                    XdgActivationEvent::DestroyActivationRequest { .. } => {}
                }
            },
            log.clone(),
        );

        let socket_name = if listen_on_socket {
            let socket_name = display
                .borrow_mut()
                .add_socket_auto()
                .unwrap()
                .into_string()
                .unwrap();
            info!(log, "Listening on wayland socket"; "name" => socket_name.clone());
            ::std::env::set_var("WAYLAND_DISPLAY", &socket_name);
            Some(socket_name)
        } else {
            None
        };

        // init input
        let seat_name = String::from("Qubes Virtual Seat");

        let (mut seat, _) = Seat::new(&mut display.borrow_mut(), seat_name.clone(), log.clone());

        let cursor_status = Arc::new(Mutex::new(CursorImageStatus::Default));

        let cursor_status2 = cursor_status.clone();
        let pointer =
            seat.add_pointer(move |new_status| *cursor_status2.lock().unwrap() = new_status);

        init_data_device(
            &mut display.borrow_mut(),
            drop,
            |_, _| DndAction::empty(),
            log.clone(),
        );
        init_tablet_manager_global(&mut display.borrow_mut());

        let cursor_status3 = cursor_status.clone();
        seat.tablet_seat()
            .on_cursor_surface(move |_tool, new_status| {
                // TODO: tablet tools should have their own cursors
                *cursor_status3.lock().unwrap() = new_status;
            });

        let keyboard = seat
            .add_keyboard(XkbConfig::default(), 200, 25, |seat, focus| {
                set_data_device_focus(seat, focus.and_then(|s| s.as_ref().client()))
            })
            .expect("Failed to initialize the keyboard");

        #[cfg(feature = "xwayland")]
        let xwayland = {
            let (xwayland, channel) = XWayland::new(handle.clone(), display.clone(), log.clone());
            let ret = handle.insert_source(channel, |event, _, anvil_state| match event {
                XWaylandEvent::Ready { connection, client } => {
                    anvil_state.xwayland_ready(connection, client)
                }
                XWaylandEvent::Exited => anvil_state.xwayland_exited(),
            });
            if let Err(e) = ret {
                error!(
                    log,
                    "Failed to insert the XWaylandSource into the event loop: {}", e
                );
            }
            xwayland
        };

        AnvilState {
            backend_data: Rc::new(RefCell::new(backend_data)),
            running: Arc::new(AtomicBool::new(true)),
            display,
            handle,
            log,
            pointer,
            socket_name,
            keyboard,
            cursor_status,
            seat_name,
            seat,
            #[cfg(feature = "xwayland")]
            xwayland,
        }
    }
}

pub trait Backend {
    fn seat_name(&self) -> String;
}
