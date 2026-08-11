//! Limine boot protocol requests.
//!
//! Layouts mirror `include/limine.h` (C), base revision 3. The marker
//! values, section names, and request ordering replicate
//! `kernel/limine_requests.c` exactly so Limine's section scan finds
//! them identically to the C build.

#![allow(clippy::missing_safety_doc)]

pub const LIMINE_MEMMAP_USABLE: u64 = 0;

const LIMINE_COMMON_MAGIC: [u64; 2] = [0xc7b1dd30df4c8b88, 0x0a82e883a194f07b];

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LimineVideoMode {
    pub pitch: u64,
    pub width: u64,
    pub height: u64,
    pub bpp: u16,
    pub memory_model: u8,
    pub red_mask_size: u8,
    pub red_mask_shift: u8,
    pub green_mask_size: u8,
    pub green_mask_shift: u8,
    pub blue_mask_size: u8,
    pub blue_mask_shift: u8,
}

#[repr(C)]
pub struct LimineFramebuffer {
    pub address: *mut u8,
    pub width: u64,
    pub height: u64,
    pub pitch: u64,
    pub bpp: u16,
    pub memory_model: u8,
    pub red_mask_size: u8,
    pub red_mask_shift: u8,
    pub green_mask_size: u8,
    pub green_mask_shift: u8,
    pub blue_mask_size: u8,
    pub blue_mask_shift: u8,
    pub unused: [u8; 7],
    pub edid_size: u64,
    pub edid: *mut u8,
    pub mode_count: u64,
    pub modes: *mut *mut LimineVideoMode,
}

#[repr(C)]
pub struct LimineFramebufferResponse {
    pub revision: u64,
    pub framebuffer_count: u64,
    pub framebuffers: *mut *mut LimineFramebuffer,
}

#[repr(C)]
pub struct LimineFramebufferRequest {
    pub id: [u64; 4],
    pub revision: u64,
    pub response: *mut LimineFramebufferResponse,
}

#[repr(C)]
pub struct LimineMemmapEntry {
    pub base: u64,
    pub length: u64,
    pub type_: u64,
}

#[repr(C)]
pub struct LimineMemmapResponse {
    pub revision: u64,
    pub entry_count: u64,
    pub entries: *mut *mut LimineMemmapEntry,
}

#[repr(C)]
pub struct LimineMemmapRequest {
    pub id: [u64; 4],
    pub revision: u64,
    pub response: *mut LimineMemmapResponse,
}

#[repr(C)]
pub struct LimineHhdmResponse {
    pub revision: u64,
    pub offset: u64,
}

#[repr(C)]
pub struct LimineHhdmRequest {
    pub id: [u64; 4],
    pub revision: u64,
    pub response: *mut LimineHhdmResponse,
}

#[repr(C)]
pub struct LimineUuid {
    pub a: u32,
    pub b: u16,
    pub c: u16,
    pub d: [u8; 8],
}

#[repr(C)]
pub struct LimineFile {
    pub revision: u64,
    pub address: *mut u8,
    pub size: u64,
    pub path: *mut u8,
    pub cmdline: *mut u8,
    pub media_type: u32,
    pub unused: u32,
    pub tftp_ip: u32,
    pub tftp_port: u32,
    pub partition_index: u32,
    pub mbr_disk_id: u32,
    pub gpt_disk_uuid: LimineUuid,
    pub gpt_part_uuid: LimineUuid,
    pub part_uuid: LimineUuid,
}

#[repr(C)]
pub struct LimineModuleResponse {
    pub revision: u64,
    pub module_count: u64,
    pub modules: *mut *mut LimineFile,
}

#[repr(C)]
pub struct LimineModuleRequest {
    pub id: [u64; 4],
    pub revision: u64,
    pub response: *mut LimineModuleResponse,
}

/// Base revision marker (LIMINE_BASE_REVISION(3) in C).
#[used]
#[no_mangle]
pub static limine_base_revision: [u64; 3] =
    [0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3];

