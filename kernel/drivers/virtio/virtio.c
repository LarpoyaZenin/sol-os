#include "virtio.h"
#include "klog.h"
#include "mm/pmm.h"
#include "arch/x86_64/paging.h"
#include <stddef.h>

/* Common config register offsets (virtio spec 4.1.4.3.2). */
#define CC_DEVICE_FEATURE_SELECT 0x00u
#define CC_DEVICE_FEATURE        0x04u
#define CC_DRIVER_FEATURE_SELECT 0x08u
#define CC_DRIVER_FEATURE        0x0Cu
#define CC_NUM_QUEUES            0x12u
#define CC_DEVICE_STATUS         0x14u
#define CC_QUEUE_SELECT          0x16u
#define CC_QUEUE_SIZE            0x18u
#define CC_QUEUE_ENABLE          0x1Cu
#define CC_QUEUE_NOTIFY_OFF      0x1Eu
#define CC_QUEUE_DESC            0x20u
#define CC_QUEUE_DRIVER          0x28u
#define CC_QUEUE_DEVICE          0x30u

/* Sanity bound on the capability chain and region sizes. */
#define VIRTIO_COMMON_CFG_MIN    0x24u   /* through queue_device (0x30+4) */
#define VIRTIO_NOTIFY_CAP_MIN    0x14u   /* virtio_pci_cap + multiplier */
#define VIRTIO_DEVICE_CFG_MIN    0x08u   /* select + subsel + size + reserved */

static inline uint32_t mmio_read32(const volatile uint8_t *base, uint32_t off) {
    return *(const volatile uint32_t *)(base + off);
}
static inline uint16_t mmio_read16(const volatile uint8_t *base, uint32_t off) {
    return *(const volatile uint16_t *)(base + off);
}
static inline uint8_t mmio_read8(const volatile uint8_t *base, uint32_t off) {
    return *(const volatile uint8_t *)(base + off);
}
static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
static inline void mmio_write16(volatile uint8_t *base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(base + off) = val;
}
static inline void mmio_write8(volatile uint8_t *base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(base + off) = val;
}

/* Physical address of PCI BAR `bar` of the device, or -1 if it's not
 * a supported memory BAR. */
