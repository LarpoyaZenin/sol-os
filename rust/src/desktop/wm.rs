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
use crate::desktop::wallpaper;
use crate::graphics::framebuffer::Framebuffer;
use crate::input;
use crate::interrupts::timer;
use crate::memory::kheap;
use crate::netstack::{NS_ERR_CONN, NS_ERR_DNS, NS_ERR_HTTP, NS_ERR_NONET, NS_OK};

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

/* Previous frame damage for page-flip path. */
static mut G_DAMAGE_PREV_PRESENT: bool = false;
static mut G_DAMAGE_PREV_X: i64 = 0;
static mut G_DAMAGE_PREV_Y: i64 = 0;
static mut G_DAMAGE_PREV_W: i64 = 0;
static mut G_DAMAGE_PREV_H: i64 = 0;

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
const CURSOR_BMP: [[u8; 12]; 19] = [
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

fn format_dt(v: i32) -> [u8; 2] {
    let mut buf = [0u8; 2];
    let mut pos = 0;
    put_digits(&mut buf, &mut pos, v, 2);
    buf
}

fn itoa(v: usize) -> [u8; 20] {
    let mut buf = [0u8; 20];
    let mut pos = 0;
    let mut n = v;
    if n == 0 {
        buf[pos] = b'0';
        pos += 1;
    } else {
        let mut tmp = [0u8; 20];
        let mut tn = 0;
        while n > 0 && tn < 20 {
            tmp[tn] = b'0' + (n % 10) as u8;
            n /= 10;
            tn += 1;
        }
        while tn > 0 {
            tn -= 1;
            buf[pos] = tmp[tn];
            pos += 1;
        }
    }
    buf[pos] = 0;
    buf
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

/* ---- terminal ---- */

fn term_append_line(w: &mut Window, s: &[u8]) {
    if w.term_len >= TERM_SCROLL_MAX {
        for i in 0..TERM_SCROLL_MAX - 1 {
            w.term_lines[i] = w.term_lines[i + 1];
        }
        w.term_len = TERM_SCROLL_MAX - 1;
    }
    let n = s.len().min(TERM_LINE_MAX - 1);
    w.term_lines[w.term_len][..n].copy_from_slice(&s[..n]);
    w.term_lines[w.term_len][n] = 0;
    w.term_len += 1;
    w.term_scroll = 0;
}

fn term_print(w: &mut Window, s: &[u8]) {
    term_append_line(w, s);
}

fn term_print_time(w: &mut Window) {
    let mut buf = [0u8; 24];
    let mut i = 0;
    buf[i..i + 5].copy_from_slice(b"Time: ");
    i += 6;
    if let Some(dt) = rtc::read_ist() {
        put_digits(&mut buf, &mut i, dt.hour, 2);
        buf[i] = b':';
        i += 1;
        put_digits(&mut buf, &mut i, dt.minute, 2);
        buf[i] = b':';
        i += 1;
        put_digits(&mut buf, &mut i, dt.second, 2);
    } else {
        buf[i..i + 9].copy_from_slice(b"--:--:--");
        i += 8;
    }
    buf[i] = 0;
    term_print(w, &buf[..i]);
}

fn term_print_date(w: &mut Window) {
    let mut buf = [0u8; 32];
    let mut i = 0;
    buf[i..i + 6].copy_from_slice(b"Date: ");
    i += 6;
    if let Some(dt) = rtc::read_ist() {
        const MONTHS: [&[u8]; 12] = [
            b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
            b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec",
        ];
        let m = if (1..=12).contains(&dt.month) {
            MONTHS[dt.month as usize - 1]
        } else {
            b"???"
        };
        for &c in m {
            buf[i] = c;
            i += 1;
        }
        buf[i] = b' ';
        i += 1;
        put_digits(&mut buf, &mut i, dt.day, 2);
        buf[i] = b',';
        i += 1;
        buf[i] = b' ';
        i += 1;
        put_digits(&mut buf, &mut i, dt.year, 4);
    } else {
        buf[i..i + 10].copy_from_slice(b"--/--/----");
        i += 10;
    }
    buf[i] = 0;
    term_print(w, &buf[..i]);
}

fn term_print_mem(w: &mut Window) {
    let mut buf = [0u8; 96];
    let mut i = 0;
    let pre = b"Heap: ";
    buf[i..i + pre.len()].copy_from_slice(pre);
    i += pre.len();
    let used = crate::memory::kheap::used_bytes() / 1024;
    let free = crate::memory::kheap::free_bytes() / 1024;
    let mut tmp = [0u8; 32];
    let mut p = 0;
    ul_to_str(used, &mut tmp, &mut p);
    buf[i..i + p].copy_from_slice(&tmp[..p]);
    i += p;
    let mid = b" KiB used, ";
    buf[i..i + mid.len()].copy_from_slice(mid);
    i += mid.len();
    let mut p = 0;
    ul_to_str(free, &mut tmp, &mut p);
    buf[i..i + p].copy_from_slice(&tmp[..p]);
    i += p;
    let post = b" KiB free";
    buf[i..i + post.len()].copy_from_slice(post);
    i += post.len();
    buf[i] = 0;
    term_print(w, &buf[..i]);
}

fn ul_to_str(v: u64, dst: &mut [u8], pos: &mut usize) {
    let mut tmp = [0u8; 24];
    let mut n = 0;
    if v == 0 {
        tmp[n] = b'0';
        n += 1;
    } else {
        let mut x = v;
        while x > 0 && n < 22 {
            tmp[n] = b'0' + (x % 10) as u8;
            x /= 10;
            n += 1;
        }
        tmp[..n].reverse();
    }
    dst[*pos..*pos + n].copy_from_slice(&tmp[..n]);
    *pos += n;
}

fn term_exec(w: &mut Window) {
    let mut echo = [0u8; TERM_LINE_MAX];
    let mut n = 0;
    for &c in TERM_PROMPT.iter() {
        if n < TERM_LINE_MAX - 1 {
            echo[n] = c;
            n += 1;
        }
    }
    for i in 0..w.term_input_len {
        if n < TERM_LINE_MAX - 1 {
            echo[n] = w.term_input[i];
            n += 1;
        }
    }
    echo[n] = 0;
    term_append_line(w, &echo[..n]);

    let cmd_len = w.term_input_len.min(TERM_LINE_MAX - 1);
    let mut cmd = [0u8; TERM_LINE_MAX];
    cmd[..cmd_len].copy_from_slice(&w.term_input[..cmd_len]);
    w.term_input = [0; TERM_LINE_MAX];
    w.term_input_len = 0;
    w.term_cursor_col = 0;

    crate::kprintln!("[terminal] exec: {:?}", &cmd[..cmd_len]);

    let mut args_start = 0;
    while args_start < cmd_len && cmd[args_start] == b' ' {
        args_start += 1;
    }
    if args_start >= cmd_len {
        return;
    }

    let mut tok_end = args_start;
    while tok_end < cmd_len && cmd[tok_end] != b' ' {
        tok_end += 1;
    }
    let tok = &cmd[args_start..tok_end];
    let rest = if tok_end < cmd_len { &cmd[tok_end + 1..cmd_len] } else { &[][..] };

    if tok == b"help" {
        term_print(w, b"Commands: help, clear, about, echo,");
        term_print(w, b"time, date, mem, reboot.");
    } else if tok == b"clear" {
        w.term_len = 0;
        w.term_scroll = 0;
    } else if tok == b"about" {
        term_print(w, b"Sol OS");
        term_print(w, b"Version 0.1");
        term_print(w, b"Architecture: x86_64");
        term_print(w, b"Resolution: 1920x1080");
        term_print(w, b"Bootloader: Limine");
    } else if tok == b"echo" {
        term_print(w, rest);
    } else if tok == b"time" {
        term_print_time(w);
    } else if tok == b"date" {
        term_print_date(w);
    } else if tok == b"mem" {
        term_print_mem(w);
    } else if tok == b"reboot" {
        term_print(w, b"Rebooting...");
        sys_reboot();
    } else {
        term_print(w, b"Unknown command. Type 'help'.");
    }
}

fn term_feed(w: &mut Window, c: u8) {
    if c >= 0x20 && c <= 0x7E {
        if w.term_input_len < TERM_LINE_MAX - 1 {
            if w.term_cursor_col < w.term_input_len {
                for i in (w.term_cursor_col..w.term_input_len).rev() {
                    w.term_input[i + 1] = w.term_input[i];
                }
            }
            w.term_input[w.term_cursor_col] = c;
            w.term_input_len += 1;
            w.term_cursor_col += 1;
        }
        return;
    }
    if c == 8 {
        if w.term_cursor_col > 0 {
            if w.term_cursor_col < w.term_input_len {
                for i in w.term_cursor_col - 1..w.term_input_len - 1 {
                    w.term_input[i] = w.term_input[i + 1];
                }
            }
            w.term_input_len -= 1;
            w.term_cursor_col -= 1;
            w.term_input[w.term_input_len] = 0;
        }
        return;
    }
    if c == 10 {
        term_exec(w);
        return;
    }
    if c == 0x01 || c == 0x05 {
        w.term_scroll += 1;
        if w.term_scroll > w.term_len {
            w.term_scroll = w.term_len;
        }
        return;
    }
    if c == 0x02 || c == 0x06 {
        if w.term_scroll > 0 {
            w.term_scroll -= 1;
        }
        return;
    }
    if c == 0x03 {
        if w.term_cursor_col > 0 {
            w.term_cursor_col -= 1;
        }
        return;
    }
    if c == 0x04 {
        if w.term_cursor_col < w.term_input_len {
            w.term_cursor_col += 1;
        }
        return;
    }
}

fn term_render(w: &Window, active: bool) {
    let bx = w.x + 1;
    let by = w.y + 1 + TITLE_H;
    let bw = w.w - 2;
    let bh = w.h - 2 - TITLE_H;
    bb::fill_rect(bx, by, bw, bh, TERM_BG);

    let rows = ((bh - 2) / TERM_ROW_H).max(2);
    let cols = ((bw - 8) / FONT_W).max(8).min(TERM_LINE_MAX as i64 - 1);

    let tx = bx + 4;
    let ty = by + 2;

    let sb = rows - 1;
    let start = if w.term_len as i64 > sb + w.term_scroll as i64 {
        w.term_len as i64 - sb - w.term_scroll as i64
    } else {
        0
    };
    for i in 0..sb {
        let ly = ty + i * TERM_ROW_H;
        let line = if (start + i) < w.term_len as i64 {
            &w.term_lines[(start + i) as usize]
        } else {
            &[0; TERM_LINE_MAX]
        };
        let mut x = tx;
        for k in 0..cols {
            if k as usize >= line.len() || line[k as usize] == 0 {
                break;
            }
            bb::draw_char(x, ly, line[k as usize], TERM_TEXT);
            x += FONT_W;
        }
    }

    let iy = ty + sb * TERM_ROW_H;
    let mut x = tx;
    let mut ncells: i64 = 0;
    for &c in TERM_PROMPT.iter() {
        if ncells >= cols {
            break;
        }
        bb::draw_char(x, iy, c, TERM_TEXT);
        x += FONT_W;
        ncells += 1;
    }
    let max_input = (cols - ncells).max(0);
    for k in 0..w.term_input_len.min(max_input as usize) {
        bb::draw_char(x, iy, w.term_input[k], TERM_TEXT);
        x += FONT_W;
        ncells += 1;
    }
    if active && ((timer::ticks() / 100) & 1) == 1 {
        let cx = tx + ((ncells + w.term_cursor_col as i64).min(cols)) * FONT_W;
        bb::fill_rect(cx, iy + FONT_H + 1, FONT_W, 2, TERM_TEXT);
    }
}

/* ---- browser ---- */

fn br_copy(dst: &mut [u8], src: &[u8]) {
    let n = src.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}

fn br_prefix(s: &[u8], pfx: &[u8]) -> bool {
    s.starts_with(pfx)
}

fn br_web_set_err(w: &mut Window, msg: &[u8]) {
    let pre = b"Could not load the page: ";
    let mut i = 0;
    for &c in pre.iter() {
        if i < BR_WEB_MAX - 1 {
            w.br_web[i] = c;
            i += 1;
        }
    }
    for &c in msg.iter() {
        if i < BR_WEB_MAX - 1 {
            w.br_web[i] = c;
            i += 1;
        }
    }
    w.br_web[i] = 0;
    w.br_web_len = i;
    w.br_web_state = 2; /* BR_WEB_ERR */
}

fn br_web_done(ctx: *mut (), status: i32, off: usize, len: usize) {
    let idx = ctx as usize;
    unsafe {
        let w = &mut G_WINS[idx];
        if !w.used {
            return;
        }
        if status == NS_OK && off <= BR_WEB_MAX && len <= BR_WEB_MAX {
            let copy_len = len.min(BR_WEB_MAX - off);
            let mut tmp = [0u8; BR_WEB_MAX];
            tmp[..copy_len].copy_from_slice(&w.br_web[off..off + copy_len]);
            w.br_web[..copy_len].copy_from_slice(&tmp[..copy_len]);
            w.br_web_len = copy_len;
            if w.br_web_len < BR_WEB_MAX {
                w.br_web[w.br_web_len] = 0;
            }
            w.br_web_state = 1; /* BR_WEB_OK */
        } else {
            let msg: &[u8] = match status {
                NS_ERR_NONET => b"no network device.",
                NS_ERR_DNS => b"could not resolve the host name.",
                NS_ERR_CONN => b"connection failed or timed out.",
                NS_ERR_HTTP => b"the server returned an error.",
                _ => b"request aborted.",
            };
            br_web_set_err(w, msg);
        }
        redraw_rect(w.x, w.y, w.w, w.h);
    }
}

fn br_navigate(w: &mut Window) {
    let n = w.br_input_len;
    let mut has_print = false;
    for i in 0..n {
        if w.br_input[i] != b' ' && w.br_input[i] != b'\t' {
            has_print = true;
            break;
        }
    }
    if !has_print {
        return;
    }

    let mut tok = [0u8; TERM_LINE_MAX];
    let mut ti = 0;
    for i in 0..n {
        let mut c = w.br_input[i];
        if c >= b'A' && c <= b'Z' {
            c = c - b'A' + b'a';
        }
        if ti < tok.len() - 1 {
            tok[ti] = c;
            ti += 1;
        }
    }
    while ti > 0 && (tok[ti - 1] == b' ' || tok[ti - 1] == b'\t') {
        ti -= 1;
    }
    tok[ti] = 0;

    let s = &tok[..ti];
    let mut dom = [0u8; TERM_LINE_MAX];
    let mut di = 0;
    let start = if br_prefix(s, b"http://") {
        7
    } else if br_prefix(s, b"https://") {
        8
    } else {
        0
    };
    let rest = &s[start..];
    let mut has_space = false;
    for &c in rest.iter() {
        if c == b' ' || c == b'\t' {
            has_space = true;
            break;
        }
    }

    if !has_space {
        for &c in rest.iter() {
            if di >= dom.len() - 1 {
                break;
            }
            if c == b'/' || c == b'?' || c == b'#' {
                break;
            }
            dom[di] = c;
            di += 1;
        }
        dom[di] = 0;
        if br_prefix(&dom[..di], b"www.") {
            dom.copy_within(4..di, 0);
            dom[di - 4] = 0;
        }

        w.br_page = 1; /* BR_PAGE_SITE */
        w.br_site_kind = 0; /* BR_SITE_GENERIC */
        br_copy(&mut w.br_site_name, &dom[..di]);
        br_copy(&mut w.br_title, &dom[..di]);

        if br_prefix(&dom[..di], b"youtube") || br_prefix(&dom[..di], b"youtube.com") {
            w.br_site_kind = 1; /* BR_SITE_YOUTUBE */
            br_copy(&mut w.br_title, b"YouTube");
            w.br_web_state = 0; /* BR_WEB_IDLE */
            return;
        }
        if br_prefix(&dom[..di], b"google") || br_prefix(&dom[..di], b"google.com") {
            w.br_site_kind = 2; /* BR_SITE_GOOGLE */
            br_copy(&mut w.br_title, b"Google");
            w.br_web_state = 0;
            return;
        }
        if br_prefix(&dom[..di], b"sol.os") || br_prefix(&dom[..di], b"solos") {
            w.br_page = 0; /* BR_PAGE_HOME */
            br_copy(&mut w.br_title, b"sol.os/home");
            w.br_web_state = 0;
            return;
        }

        w.br_web_len = 0;
        w.br_web_state = 1; /* BR_WEB_BUSY */
        let idx = unsafe { (w as *mut Window as usize - G_WINS.as_ptr() as usize) / core::mem::size_of::<Window>() };
        crate::netstack::http_get(&dom[..di], b"/", w.br_web.as_mut_ptr(), BR_WEB_MAX, Some(br_web_done), idx as *mut ());
        return;
    }

    w.br_page = 2; /* BR_PAGE_SEARCH */
    br_copy(&mut w.br_site_name, s);
    br_copy(&mut w.br_title, b"Search");
}

fn br_feed(w: &mut Window, c: u8) {
    if c >= 0x20 && c <= 0x7E {
        if w.br_input_len < TERM_LINE_MAX - 1 {
            if w.br_cursor_col < w.br_input_len {
                for i in (w.br_cursor_col..w.br_input_len).rev() {
                    w.br_input[i + 1] = w.br_input[i];
                }
            }
            w.br_input[w.br_cursor_col] = c;
            w.br_input_len += 1;
            w.br_cursor_col += 1;
        }
        return;
    }
    if c == 8 {
        if w.br_cursor_col > 0 {
            if w.br_cursor_col < w.br_input_len {
                for i in w.br_cursor_col - 1..w.br_input_len - 1 {
                    w.br_input[i] = w.br_input[i + 1];
                }
            }
            w.br_input_len -= 1;
            w.br_cursor_col -= 1;
            w.br_input[w.br_input_len] = 0;
        }
        return;
    }
    if c == 0x03 {
        if w.br_cursor_col > 0 {
            w.br_cursor_col -= 1;
        }
        return;
    }
    if c == 0x04 {
        if w.br_cursor_col < w.br_input_len {
            w.br_cursor_col += 1;
        }
        return;
    }
    if c == 10 {
        br_navigate(w);
        return;
    }
}

/* ---- browser pages ---- */

fn build_ist_datetime() -> ([u8; 32], i32) {
    let mut dt = [0u8; 32];
    let mut dow = 0;
    if let Some(r) = rtc::read_ist() {
        let _days_full: [&[u8]; 7] = [
            b"Sunday", b"Monday", b"Tuesday", b"Wednesday",
            b"Thursday", b"Friday", b"Saturday",
        ];
        let _months: [&[u8]; 12] = [
            b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
            b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec",
        ];
        let y = r.year as i32;
        let m = r.month as i32;
        let d = r.day as i32;
        let _hh = r.hour as i32;
        let _mm = r.minute as i32;
        let _ss = r.second as i32;
        /* Sakamoto's algorithm */
        let t = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4];
        let yy = if m < 3 { y - 1 } else { y };
        dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m as usize - 1] + d) % 7;
        dow = dow.rem_euclid(7);

        let mut i = 0;
        let label = b"Indian Standard Time (IST)";
        for &c in label.iter() {
            dt[i] = c;
            i += 1;
        }
        dt[i] = 0;
    }
    (dt, dow)
}

