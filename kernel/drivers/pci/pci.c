#include "pci.h"
#include "klog.h"
#include <stddef.h>

/* PCI config-space access via the standard I/O ports.
 *
 * CONFIG_ADDRESS layout (bit 31 set = enabled):
 *   bits 31:31  enable
 *   bits 23:16  bus number
 *   bits 15:11  device number
 *   bits 10:8   function number
 *   bits 7:2    register offset (dword aligned)
 * Writes go to CONFIG_DATA. */

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

#define PCI_VENDOR_ID_OFFSET   0x00u
#define PCI_HEADER_TYPE_OFFSET 0x0Eu

#define PCI_VENDOR_NONE 0xFFFFu

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static uint32_t pci_make_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func << 8)
         | (offset & 0xFCu);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (uint16_t)(pci_config_read32(bus, dev, func, offset) >> ((offset & 3u) * 8u));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (uint8_t)(pci_config_read32(bus, dev, func, offset) >> ((offset & 3u) * 8u));
}

/* ---- device table ---- */

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static uint32_t     pci_count = 0;

uint32_t pci_device_count(void) {
    return pci_count;
}

const pci_device_t *pci_device_at(uint32_t i) {
    if (i >= pci_count) return NULL;
    return &pci_devices[i];
}

int pci_find_device(uint16_t vendor, uint16_t device_id) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].vendor == vendor &&
            pci_devices[i].device_id == device_id) {
            return (int)i;
        }
    }
    return -1;
}

uint8_t pci_cap_first(uint8_t bus, uint8_t dev, uint8_t func) {
    /* Bit 4 of the status register: capabilities list present. */
    uint16_t status = pci_config_read16(bus, dev, func, 0x06u);
    if (!(status & 0x0010u)) return 0;

    uint8_t ptr = pci_config_read8(bus, dev, func, 0x34u);
    /* Valid capability pointers live in 0x40-0xFF. */
    return (ptr >= 0x40u) ? ptr : 0;
}

uint8_t pci_cap_next(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    if (offset == 0 || offset >= 0xFFu) return 0;
    uint8_t next = pci_config_read8(bus, dev, func, (uint8_t)(offset + 1));
    return (next >= 0x40u) ? next : 0;
}

/* Records one present function, guarding the table bounds. */
static void pci_record(uint8_t bus, uint8_t dev, uint8_t func) {
    if (pci_count >= PCI_MAX_DEVICES) {
        klog("PCI: device table full (%u), skipping bus %u dev %u func %u\n",
             PCI_MAX_DEVICES, bus, dev, func);
        return;
    }

    pci_device_t *p = &pci_devices[pci_count];
    p->bus = bus;
    p->dev = dev;
    p->func = func;
    p->vendor     = pci_config_read16(bus, dev, func, 0x00u);
    p->device_id  = pci_config_read16(bus, dev, func, 0x02u);
    p->revision   = pci_config_read8 (bus, dev, func, 0x08u);
    p->prog_if    = pci_config_read8 (bus, dev, func, 0x09u);
    p->sub_class  = pci_config_read8 (bus, dev, func, 0x0Au);
    p->base_class = pci_config_read8 (bus, dev, func, 0x0Bu);
    p->header_type = (uint8_t)(pci_config_read8(bus, dev, func, PCI_HEADER_TYPE_OFFSET) & 0x7Fu);

    /* Base address registers only exist in header type 0. */
    if (p->header_type == 0x00u) {
        for (int i = 0; i < 6; i++) {
            p->bars[i] = pci_config_read32(bus, dev, func, (uint8_t)(0x10u + i * 4u));
        }
    }

    pci_count++;
}

static const char *pci_class_name(uint8_t base_class) {
    switch (base_class) {
        case 0x00: return "Legacy";
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Comm";
        case 0x08: return "System peripheral";
        case 0x09: return "Input";
        case 0x0C: return "Serial bus";       /* USB / SMBus */
        case 0x0D: return "Wireless";
        case 0x0E: return "Intelligent I/O";
        case 0xFF: return "VGA/unknown";
        default:   return "Other";
    }
}

uint32_t pci_init(void) {
    pci_count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, (uint8_t)dev, 0, PCI_VENDOR_ID_OFFSET);
            if (vendor == PCI_VENDOR_NONE) continue;

            uint8_t header = pci_config_read8((uint8_t)bus, (uint8_t)dev, 0, PCI_HEADER_TYPE_OFFSET);
            uint32_t num_funcs = (header & 0x80u) ? 8u : 1u;

            for (uint32_t func = 0; func < num_funcs; func++) {
                vendor = pci_config_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func, PCI_VENDOR_ID_OFFSET);
                if (vendor == PCI_VENDOR_NONE) continue;
                pci_record((uint8_t)bus, (uint8_t)dev, (uint8_t)func);
            }
        }
    }

    klog("PCI: %u device(s) found\n", pci_count);
    for (uint32_t i = 0; i < pci_count; i++) {
        const pci_device_t *p = &pci_devices[i];
        klog("  [%x:%x.%u] %x:%x class=%x/%x %s\n",
             p->bus, p->dev, p->func, p->vendor, p->device_id,
             p->base_class, p->sub_class,
             pci_class_name(p->base_class));
    }

    return pci_count;
}
