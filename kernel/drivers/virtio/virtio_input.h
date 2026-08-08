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

/* Aggregate event counters (across all devices) for diagnostics. */
uint64_t virtio_input_irq_count(void);
uint64_t virtio_input_event_count(void);
uint64_t virtio_input_key_count(void);
uint64_t virtio_input_rel_count(void);
uint64_t virtio_input_dropped_count(void);

#endif /* SOL_VIRTIO_INPUT_H */