fn browser_home(cx: i64, py: i64, cw: i64, ph: i64) {
    bb::fill_rect(cx, py, cw, ph, 0x00FFFFFF);
    if cw < 320 || ph < 220 {
        return;
    }
    let (dt_str, _dow) = build_ist_datetime();

    let wm_w = (b"SOL OS".len() as i64) * FONT_W * 2;
    bb::draw_string_scaled(cx + (cw - wm_w) / 2, py + 34, b"SOL OS", 0x002766A8, 2);

    let st_w = (b"Welcome to the Sol OS browser.".len() as i64) * FONT_W;
    bb::draw_string(cx + (cw - st_w) / 2, py + 34 + 2 * FONT_H + 12, b"Welcome to the Sol OS browser.", 0x00607080);

    let sw = if cw < 560 { cw - 80 } else { 480 }.max(120);
    let sb_x = cx + (cw - sw) / 2;
    let sb_y = py + 34 + 2 * FONT_H + 12 + FONT_H + 26;
    bb::fill_rect(sb_x, sb_y, sw, 26, 0x00F4F6F9);
    bb::draw_rect(sb_x, sb_y, sw, 26, 0x00DDE2EA);
    bb::draw_string(sb_x + 10, sb_y + (26 - FONT_H) / 2, b"Search or type a URL...", 0x008090A0);

    let pw = 400.min(cw - 60);
    let px0 = cx + (cw - pw) / 2;
    let py0 = sb_y + 26 + 30;
    bb::fill_rect(px0, py0, pw, 132, 0x00F4F6F9);
    bb::draw_rect(px0, py0, pw, 132, 0x00DDE2EA);

    let lab_w = (dt_str.len() as i64) * FONT_W;
    bb::draw_string(px0 + (pw - lab_w) / 2, py0 + 10, &dt_str, 0x002766A8);

    if let Some(dt) = rtc::read_ist() {
        let _days_full: [&[u8]; 7] = [
            b"Sunday", b"Monday", b"Tuesday", b"Wednesday",
            b"Thursday", b"Friday", b"Saturday",
        ];
        let _months: [&[u8]; 12] = [
            b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
            b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec",
        ];
        let mut buf = [0u8; 64];
        let mut i = 0;
        let tlen = dt_str.iter().position(|&b| b == 0).unwrap_or(dt_str.len());
        buf[..tlen].copy_from_slice(&dt_str[..tlen]);
        i += tlen;
        let time_str = format_dt(dt.hour);
        buf[i..i + time_str.len()].copy_from_slice(&time_str);
        i += time_str.len();
        buf[i] = b':';
        i += 1;
        let min_str = &format_dt(dt.minute);
        buf[i..i + min_str.len()].copy_from_slice(min_str);
        i += min_str.len();
        buf[i] = b':';
        i += 1;
        let sec_str = &format_dt(dt.second);
        buf[i..i + sec_str.len()].copy_from_slice(sec_str);
        i += sec_str.len();
        buf[i] = 0;
    }
}

