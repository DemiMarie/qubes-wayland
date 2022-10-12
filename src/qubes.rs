use qubes_gui::WindowID;
use std::{
    collections::BTreeMap,
    convert::TryInto,
    num::NonZeroU32,
    os::raw::{c_int, c_void},
    os::unix::io::AsRawFd,
    ptr,
    task::Poll,
};

pub const OUTPUT_NAME: &str = "qubes";

// NOTE: Enabling and disabling GUI messages
//
// The C code in the GUI agent is, for the most part, not aware of the GUI
// protocol state machine.  This is by design, as it keeps the C code (which is
// much harder to maintain than Rust) simple.  However, this can cause problems
// when reconnecting.  The GUI agent will not stop sending messages to the GUI
// daemon just because the vchan has disconnected.  The Rust code will queue
// these messages and send them when a new connection is made.  This leads to
// the GUI daemon disconnecting with a message of the form “msg 0x93 without
// CREATE for 0x1” (the actual window and message number may vary).
//
// To prevent such problems, the Rust code maintains an explicit “enabled” flag
// This flag is unset when the agent becomes disconnected, and is set when the
// agent reconnects.  When the flag is unset, all messages from the C code are
// silently ignored.  This keeps the C code simple and ensures that no messages
// are sent until the C code has recreated all of the windows.

pub struct QubesData {
    enabled: bool, // See NOTE: Enabling and disabling GUI messages
    pub agent: qubes_gui_client::Client,
    wid: u32,
    pub map: BTreeMap<NonZeroU32, *mut c_void>,
    start: std::time::Instant,
}

impl QubesData {
    fn id(&mut self, userdata: *mut c_void) -> NonZeroU32 {
        let id = self.wid;
        self.wid = id
            .checked_add(1)
            .expect("not yet implemented: wid wrapping");
        let id = id
            .try_into()
            .expect("IDs start at 1 and do not wrap, so they cannot be zero; qed");
        assert!(self.map.insert(id, userdata).is_none());
        id
    }

    fn destroy_id(&mut self, WindowID { window }: WindowID) {
        if let Some(id) = window {
            let v = self
                .map
                .get_mut(&id)
                .expect("Bogus call to delete_id: ID not found in map!");
            assert!(!(*v).is_null(), "delete_id called twice");
            *v = ptr::null_mut();
        }
    }

    unsafe fn on_fd_ready(
        &mut self,
        is_readable: bool,
        callback: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, qubes_gui::Header, *const u8),
        global_userdata: *mut c_void,
    ) {
        let Self {
            ref mut agent,
            ref mut enabled,
            ..
        } = self;
        let mut protocol_error = |agent: &qubes_gui_client::Client| {
            *enabled = false;
            let hdr = qubes_gui::Header {
                ty: 0,
                window: qubes_gui::WindowID { window: None },
                untrusted_len: if agent.needs_reconnect() { 1 } else { 3 },
            };
            callback(global_userdata, ptr::null_mut(), 0, hdr, ptr::null())
        };
        if agent.needs_reconnect() {
            protocol_error(agent);
            return;
        }
        if is_readable {
            agent.wait();
        }
        loop {
            let res = agent.read_message();
            match res {
                Poll::Ready(Ok((hdr, body))) => {
                    assert!(body.len() < (1usize << 20));
                    assert_eq!(body.len() as u32, hdr.untrusted_len);
                    let delta = (std::time::Instant::now() - self.start).as_millis() as u32;
                    if let Some(nz) = hdr.window.window {
                        if hdr.ty == qubes_gui::MSG_DESTROY {
                            use std::collections::btree_map::Entry;
                            let p = self.map.entry(nz);
                            match p {
                                Entry::Vacant(_) => protocol_error(agent),
                                Entry::Occupied(e) => match e.remove_entry() {
                                    (_, v) if v.is_null() => {}
                                    _ => protocol_error(agent),
                                },
                            }
                        } else if let Some(&userdata) = self.map.get(&nz) {
                            if !userdata.is_null() {
                                callback(global_userdata, userdata, delta, hdr, body.as_ptr())
                            }
                        }
                    } else {
                        callback(global_userdata, ptr::null_mut(), delta, hdr, body.as_ptr());
                    }
                }
                Poll::Pending => {
                    if agent.reconnected() {
                        *enabled = true;
                        let hdr = qubes_gui::Header {
                            ty: 0,
                            window: qubes_gui::WindowID { window: None },
                            untrusted_len: 2,
                        };
                        let delta = (std::time::Instant::now() - self.start).as_millis() as u32;
                        callback(global_userdata, ptr::null_mut(), delta, hdr, ptr::null());
                    }
                    break;
                }

                Poll::Ready(Err(_)) => {
                    protocol_error(agent);
                    break;
                }
            }
        }
    }
}

type RustBackend = QubesData;

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_generate_id(
    backend: *mut c_void,
    userdata: *mut c_void,
) -> u32 {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).id(userdata)) {
        Ok(e) => e.into(),
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Unexpected panic");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_reconnect(backend: *mut c_void) -> bool {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).agent.reconnect()) {
        Ok(e) => e.is_ok(),
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Unexpected panic");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_fd(backend: *mut c_void) -> c_int {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).agent.as_raw_fd()) {
        Ok(e) => e,
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Unexpected panic");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub extern "C" fn qubes_rust_backend_create(domid: u16) -> *mut c_void {
    match std::panic::catch_unwind(|| setup_qubes_backend(domid)) {
        Ok(e) => Box::into_raw(Box::new(e)) as *mut _,
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Error initializing Rust code");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_on_fd_ready(
    backend: *mut c_void,
    is_readable: bool,
    callback: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, qubes_gui::Header, *const u8),
    global_userdata: *mut c_void,
) -> bool {
    match std::panic::catch_unwind(|| {
        (*(backend as *mut RustBackend)).on_fd_ready(is_readable, callback, global_userdata)
    }) {
        Ok(()) => true,
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Error in Rust event handler");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_send_message(
    backend: &mut RustBackend,
    header: &qubes_gui::Header,
) {
    // untrusted_len is actually trusted here
    let slice = core::slice::from_raw_parts(
        header as *const _ as *const u8,
        header.untrusted_len as usize + core::mem::size_of::<qubes_gui::Header>(),
    );
    if !backend.enabled {
        return;
    }
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if header.ty == qubes_gui::MSG_DESTROY {
            backend.destroy_id(header.window);
        }
        backend.agent.send_raw_bytes(slice)
    })) {
        Ok(_) => {}
        Err(_) => {
            core::mem::forget(std::panic::catch_unwind(|| {
                eprintln!("Unexpected panic");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_free(backend: *mut c_void) {
    if !backend.is_null() {
        drop(Box::from_raw(backend as *mut RustBackend))
    }
}

fn setup_qubes_backend(domid: u16) -> RustBackend {
    let (agent, conf) = qubes_gui_client::Client::agent(domid).unwrap();
    // we now have a agent 🙂
    eprintln!("Configuration parameters: {:?}", conf);
    QubesData {
        agent,
        enabled: true,
        wid: 1,
        map: Default::default(),
        start: std::time::Instant::now(),
    }
}