/// Section markers bracketing the requests region (same values/sections
/// as `kernel/limine_requests.c`).
#[used]
#[link_section = ".limine_requests_start"]
static LIMINE_REQUESTS_START_MARKER: [u64; 4] = [
    LIMINE_COMMON_MAGIC[0],
    LIMINE_COMMON_MAGIC[1],
    0,
    0,
];

#[used]
#[link_section = ".limine_requests"]
static mut FRAMEBUFFER_REQUEST: LimineFramebufferRequest = LimineFramebufferRequest {
    id: [
        0xc7b1dd30df4c8b88,
        0x0a82e883a194f07b,
        0x9d5827dcd881dd75,
        0xa3148604f6fab11b,
    ],
    revision: 0,
    response: core::ptr::null_mut(),
};

#[used]
#[link_section = ".limine_requests"]
static mut MEMMAP_REQUEST: LimineMemmapRequest = LimineMemmapRequest {
    id: [
        0xc7b1dd30df4c8b88,
        0x0a82e883a194f07b,
        0x67cf3d9d378a806f,
        0xe304acdfc50c3c62,
    ],
    revision: 0,
    response: core::ptr::null_mut(),
};

#[used]
#[link_section = ".limine_requests"]
static mut HHDM_REQUEST: LimineHhdmRequest = LimineHhdmRequest {
    id: [
        0xc7b1dd30df4c8b88,
        0x0a82e883a194f07b,
        0x48dcf1cb8ad2b852,
        0x63984e959a98244b,
    ],
    revision: 0,
    response: core::ptr::null_mut(),
};

#[used]
#[link_section = ".limine_requests"]
static mut MODULE_REQUEST: LimineModuleRequest = LimineModuleRequest {
    id: [
        0xc7b1dd30df4c8b88,
        0x0a82e883a194f07b,
        0x3e7e279702be32af,
        0xca1c4f3bd1280cee,
    ],
    revision: 0,
    response: core::ptr::null_mut(),
};

#[used]
#[link_section = ".limine_requests_end"]
static LIMINE_REQUESTS_END_MARKER: [u64; 2] =
    [LIMINE_COMMON_MAGIC[0], LIMINE_COMMON_MAGIC[1]];

pub fn base_revision() -> u64 {
    limine_base_revision[2]
}

pub unsafe fn framebuffer() -> Option<&'static LimineFramebuffer> {
    let response = unsafe { FRAMEBUFFER_REQUEST.response.as_ref() }?;
    if response.framebuffer_count == 0 {
        return None;
    }
    let first = unsafe { response.framebuffers.as_ref() }?;
    unsafe { (*first).as_ref() }
}

pub unsafe fn hhdm_offset() -> Option<u64> {
    let response = unsafe { HHDM_REQUEST.response.as_ref() }?;
    Some(response.offset)
}

/// Borrows the raw memory-map response (filled by Limine at boot).
pub unsafe fn memmap_response() -> Option<&'static LimineMemmapResponse> {
    let response = unsafe { MEMMAP_REQUEST.response.as_ref() }?;
    Some(response)
}

/// Returns `(entry_count, total_usable_bytes)`.
pub unsafe fn memmap() -> Option<(u64, u64)> {
    let response = unsafe { MEMMAP_REQUEST.response.as_ref() }?;
    let entries = unsafe { response.entries.as_ref() }?;
    let mut usable = 0u64;
    for i in 0..response.entry_count as usize {
        let entry = unsafe { (*entries).add(i).as_ref() }?;
        if entry.type_ == LIMINE_MEMMAP_USABLE {
            usable += entry.length;
        }
    }
    Some((response.entry_count, usable))
}

/// Returns the boot modules (Limine `--module` files), if any.
pub unsafe fn modules() -> Option<&'static LimineModuleResponse> {
    let response = unsafe { MODULE_REQUEST.response.as_ref() }?;
    Some(response)
}
