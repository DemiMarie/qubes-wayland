use std::{
    collections::BTreeMap,
    convert::{TryFrom, TryInto},
    num::NonZeroU32,
    os::raw::{c_int, c_void},
    os::unix::io::AsRawFd,
    ptr,
    task::Poll,
};

pub const OUTPUT_NAME: &str = "qubes";

pub struct QubesData {
    pub agent: qubes_gui_client::Client,
    pub connection: qubes_gui_gntalloc::Agent,
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

    fn destroy_id(&mut self, id: u32) {
        if let Ok(id) = NonZeroU32::try_from(id) {
            self.map.remove(&id).expect("double free!");
        }
    }

    unsafe fn on_fd_ready(
        &mut self,
        is_readable: bool,
        callback: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, qubes_gui::Header, *const u8),
        global_userdata: *mut c_void,
    ) {
        if is_readable {
            self.agent.wait();
        }
        loop {
            match self.agent.read_header().map(Result::unwrap) {
                Poll::Pending => return,
                Poll::Ready((hdr, body)) => {
                    assert!(body.len() < (1usize << 20));
                    assert_eq!(body.len() as u32, hdr.untrusted_len);
                    let delta = (std::time::Instant::now() - self.start).as_millis() as u32;
                    if let Ok(nz) = NonZeroU32::try_from(hdr.window) {
                        if let Some(&userdata) = self.map.get(&nz) {
                            callback(global_userdata, userdata, delta, hdr, body.as_ptr())
                        }
                    } else {
                        callback(global_userdata, ptr::null_mut(), delta, hdr, body.as_ptr());
                    }
                }
            }
        }
    }

    fn is_channel_closed(&self) -> bool {
        return false;
    }
}

type RustBackend = QubesData;

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_is_channel_closed(backend: *mut c_void) -> bool {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).is_channel_closed()) {
        Ok(e) => e,
        Err(_) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Error in Rust event handler");
            }));
            std::process::abort();
        }
    }
}

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
pub unsafe extern "C" fn qubes_rust_delete_id(backend: *mut c_void, id: u32) {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).destroy_id(id)) {
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
pub unsafe extern "C" fn qubes_rust_send_message(backend: *mut c_void, header: &qubes_gui::Header) {
    // untrusted_len is actually trusted here
    let slice = core::slice::from_raw_parts(
        header as *const _ as *const u8,
        header.untrusted_len as usize + core::mem::size_of::<qubes_gui::Header>(),
    );
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).agent.send_raw_bytes(slice))
    {
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
        Box::from_raw(backend as *mut RustBackend);
    }
}

fn setup_qubes_backend(domid: u16) -> RustBackend {
    let connection = qubes_gui_gntalloc::new(domid).unwrap();
    let (agent, conf) = qubes_gui_client::Client::agent(domid).unwrap();
    // we now have a agent 🙂
    eprintln!("Configuration parameters: {:?}", conf);
    QubesData {
        agent,
        connection,
        wid: 1,
        map: Default::default(),
        start: std::time::Instant::now(),
    }
}