static int virtio_bar_base(const pci_device_t *p, uint8_t bar, uintptr_t *out) {
    if (bar >= 6) return -1;
    uint32_t lo = p->bars[bar];
    if (lo & 1u) return -1;              /* I/O space BAR — unsupported */

    uint64_t addr;
    if (lo & 0x4u) {                     /* 64-bit BAR */
        if (bar + 1 >= 6) return -1;
        addr = ((uint64_t)p->bars[bar + 1] << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
    } else {
        addr = (uint64_t)(lo & 0xFFFFFFF0u);
    }
    *out = (uintptr_t)addr;
    return 0;
}

int virtio_device_init(struct virtio_device *d, const pci_device_t *p, uint64_t hhdm) {
    d->hhdm = hhdm;
    d->bus = p->bus;
    d->dev = p->dev;
    d->func = p->func;
    d->notify_off_multiplier = 1;

    /* Walk the capability list, mapping each virtio region. */
    for (uint8_t off = pci_cap_first(p->bus, p->dev, p->func); off != 0;
         off = pci_cap_next(p->bus, p->dev, p->func, off)) {
        uint8_t id = pci_config_read8(p->bus, p->dev, p->func, off);
        if (id != VIRTIO_PCI_CAP_VNDR) continue;

        uint8_t  cfg_type = pci_config_read8(p->bus, p->dev, p->func, (uint8_t)(off + 3));
        uint8_t  bar      = pci_config_read8(p->bus, p->dev, p->func, (uint8_t)(off + 4));
        uint32_t cap_off  = pci_config_read32(p->bus, p->dev, p->func, (uint8_t)(off + 8));
        uint32_t cap_len  = pci_config_read32(p->bus, p->dev, p->func, (uint8_t)(off + 12));

        uintptr_t bar_base;
        if (virtio_bar_base(p, bar, &bar_base) != 0) {
            klog("virtio: unsupported BAR %u on %x:%x.%u\n", bar, p->bus, p->dev, p->func);
            continue;
        }

        /* Limine's HHDM only maps usable RAM — device MMIO must be
         * mapped explicitly before it can be accessed. */
        paging_map_physical(hhdm, bar_base + cap_off, cap_len);

        uintptr_t vaddr = hhdm + bar_base + cap_off;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            if (cap_len < VIRTIO_COMMON_CFG_MIN) {
                klog("virtio: common cfg too small (%u bytes)\n", cap_len);
                return -1;
            }
            d->common = (volatile uint8_t *)vaddr;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            if (cap_len < VIRTIO_NOTIFY_CAP_MIN) {
                klog("virtio: notify cap too small (%u bytes)\n", cap_len);
                return -1;
            }
            d->notify = (volatile uint8_t *)vaddr;
            d->notify_off_multiplier = pci_config_read32(p->bus, p->dev, p->func, (uint8_t)(off + 16));
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            if (cap_len < 1) {
                klog("virtio: ISR cap too small\n");
                return -1;
            }
            d->isr = (volatile uint8_t *)vaddr;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            if (cap_len < VIRTIO_DEVICE_CFG_MIN) {
                klog("virtio: device cfg cap too small (%u bytes)\n", cap_len);
                return -1;
            }
            d->device_cfg = (volatile uint8_t *)vaddr;
            break;
        default:
            break;
        }
    }

    if (d->common == NULL || d->notify == NULL || d->isr == NULL) {
        klog("virtio: missing capability regions on %x:%x.%u\n", p->bus, p->dev, p->func);
        return -1;
    }

    klog("virtio: %x:%x.%u caps: common=%p notify=%p (mult %u) isr=%p%s\n",
         p->bus, p->dev, p->func,
         (void *)d->common, (void *)d->notify, d->notify_off_multiplier,
         (void *)d->isr,
         d->device_cfg != NULL ? "" : " (no device cfg)");

    /* Reset, then acknowledge + DRIVER per spec. */
    mmio_write8(d->common, CC_DEVICE_STATUS, 0);
    (void)mmio_read8(d->common, CC_DEVICE_STATUS);   /* read back after reset */
    mmio_write8(d->common, CC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(d->common, CC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Read device features (both 32-bit words). */
    mmio_write32(d->common, CC_DEVICE_FEATURE_SELECT, 0);
    d->device_features_lo = mmio_read32(d->common, CC_DEVICE_FEATURE);
    mmio_write32(d->common, CC_DEVICE_FEATURE_SELECT, 1);
    d->device_features_hi = mmio_read32(d->common, CC_DEVICE_FEATURE);
    mmio_write32(d->common, CC_DEVICE_FEATURE_SELECT, 0);

    if (!(d->device_features_hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
        klog("virtio: device does not offer VIRTIO_F_VERSION_1 (features %x %x)\n",
             d->device_features_lo, d->device_features_hi);
        virtio_device_failed(d);
        return -1;
    }

    /* Advertise VIRTIO_F_VERSION_1 (mandatory on the modern transport). */
    d->driver_features_lo = 0;
    d->driver_features_hi = 1u << (VIRTIO_F_VERSION_1 - 32);
    mmio_write32(d->common, CC_DRIVER_FEATURE_SELECT, 0);
    mmio_write32(d->common, CC_DRIVER_FEATURE, d->driver_features_lo);
    mmio_write32(d->common, CC_DRIVER_FEATURE_SELECT, 1);
    mmio_write32(d->common, CC_DRIVER_FEATURE, d->driver_features_hi);
    mmio_write32(d->common, CC_DRIVER_FEATURE_SELECT, 0);

    return 0;
}

void virtio_device_set_feature(struct virtio_device *d, uint32_t bit) {
    uint32_t word = bit / 32;
    uint32_t mask = 1u << (bit % 32);
    uint32_t value;

    if (word == 0) {
        d->driver_features_lo |= mask;
        value = d->driver_features_lo;
    } else {
        d->driver_features_hi |= mask;
        value = d->driver_features_hi;
    }
    mmio_write32(d->common, CC_DRIVER_FEATURE_SELECT, word);
    mmio_write32(d->common, CC_DRIVER_FEATURE, value);
    mmio_write32(d->common, CC_DRIVER_FEATURE_SELECT, 0);
}

int virtio_device_finish_features(struct virtio_device *d) {
    mmio_write8(d->common, CC_DEVICE_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    uint8_t status = mmio_read8(d->common, CC_DEVICE_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        klog("virtio: FEATURES_OK not accepted (status %x)\n", status);
        virtio_device_failed(d);
        return -1;
    }
    return 0;
}

void virtio_device_set_driver_ok(struct virtio_device *d) {
    uint8_t status = mmio_read8(d->common, CC_DEVICE_STATUS);
    mmio_write8(d->common, CC_DEVICE_STATUS, (uint8_t)(status | VIRTIO_STATUS_DRIVER_OK));
}

void virtio_device_failed(struct virtio_device *d) {
    uint8_t status = mmio_read8(d->common, CC_DEVICE_STATUS);
    mmio_write8(d->common, CC_DEVICE_STATUS, (uint8_t)(status | VIRTIO_STATUS_FAILED));
}

/* ---- virtqueue ---- */

uint16_t virtio_queue_size(struct virtio_device *d, uint16_t queue_index) {
    mmio_write16(d->common, CC_QUEUE_SELECT, queue_index);
    return mmio_read16(d->common, CC_QUEUE_SIZE);
}

/* Each ring region is page-sized for queue_size <= 256, so a single
 * physical page per region suffices and keeps the rings page-aligned. */
#define VIRTIO_MAX_QUEUE_SIZE 256u

static int virtio_queue_init_one(struct virtio_device *d, uint16_t qidx,
                                 uint16_t queue_size) {
    if (qidx >= VIRTIO_MAX_QUEUES) return -1;
    struct virtio_vq *vq = &d->vq[qidx];

    if (queue_size < 2) {
        klog("virtio: queue %u: device offered an unusable size (%u)\n",
             qidx, queue_size);
        return -1;
    }
    if (queue_size > VIRTIO_MAX_QUEUE_SIZE) queue_size = VIRTIO_MAX_QUEUE_SIZE;

    vq->desc_phys  = pmm_alloc_page();
    vq->avail_phys = pmm_alloc_page();
    vq->used_phys  = pmm_alloc_page();
    if (vq->desc_phys == 0 || vq->avail_phys == 0 || vq->used_phys == 0) {
        klog("virtio: queue %u: out of memory for virtqueue\n", qidx);
        if (vq->desc_phys  != 0) pmm_free_page(vq->desc_phys);
        if (vq->avail_phys != 0) pmm_free_page(vq->avail_phys);
        if (vq->used_phys  != 0) pmm_free_page(vq->used_phys);
        vq->desc_phys = vq->avail_phys = vq->used_phys = 0;
        return -1;
    }

    vq->queue_size = queue_size;
    vq->desc  = (struct virtq_desc *)(d->hhdm + vq->desc_phys);
    vq->avail = (struct virtq_avail *)(d->hhdm + vq->avail_phys);
    vq->used  = (struct virtq_used *)(d->hhdm + vq->used_phys);

    /* Link every descriptor into the free pool. */
    for (uint16_t i = 0; i < queue_size; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }
    vq->desc[queue_size - 1].next = 0;
    vq->free_head = 0;
    vq->free_count = queue_size;
    vq->avail_idx = 0;
    vq->used_idx = 0;
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;

    /* Program the device. */
    mmio_write16(d->common, CC_QUEUE_SELECT, qidx);
    mmio_write16(d->common, CC_QUEUE_SIZE, queue_size);
    mmio_write32(d->common, CC_QUEUE_DESC,     (uint32_t)vq->desc_phys);
    mmio_write32(d->common, CC_QUEUE_DESC + 4, (uint32_t)(vq->desc_phys >> 32));
    mmio_write32(d->common, CC_QUEUE_DRIVER,     (uint32_t)vq->avail_phys);
    mmio_write32(d->common, CC_QUEUE_DRIVER + 4, (uint32_t)(vq->avail_phys >> 32));
    mmio_write32(d->common, CC_QUEUE_DEVICE,     (uint32_t)vq->used_phys);
    mmio_write32(d->common, CC_QUEUE_DEVICE + 4, (uint32_t)(vq->used_phys >> 32));

    uint16_t notify_off = mmio_read16(d->common, CC_QUEUE_NOTIFY_OFF);
    vq->notify_addr = (uintptr_t)d->notify + (uintptr_t)notify_off * d->notify_off_multiplier;

    mmio_write16(d->common, CC_QUEUE_ENABLE, 1);

    uint16_t accepted = mmio_read16(d->common, CC_QUEUE_SIZE);
    if (accepted < queue_size) {
        klog("virtio: queue %u: device accepted a smaller size (%u < %u)\n",
             qidx, accepted, queue_size);
        vq->queue_size = accepted;
    }

    klog("virtio: queue %u: %u descs (accepted %u) notify_off=%u\n",
         qidx, queue_size, vq->queue_size, notify_off);
    return 0;
}

int virtio_queue_init(struct virtio_device *d, uint16_t queue_index, uint16_t queue_size) {
    return virtio_queue_init_one(d, queue_index, queue_size);
}

int virtio_queue_add_buffer(struct virtio_device *d, uint16_t queue_index,
                            uintptr_t phys, uint32_t len, int device_writes) {
    uint16_t id;
    return virtio_queue_add_buffer_id(d, queue_index, phys, len, device_writes, &id);
}

int virtio_queue_add_buffer_id(struct virtio_device *d, uint16_t queue_index,
                               uintptr_t phys, uint32_t len, int device_writes,
                               uint16_t *out_id) {
    if (virtio_queue_alloc_desc(d, queue_index, out_id) != 0) return -1;
    virtio_queue_desc_fill(d, queue_index, *out_id, phys, len, device_writes);
    virtio_queue_submit(d, queue_index, *out_id);
    return 0;
}

int virtio_queue_alloc_desc(struct virtio_device *d, uint16_t queue_index, uint16_t *out_id) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return -1;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (vq->free_count == 0) return -1;

    uint16_t id = vq->free_head;
    vq->free_head = vq->desc[id].next;
    vq->free_count--;
    if (out_id != NULL) *out_id = id;
    return 0;
}

void virtio_queue_desc_fill(struct virtio_device *d, uint16_t queue_index, uint16_t desc_id,
                            uintptr_t phys, uint32_t len, int device_writes) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (desc_id >= vq->queue_size) return;

    vq->desc[desc_id].addr  = phys;
    vq->desc[desc_id].len   = len;
    vq->desc[desc_id].flags = device_writes ? VIRTQ_DESC_F_WRITE : 0u;
    vq->desc[desc_id].next  = 0;
}

void virtio_queue_submit(struct virtio_device *d, uint16_t queue_index, uint16_t desc_id) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (desc_id >= vq->queue_size) return;

    vq->avail->ring[vq->avail_idx % vq->queue_size] = desc_id;
    __asm__ volatile ("" ::: "memory");     /* ring entry before idx */
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
}

void virtio_queue_free_desc(struct virtio_device *d, uint16_t queue_index, uint16_t desc_id) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (desc_id >= vq->queue_size) return;

    vq->desc[desc_id].next = vq->free_head;
    vq->free_head = desc_id;
    vq->free_count++;
}