fn browser_web(cx: i64, py: i64, cw: i64, ph: i64, w: &Window) {
    let mut banner = [0u8; TERM_LINE_MAX + 24];
    let mut bi = 0;
    let pre_bytes: &[u8] = if w.br_web_state == 1 {
        b"Loading http://"
    } else if w.br_web_state == 2 {
        b"Fetched http://"
    } else {
        b"Page could not be loaded"
    };
    bi = pre_bytes.len();
    banner[..bi].copy_from_slice(pre_bytes);
    for &c in w.br_site_name.iter() {
        if c == 0 || bi >= banner.len() - 1 {
            break;
        }
        banner[bi] = c;
        bi += 1;
    }
    if w.br_web_state == 1 {
        let mid = b" - ";
        banner[bi..bi + mid.len()].copy_from_slice(mid);
        bi += mid.len();
        let len_str = itoa(w.br_web_len);
        for &c in len_str.iter() {
            if bi < banner.len() - 1 && c != 0 {
                banner[bi] = c;
                bi += 1;
            }
        }
        let suf = b" bytes";
        banner[bi..bi + suf.len()].copy_from_slice(suf);
        bi += suf.len();
    }
    banner[bi] = 0;
    bb::draw_string(cx + 16, py + 10, &banner[..bi], 0x00808080);
    bb::fill_rect(cx + 16, py + 10 + FONT_H + 6, cw - 32, 1, 0x00E8EBF0);

    if w.br_web_state == 1 {
        bb::draw_string(cx + 16, py + 42, b"Fetching over the SLIRP gateway...", 0x00404040);
        return;
    }
    if w.br_web_state == 2 {
        bb::draw_string(cx + 16, py + 42, &w.br_web[..w.br_web_len], 0x00C0392B);
        return;
    }

    /* strip tags + entities into bounded plain text */
    let mut text = [0u8; BR_WEB_MAX];
    let mut o = 0;
    let mut in_tag = false;
    let mut in_ent = false;
    let mut prev_space = true;
    for i in 0..w.br_web_len.min(BR_WEB_MAX - 1) {
        let c = w.br_web[i];
        if c == b'<' {
            in_tag = true;
            continue;
        }
        if in_tag {
            if c == b'>' {
                in_tag = false;
            }
            continue;
        }
        if c == b'&' {
            in_ent = true;
            continue;
        }
        if in_ent {
            if c == b';' {
                in_ent = false;
            }
            continue;
        }
        if c == b'\t' || c == b'\r' || c == b'\n' {
            if !prev_space && o < text.len() - 1 {
                text[o] = b' ';
                o += 1;
            }
            prev_space = true;
            continue;
        }
        if o < text.len() - 1 {
            text[o] = c;
            o += 1;
        }
        prev_space = false;
    }
    text[o] = 0;
    br_wrap_text(cx + 16, py + 42, cw - 32, &text[..o], 0x00333333);
}

