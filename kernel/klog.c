#include "klog.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void klog_init(void) {
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x03);    /* divisor lo byte: 38400 baud */
    outb(COM1 + 1, 0x00);    /* divisor hi byte */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);    /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_tx_empty()) { }
    outb(COM1, (uint8_t)c);
}

static void klog_puts(const char *s) {
    if (s == NULL) s = "(null)";
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void klog_putuint(unsigned long v, unsigned base, int upper) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

static void klog_putint(long v) {
    if (v < 0) {
        serial_putc('-');
        klog_putuint((unsigned long)(-v), 10, 0);
    } else {
        klog_putuint((unsigned long)v, 10, 0);
    }
}

/* Minimal printf-like logger. Supports %s %d %u %x %X %ld %lu %lx
 * %p %c %% only — intentionally not a full libc printf (see sol-libc
 * design notes: stdio starts as "printf-like kernel logging", not a
 * general-purpose formatter). The l-variants and %p take 64-bit
 * arguments and print full-width. */
void klog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') serial_putc('\r');
            serial_putc(*p);
            continue;
        }

        p++;
        switch (*p) {
            case 's': klog_puts(va_arg(args, const char *)); break;
            case 'd': klog_putint(va_arg(args, int)); break;
            case 'u': klog_putuint(va_arg(args, unsigned int), 10, 0); break;
            case 'x': klog_putuint(va_arg(args, unsigned int), 16, 0); break;
            case 'X': klog_putuint(va_arg(args, unsigned int), 16, 1); break;
            case 'l':
                p++;
                switch (*p) {
                    case 'd': klog_putint(va_arg(args, long)); break;
                    case 'u': klog_putuint((unsigned long)va_arg(args, unsigned long), 10, 0); break;
                    case 'x': klog_putuint((unsigned long)va_arg(args, unsigned long), 16, 0); break;
                    case 'X': klog_putuint((unsigned long)va_arg(args, unsigned long), 16, 1); break;
                    default:
                        serial_putc('%');
                        serial_putc('l');
                        serial_putc(*p);
                }
                break;
            case 'p': klog_putuint((unsigned long)(uintptr_t)va_arg(args, void *), 16, 0); break;
            case 'c': serial_putc((char)va_arg(args, int)); break;
            case '%': serial_putc('%'); break;
            default:
                serial_putc('%');
                serial_putc(*p);
        }
    }

    va_end(args);
}
