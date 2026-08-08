#ifndef SOL_TIMER_H
#define SOL_TIMER_H

#include <stdint.h>

/* Programs PIT channel 0 to fire IRQ0 at approximately `hz` times
 * per second, registers its handler, and unmasks IRQ0. Call after
 * idt_init() and pic_remap(). */
void timer_init(uint32_t hz);

/* Number of timer ticks since timer_init() was called. Useful later
 * for the scheduler's time-slicing (Phase 4) — exposed now so
 * kmain.c can prove the timer is actually firing. */
uint64_t timer_get_ticks(void);

#endif /* SOL_TIMER_H */