fn br_wrap_text(mut x: i64, y: i64, max_w: i64, s: &[u8], fg: u32) -> i64 {
    let x0 = x;
    let mut word = [0u8; 64];
    let mut wi = 0;
    let mut y = y;
    for &c in s.iter() {
        if c == b' ' || c == 0 || wi == word.len() - 1 {
            word[wi] = 0;
            let ww = (wi as i64) * FONT_W;
            if wi > 0 && x - x0 + ww > max_w {
                x = x0;
                y += FONT_H + 2;
            }
            if wi > 0 {
                bb::draw_string(x, y, &word[..wi], fg);
            }
            x += ww + FONT_W;
            wi = 0;
            if c == 0 {
                break;
            }
        } else {
            word[wi] = c;
            wi += 1;
        }
    }
    y
}

fn browser_site(cx: i64, py: i64, cw: i64, ph: i64, w: &Window) {
    bb::fill_rect(cx, py, cw, ph, 0x00FFFFFF);
    if cw < 320 || ph < 200 {
        return;
    }
    if w.br_web_state != 0 {
        browser_web(cx, py, cw, ph, w);
        return;
    }
    if w.br_site_kind == 1 {
        let hdr = 40;
        bb::fill_rect(cx, py, cw, hdr, 0x00FF0000);
        bb::draw_string_scaled(cx + 18, py + (hdr - 2 * FONT_H) / 2, b"YouTube", 0x00FFFFFF, 2);
        let pill_w = 200;
        let pill_x = cx + cw - 18 - pill_w;
        bb::fill_rect(pill_x, py + (hdr - 22) / 2, pill_w, 22, 0x00E5E5E5);
        bb::draw_string(pill_x + 8, py + (hdr - FONT_H) / 2, b"Search", 0x00606060);
        let mut y = py + hdr + 14;
        bb::draw_string(cx + 18, y, b"Recommended", 0x000F0F0F);
        y += FONT_H + 10;
        let titles: [&[u8]; 6] = [
            b"Sol OS in 60 seconds",
            b"How the window manager works",
            b"The IST clock deep dive",
            b"Virtio-net, no wires",
            b"Framebuffer pixel art",
            b"Booting a 64-bit kernel",
        ];
        let subs: [&[u8]; 6] = [
            b"Sol OS  |  1.2M views",
            b"Sol OS  |  980K views",
            b"Sol OS  |  740K views",
            b"Sol OS  |  1.1M views",
            b"Sol OS  |  610K views",
            b"Sol OS  |  2.3M views",
        ];
        let cols = [0x00FF5A5F, 0x004B8BBE, 0x007B4B9A, 0x00E88C1F, 0x004CAF50, 0x00E25B5B];
        let pad = 18;
        let gap = 12;
        let card_w = (cw - pad * 2 - gap * 2) / 3;
        let mut thumb_h = card_w * 9 / 16;
        if thumb_h > 108 {
            thumb_h = 108;
        }
        for r in 0..2 {
            for k in 0..3 {
                let idx = (r * 3 + k) as usize;
                if idx >= titles.len() {
                    break;
                }
                let tx = cx + pad + k * (card_w + gap);
                let ty = y + r * (thumb_h + FONT_H * 3 + 8);
                if ty + thumb_h + FONT_H * 3 + 6 > py + ph {
                    break;
                }
                bb::fill_rect(tx, ty, card_w, thumb_h, cols[idx]);
                bb::draw_char(tx + card_w / 2 - FONT_W / 2, ty + (thumb_h - FONT_H) / 2, b'>', 0x00FFFFFF);
                bb::draw_string(tx, ty + thumb_h + 6, titles[idx], 0x000F0F0F);
                bb::draw_string(tx, ty + thumb_h + 6 + FONT_H + 2, subs[idx], 0x00606060);
            }
        }
        return;
    }
    if w.br_site_kind == 2 {
        let word = b"Google";
        let gcols = [0x004285F4, 0x00EA4335, 0x00FBBC05, 0x004285F4, 0x0034A853, 0x00EA4335];
        let scale = 3;
        let wm_w = (word.len() as i64) * FONT_W * scale;
        let wmy = py + 30;
        for i in 0..word.len() {
            let one = [word[i]];
            bb::draw_string_scaled(cx + (cw - wm_w) / 2 + i as i64 * FONT_W * scale, wmy, &one, gcols[i], scale);
        }
        let sbw = 520.min(cw - 60);
        let sby = wmy + FONT_H * scale + 26;
        bb::fill_rect(cx + (cw - sbw) / 2, sby, sbw, 40, 0x00FFFFFF);
        bb::draw_rect(cx + (cw - sbw) / 2, sby, sbw, 40, 0x00DADCE0);
        bb::draw_string(cx + (cw - sbw) / 2 + 16, sby + (40 - FONT_H) / 2, b"Search Google or type a URL", 0x008090A0);
        let b1w = 104;
        let b2w = 140;
        let by0 = sby + 40 + 16;
        let sum = b1w + 12 + b2w;
        let bx0 = cx + (cw - sum) / 2;
        bb::fill_rect(bx0, by0, b1w, 32, 0x00F8F9FA);
        bb::draw_rect(bx0, by0, b1w, 32, 0x00DADCE0);
        bb::draw_string(bx0 + (b1w - 13 * FONT_W) / 2, by0 + (32 - FONT_H) / 2, b"Google Search", 0x003C4043);
        bb::fill_rect(bx0 + b1w + 12, by0, b2w, 32, 0x00F8F9FA);
        bb::draw_rect(bx0 + b1w + 12, by0, b2w, 32, 0x00DADCE0);
        bb::draw_string(bx0 + b1w + 12 + (b2w - 17 * FONT_W) / 2, by0 + (32 - FONT_H) / 2, b"I'm Feeling Lucky", 0x003C4043);
        return;
    }

    let wm_w = (w.br_site_name.iter().position(|&b| b == 0).unwrap_or(0) as i64) * FONT_W * 2;
    let mut scale = 2;
    if wm_w > cw - 40 {
        scale = 1;
    }
    bb::fill_rect(cx, py, cw, 44, 0x002766A8);
    let name_len = w.br_site_name.iter().position(|&b| b == 0).unwrap_or(0);
    bb::draw_string_scaled(cx + (cw - name_len as i64 * FONT_W * scale) / 2, py + (44 - FONT_H * 2) / 2, &w.br_site_name[..name_len], 0x00FFFFFF, scale);
    bb::draw_string(cx + 16, py + 44 + 12, b"Home   About   Contact", 0x001B2A4A);
    let para: [&[u8]; 5] = [
        b"This is a static preview of the page you asked for.",
        b"sol-os runs its own virtual network with no real",
        b"internet access, so live content cannot load here.",
        b"The address bar and tab already work; try a search",
        b"query to see the mock results page instead.",
    ];
    let mut pyy = py + 44 + 12 + FONT_H + 20;
    for line in para.iter() {
        bb::draw_string(cx + 16, pyy, line, 0x00404448);
        pyy += FONT_H + 4;
    }
    bb::fill_rect(cx + cw - 150, py + 44 + 12, 134, 20, 0x00F4F6F9);
    bb::draw_rect(cx + cw - 150, py + 44 + 12, 134, 20, 0x00DDE2EA);
    bb::draw_string(cx + cw - 144, py + 44 + 12 + 6, b"sol.os preview", 0x008090A0);
}

