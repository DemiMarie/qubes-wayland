use std::{
    cell::RefCell, collections::BTreeMap, convert::TryInto, num::NonZeroU32, os::unix::io::AsRawFd,
    rc::Rc, sync::atomic::Ordering, sync::Mutex, task::Poll, time::Duration,
};

use qubes_gui_agent_proto::DaemonToAgentEvent;

pub const OUTPUT_NAME: &str = "qubes";

pub struct QubesData {
    pub agent: qubes_gui_client::Client,
    pub connection: qubes_gui_gntalloc::Agent,
    wid: u32,
    pub map: BTreeMap<NonZeroU32, ()>,
    last_width: u32,
    last_height: u32,
    buf: qubes_gui_gntalloc::Buffer,
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

    fn on_fd_ready(&mut self, is_readable: bool) {
        if is_readable {
            self.agent.wait();
        }
        loop {
            match self.agent.read_header().map(Result::unwrap) {
                Poll::Pending => return,
                Poll::Ready((hdr, body)) => {
                    assert!(body.len() < (1usize << 20));
                    assert_eq!(body.len() as u32, hdr.untrusted_len);
                    if hdr.window == 1 {
                        eprintln!("Got an event for our own window!");
                    }
                }
            }
        }
    }
}

type RustBackend = QubesData;

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_fd(
    backend: *mut std::os::raw::c_void,
) -> std::os::raw::c_int {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).agent.as_raw_fd()) {
        Ok(e) => e,
        Err(e) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Unexpected panic");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub extern "C" fn qubes_rust_backend_create(domid: u16) -> *mut std::os::raw::c_void {
    match std::panic::catch_unwind(|| setup_qubes_backend(domid)) {
        Ok(e) => Box::into_raw(Box::new(e)) as *mut _,
        Err(e) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Error initializing Rust code");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_on_fd_ready(
    backend: *mut std::os::raw::c_void,
    is_readable: bool,
) -> bool {
    match std::panic::catch_unwind(|| (*(backend as *mut RustBackend)).on_fd_ready(is_readable)) {
        Ok(()) => true,
        Err(e) => {
            drop(std::panic::catch_unwind(|| {
                eprintln!("Error in Rust event handler");
            }));
            std::process::abort();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn qubes_rust_backend_free(backend: *mut std::os::raw::c_void) {
    if !backend.is_null() {
        Box::from_raw(backend as *mut RustBackend);
    }
}

fn setup_qubes_backend(domid: u16) -> RustBackend {
    let mut connection = qubes_gui_gntalloc::new(domid).unwrap();
    let (mut agent, conf) = qubes_gui_client::Client::agent(domid).unwrap();
    // we now have a agent 🙂
    eprintln!("Configuration parameters: {:?}", conf);
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
    QubesData {
        agent,
        connection,
        wid: 2,
        map: Default::default(),
        last_width: width,
        last_height: height,
        buf,
    }
}
