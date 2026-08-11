//! Window manager, compositor, terminal emulator and browser.
//!
//! Faithful port of `kernel/desktop.c`. The whole desktop (gradient,
//! icons, windows, taskbar, Start menu) is composited into the
//! full-screen backbuffer scoped to a damage region, and `present()`
//! blits the accumulated damage to the framebuffer once per poll. The
//! Rust port uses single-buffer presentation only (no BGA page flip):
//! `render_*` and the blit are pixel-identical for QEMU screendumps.

use crate::desktop::backbuffer as bb;
use crate::desktop::font::{FONT_H, FONT_W};
use crate::desktop::rtc;
use crate::graphics::framebuffer::Framebuffer;
use crate::input;
use crate::interrupts::timer;
use crate::memory::kheap;

/* ---- layout constants ---- */

const TASKBAR_H: i64 = 48;
const START_W: i64 = 96;
const START_H: i64 = 34;
const CLOCK_MARGIN: i64 = 16;
const TASK_WIN_W: i64 = 150;
const TASK_WIN_H: i64 = 34;
const TASK_GAP: i64 = 8;

const CURSOR_W: i64 = 12;
const CURSOR_H: i64 = 20;

const TITLE_H: i64 = 26;
const BTN_W: i64 = 22;
const BTN_H: i64 = 20;
const BTN_MARGIN: i64 = 4;
const BTN_GAP: i64 = 2;

const MAX_WINDOWS: usize = 8;
const MAX_WIN_LINES: usize = 8;

const MIN_WIN_W: i64 = 200;
const MIN_WIN_H: i64 = 120;

/* Bottom-right corner grab area for window resizing. */
const RESIZE_GUTTER: i64 = 18;

/* Fetched-page buffer for the browser (headers stripped). */
const BR_WEB_MAX: usize = 8192;

/* Start menu geometry. */
const MENU_W: i64 = 200;
const MENU_ITEM_H: i64 = 40;
const MENU_PAD: i64 = 6;

/* Terminal application. */
const TERM_SCROLL_MAX: usize = 80;
const TERM_LINE_MAX: usize = 88;
const TERM_ROW_H: i64 = 10;
const TERM_BG: u32 = 0x00131720;
const TERM_TEXT: u32 = 0x00FFFFFF;
const TERM_PROMPT: &[u8] = b"solos@sol$ ";

const ICON_W: i64 = 72;
const ICON_H: i64 = 72;
const ICON_X0: i64 = 24;
const ICON_Y0: i64 = 24;
const ICON_ROW_STEP: i64 = 98;
const ICON_LABEL_H: i64 = 16;

/* ---- palette ---- */

const TASKBAR_BG: u32 = 0x001F2A3C;
const TASKBAR_EDGE: u32 = 0x00F5A623;
const START_BG: u32 = 0x00F5A623;
const START_TEXT: u32 = 0x001B2A4A;
const CLOCK_TEXT: u32 = 0x00FFFFFF;
const TASKBAR_BTN_BG: u32 = 0x00313E55;
const TASKBAR_BTN_ACT: u32 = 0x00F5A623;
const TASKBAR_BTN_TEXT: u32 = 0x00FFFFFF;
const TASKBAR_BTN_TACT: u32 = 0x001B2A4A;
const CURSOR_OUTLINE: u32 = 0x00000000;
const CURSOR_FILL: u32 = 0x00FFFFFF;

const WIN_BORDER: u32 = 0x000B1119;
const TITLE_ACTIVE: u32 = 0x00F5A623;
const TITLE_ACT_TEXT: u32 = 0x001B2A4A;
const TITLE_INACT: u32 = 0x00505E6E;
const TITLE_INACT_TEXT: u32 = 0x00FFFFFF;
const BTN_BG_ACT: u32 = 0x00D98E17;
const BTN_BG_INACT: u32 = 0x00424F5E;
const BTN_GLYPH: u32 = 0x00FFFFFF;
const BODY_BG: u32 = 0x00F4F6F9;
const BODY_TEXT: u32 = 0x001B2A4A;

/* ---- window state ---- */

/* kind: 0 = info window, 1 = terminal, 2 = browser */
const KIND_INFO: u8 = 0;
const KIND_TERM: u8 = 1;
const KIND_BROWSER: u8 = 2;

const BR_PAGE_HOME: u8 = 0;
const BR_PAGE_SITE: u8 = 1;
const BR_PAGE_SEARCH: u8 = 2;

const BR_SITE_GENERIC: u8 = 0;
const BR_SITE_YOUTUBE: u8 = 1;
const BR_SITE_GOOGLE: u8 = 2;

const BR_WEB_IDLE: u8 = 0;

