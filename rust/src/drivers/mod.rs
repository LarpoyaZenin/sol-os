//! Device drivers: PCI bus enumeration, PS/2 keyboard + mouse, and the
//! VirtIO transport + input driver. All input drivers feed the central
//! `crate::input` pipeline.

pub mod pci;
pub mod ps2;
pub mod virtio;
