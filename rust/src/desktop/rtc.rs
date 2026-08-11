//! Minimal CMOS RTC (MC146818) reader for the taskbar clock/date.
//!
//! Port of `kernel/arch/x86_64/rtc.c`. Ports: 0x70 = register address
//! (with bit 7 set to keep NMI disabled while reading), 0x71 =
//! register data. `read_ist` converts the UTC clock to Indian Standard
//! Time (UTC + 5:30), rolling the calendar date over midnight.

pub struct RtcDateTime {
    pub year: i32,   /* four-digit year */
    pub month: i32,  /* 1-12 */
    pub day: i32,    /* 1-31 */
    pub hour: i32,   /* 0-23 */
    pub minute: i32, /* 0-59 */
    pub second: i32, /* 0-59 */
}

const RTC_ADDR: u16 = 0x70;
const RTC_DATA: u16 = 0x71;

const RTC_SEC: u8 = 0x00;
const RTC_MIN: u8 = 0x02;
const RTC_HOUR: u8 = 0x04;
const RTC_DAY: u8 = 0x07;
const RTC_MONTH: u8 = 0x08;
const RTC_YEAR: u8 = 0x09;
const RTC_STAT_A: u8 = 0x0A;
const RTC_STAT_B: u8 = 0x0B;

#[inline]
fn inb(port: u16) -> u8 {
    let ret: u8;
    unsafe {
        core::arch::asm!(
            "in al, dx",
            out("al") ret,
            in("dx") port,
            options(nomem, nostack, preserves_flags)
        );
    }
    ret
}

#[inline]
fn outb(port: u16, val: u8) {
    unsafe {
        core::arch::asm!(
            "out dx, al",
            in("dx") port,
            in("al") val,
            options(nomem, nostack, preserves_flags)
        );
    }
}

/// Returns true once the RTC is not in the middle of updating its
/// registers (status A bit 7 = update-in-progress).
fn rtc_update_done() -> bool {
    for _ in 0..100000u32 {
        outb(RTC_ADDR, RTC_STAT_A | 0x80);
        if inb(RTC_DATA) & 0x80 == 0 {
            return true;
        }
    }
    false
}

fn rtc_read_reg(reg: u8) -> u8 {
    outb(RTC_ADDR, reg | 0x80);
    inb(RTC_DATA)
}

fn bcd_to_bin(v: u8) -> i32 {
    (((v >> 4) & 0x0F) * 10 + (v & 0x0F)) as i32
}

/// Reads the current wall-clock time/date from the CMOS RTC, waiting
/// out any update-in-progress and normalizing BCD and 12-hour format.
/// Returns false only if no RTC is present.
fn rtc_read() -> Option<RtcDateTime> {
    if !rtc_update_done() {
        return None;
    }

    let sec = rtc_read_reg(RTC_SEC);
    let min = rtc_read_reg(RTC_MIN);
    let hour = rtc_read_reg(RTC_HOUR);
    let day = rtc_read_reg(RTC_DAY);
    let month = rtc_read_reg(RTC_MONTH);
    let year = rtc_read_reg(RTC_YEAR);
    let b = rtc_read_reg(RTC_STAT_B);

    let binary = b & 0x04 != 0;
    let hour12 = b & 0x02 == 0; /* clear = 12-hour mode */

    let mut s = if binary { sec as i32 } else { bcd_to_bin(sec) };
    let mut m = if binary { min as i32 } else { bcd_to_bin(min) };
    let mut h = if binary { hour as i32 } else { bcd_to_bin(hour & 0x7F) };
    let d = if binary { day as i32 } else { bcd_to_bin(day) };
    let mo = if binary { month as i32 } else { bcd_to_bin(month) };
    let y = if binary { year as i32 } else { bcd_to_bin(year) };

    if hour12 {
        let pm = hour & 0x80 != 0;
        if pm && h != 12 {
            h += 12;
        }
        if !pm && h == 12 {
            h = 0;
        }
    }

    Some(RtcDateTime {
        year: 2000 + y,
        month: mo,
        day: d,
        hour: h,
        minute: m,
        second: s,
    })
}

/* IST is UTC + 5:30. */
const IST_OFFSET_MIN: i32 = 330;

fn days_in_month(month: i32, year: i32) -> i32 {
    const DIM: [i32; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if !(1..=12).contains(&month) {
        return 31;
    }
    if month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        return 29;
    }
    DIM[(month - 1) as usize]
}

/// Same as `rtc_read` but converts the CMOS clock (UTC) to Indian
/// Standard Time, rolling the calendar date over midnight.
pub fn read_ist() -> Option<RtcDateTime> {
    let mut dt = rtc_read()?;

    let total = dt.hour * 60 + dt.minute + IST_OFFSET_MIN;
    dt.hour = (total / 60) % 24;
    dt.minute = total % 60;
    if total >= 24 * 60 {
        dt.day += 1;
        if dt.day > days_in_month(dt.month, dt.year) {
            dt.day = 1;
            dt.month += 1;
            if dt.month > 12 {
                dt.month = 1;
                dt.year += 1;
            }
        }
    }
    Some(dt)
}
