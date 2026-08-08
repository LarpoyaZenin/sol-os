#ifndef SOL_KLOG_H
#define SOL_KLOG_H

/* Initializes the serial (COM1) port used for early boot logging.
 * Serial is used instead of the framebuffer for the very first
 * log lines because it works before framebuffer.c has anywhere
 * to draw to, and it's readable from the QEMU host via -serial
 * stdio without needing a display at all. */
void klog_init(void);

/* Minimal printf-like logger. Supports %s %d %u %x %c %% only —
 * intentionally not a full libc printf (see sol-libc design notes
 * in the project docs: stdio starts as "printf-like kernel
 * logging", not a general-purpose formatter). */
void klog(const char *fmt, ...);

#endif /* SOL_KLOG_H */
