#ifndef SOL_VIRTIO_H
#define SOL_VIRTIO_H

#include <stdint.h>
#include "drivers/pci/pci.h"

/* Virtio "modern" (1.0) transport over PCI.
 *
 * The device exposes four regions — common config, notify, ISR, and
 * device config — each located through a PCI vendor-specific
 * capability (VIRTIO_PCI_CAP_VNDR). The common config drives feature
 * negotiation and queue setup; notifications are a single store to
 * the notify region; the ISR region is read to acknowledge
 * interrupts. Split virtqueues are used (descriptor table, available
 * ring, used ring), each region page-aligned in physical memory. */

/* PCI vendor-specific capability used to locate the virtio regions. */
#define VIRTIO_PCI_CAP_VNDR       0x09u

#define VIRTIO_PCI_CAP_COMMON_CFG 1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define VIRTIO_PCI_CAP_ISR_CFG    3u
#define VIRTIO_PCI_CAP_DEVICE_CFG 4u

/* Device status bits (common cfg, offset 0x14). */
#define VIRTIO_STATUS_ACKNOWLEDGE          0x01u
#define VIRTIO_STATUS_DRIVER               0x02u
#define VIRTIO_STATUS_DRIVER_OK            0x04u
#define VIRTIO_STATUS_FEATURES_OK          0x08u
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET   0x40u
#define VIRTIO_STATUS_FAILED               0x80u

/* Generic feature bit that MUST be negotiated on the modern transport. */
#define VIRTIO_F_VERSION_1 32u

/* Split virtqueue descriptor flags. */
#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

/* Split virtqueue structures (virtio spec section 2.6). The rings are
 * guest memory mapped through the HHDM; the device addresses them by
 * physical address. */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

struct virtio_device {
    uintptr_t hhdm;
    uint8_t bus, dev, func;

    /* Mapped regions (higher-half virtual addresses). */
    volatile uint8_t *common;      /* common config MMIO */
    volatile uint8_t *notify;      /* notify MMIO base */
    volatile uint8_t *isr;         /* ISR status byte */
    volatile uint8_t *device_cfg;  /* device config MMIO */
    uint32_t notify_off_multiplier;

    uint32_t device_features_lo, device_features_hi;
    uint32_t driver_features_lo, driver_features_hi;

    struct {
        uint16_t queue_size;
        uint16_t free_head;    /* head of unused-descriptor pool */
        uint16_t free_count;
        uint16_t avail_idx;    /* free-running available-ring index */
        uint16_t used_idx;     /* next used element to consume */

        struct virtq_desc  *desc;
        struct virtq_avail *avail;
        struct virtq_used  *used;

        uintptr_t desc_phys, avail_phys, used_phys;
        uintptr_t notify_addr;
    } vq;
};

/* Locates the four capability regions on the given PCI function,
 * resets the device, acknowledges + DRIVER status, reads device
 * features, and records VIRTIO_F_VERSION_1 in the driver features.
 * The driver then negotiates its own feature bits, calls
 * virtio_device_finish_features, sets up its queue(s), and finally
 * virtio_device_set_driver_ok. Returns 0 on success. */
int virtio_device_init(struct virtio_device *d, const pci_device_t *p, uint64_t hhdm);

/* Negotiates feature bit `bit` (driver-side) with the device. */
void virtio_device_set_feature(struct virtio_device *d, uint32_t bit);

/* Writes FEATURES_OK and verifies the device accepted it. 0 on ok. */
int virtio_device_finish_features(struct virtio_device *d);

void virtio_device_set_driver_ok(struct virtio_device *d);

/* Marks the device failed after a fatal negotiation/init error. */
void virtio_device_failed(struct virtio_device *d);

/* Device config access (VirtIO input driver reads select/subsel
 * probes through this region). */
static inline uint8_t virtio_device_cfg_read8(struct virtio_device *d, uint32_t off) {
    return *(volatile uint8_t *)(d->device_cfg + off);
}
static inline void virtio_device_cfg_write8(struct virtio_device *d, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(d->device_cfg + off) = val;
}

/* Queue size the device advertises for `queue_index`. */
uint16_t virtio_queue_size(struct virtio_device *d, uint16_t queue_index);

/* Allocates and programs the (single) virtqueue for the device and
 * enables it. queue_size is clamped to what the device offered.
 * Returns 0 on success. */
int virtio_queue_init(struct virtio_device *d, uint16_t queue_size);

/* Publishes one descriptor to the available ring. `phys` is the
 * guest-physical address of the buffer; `device_writes` marks it
 * write-only for the device (input event buffers). Returns 0 on
 * success, -1 if the descriptor pool is empty. */
int virtio_queue_add_buffer(struct virtio_device *d, uintptr_t phys, uint32_t len, int device_writes);

/* Re-publishes a descriptor that was returned on the used ring,
 * recycling it without touching its address/len/flags. */
void virtio_queue_recycle(struct virtio_device *d, uint16_t desc_id);

/* Returns 1 and fills *desc_id and *len with the next used element,
 * or 0 if none is available yet. */
int virtio_queue_pop_used(struct virtio_device *d, uint16_t *desc_id, uint32_t *len);

/* Notifies the device that the available ring has advanced. */
void virtio_queue_kick(struct virtio_device *d);

#endif /* SOL_VIRTIO_H */
