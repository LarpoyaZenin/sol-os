//! Minimal PCI bus enumeration over the 0xCF8/0xCFC config ports.
//! Faithful port of `kernel/drivers/pci/pci.c` (C).
//!
//! The scan walks buses 0-255, devices 0-31, and functions 0-7
//! (skipping function 1-7 of single-function devices via the
//! multifunction bit in the header type). Every present function is
//! recorded in a fixed-size, bounds-checked table. Only the surface
//! the VirtIO input driver needs is ported: device discovery, config
//! accessors, and capability-list walking.

pub const PCI_MAX_DEVICES: usize = 256;

const PCI_CONFIG_ADDRESS: u16 = 0xCF8;
const PCI_CONFIG_DATA: u16 = 0xCFC;

const PCI_VENDOR_ID_OFFSET: u8 = 0x00;
const PCI_HEADER_TYPE_OFFSET: u8 = 0x0E;

const PCI_VENDOR_NONE: u16 = 0xFFFF;

#[derive(Clone, Copy)]
pub struct PciDevice {
    pub bus: u8,
    pub dev: u8,
    pub func: u8,
    pub vendor: u16,
    pub device_id: u16,
    pub revision: u8,
    pub prog_if: u8,
    pub sub_class: u8,
    pub base_class: u8,
    pub header_type: u8,
    pub bars: [u32; 6],
}

const PCI_DEVICE_ZERO: PciDevice = PciDevice {
    bus: 0,
    dev: 0,
    func: 0,
    vendor: 0,
    device_id: 0,
    revision: 0,
    prog_if: 0,
    sub_class: 0,
    base_class: 0,
    header_type: 0,
    bars: [0; 6],
};

static mut PCI_DEVICES: [PciDevice; PCI_MAX_DEVICES] = [PCI_DEVICE_ZERO; PCI_MAX_DEVICES];
static mut PCI_COUNT: usize = 0;

#[inline]
unsafe fn inl(port: u16) -> u32 {
    let val: u32;
    unsafe {
        core::arch::asm!(
            "in eax, dx",
            out("eax") val,
            in("dx") port,
            options(nomem, nostack, preserves_flags)
        );
    }
    val
}

#[inline]
unsafe fn outl(port: u16, val: u32) {
    unsafe {
        core::arch::asm!(
            "out dx, eax",
            in("dx") port,
            in("eax") val,
            options(nomem, nostack, preserves_flags)
        );
    }
}

/* CONFIG_ADDRESS layout (bit 31 set = enabled): bits 23:16 bus,
 * 15:11 device, 10:8 function, 7:2 register offset (dword aligned). */
fn make_address(bus: u8, dev: u8, func: u8, offset: u8) -> u32 {
    0x8000_0000
        | ((bus as u32) << 16)
        | ((dev as u32) << 11)
        | ((func as u32) << 8)
        | (offset as u32 & 0xFC)
}

pub fn config_read32(bus: u8, dev: u8, func: u8, offset: u8) -> u32 {
    unsafe {
        outl(PCI_CONFIG_ADDRESS, make_address(bus, dev, func, offset));
        inl(PCI_CONFIG_DATA)
    }
}

// Unused by the current drivers; kept as part of the ported C API
// surface (future drivers will need it for enabling bus mastering).
#[allow(dead_code)]
pub fn config_write32(bus: u8, dev: u8, func: u8, offset: u8, value: u32) {
    unsafe {
        outl(PCI_CONFIG_ADDRESS, make_address(bus, dev, func, offset));
        outl(PCI_CONFIG_DATA, value);
    }
}

pub fn config_read16(bus: u8, dev: u8, func: u8, offset: u8) -> u16 {
    (config_read32(bus, dev, func, offset) >> ((offset & 3) * 8)) as u16
}

pub fn config_read8(bus: u8, dev: u8, func: u8, offset: u8) -> u8 {
    (config_read32(bus, dev, func, offset) >> ((offset & 3) * 8)) as u8
}

/// Number of devices recorded by `init`.
pub fn device_count() -> usize {
    unsafe { PCI_COUNT }
}

/// Device at table index `i`, or None if out of range.
pub fn device_at(i: usize) -> Option<&'static PciDevice> {
    if i >= unsafe { PCI_COUNT } {
        return None;
    }
    Some(unsafe { &PCI_DEVICES[i] })
}