fn browser_search(cx: i64, py: i64, cw: i64, ph: i64, w: &Window) {
    bb::fill_rect(cx, py, cw, ph, 0x00FFFFFF);
    if cw < 320 || ph < 160 {
        return;
    }
    let mut head = [0u8; TERM_LINE_MAX + 16];
    let pre = b"Results for \"";
    head[..pre.len()].copy_from_slice(pre);
    let mut hi = pre.len();
    for i in 0..w.br_site_name.len().min(TERM_LINE_MAX - hi - 2) {
        if w.br_site_name[i] == 0 {
            break;
        }
        head[hi] = w.br_site_name[i];
        hi += 1;
    }
    head[hi] = b'"';
    hi += 1;
    head[hi] = 0;
    bb::draw_string(cx + 20, py + 14, &head[..hi], 0x001B2A4A);
    let mut y = py + 14 + FONT_H + 10;
    bb::fill_rect(cx + 20, y, cw - 40, 1, 0x00E8EBF0);
    y += 10;
    let rtitles: [&[u8]; 4] = [
        b"Sol OS - the 64-bit microkernel you can build",
        b"Sol OS browser - IST clock preview",
        b"Virtio-net driver - source on GitHub",
        b"The framebuffer compositor, explained",
    ];
    let rurls: [&[u8]; 4] = [
        b"sol-os.dev/docs",
        b"sol-os.dev/browser",
        b"github.com/sol-os/virtio-net",
        b"wiki.sol-os.org/compositor",
    ];
    let rsnp: [&[u8]; 5] = [
        b"Boots a graphical desktop in about 8 seconds, with a window",
        b"manager, terminal emulator and this very browser.",
        b"A live Indian Standard Time clock, built on the RTC driver.",
        b"A real virtio-net driver for ICMP echo and ARP replies.",
        b"Per-window damage tracking with page flipping and vblank gating.",
    ];
    for i in 0..rtitles.len() {
        if y + FONT_H * 3 > py + ph {
            break;
        }
        bb::draw_string(cx + 20, y, rtitles[i], 0x00AB0D1A);
        y += FONT_H + 2;
        bb::draw_string(cx + 20, y, rurls[i], 0x00216600);
        y += FONT_H + 2;
        y = br_wrap_text(cx + 20, y, cw - 40, rsnp[i], 0x0056514D);
        y += 14;
        if i + 1 < rtitles.len() {
            bb::fill_rect(cx + 20, y, cw - 40, 1, 0x00E8EBF0);
        }
        y += 10;
    }
    if y + FONT_H < py + ph {
        bb::draw_string(cx + 20, py + ph - FONT_H - 8, b"sol.os search - mock index, no real network", 0x00808080);
    }
}