void virtio_queue_recycle(struct virtio_device *d, uint16_t queue_index, uint16_t desc_id) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (desc_id >= vq->queue_size) return;

    vq->avail->ring[vq->avail_idx % vq->queue_size] = desc_id;
    __asm__ volatile ("" ::: "memory");
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
}

int virtio_queue_pop_used(struct virtio_device *d, uint16_t queue_index,
                          uint16_t *desc_id, uint32_t *len) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return 0;
    struct virtio_vq *vq = &d->vq[queue_index];
    if (vq->used_idx == vq->used->idx) return 0;   /* nothing new */
    __asm__ volatile ("" ::: "memory");            /* element visible once idx moves */

    const struct virtq_used_elem *e = &vq->used->ring[vq->used_idx % vq->queue_size];
    *desc_id = e->id;
    *len = e->len;
    vq->used_idx++;
    return 1;
}

void virtio_queue_kick(struct virtio_device *d, uint16_t queue_index) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return;
    struct virtio_vq *vq = &d->vq[queue_index];
    /* Ensure the available-ring update is visible before the MMIO
     * notification. mfence is the safe choice here even though the
     * codebase otherwise avoids SSE-generated code. */
    __asm__ volatile ("mfence" ::: "memory");
    *(volatile uint16_t *)vq->notify_addr = 0;
}
