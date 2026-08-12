//! Desktop compositor and window manager.
//!
//! Re-exports the desktop subsystems so callers use `crate::desktop::...`
//! instead of reaching into submodules.

pub mod backbuffer;
pub mod font;
pub mod png;
pub mod rtc;
pub mod wallpaper;
pub mod wm;

pub use wm::{desktop_init, desktop_poll};