/* ---- window rendering ---- */

fn win_render(w: &Window) {
    let x = w.x;
    let y = w.y;
    let bw = w.w;
    let bh = w.h;
    let active = unsafe { win_topmost_index() == G_WINS.iter().position(|r| r.order == w.order && r.used).unwrap_or(0) as i64 };
    let title_bg = if active { TITLE_ACTIVE } else { TITLE_INACT };
    let title_tx = if active { TITLE_ACT_TEXT } else { TITLE_INACT_TEXT };
    let btn_bg = if active { BTN_BG_ACT } else { BTN_BG_INACT };

    bb::draw_rect(x, y, bw, bh, WIN_BORDER);
    bb::fill_rect(x + 1, y + 1, bw - 2, TITLE_H, title_bg);
    bb::draw_string(x + 7, y + (TITLE_H - FONT_H) / 2, w.title, title_tx);

    let c0 = x + bw - BTN_MARGIN - BTN_W;
    let m0 = c0 - BTN_W - BTN_GAP;
    let n0 = m0 - BTN_W - BTN_GAP;
    let by = y + 3;

    bb::fill_rect(c0, by, BTN_W, BTN_H, btn_bg);
    bb::fill_rect(m0, by, BTN_W, BTN_H, btn_bg);
    bb::fill_rect(n0, by, BTN_W, BTN_H, btn_bg);

    bb::fill_rect(n0 + (BTN_W - 12) / 2, by + BTN_H / 2 - 1, 12, 2, BTN_GLYPH);
    let gx = m0 + (BTN_W - 12) / 2;
    let gy = by + (BTN_H - 10) / 2;
    bb::fill_rect(gx, gy, 12, 1, BTN_GLYPH);
    bb::fill_rect(gx, gy + 9, 12, 1, BTN_GLYPH);
    bb::fill_rect(gx, gy + 1, 1, 8, BTN_GLYPH);
    bb::fill_rect(gx + 11, gy + 1, 1, 8, BTN_GLYPH);
    bb::draw_char(c0 + (BTN_W - FONT_W) / 2, by + (BTN_H - FONT_H) / 2, b'x', BTN_GLYPH);

    if w.kind == KIND_TERM {
        term_render(w, active);
        return;
    }
    if w.kind == KIND_BROWSER {
        let cx = x + 1;
        let cy = y + 1 + TITLE_H;
        let cw = bw - 2;
        let ch = bh - 2 - TITLE_H;
        bb::fill_rect(cx, cy, cw, ch, 0x00E8EBF0);
        let tb_h = 30;
        let nb_h = 34;
        let sb = 20;
        let tab_y = cy + 6;
        let tab_w = 180;
        let tab_h = 24;
        bb::fill_rect(cx + sb, tab_y, tab_w, tab_h, 0x00FFFFFF);
        let ntb_x = cx + sb + tab_w + 6;
        bb::fill_rect(ntb_x, tab_y, 26, tab_h, 0x00DDE2EA);
        bb::fill_rect(ntb_x + 9, tab_y + tab_h / 2 - 1, 8, 2, 0x008090A0);
        bb::fill_rect(ntb_x + 12, tab_y + 4, 2, 16, 0x008090A0);
        let title_len = w.br_title.iter().position(|&b| b == 0).unwrap_or(0);
        bb::draw_string(cx + sb + 7, tab_y + (tab_h - FONT_H) / 2, &w.br_title[..title_len.min(w.br_title.len())], BODY_TEXT);
        let nav_y = cy + tb_h;
        let page_y = nav_y + nb_h;
        bb::fill_rect(cx, nav_y, cw, nb_h, 0x00F4F6F9);
        bb::fill_rect(cx + sb, nav_y + 5, 24, 24, 0x00FFFFFF);
        bb::fill_rect(cx + sb + 26, nav_y + 5, 24, 24, 0x00FFFFFF);
        bb::draw_char(cx + sb + 8, nav_y + (24 - FONT_H) / 2 + 5, b'<', 0x00A0A8B4);
        bb::draw_char(cx + sb + 34, nav_y + (24 - FONT_H) / 2 + 5, b'>', 0x00A0A8B4);
        let ab_x = cx + sb + 56;
        let ab_w = cw - sb * 2 - 56;
        bb::fill_rect(ab_x, nav_y + 5, ab_w, 24, 0x00FFFFFF);
        let ab_tx = ab_x + 8;
        let ab_ty = nav_y + (24 - FONT_H) / 2 + 5;
        let nfit = ((ab_w - 16) / FONT_W).max(1) as usize;
        let mut start: usize = 0;
        if w.br_input_len > nfit {
            start = w.br_input_len - nfit;
            if w.br_cursor_col as usize >= start + nfit {
                start = w.br_cursor_col as usize - nfit + 1;
            }
            if (w.br_cursor_col as usize) < start {
                start = w.br_cursor_col as usize;
            }
            if start > w.br_input_len {
                start = w.br_input_len;
            }
        }
        let mut x = ab_tx;
        if w.br_input_len == 0 {
            bb::draw_string(ab_tx, ab_ty, b"Search or type a URL...", 0x008090A0);
        } else {
            for k in start..w.br_input_len.min(start + nfit) {
                bb::draw_char(x, ab_ty, w.br_input[k], 0x001B2A4A);
                x += FONT_W;
            }
        }
        if active && ((timer::ticks() / 100) & 1) == 1 {
            let caret = ab_tx + ((w.br_cursor_col - start).max(0) as i64) * FONT_W;
            if caret < ab_tx + ab_w - 16 {
                bb::fill_rect(caret, ab_ty + 1, 1, FONT_H, 0x001B2A4A);
            }
        }
        let pph = cy + ch - page_y;
        if w.br_page == 1 {
            browser_site(cx, page_y, cw, pph, w);
        } else if w.br_page == 2 {
            browser_search(cx, page_y, cw, pph, w);
        } else {
            browser_home(cx, page_y, cw, pph);
        }
        if !w.maximized {
            let gr = x + bw - 12;
            let gb = y + bh - 12;
            bb::fill_rect(gr, gb + 8, 10, 2, 0x00C0C8D4);
            bb::fill_rect(gr + 4, gb + 4, 10, 2, 0x00C0C8D4);
            bb::fill_rect(gr + 8, gb, 10, 2, 0x00C0C8D4);
        }
        return;
    }

    bb::fill_rect(x + 1, y + 1 + TITLE_H, bw - 2, bh - 2 - TITLE_H, BODY_BG);
    let lx = x + 8;
    let mut ly = y + 1 + TITLE_H + 6;
    for li in 0..w.nlines.min(MAX_WIN_LINES) {
        bb::draw_string(lx, ly, w.lines[li], BODY_TEXT);
        ly += FONT_H + 4;
    }
    if !w.maximized {
        let gr = x + bw - 12;
        let gb = y + bh - 12;
        bb::fill_rect(gr, gb + 8, 10, 2, 0x00C0C8D4);
        bb::fill_rect(gr + 4, gb + 4, 10, 2, 0x00C0C8D4);
        bb::fill_rect(gr + 8, gb, 10, 2, 0x00C0C8D4);
    }
}