#[derive(Clone, Copy)]
struct Window {
    used: bool,
    x: i64,
    y: i64,
    w: i64,
    h: i64,
    title: &'static [u8],
    lines: [&'static [u8]; MAX_WIN_LINES],
    nlines: usize,
    order: u32,
    minimized: bool,
    maximized: bool,
    rest_x: i64,
    rest_y: i64,
    rest_w: i64,
    rest_h: i64,
    kind: u8,
    term_lines: [[u8; TERM_LINE_MAX]; TERM_SCROLL_MAX],
    term_len: usize,
    term_scroll: usize,
    term_input: [u8; TERM_LINE_MAX],
    term_input_len: usize,
    term_cursor_col: usize,
    br_input: [u8; TERM_LINE_MAX],
    br_input_len: usize,
    br_cursor_col: usize,
    br_page: u8,
    br_site_kind: u8,
    br_site_name: [u8; TERM_LINE_MAX],
    br_title: [u8; TERM_LINE_MAX],
    br_web: [u8; BR_WEB_MAX],
    br_web_len: usize,
    br_web_state: u8,
}

const WINDOW_INIT: Window = Window {
    used: false,
    x: 0,
    y: 0,
    w: 0,
    h: 0,
    title: &[],
    lines: [&[]; MAX_WIN_LINES],
    nlines: 0,
    order: 0,
    minimized: false,
    maximized: false,
    rest_x: 0,
    rest_y: 0,
    rest_w: 0,
    rest_h: 0,
    kind: KIND_INFO,
    term_lines: [[0; TERM_LINE_MAX]; TERM_SCROLL_MAX],
    term_len: 0,
    term_scroll: 0,
    term_input: [0; TERM_LINE_MAX],
    term_input_len: 0,
    term_cursor_col: 0,
    br_input: [0; TERM_LINE_MAX],
    br_input_len: 0,
    br_cursor_col: 0,
    br_page: BR_PAGE_HOME,
    br_site_kind: BR_SITE_GENERIC,
    br_site_name: [0; TERM_LINE_MAX],
    br_title: [0; TERM_LINE_MAX],
    br_web: [0; BR_WEB_MAX],
    br_web_len: 0,
    br_web_state: BR_WEB_IDLE,
};

static mut G_WINS: [Window; MAX_WINDOWS] = [WINDOW_INIT; MAX_WINDOWS];
static mut G_ORDER_COUNTER: u32 = 0;
static mut G_DRAG: i64 = -1;
static mut G_DRAG_OFF_X: i64 = 0;
static mut G_DRAG_OFF_Y: i64 = 0;
static mut G_RESIZE: i64 = -1;
static mut G_RESIZE_X: i64 = 0;
static mut G_RESIZE_Y: i64 = 0;
static mut G_RESIZE_W: i64 = 0;
static mut G_RESIZE_H: i64 = 0;
static mut G_START_MENU: bool = false;
static mut G_ICON_OPENS: u32 = 0;

static mut G_CURSOR_X: i64 = 0;
static mut G_CURSOR_Y: i64 = 0;
static mut G_CURSOR_BB_X: i64 = -1;
static mut G_CURSOR_BB_Y: i64 = -1;
static mut G_LAST_SECOND: u64 = 0;

/* Deferred damage: the union of everything redrawn this frame, blitted
 * once in present(). */
static mut G_DAMAGE_PRESENT: bool = false;
static mut G_DAMAGE_X: i64 = 0;
static mut G_DAMAGE_Y: i64 = 0;
static mut G_DAMAGE_W: i64 = 0;
static mut G_DAMAGE_H: i64 = 0;

/* ---- desktop icons ---- */

#[derive(Clone, Copy)]
struct Icon {
    label: &'static [u8],
    glyph: u8,
    color: u32,
    kind: u8,
    win_title: &'static [u8],
    win_lines: &'static [&'static [u8]],
    win_w: i64,
    win_h: i64,
}

const IC_TERMINAL_LINES: [&[u8]; 4] = [
    b"Sol OS terminal",
    b"A shell lands in a later phase.",
    b"For now this window exists to",
    b"prove the desktop can launch.",
];
const IC_FILES_LINES: [&[u8]; 8] = [
    b"Files",
    b"Home",
    b"  Desktop",
    b"  Documents",
    b"  Downloads",
    b"  Pictures",
    b"  Music",
    b"  Videos",
];
const IC_SETTINGS_LINES: [&[u8]; 8] = [
    b"Settings",
    b"  Resolution: 1920x1080",
    b"  Color depth: 32 bpp",
    b"Appearance",
    b"  Theme: Sol OS default",
    b"System",
    b"  Architecture: x86_64",
    b"  Sol OS version 0.1",
];
const IC_ABOUT_LINES: [&[u8]; 6] = [
    b"Sol OS",
    b"Version 0.1",
    b"Architecture: x86_64",
    b"Resolution: 1920x1080",
    b"Bootloader: Limine",
    b"Welcome to Sol OS.",
];

