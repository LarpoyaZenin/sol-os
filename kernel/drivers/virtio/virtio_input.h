#ifndef SOL_VIRTIO_INPUT_H
#define SOL_VIRTIO_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* VirtIO input driver (keyboard + mouse) over the modern PCI
 * transport. Discovers every 1AF4:1052 (VIRTIO_ID_INPUT) function,
 * negotiates VIRTIO_INPUT_F_EVENTS, and drains the event virtqueue.
 * Events arrive as fixed 8-byte virtio_input_event packets (le16
 * type, le16 code, le32 value); every used descriptor is recycled
 * back to the queue immediately so the device never has to drop a
 * group for lack of buffers. */

/* Probes the PCI bus for VirtIO input devices and brings them up.
 * Must be called after interrupts are enabled (it registers IRQ
 * handlers) and with the HHDM offset from the bootloader. Returns the
 * number of devices initialized. */
uint32_t virtio_input_init(uint64_t hhdm);

/* Drains any pending events from all devices. Safe to call from the
 * main loop; interrupts are disabled internally while draining. */
void virtio_input_poll(void);

uint32_t virtio_input_device_count(void);

/* Pointer-device (mouse) state accumulated across all VirtIO input
 * devices since the last call. Mirrors arch/x86_64/mouse.h's
 * mouse_get_delta(): drains the deltas to zero and reports the
 * currently-held buttons. Button bitmask (uint8_t): bit0 = left,
 * bit1 = right, bit2 = middle, bit3 = side, bit4 = extra. Returns
 * true if there was pending motion. */
bool virtio_mouse_get_delta(int32_t *dx, int32_t *dy, uint8_t *buttons);

/* Pops the oldest buffered character typed on any VirtIO keyboard,
 * or returns false if the buffer is empty. Non-blocking by design so
 * the desktop main loop can poll it without stalling. Key events are
 * queued on the key-down edge (autorepeat is ignored). Printable
 * characters are passed through (0x20..0x7E); the navigation keys
 * map to sentinel control values the terminal understands:
 *   0x01 page up   0x02 page down
 *   0x03 cursor left   0x04 cursor right
 *   0x05 up   0x06 down
 * while enter is 0x0A and backspace is 0x08. */
bool virtio_keyboard_read_char(char *out);

/* Aggregate event counters (across all devices) for diagnostics. */
uint64_t virtio_input_irq_count(void);
uint64_t virtio_input_event_count(void);
uint64_t virtio_input_key_count(void);
uint64_t virtio_input_rel_count(void);
uint64_t virtio_input_dropped_count(void);

#endif /* SOL_VIRTIO_INPUT_H */