/* ---- click dispatch ---- */

fn handle_left_press(px: i64, py: i64) {
    if py >= work_h() {
        if px >= start_x() && px < start_x() + START_W && py >= start_y() && py < start_y() + START_H {
            crate::kprintln!("[desktop] start button");
            if unsafe { G_START_MENU } {
                close_start_menu();
            } else {
                open_start_menu();
            }
            return;
        }
        if unsafe { G_START_MENU } {
            close_start_menu();
        }
        let wi = taskbar_win_at(px, py);
        if wi >= 0 {
            crate::kprintln!("[desktop] taskbar '{}'", unsafe { core::str::from_utf8(G_WINS[wi as usize].title).unwrap_or("?") });
            if unsafe { G_WINS[wi as usize].minimized } {
                win_restore_from_taskbar(wi as usize);
            } else {
                win_raise(wi as usize);
            }
        }
        return;
    }
    if unsafe { G_START_MENU } && px >= menu_x() && px < menu_x() + MENU_W && py >= menu_y() && py < menu_y() + menu_h() {
        let row = ((py - menu_y() - MENU_PAD) / MENU_ITEM_H) as usize;
        if row < G_ICONS.len() {
            close_start_menu();
            icon_open(row);
        }
        return;
    }
    if unsafe { G_START_MENU } {
        close_start_menu();
    }
    let wi = win_topmost_at(px, py);
    if wi < 0 {
        let ic = icon_at(px, py);
        if ic >= 0 {
            crate::kprintln!("[desktop] open '{}'", core::str::from_utf8(G_ICONS[ic as usize].label).unwrap_or("?"));
            icon_open(ic as usize);
        }
        return;
    }
    let wi = wi as usize;
    let btn = unsafe { win_button_at(&G_WINS[wi], px, py) };
    if btn == b'c' {
        crate::kprintln!("[desktop] close '{}'", unsafe { core::str::from_utf8(G_WINS[wi].title).unwrap_or("?") });
        win_close(wi);
        return;
    }
    if btn == b'x' {
        win_toggle_max(wi);
        return;
    }
    if btn == b'm' {
        win_minimize(wi);
        return;
    }
    if !unsafe { G_WINS[wi].maximized } && px >= unsafe { G_WINS[wi].x + G_WINS[wi].w - RESIZE_GUTTER } && px < unsafe { G_WINS[wi].x + G_WINS[wi].w } && py >= unsafe { G_WINS[wi].y + G_WINS[wi].h - RESIZE_GUTTER } && py < unsafe { G_WINS[wi].y + G_WINS[wi].h } {
        win_raise(wi);
        unsafe {
            G_RESIZE = wi as i64;
            G_RESIZE_X = px;
            G_RESIZE_Y = py;
            G_RESIZE_W = G_WINS[wi].w;
            G_RESIZE_H = G_WINS[wi].h;
        }
        return;
    }
    if py < unsafe { G_WINS[wi].y } + TITLE_H {
        win_raise(wi);
        unsafe {
            G_DRAG = wi as i64;
            G_DRAG_OFF_X = px - G_WINS[wi].x;
            G_DRAG_OFF_Y = py - G_WINS[wi].y;
        }
        crate::kprintln!("[desktop] drag '{}'", unsafe { core::str::from_utf8(G_WINS[wi].title).unwrap_or("?") });
    } else {
        win_raise(wi);
    }
}

/* ---- public API ---- */

pub fn desktop_init(fb: &crate::graphics::framebuffer::Framebuffer, hhdm_offset: u64) {
    if fb.width == 0 || fb.height == 0 {
        crate::kprintln!("[desktop] FATAL: zero-size framebuffer");
        loop {
            unsafe { core::arch::asm!("cli; hlt", options(nomem, nostack, preserves_flags)); }
        }
    }
    if !bb::init(fb.width as i64, fb.height as i64) {
        crate::kprintln!("[desktop] FATAL: no heap for backbuffer");
        loop {
            unsafe { core::arch::asm!("cli; hlt", options(nomem, nostack, preserves_flags)); }
        }
    }

    wallpaper::init();

    gfx_selftest();

    unsafe {
        G_CURSOR_X = bb::screen_w() / 2 - CURSOR_W / 2;
        G_CURSOR_Y = bb::screen_h() / 2 - CURSOR_H / 2;
        G_CURSOR_BB_X = G_CURSOR_X;
        G_CURSOR_BB_Y = G_CURSOR_Y;
        G_DRAG = -1;
        G_RESIZE = -1;
        G_DAMAGE_PRESENT = false;
        G_DAMAGE_PREV_PRESENT = false;
        G_LAST_SECOND = timer::ticks() / 100;
    }

    let about_lines: [&[u8]; 6] = [
        b"Welcome to Sol OS!",
        b"Desktop: mouse via VirtIO.",
        b"Drag the title bar to move windows.",
        b"Bottom-right corner resizes windows.",
        b"Start menu: icons and the terminal.",
        b"Terminal: 'help' lists commands.",
    ];
    let lines: [&[u8]; 6] = [about_lines[0], about_lines[1], about_lines[2], about_lines[3], about_lines[4], about_lines[5]];
    win_open(b"About Sol OS", 120, 90, 460, 240, KIND_INFO, &lines);

    scene_region(0, 0, bb::screen_w(), bb::screen_h());

    unsafe {
        G_CURSOR_BB_X = G_CURSOR_X;
        G_CURSOR_BB_Y = G_CURSOR_Y;
    }
    cursor_draw_bb();

    bb::blit_full(fb);

    crate::kprintln!(
        "[desktop] {}x{} desktop up, window(s), heap free {} KiB",
        fb.width,
        fb.height,
        crate::memory::kheap::free_bytes() / 1024
    );
}

/* ---- presenting ---- */

fn present_single(fb: &crate::graphics::framebuffer::Framebuffer) {
    unsafe {
        if !G_DAMAGE_PRESENT {
            return;
        }
        bb::blit_rect(G_DAMAGE_X, G_DAMAGE_Y, G_DAMAGE_W, G_DAMAGE_H, fb);
        G_DAMAGE_PRESENT = false;
    }
}

