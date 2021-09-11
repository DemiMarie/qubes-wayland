fn main() {
    use slog::Drain as _;
    // A logger facility, here we use the terminal here
    let log = slog::Logger::root(
        slog_async::Async::default(slog_term::term_full().fuse()).fuse(),
        //std::sync::Mutex::new(slog_term::term_full().fuse()).fuse(),
        slog::o!(),
    );
    let _guard = slog_scope::set_global_logger(log.clone());
    slog_stdlog::init().expect("Could not setup log backend");
    anvil::qubes::run_qubes(log, std::env::args_os())
}
