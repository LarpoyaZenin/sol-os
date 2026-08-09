#include "rtc.h"
#include <stdint.h>

/* Minimal CMOS RTC (MC146818) reader for the taskbar clock/date.
 * Ports: 0x70 = register address (with bit 7 set to keep NMI
 * disabled while reading), 0x71 = register data. */

#define RTC_ADDR  0x70u
#define RTC_DATA  0x71u

#define RTC_SEC   0x00u
#define RTC_MIN   0x02u
#define RTC_HOUR  0x04u
#define RTC_DAY   0x07u
#define RTC_MONTH 0x08u
#define RTC_YEAR  0x09u
#define RTC_STAT_A 0x0Au
#define RTC_STAT_B 0x0Bu

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Returns true once the RTC is not in the middle of updating its
 * registers (status A bit 7 = update-in-progress). */
static int rtc_update_done(void) {
    for (int i = 0; i < 100000; i++) {
        outb(RTC_ADDR, RTC_STAT_A | 0x80u);
        if (!(inb(RTC_DATA) & 0x80u)) return 1;
    }
    return 0;
}

static uint8_t rtc_read_reg(uint8_t reg) {
    outb(RTC_ADDR, reg | 0x80u);
    return inb(RTC_DATA);
}

static int bcd_to_bin(uint8_t v) {
    return (int)(((v >> 4) & 0x0Fu) * 10 + (v & 0x0Fu));
}

bool rtc_read(struct rtc_datetime *dt) {
    if (!rtc_update_done()) return false;

    uint8_t sec   = rtc_read_reg(RTC_SEC);
    uint8_t min   = rtc_read_reg(RTC_MIN);
    uint8_t hour  = rtc_read_reg(RTC_HOUR);
    uint8_t day   = rtc_read_reg(RTC_DAY);
    uint8_t month = rtc_read_reg(RTC_MONTH);
    uint8_t year  = rtc_read_reg(RTC_YEAR);
    uint8_t b     = rtc_read_reg(RTC_STAT_B);

    int binary   = (b & 0x04u) != 0;
    int hour12   = (b & 0x02u) == 0;   /* clear = 12-hour mode */

    int s = binary ? (int)sec   : bcd_to_bin(sec);
    int m = binary ? (int)min   : bcd_to_bin(min);
    int h = binary ? (int)hour  : bcd_to_bin(hour & 0x7Fu);
    int d = binary ? (int)day   : bcd_to_bin(day);
    int mo = binary ? (int)month : bcd_to_bin(month);
    int y = binary ? (int)year  : bcd_to_bin(year);

    if (hour12) {
        int pm = (hour & 0x80u) != 0;
        if (pm && h != 12) h += 12;
        if (!pm && h == 12) h = 0;
    }

    dt->year   = 2000 + y;
    dt->month  = mo;
    dt->day    = d;
    dt->hour   = h;
    dt->minute = m;
    dt->second = s;
    return true;
}

/* IST is UTC + 5:30. */
#define IST_OFFSET_MIN 330

static int days_in_month(int month, int year) {
    static const int dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return 29;
    return dim[month - 1];
}

/* Same as rtc_read() but converts the CMOS clock (UTC) to Indian
 * Standard Time, rolling the calendar date over midnight. */
bool rtc_read_ist(struct rtc_datetime *dt) {
    if (!rtc_read(dt)) return false;

    int total = dt->hour * 60 + dt->minute + IST_OFFSET_MIN;
    dt->hour = (total / 60) % 24;
    dt->minute = total % 60;
    if (total >= 24 * 60) {
        dt->day++;
        if (dt->day > days_in_month(dt->month, dt->year)) {
            dt->day = 1;
            dt->month++;
            if (dt->month > 12) {
                dt->month = 1;
                dt->year++;
            }
        }
    }
    return true;
}