const BROWSER_ICON_W: i64 = 720;
const BROWSER_ICON_H: i64 = 460;

const G_ICONS: [Icon; 5] = [
    Icon {
        label: b"Terminal",
        glyph: b'>',
        color: 0x00458BD9,
        kind: KIND_TERM,
        win_title: b"Terminal",
        win_lines: &IC_TERMINAL_LINES,
        win_w: 460,
        win_h: 220,
    },
    Icon {
        label: b"Files",
        glyph: b'F',
        color: 0x00F5A623,
        kind: KIND_INFO,
        win_title: b"Files",
        win_lines: &IC_FILES_LINES,
        win_w: 460,
        win_h: 220,
    },
    Icon {
        label: b"Settings",
        glyph: b'S',
        color: 0x003AAFA9,
        kind: KIND_INFO,
        win_title: b"Settings",
        win_lines: &IC_SETTINGS_LINES,
        win_w: 460,
        win_h: 220,
    },
    Icon {
        label: b"About",
        glyph: b'A',
        color: 0x006B5B95,
        kind: KIND_INFO,
        win_title: b"About Sol OS",
        win_lines: &IC_ABOUT_LINES,
        win_w: 460,
        win_h: 220,
    },
    Icon {
        label: b"Browser",
        glyph: b'B',
        color: 0x00E5484D,
        kind: KIND_BROWSER,
        win_title: b"Browser",
        win_lines: &[],
        win_w: BROWSER_ICON_W,
        win_h: BROWSER_ICON_H,
    },
];

/* 0 = transparent, 1 = black outline, 2 = white fill. */
const CURSOR_BMP: [[u8; 12]; 20] = [
    [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1],
    [1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1],
    [0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 1],
    [0, 0, 0, 0, 1, 2, 2, 2, 2, 2, 1, 0],
    [0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0],
    [0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0],
];

/* ---- helpers ---- */

fn work_h() -> i64 {
    bb::screen_h() - TASKBAR_H
}

fn start_x() -> i64 {
    8
}

fn start_y() -> i64 {
    bb::screen_h() - TASKBAR_H + (TASKBAR_H - START_H) / 2
}

fn task_btn_x(idx: i64) -> i64 {
    112 + idx * (TASK_WIN_W + TASK_GAP)
}

fn menu_x() -> i64 {
    8
}

fn menu_h() -> i64 {
    MENU_PAD * 2 + G_ICONS.len() as i64 * MENU_ITEM_H
}

fn menu_y() -> i64 {
    work_h() - menu_h() - 4
}

fn cstr_len(s: &[u8]) -> i64 {
    let mut n = 0i64;
    for &c in s {
        if c == 0 {
            break;
        }
        n += 1;
    }
    n
}

fn cstr(s: &[u8]) -> &str {
    let s = &s[..cstr_len(s) as usize];
    core::str::from_utf8(s).unwrap_or("?")
}

/* ---- clock ---- */

fn put_digits(buf: &mut [u8], pos: &mut usize, v: i32, digits: i32) {
    let mut tmp = [0u8; 8];
    let mut n = 0;
    let mut v = v;
    if v == 0 {
        tmp[n] = b'0';
        n += 1;
    }
    while v > 0 && n < 6 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    while n < digits as usize {
        tmp[n] = b'0';
        n += 1;
    }
    while n > 0 {
        n -= 1;
        buf[*pos] = tmp[n];
        *pos += 1;
    }
}

const G_MONTHS: [&[u8]; 12] = [
    b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
    b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec",
];

fn build_clock_str(buf: &mut [u8; 48]) {
    match rtc::read_ist() {
        None => {
            buf[..8].copy_from_slice(b"--:--:--");
            buf[8] = 0;
        }
        Some(dt) => {
            let mut i = 0;
            put_digits(buf, &mut i, dt.hour, 2);
            buf[i] = b':';
            i += 1;
            put_digits(buf, &mut i, dt.minute, 2);
            buf[i] = b':';
            i += 1;
            put_digits(buf, &mut i, dt.second, 2);
            buf[i] = b' ';
            i += 1;
            let m = if dt.month >= 1 && dt.month <= 12 {
                G_MONTHS[(dt.month - 1) as usize]
            } else {
                b"???"
            };
            for &c in m {
                buf[i] = c;
                i += 1;
            }
            buf[i] = b' ';
            i += 1;
            put_digits(buf, &mut i, dt.day, 2);
            buf[i] = b',';
            i += 1;
            buf[i] = b' ';
            i += 1;
            put_digits(buf, &mut i, dt.year, 4);
            buf[i] = 0;
        }
    }
}

/* ---- window management ---- */

