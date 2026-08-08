#ifndef SOL_MOUSE_H
#define SOL_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/* Registers the IRQ12 (vector 44) handler, initializes the PS/2
 * auxiliary device on the 8042 controller, and unmasks IRQ12. Call
 * after idt_init() and pic_remap(). */
void mouse_init(void);

/* Retrieves accumulated mouse movement since the last call, plus the
 * current button state. Returns true if any event arrived. `dx`/`dy`
 * are raw PS/2 deltas (9-bit signed; dy positive = down on screen).
 * On return the accumulated deltas are reset. Safe to call from the
 * main loop with interrupts enabled. */
bool mouse_get_delta(int32_t *dx, int32_t *dy, uint8_t *buttons);

/* Number of complete 3-byte packets the mouse has delivered since
 * boot (movement and/or button changes). */
uint64_t mouse_packet_count(void);

/* Number of IRQ12 interrupts the handler has serviced since boot. */
uint64_t mouse_irq_count(void);

#endif /* SOL_MOUSE_H */
