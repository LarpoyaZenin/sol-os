#ifndef SOL_RTC_H
#define SOL_RTC_H

#include <stdbool.h>

struct rtc_datetime {
    int year;    /* four-digit year */
    int month;   /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int minute;  /* 0-59 */
    int second;  /* 0-59 */
};

/* Reads the current wall-clock time/date from the CMOS RTC (ports
 * 0x70/0x71), waiting out any update-in-progress and normalizing BCD
 * and 12-hour format. Returns false only if no RTC is present. */
bool rtc_read(struct rtc_datetime *dt);

/* Same as rtc_read() but with the clock converted to Indian Standard
 * Time (UTC + 5:30), including day/month/year rollover. */
bool rtc_read_ist(struct rtc_datetime *dt);

#endif /* SOL_RTC_H */