fn win_topmost_index() -> i64 {
    let mut best = 0u32;
    let mut besti = -1i64;
    unsafe {
        for i in 0..MAX_WINDOWS {
            let w = &G_WINS[i];
            if !w.used || w.minimized {
                continue;
            }
            if besti < 0 || w.order > best {
                best = w.order;
                besti = i as i64;
            }
        }
    }
    besti
}

fn win_is_topmost(idx: usize) -> bool {
    win_topmost_index() == idx as i64
}

fn win_count() -> i64 {
    let mut n = 0i64;
    unsafe {
        for w in G_WINS.iter() {
            if w.used {
                n += 1;
            }
        }
    }
    n
}

fn win_contains(w: &Window, px: i64, py: i64) -> bool {
    px >= w.x && py >= w.y && px < w.x + w.w && py < w.y + w.h
}

fn win_button_at(w: &Window, px: i64, py: i64) -> u8 {
    if !win_contains(w, px, py) {
        return 0;
    }
    if py < w.y || py >= w.y + TITLE_H {
        return 0;
    }
    let by = w.y + 3;
    if py < by || py >= by + BTN_H {
        return 0;
    }
    let c0 = w.x + w.w - BTN_MARGIN - BTN_W;
    let m0 = c0 - BTN_W - BTN_GAP;
    let n0 = m0 - BTN_W - BTN_GAP;
    if px >= c0 {
        return b'c';
    }
    if px >= m0 {
        return b'x';
    }
    if px >= n0 {
        return b'm';
    }
    0
}

fn win_topmost_at(px: i64, py: i64) -> i64 {
    let mut best = -1i64;
    unsafe {
        for i in 0..MAX_WINDOWS {
            let w = &G_WINS[i];
            if !w.used || w.minimized {
                continue;
            }
            if !win_contains(w, px, py) {
                continue;
            }
            if best < 0 || w.order > G_WINS[best as usize].order {
                best = i as i64;
            }
        }
    }
    best
}

fn taskbar_win_at(px: i64, py: i64) -> i64 {
    if py < start_y() || py >= start_y() + TASK_WIN_H {
        return -1;
    }
    let mut slot = 0i64;
    unsafe {
        for i in 0..MAX_WINDOWS {
            if !G_WINS[i].used {
                continue;
            }
            let bx = task_btn_x(slot);
            if px >= bx && px < bx + TASK_WIN_W {
                return i as i64;
            }
            slot += 1;
        }
    }
    -1
}

fn win_rect_intersects(w: &Window, x: i64, y: i64, ww: i64, hh: i64) -> bool {
    w.x < x + ww && x < w.x + w.w && w.y < y + hh && y < w.y + w.h
}

