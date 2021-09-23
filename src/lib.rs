#![forbid(rust_2018_idioms)]
#![deny(unreachable_code)]

#[macro_use]
extern crate slog;

pub mod qubes;
pub mod shell;
pub mod state;
#[cfg(feature = "xwayland")]
pub mod xwayland;

pub use state::AnvilState;
