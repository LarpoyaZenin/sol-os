#ifndef SOL_PCI_H
#define SOL_PCI_H

#include <stdint.h>

/* PCI bus enumeration.
 *
 * Config space is accessed through the traditional 0xCF8/0xCFC I/O
 * ports. The scan walks buses 0-255, devices 0-31, and functions 0-7
 * (skipping function 1-7 of single-function devices via the
 * multifunction bit in the header type). Every present device is
 * recorded in a fixed-size, bounds-checked table — no heap is used,
 * so PCI discovery is available even before/independently of the
 * kernel heap. */

#define PCI_MAX_DEVICES 256u

typedef struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  prog_if;
    uint8_t  sub_class;
    uint8_t  base_class;
    uint8_t  header_type;
    uint32_t bars[6];   /* meaningful only for header type 0 */
} pci_device_t;

/* Scans the bus and fills the internal device table. Returns the
 * number of devices found (never more than PCI_MAX_DEVICES). */
uint32_t pci_init(void);

/* Number of devices in the table. */
uint32_t pci_device_count(void);

/* Device at index i, or NULL if i is out of range. */
const pci_device_t *pci_device_at(uint32_t i);

/* Index of the first device matching (vendor, device_id), or -1. */
int pci_find_device(uint16_t vendor, uint16_t device_id);

/* Capability-list walking. Returns the config-space offset of the
 * first capability (0 if the device has no capability list), and the
 * next capability after `offset` (0 if none). Used by drivers that
 * need vendor-specific capabilities (e.g. VirtIO). */
uint8_t pci_cap_first(uint8_t bus, uint8_t dev, uint8_t func);
uint8_t pci_cap_next(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/* Low-level config-space accessors (for drivers). */
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);

#endif /* SOL_PCI_H */