fn win_open(
    title: &'static [u8],
    mut x: i64,
    mut y: i64,
    mut w: i64,
    mut h: i64,
    kind: u8,
    lines: &[&'static [u8]],
) -> i64 {
    if x < 0 {
        x = 0;
    }
    if y < 0 {
        y = 0;
    }
    if x + w > bb::screen_w() {
        x = bb::screen_w() - w;
    }
    if y + h > work_h() {
        y = work_h() - h;
    }
    if w < MIN_WIN_W {
        w = MIN_WIN_W;
    }
    if h < MIN_WIN_H {
        h = MIN_WIN_H;
    }

    unsafe {
        for i in 0..MAX_WINDOWS {
            let n = &mut G_WINS[i];
            if n.used {
                continue;
            }
            n.used = true;
            n.x = x;
            n.y = y;
            n.w = w;
            n.h = h;
            n.title = title;
            n.kind = kind;
            G_ORDER_COUNTER = G_ORDER_COUNTER.wrapping_add(1);
            n.order = G_ORDER_COUNTER;
            n.minimized = false;
            n.maximized = false;
            if kind == KIND_TERM {
                n.term_len = 0;
                n.term_scroll = 0;
                n.term_input = [0; TERM_LINE_MAX];
                n.term_input_len = 0;
                n.term_cursor_col = 0;
                term_append_line(n, b"Sol OS terminal 0.1");
                term_append_line(n, b"Type 'help' for commands.");
            } else if kind == KIND_BROWSER {
                let init: &[u8] = b"sol.os/home";
                n.br_input = [0; TERM_LINE_MAX];
                n.br_input_len = init.len();
                n.br_input[..init.len()].copy_from_slice(init);
                n.br_cursor_col = n.br_input_len;
                n.br_page = BR_PAGE_HOME;
                n.br_site_kind = BR_SITE_GENERIC;
                n.br_web_len = 0;
                n.br_web_state = BR_WEB_IDLE;
                br_copy(&mut n.br_site_name, b"sol.os/home");
                br_copy(&mut n.br_title, b"sol.os/home");
            } else {
                n.nlines = lines.len().min(MAX_WIN_LINES);
                n.lines = [&[]; MAX_WIN_LINES];
                for li in 0..n.nlines {
                    n.lines[li] = lines[li];
                }
            }
            redraw_all_titles();
            return i as i64;
        }
    }
    crate::kprintln!("[desktop] win_open: no free slot");
    -1
}

fn win_raise(idx: usize) {
    unsafe {
        if !G_WINS[idx].used {
            return;
        }
        let prev = win_topmost_index();
        if prev == idx as i64 {
            return;
        }
        G_ORDER_COUNTER = G_ORDER_COUNTER.wrapping_add(1);
        G_WINS[idx].order = G_ORDER_COUNTER;
        let (px, py, pw, ph) = (
            G_WINS[prev as usize].x,
            G_WINS[prev as usize].y,
            G_WINS[prev as usize].w,
            G_WINS[prev as usize].h,
        );
        let (wx, wy, ww, wh) = (
            G_WINS[idx].x,
            G_WINS[idx].y,
            G_WINS[idx].w,
            G_WINS[idx].h,
        );
        redraw_union(wx, wy, ww, wh, px, py, pw, ph);
        redraw_all_titles();
        redraw_taskbar();
    }
}

fn win_close(idx: usize) {
    unsafe {
        if !G_WINS[idx].used {
            return;
        }
        let (x, y, bw, bh) = (G_WINS[idx].x, G_WINS[idx].y, G_WINS[idx].w, G_WINS[idx].h);
        G_WINS[idx].used = false;
        if G_DRAG == idx as i64 {
            G_DRAG = -1;
        }
        if G_RESIZE == idx as i64 {
            G_RESIZE = -1;
        }
        redraw_rect(x, y, bw, bh);
        redraw_all_titles();
        redraw_taskbar();
    }
}

fn win_minimize(idx: usize) {
    unsafe {
        if !G_WINS[idx].used || G_WINS[idx].minimized {
            return;
        }
        let (x, y, bw, bh) = (G_WINS[idx].x, G_WINS[idx].y, G_WINS[idx].w, G_WINS[idx].h);
        G_WINS[idx].minimized = true;
        if G_DRAG == idx as i64 {
            G_DRAG = -1;
        }
        if G_RESIZE == idx as i64 {
            G_RESIZE = -1;
        }
        redraw_rect(x, y, bw, bh);
        redraw_all_titles();
        redraw_taskbar();
    }
}

fn win_restore_from_taskbar(idx: usize) {
    unsafe {
        if !G_WINS[idx].used {
            return;
        }
        G_WINS[idx].minimized = false;
        G_ORDER_COUNTER = G_ORDER_COUNTER.wrapping_add(1);
        G_WINS[idx].order = G_ORDER_COUNTER;
        let (x, y, w, h) = (G_WINS[idx].x, G_WINS[idx].y, G_WINS[idx].w, G_WINS[idx].h);
        redraw_rect(x, y, w, h);
        redraw_all_titles();
        redraw_taskbar();
    }
}

fn win_toggle_max(idx: usize) {
    unsafe {
        if !G_WINS[idx].used {
            return;
        }
        let (ox, oy, ow, oh) = (G_WINS[idx].x, G_WINS[idx].y, G_WINS[idx].w, G_WINS[idx].h);
        if !G_WINS[idx].maximized {
            G_WINS[idx].rest_x = G_WINS[idx].x;
            G_WINS[idx].rest_y = G_WINS[idx].y;
            G_WINS[idx].rest_w = G_WINS[idx].w;
            G_WINS[idx].rest_h = G_WINS[idx].h;
            G_WINS[idx].x = 0;
            G_WINS[idx].y = 0;
            G_WINS[idx].w = bb::screen_w();
            G_WINS[idx].h = work_h();
            G_WINS[idx].maximized = true;
        } else {
            G_WINS[idx].x = G_WINS[idx].rest_x;
            G_WINS[idx].y = G_WINS[idx].rest_y;
            G_WINS[idx].w = G_WINS[idx].rest_w;
            G_WINS[idx].h = G_WINS[idx].rest_h;
            G_WINS[idx].maximized = false;
        }
        let (x, y, w, h) = (G_WINS[idx].x, G_WINS[idx].y, G_WINS[idx].w, G_WINS[idx].h);
        redraw_union(ox, oy, ow, oh, x, y, w, h);
        redraw_taskbar();
    }
}

/* ---- desktop icons ---- */

fn icon_draw() {
    for i in 0..G_ICONS.len() {
        let ix = ICON_X0;
        let iy = ICON_Y0 + i as i64 * ICON_ROW_STEP;
        bb::fill_rect(ix, iy, ICON_W, ICON_H, WIN_BORDER);
        bb::fill_rect(ix + 2, iy + 2, ICON_W - 4, ICON_H - 4, G_ICONS[i].color);
        bb::draw_char(
            ix + (ICON_W - FONT_W) / 2,
            iy + (ICON_H - FONT_H) / 2,
            G_ICONS[i].glyph,
            0x00FFFFFF,
        );
        let label = G_ICONS[i].label;
        let lw = cstr_len(label) * FONT_W;
        bb::draw_string(ix + (ICON_W - lw) / 2, iy + ICON_H + 6, label, 0x00FFFFFF);
    }
}

fn icon_at(px: i64, py: i64) -> i64 {
    for i in 0..G_ICONS.len() {
        let ix = ICON_X0;
        let iy = ICON_Y0 + i as i64 * ICON_ROW_STEP;
        if px >= ix && px < ix + ICON_W && py >= iy && py < iy + ICON_H + ICON_LABEL_H {
            return i as i64;
        }
    }
    -1
}

fn icon_open(idx: usize) {
    let ic = &G_ICONS[idx];
    let x = 300 + (unsafe { G_ICON_OPENS } % 4) as i64 * 36;
    let y = 90 + (unsafe { G_ICON_OPENS } % 4) as i64 * 28;
    unsafe { G_ICON_OPENS += 1; }
    let wi = win_open(ic.win_title, x, y, ic.win_w, ic.win_h, ic.kind, ic.win_lines);
    if wi >= 0 {
        let (wx, wy, ww, wh) = unsafe {
            let w = &G_WINS[wi as usize];
            (w.x, w.y, w.w, w.h)
        };
        redraw_rect(wx, wy, ww, wh);
        redraw_taskbar();
    }
}

/* ---- damage tracking ---- */

fn damage_add(x: i64, y: i64, w: i64, h: i64) {
    let Some((x0, y0, x1, y1)) = bb::clip_region(x, y, w, h) else {
        return;
    };
    unsafe {
        if !G_DAMAGE_PRESENT {
            G_DAMAGE_X = x0;
            G_DAMAGE_Y = y0;
            G_DAMAGE_W = x1 - x0;
            G_DAMAGE_H = y1 - y0;
            G_DAMAGE_PRESENT = true;
        } else {
            let nx0 = G_DAMAGE_X.min(x0);
            let ny0 = G_DAMAGE_Y.min(y0);
            let nx1 = (G_DAMAGE_X + G_DAMAGE_W).max(x1);
            let ny1 = (G_DAMAGE_Y + G_DAMAGE_H).max(y1);
            G_DAMAGE_X = nx0;
            G_DAMAGE_Y = ny0;
            G_DAMAGE_W = nx1 - nx0;
            G_DAMAGE_H = ny1 - ny0;
        }
    }
}

fn damage_touches(x: i64, y: i64, w: i64, h: i64) -> bool {
    unsafe {
        if !G_DAMAGE_PRESENT {
            return false;
        }
        x < G_DAMAGE_X + G_DAMAGE_W
            && x + w > G_DAMAGE_X
            && y < G_DAMAGE_Y + G_DAMAGE_H
            && y + h > G_DAMAGE_Y
    }
}

/* ---- cursor ---- */

fn cursor_draw_bb() {
    let clip_was_active = bb::clip_active();
    bb::clip_save_clear();
    for row in 0..CURSOR_H as usize {
        for col in 0..CURSOR_W as usize {
            let v = CURSOR_BMP[row][col];
            if v == 0 {
                continue;
            }
            bb::put_pixel(
                unsafe { G_CURSOR_X } + col as i64,
                unsafe { G_CURSOR_Y } + row as i64,
                if v == 1 { CURSOR_OUTLINE } else { CURSOR_FILL },
            );
        }
    }
    if clip_was_active {
        bb::clip_restore();
    }
}

/* ---- redraw helpers ---- */

fn redraw_rect(x: i64, y: i64, w: i64, h: i64) {
    scene_region(x, y, w, h);
    damage_add(x, y, w, h);
}

fn redraw_union(x0: i64, y0: i64, w0: i64, h0: i64, x1: i64, y1: i64, w1: i64, h1: i64) {
    let ux = x0.min(x1);
    let uy = y0.min(y1);
    let ux1 = (x0 + w0).max(x1 + w1);
    let uy1 = (y0 + h0).max(y1 + h1);
    if ux1 <= ux || uy1 <= uy {
        return;
    }
    redraw_rect(ux, uy, ux1 - ux, uy1 - uy);
}

fn redraw_taskbar() {
    redraw_rect(0, work_h(), bb::screen_w(), TASKBAR_H);
}

fn redraw_all_titles() {
    unsafe {
        for i in 0..MAX_WINDOWS {
            let w = &G_WINS[i];
            if !w.used || w.minimized {
                continue;
            }
            redraw_rect(w.x, w.y, w.w, TITLE_H);
        }
    }
}

/* ---- scene composite ---- */

fn scene_region(x: i64, y: i64, w: i64, h: i64) {
    let Some((x0, y0, x1, y1)) = bb::clip_region(x, y, w, h) else {
        return;
    };

    /* Scope every backbuffer write in this composite to the damaged
     * region, so no full-scene element (e.g. the icons) can clobber a
     * window it is not repainting. */
    let prev_active = bb::clip_active();
    let (sx0, sy0, sx1, sy1) = bb::clip_bounds();
    bb::clip_set(x0, y0, x1 - x0, y1 - y0);

    bb::fill_rect_gradient(x0, y0, x1, y1);

    icon_draw();

    /* Windows in z-order (ascending order field). */
    let mut sorted = [0usize; MAX_WINDOWS];
    let mut n = 0usize;
    unsafe {
        for i in 0..MAX_WINDOWS {
            if !G_WINS[i].used || G_WINS[i].minimized {
                continue;
            }
            let mut j = n;
            while j > 0 && G_WINS[sorted[j - 1]].order > G_WINS[i].order {
                sorted[j] = sorted[j - 1];
                j -= 1;
            }
            sorted[j] = i;
            n += 1;
        }
    }
    for k in 0..n {
        let win = unsafe { &G_WINS[sorted[k]] };
        if !win_rect_intersects(win, x, y, w, h) {
            continue;
        }
        win_render(win);
    }

    if y1 > work_h() {
        render_taskbar();
    }

    /* The Start menu floats above everything else when open. */
    if unsafe { G_START_MENU } {
        let mx = menu_x();
        let my = menu_y();
        if x < mx + MENU_W && mx < x + w && y < my + menu_h() && my < y + h {
            render_start_menu();
        }
    }

    bb::restore_clip(prev_active, sx0, sy0, sx1, sy1);
}

/* ---- graphics integrity ---- */

fn desktop_gfx_integrity() -> bool {
    let mut ok = true;
    let corrupt = bb::canary_corruption();
    if corrupt != 0 {
        crate::kprintln!("[gfx] CANARY CORRUPTED: got {:#x}", corrupt);
        ok = false;
    }
    unsafe {
        for i in 0..MAX_WINDOWS {
            let w = &G_WINS[i];
            if !w.used {
                continue;
            }
            if w.x < 0
                || w.y < 0
                || w.w <= 0
                || w.h <= 0
                || w.x + w.w > bb::screen_w()
                || w.y + w.h > bb::screen_h()
            {
                crate::kprintln!(
                    "[gfx] window {} geometry corrupted: {},{} {}x{}",
                    i,
                    w.x,
                    w.y,
                    w.w,
                    w.h
                );
                ok = false;
            }
        }
    }
    ok
}

/* Graphics selftest: exercises fill_rect / draw_char / draw_string
 * against edge-case coordinates and confirms, by reading the
 * backbuffer back, that every operation clipped exactly to the
 * framebuffer bounds. */
fn gfx_selftest() -> bool {
    let mut pass = true;
    let g_w = bb::screen_w();
    let g_h = bb::screen_h();

    bb::clear_region(0, 0, g_w, g_h, 0x00000000);

    /* Half-off-screen rects: clip to the visible part. */
    bb::fill_rect(-10, 5, 40, 30, 0x00111111);
    if bb::pixel(0, 5) != 0x00111111 {
        pass = false;
    }
    if bb::pixel(29, 5) != 0x00111111 {
        pass = false;
    }
    if bb::pixel(30, 5) != 0x00000000 {
        pass = false;
    }

    bb::fill_rect(g_w - 5, 10, 40, 30, 0x00222222);
    if bb::pixel(g_w - 5, 10) != 0x00222222 {
        pass = false;
    }
    if bb::pixel(g_w - 1, 10) != 0x00222222 {
        pass = false;
    }

    bb::fill_rect(10, -10, 40, 30, 0x00333333);
    if bb::pixel(20, 0) != 0x00333333 {
        pass = false;
    }

    bb::fill_rect(10, g_h - 5, 40, 30, 0x00444444);
    if bb::pixel(20, g_h - 5) != 0x00444444 {
        pass = false;
    }
    if bb::pixel(20, g_h - 1) != 0x00444444 {
        pass = false;
    }

    /* Oversized, zero-size and negative-size rects must be inert. */
    bb::fill_rect(-500, -500, 10000, 10000, 0x00555555);
    if bb::pixel(g_w - 1, g_h - 1) != 0x00555555 {
        pass = false;
    }
    if bb::pixel(0, 0) != 0x00555555 {
        pass = false;
    }

    bb::fill_rect(100, 50, 0, 0, 0x00666666);
    if bb::pixel(100, 50) != 0x00555555 {
        pass = false;
    }

    bb::fill_rect(100, 50, -20, -20, 0x00666666);
    if bb::pixel(100, 50) != 0x00555555 {
        pass = false;
    }

    /* Fully off-screen rects must do nothing. */
    bb::fill_rect(-100, -100, 10, 10, 0x00777777);
    bb::fill_rect(g_w, 0, 100, 100, 0x00777777);
    bb::fill_rect(0, g_h, 100, 100, 0x00777777);
    if bb::pixel(0, 0) != 0x00555555 {
        pass = false;
    }

    /* Glyphs partially off-screen must not write out of bounds. */
    bb::draw_char(-3, -3, b'A', 0x00888888);
    bb::draw_string(g_w - 10, g_h - 6, b"edge", 0x00888888);

    if !desktop_gfx_integrity() {
        pass = false;
    }

    crate::kprintln!("[gfx] selftest: {}", if pass { "PASS" } else { "FAIL" });
    pass
}

/* ---- taskbar ---- */

fn render_taskbar() {
    let y = bb::screen_h() - TASKBAR_H;
    bb::fill_rect(0, y, bb::screen_w(), TASKBAR_H, TASKBAR_BG);
    bb::fill_rect(0, y, bb::screen_w(), 1, TASKBAR_EDGE);

    bb::fill_rect(start_x(), start_y(), START_W, START_H, START_BG);
    bb::draw_string(
        start_x() + 12,
        start_y() + (START_H - FONT_H) / 2,
        b"Start",
        START_TEXT,
    );

    let mut slot = 0i64;
    unsafe {
        for i in 0..MAX_WINDOWS {
            let w = &G_WINS[i];
            if !w.used {
                continue;
            }
            let active = win_is_topmost(i);
            let bx = task_btn_x(slot);
            let by = start_y();
            bb::fill_rect(
                bx,
                by,
                TASK_WIN_W,
                TASK_WIN_H,
                if active { TASKBAR_BTN_ACT } else { TASKBAR_BTN_BG },
            );
            let tx = bx + 8;
            let ty = by + (TASK_WIN_H - FONT_H) / 2;
            bb::draw_string(
                tx,
                ty,
                w.title,
                if active { TASKBAR_BTN_TACT } else { TASKBAR_BTN_TEXT },
            );
            slot += 1;
        }
    }

    let mut buf = [0u8; 48];
    build_clock_str(&mut buf);
    let tw = cstr_len(&buf) * FONT_W;
    let cx = bb::screen_w() - CLOCK_MARGIN - tw;
    let cy = y + (TASKBAR_H - FONT_H) / 2;
    bb::draw_string(cx, cy, &buf, CLOCK_TEXT);
}

fn render_clock() {
    let mut buf = [0u8; 48];
    build_clock_str(&mut buf);
    let tw = cstr_len(&buf) * FONT_W;
    let x = bb::screen_w() - CLOCK_MARGIN - tw;
    let y = bb::screen_h() - TASKBAR_H + (TASKBAR_H - FONT_H) / 2;
    let pad: i64 = 8;
    bb::clear_region(
        x - pad,
        bb::screen_h() - TASKBAR_H + 4,
        tw + 2 * pad,
        TASKBAR_H - 8,
        TASKBAR_BG,
    );
    bb::draw_string(x, y, &buf, CLOCK_TEXT);
    damage_add(
        x - pad,
        bb::screen_h() - TASKBAR_H + 4,
        tw + 2 * pad,
        TASKBAR_H - 8,
    );
}

/* ---- Start menu ---- */

fn render_start_menu() {
    let mx = menu_x();
    let my = menu_y();
    bb::fill_rect(mx, my, MENU_W, menu_h(), TASKBAR_BG);
    bb::draw_rect(mx, my, MENU_W, menu_h(), WIN_BORDER);

    for i in 0..G_ICONS.len() {
        let iy = my + MENU_PAD + i as i64 * MENU_ITEM_H;
        bb::fill_rect(
            mx + MENU_PAD,
            iy,
            MENU_W - 2 * MENU_PAD,
            MENU_ITEM_H - 6,
            TASKBAR_BTN_BG,
        );
        let ty = iy + (MENU_ITEM_H - 6 - FONT_H) / 2;
        bb::draw_char(mx + MENU_PAD + 8, ty, G_ICONS[i].glyph, 0x00FFFFFF);
        bb::draw_string(mx + MENU_PAD + 30, ty, G_ICONS[i].label, CLOCK_TEXT);
    }
}

fn close_start_menu() {
    unsafe {
        if !G_START_MENU {
            return;
        }
        G_START_MENU = false;
    }
    redraw_rect(menu_x(), menu_y(), MENU_W, menu_h());
}

fn open_start_menu() {
    unsafe {
        if G_START_MENU {
            return;
        }
        G_START_MENU = true;
    }
    redraw_rect(menu_x(), menu_y(), MENU_W, menu_h());
}

// __WM_PART2__