/// Table index of the first device matching (vendor, device_id), or
/// None.
// Unused by the current drivers; kept as part of the ported C API
// surface.
#[allow(dead_code)]
pub fn find_device(vendor: u16, device_id: u16) -> Option<usize> {
    let count = unsafe { PCI_COUNT };
    for i in 0..count {
        let d = unsafe { &PCI_DEVICES[i] };
        if d.vendor == vendor && d.device_id == device_id {
            return Some(i);
        }
    }
    None
}

/// Config-space offset of the first capability (0 if none). Valid
/// capability pointers live in 0x40-0xFF.
pub fn cap_first(bus: u8, dev: u8, func: u8) -> u8 {
    let status = config_read16(bus, dev, func, 0x06);
    if status & 0x0010 == 0 {
        return 0; /* bit 4 of status: capabilities list present */
    }
    let ptr = config_read8(bus, dev, func, 0x34);
    if ptr >= 0x40 {
        ptr
    } else {
        0
    }
}

/// Capability after `offset` (0 if none).
pub fn cap_next(bus: u8, dev: u8, func: u8, offset: u8) -> u8 {
    if offset == 0 || offset >= 0xFF {
        return 0;
    }
    let next = config_read8(bus, dev, func, offset + 1);
    if next >= 0x40 {
        next
    } else {
        0
    }
}

fn record(bus: u8, dev: u8, func: u8) {
    let count = unsafe { PCI_COUNT };
    if count >= PCI_MAX_DEVICES {
        crate::kprintln!(
            "[rust] PCI: device table full ({}), skipping bus {} dev {} func {}",
            PCI_MAX_DEVICES,
            bus,
            dev,
            func
        );
        return;
    }
    unsafe {
        let p = &mut PCI_DEVICES[count];
        p.bus = bus;
        p.dev = dev;
        p.func = func;
        p.vendor = config_read16(bus, dev, func, 0x00);
        p.device_id = config_read16(bus, dev, func, 0x02);
        p.revision = config_read8(bus, dev, func, 0x08);
        p.prog_if = config_read8(bus, dev, func, 0x09);
        p.sub_class = config_read8(bus, dev, func, 0x0A);
        p.base_class = config_read8(bus, dev, func, 0x0B);
        p.header_type = config_read8(bus, dev, func, PCI_HEADER_TYPE_OFFSET) & 0x7F;

        /* Base address registers only exist in header type 0. */
        if p.header_type == 0x00 {
            for i in 0..6 {
                p.bars[i] = config_read32(bus, dev, func, 0x10 + (i as u8) * 4);
            }
        }

        PCI_COUNT += 1;
    }
}

fn class_name(base_class: u8) -> &'static str {
    match base_class {
        0x00 => "Legacy",
        0x01 => "Storage",
        0x02 => "Network",
        0x03 => "Display",
        0x04 => "Multimedia",
        0x05 => "Memory",
        0x06 => "Bridge",
        0x07 => "Comm",
        0x08 => "System peripheral",
        0x09 => "Input",
        0x0C => "Serial bus",
        0x0D => "Wireless",
        0x0E => "Intelligent I/O",
        0xFF => "VGA/unknown",
        _ => "Other",
    }
}

/// Scans all buses and fills the device table. Returns the number of
/// devices found (never more than PCI_MAX_DEVICES).
pub fn init() -> usize {
    unsafe { PCI_COUNT = 0 };

    for bus in 0..256u32 {
        for dev in 0..32u32 {
            let mut vendor = config_read16(bus as u8, dev as u8, 0, PCI_VENDOR_ID_OFFSET);
            if vendor == PCI_VENDOR_NONE {
                continue;
            }
            let header = config_read8(bus as u8, dev as u8, 0, PCI_HEADER_TYPE_OFFSET);
            let num_funcs = if header & 0x80 != 0 { 8 } else { 1 };
            for func in 0..num_funcs {
                vendor = config_read16(bus as u8, dev as u8, func as u8, PCI_VENDOR_ID_OFFSET);
                if vendor == PCI_VENDOR_NONE {
                    continue;
                }
                record(bus as u8, dev as u8, func as u8);
            }
        }
    }

    let count = unsafe { PCI_COUNT };
    crate::kprintln!("[rust] PCI: {} device(s) found", count);
    for i in 0..count {
        let d = unsafe { &PCI_DEVICES[i] };
        crate::kprintln!(
            "[rust]   [{:x}:{:x}.{}] {:x}:{:x} class={:x}/{:x} {}",
            d.bus,
            d.dev,
            d.func,
            d.vendor,
            d.device_id,
            d.base_class,
            d.sub_class,
            class_name(d.base_class)
        );
    }
    count
}
