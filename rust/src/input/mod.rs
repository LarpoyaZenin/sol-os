//! Centralized input subsystem.
//!
//! Every input driver — PS/2 keyboard, PS/2 mouse, VirtIO keyboard,
//! VirtIO mouse — feeds a single logical-event pipeline here. The
//! pipeline carries *physical* key events first and only derives the
//! logical character afterwards, so modifier state (either shift key,
//! caps lock) is tracked once, centrally, instead of being baked into
//! each driver's own character table (the flaw in the old C
//! `keyboard.c`, which converted scancodes to unshifted characters
//! immediately and dropped every break code).
//!
//! Key identities use the Linux input-event key codes, which equal the
//! AT set-1 make codes for the typing area, so PS/2 and VirtIO share
//! one namespace: `submit_key(code, pressed)` accepts either source.
//! Events land in a bounded single-producer/single-consumer ring (the
//! producer is IRQ context, the consumer is the main loop).

use core::sync::atomic::{AtomicU64, Ordering};

/// Ring-buffer capacity for queued input events.
const EVENT_BUF_SIZE: usize = 512;

/* Linux input / AT set-1 make codes used by the typing area and the
 * modifiers we track. Break (release) codes are make | 0x80 on PS/2;
 * VirtIO sends the plain code with value 0 for release. */
pub const KEY_LEFTSHIFT: u16 = 42;
pub const KEY_RIGHTSHIFT: u16 = 54;
pub const KEY_CAPSLOCK: u16 = 58;

/// Modifier-independent physical identity of a key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Key {
    /// A typing key. The char is the *unshifted* identity; the
    /// produced character (after shift/caps) lives in `Event::ch`.
    Char(char),
    ShiftL,
    ShiftR,
    CapsLock,
    Enter,
    Tab,
    Backspace,
    Esc,
    /// Not mapped by this subsystem yet (function keys, navigation,
    /// keypad, ...). Still emitted so nothing is silently dropped.
    Unknown(u16),
}

/// Normalized mouse buttons across both transport types.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
    Middle,
    Side,
    Extra,
    Forward,
    Back,
}

impl MouseButton {
    /// Stable display name, used by the serial debug consumer.
    pub fn name(&self) -> &'static str {
        match self {
            MouseButton::Left => "left",
            MouseButton::Right => "right",
            MouseButton::Middle => "middle",
            MouseButton::Side => "side",
            MouseButton::Extra => "extra",
            MouseButton::Forward => "forward",
            MouseButton::Back => "back",
        }
    }
}

/// One logical input event, in submission order.
#[derive(Clone, Copy, Debug)]
pub enum Event {
    /// `code` is the shared Linux/AT-set-1 key code; `ch` is the
    /// produced character on press (None for non-typing keys and for
    /// every release).
    Key {
        code: u16,
        key: Key,
        pressed: bool,
        ch: Option<char>,
    },
    MouseMove { dx: i32, dy: i32 },
    MouseButton { button: MouseButton, pressed: bool },
    MouseScroll { delta: i32 },
}

/* ---- event ring buffer (SPSC: IRQ producer, main-loop consumer) ---- */

static mut EVENT_BUF: [Option<Event>; EVENT_BUF_SIZE] = [None; EVENT_BUF_SIZE];
static EVENT_HEAD: core::sync::atomic::AtomicUsize = core::sync::atomic::AtomicUsize::new(0);
static EVENT_TAIL: core::sync::atomic::AtomicUsize = core::sync::atomic::AtomicUsize::new(0);

static EVENT_KEYS: AtomicU64 = AtomicU64::new(0);
static EVENT_MOUSE: AtomicU64 = AtomicU64::new(0);
static EVENT_DROPPED: AtomicU64 = AtomicU64::new(0);

fn push(ev: Event) {
    let head = EVENT_HEAD.load(Ordering::Relaxed);
    let tail = EVENT_TAIL.load(Ordering::Acquire);
    let next = (head + 1) % EVENT_BUF_SIZE;
    if next == tail {
        /* Full — drop the event but count it, so tests can prove no
         * silent loss. */
        EVENT_DROPPED.fetch_add(1, Ordering::Relaxed);
        return;
    }
    unsafe { EVENT_BUF[head] = Some(ev) };
    EVENT_HEAD.store(next, Ordering::Release);
}

/// Pops the next queued event, or None. Called from the main loop with
/// interrupts enabled; the read is guarded by cli/sti like the C
/// `*_get_*` getters.
pub fn pop() -> Option<Event> {
    unsafe { crate::interrupts::disable() };
    let ev = unsafe {
        let head = EVENT_HEAD.load(Ordering::Acquire);
        let tail = EVENT_TAIL.load(Ordering::Relaxed);
        if tail == head {
            None
        } else {
            let e = EVENT_BUF[tail].take();
            EVENT_TAIL.store((tail + 1) % EVENT_BUF_SIZE, Ordering::Release);
            e
        }
    };
    unsafe { crate::interrupts::enable() };
    ev
}

/* ---- keyboard state + layout ---- */

/// Physical press state per key code (indexed by code, not event).
static mut HELD: [bool; 256] = [false; 256];
static mut CAPS_LOCK: bool = false;