fn present_flip(fb: &crate::graphics::framebuffer::Framebuffer) {
    /* Single-buffer path only for now; page-flip requires VBE double
     * buffer support which is not yet exposed in the Rust framebuffer
     * module. */
    present_single(fb);
}

pub fn desktop_poll(fb: &crate::graphics::framebuffer::Framebuffer) {
    unsafe {
        while let Some(ev) = crate::input::pop() {
            match ev {
                crate::input::Event::Key { code: _, key: _, pressed: _, ch: Some(c) } => {
                    let top = win_topmost_index();
                    if top < 0 {
                        continue;
                    }
                    let w = &mut G_WINS[top as usize];
                    if !w.used {
                        continue;
                    }
                    if w.kind == KIND_TERM {
                        term_feed(w, c as u8);
                        redraw_rect(w.x, w.y, w.w, w.h);
                    } else if w.kind == KIND_BROWSER {
                        br_feed(w, c as u8);
                        redraw_rect(w.x, w.y, w.w, w.h);
                    }
                }
                _ => {}
            }
        }
    }

    crate::netstack::poll();

    unsafe {
        let mut dx: i32 = 0;
        let mut dy: i32 = 0;
        let mut btns: u8 = 0;
        /* Mouse input comes through the centralized input pipeline.
         * For now, we accumulate relative motion from mouse events. */
        while let Some(ev) = crate::input::pop() {
            match ev {
                crate::input::Event::MouseMove { dx: ddx, dy: ddy } => {
                    dx += ddx;
                    dy += ddy;
                }
                crate::input::Event::MouseButton { button, pressed } => {
                    let bit = match button {
                        crate::input::MouseButton::Left => 0x01,
                        crate::input::MouseButton::Right => 0x02,
                        crate::input::MouseButton::Middle => 0x04,
                        _ => 0,
                    };
                    if pressed {
                        btns |= bit;
                    } else {
                        btns &= !bit;
                    }
                }
                _ => {}
            }
        }

        G_CURSOR_X = (G_CURSOR_X + dx as i64).clamp(0, bb::screen_w() - CURSOR_W);
        G_CURSOR_Y = (G_CURSOR_Y + dy as i64).clamp(0, bb::screen_h() - CURSOR_H);
        let pressed = btns & !G_BUTTONS_PREV;
        let released = !btns & G_BUTTONS_PREV;
        G_BUTTONS_PREV = btns;

        if G_DRAG >= 0 && G_WINS[G_DRAG as usize].used && (dx != 0 || dy != 0) {
            let w = &mut G_WINS[G_DRAG as usize];
            let (ox, oy, ow, oh) = (w.x, w.y, w.w, w.h);
            if w.maximized {
                w.x = w.rest_x;
                w.y = w.rest_y;
                w.w = w.rest_w;
                w.h = w.rest_h;
                w.maximized = false;
            }
            let nx = (G_CURSOR_X - G_DRAG_OFF_X).clamp(0, bb::screen_w() - w.w);
            let ny = (G_CURSOR_Y - G_DRAG_OFF_Y).clamp(0, work_h() - w.h);
            w.x = nx;
            w.y = ny;
            redraw_union(ox, oy, ow, oh, nx, ny, w.w, w.h);
        }

        if G_RESIZE >= 0 && G_WINS[G_RESIZE as usize].used {
            let w = &mut G_WINS[G_RESIZE as usize];
            if w.maximized {
                G_RESIZE = -1;
            } else {
                let (ox, oy, ow, oh) = (w.x, w.y, w.w, w.h);
                let nw = (G_RESIZE_W + (G_CURSOR_X - G_RESIZE_X)).max(MIN_WIN_W);
                let nh = (G_RESIZE_H + (G_CURSOR_Y - G_RESIZE_Y)).max(MIN_WIN_H);
                if w.x + nw <= bb::screen_w() && w.y + nh <= work_h() {
                    w.w = nw;
                    w.h = nh;
                    redraw_union(ox, oy, ow, oh, w.x, w.y, nw, nh);
                }
            }
        }

        if pressed & 0x01 != 0 {
            handle_left_press(G_CURSOR_X, G_CURSOR_Y);
        }
        if pressed & 0x02 != 0 {
            let wi = win_topmost_at(G_CURSOR_X, G_CURSOR_Y);
            if wi >= 0 {
                crate::kprintln!("[desktop] right click on '{}'", unsafe { core::str::from_utf8(G_WINS[wi as usize].title).unwrap_or("?") });
                win_raise(wi as usize);
            }
        }
        if released & 0x01 != 0 {
            G_DRAG = -1;
            G_RESIZE = -1;
        }

        let sec = timer::ticks() / 100;
        if sec != G_LAST_SECOND {
            G_LAST_SECOND = sec;
            render_clock();
            let top = win_topmost_index();
            if top >= 0 && unsafe { G_WINS[top as usize].used } && unsafe { G_WINS[top as usize].kind } == KIND_TERM {
                redraw_rect(unsafe { G_WINS[top as usize].x }, unsafe { G_WINS[top as usize].y }, unsafe { G_WINS[top as usize].w }, unsafe { G_WINS[top as usize].h });
            }
            for i in 0..MAX_WINDOWS {
                if unsafe { G_WINS[i].used && !G_WINS[i].minimized && G_WINS[i].kind == KIND_BROWSER } {
                    redraw_rect(G_WINS[i].x, G_WINS[i].y, G_WINS[i].w, G_WINS[i].h);
                }
            }
            if !desktop_gfx_integrity() {
                crate::kprintln!("[gfx] INTEGRITY FAILURE at {} s", sec);
            }
        }

        present(fb);
    }
}

/* ---- sys_reboot helper ---- */

fn sys_reboot() {
    unsafe {
        core::arch::asm!("cli", options(nomem, nostack, preserves_flags));
        for i in 0..0x10000 {
            core::arch::asm!("hlt", options(nomem, nostack, preserves_flags));
        }
    }
}

/* ---- button state ---- */

static mut G_BUTTONS_PREV: u8 = 0;

/* ---- present ---- */

fn present(fb: &crate::graphics::framebuffer::Framebuffer) {
    unsafe {
        let moved = G_CURSOR_BB_X != G_CURSOR_X || G_CURSOR_BB_Y != G_CURSOR_Y;
        if moved {
            redraw_rect(G_CURSOR_BB_X, G_CURSOR_BB_Y, CURSOR_W, CURSOR_H);
        }
        if moved || damage_touches(G_CURSOR_X, G_CURSOR_Y, CURSOR_W, CURSOR_H) {
            cursor_draw_bb();
            damage_add(G_CURSOR_X, G_CURSOR_Y, CURSOR_W, CURSOR_H);
        }
        G_CURSOR_BB_X = G_CURSOR_X;
        G_CURSOR_BB_Y = G_CURSOR_Y;
        present_flip(fb);
    }
}