/// Feeds a key press/release from any driver. `pressed` is the raw
/// physical state (make vs. break); the modifier logic below derives
/// shift/caps and the produced character.
pub fn submit_key(code: u16, pressed: bool) {
    if code >= 256 {
        /* Extended (0xE0) PS/2 codes and anything un-mapped still
         * produce an event, with no modifier impact. */
        push(Event::Key {
            code,
            key: Key::Unknown(code),
            pressed,
            ch: None,
        });
        return;
    }
    unsafe {
        let was_down = HELD[code as usize];
        HELD[code as usize] = pressed;
        if code == KEY_CAPSLOCK && pressed && !was_down {
            CAPS_LOCK = !CAPS_LOCK;
        }
    }
    let shift = unsafe { HELD[KEY_LEFTSHIFT as usize] || HELD[KEY_RIGHTSHIFT as usize] };
    let caps = unsafe { CAPS_LOCK };
    let key = key_from_code(code);
    let ch = if pressed { key_char(key, shift, caps) } else { None };
    EVENT_KEYS.fetch_add(1, Ordering::Relaxed);
    push(Event::Key {
        code,
        key,
        pressed,
        ch,
    });
}

/// True while either shift key is physically held.
pub fn shift_pressed() -> bool {
    unsafe { HELD[KEY_LEFTSHIFT as usize] || HELD[KEY_RIGHTSHIFT as usize] }
}

/// Caps-lock latch state.
pub fn caps_lock() -> bool {
    unsafe { CAPS_LOCK }
}

/// Maps a key code to its physical identity.
fn key_from_code(code: u16) -> Key {
    use Key::*;
    match code {
        1 => Esc,
        2 => Char('1'),
        3 => Char('2'),
        4 => Char('3'),
        5 => Char('4'),
        6 => Char('5'),
        7 => Char('6'),
        8 => Char('7'),
        9 => Char('8'),
        10 => Char('9'),
        11 => Char('0'),
        12 => Char('-'),
        13 => Char('='),
        14 => Backspace,
        15 => Tab,
        16 => Char('q'),
        17 => Char('w'),
        18 => Char('e'),
        19 => Char('r'),
        20 => Char('t'),
        21 => Char('y'),
        22 => Char('u'),
        23 => Char('i'),
        24 => Char('o'),
        25 => Char('p'),
        26 => Char('['),
        27 => Char(']'),
        28 => Enter,
        30 => Char('a'),
        31 => Char('s'),
        32 => Char('d'),
        33 => Char('f'),
        34 => Char('g'),
        35 => Char('h'),
        36 => Char('j'),
        37 => Char('k'),
        38 => Char('l'),
        39 => Char(';'),
        40 => Char('\''),
        41 => Char('`'),
        42 => ShiftL,
        43 => Char('\\'),
        44 => Char('z'),
        45 => Char('x'),
        46 => Char('c'),
        47 => Char('v'),
        48 => Char('b'),
        49 => Char('n'),
        50 => Char('m'),
        51 => Char(','),
        52 => Char('.'),
        53 => Char('/'),
        54 => ShiftR,
        57 => Char(' '),
        58 => CapsLock,
        _ => Unknown(code),
    }
}

/// US layout, shift-only column for non-letter keys.
fn shifted(c: char) -> char {
    match c {
        '1' => '!',
        '2' => '@',
        '3' => '#',
        '4' => '$',
        '5' => '%',
        '6' => '^',
        '7' => '&',
        '8' => '*',
        '9' => '(',
        '0' => ')',
        '-' => '_',
        '=' => '+',
        '[' => '{',
        ']' => '}',
        '\\' => '|',
        ';' => ':',
        '\'' => '"',
        '`' => '~',
        ',' => '<',
        '.' => '>',
        '/' => '?',
        _ => c,
    }
}

/// Applies shift and caps lock to an unshifted key identity.
///
/// Letters follow shift XOR caps (shift+caps cancels to lowercase);
/// digits and punctuation follow shift only.
fn apply_shift(c: char, shift: bool, caps: bool) -> char {
    if c.is_ascii_alphabetic() {
        if shift ^ caps {
            c.to_ascii_uppercase()
        } else {
            c
        }
    } else if shift {
        shifted(c)
    } else {
        c
    }
}

/// Produced character for a pressed key under the current modifier
/// state. Non-typing keys produce None.
fn key_char(key: Key, shift: bool, caps: bool) -> Option<char> {
    match key {
        Key::Char(c) => Some(apply_shift(c, shift, caps)),
        Key::Enter => Some('\n'),
        Key::Tab => Some('\t'),
        Key::Backspace => Some('\u{8}'),
        _ => None,
    }
}

/* ---- mouse ---- */

/// Normalized relative pointer motion from any driver.
pub fn submit_mouse_move(dx: i32, dy: i32) {
    if dx == 0 && dy == 0 {
        return;
    }
    EVENT_MOUSE.fetch_add(1, Ordering::Relaxed);
    push(Event::MouseMove { dx, dy });
}

/// Normalized button press/release (PS/2 bitmask edges and VirtIO
/// BTN_* values both reduce to this).
pub fn submit_mouse_button(button: MouseButton, pressed: bool) {
    push(Event::MouseButton { button, pressed });
}

/// Vertical scroll tick (positive = away from the user).
pub fn submit_mouse_scroll(delta: i32) {
    if delta == 0 {
        return;
    }
    push(Event::MouseScroll { delta });
}

/* ---- counters (for the heartbeat / verify gates) ---- */

/// Total key events ever queued.
pub fn key_events() -> u64 {
    EVENT_KEYS.load(Ordering::Relaxed)
}

/// Total mouse (move) events ever queued.
pub fn mouse_events() -> u64 {
    EVENT_MOUSE.load(Ordering::Relaxed)
}

/// Total events dropped because the ring was full.
pub fn dropped_events() -> u64 {
    EVENT_DROPPED.load(Ordering::Relaxed)
}
