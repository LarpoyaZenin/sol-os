#include "gdt.h"
#include <stdint.h>

/* Packed GDT entry — 8 bytes, standard x86 layout. In long mode
 * most of the base/limit fields are ignored by the CPU (segmentation
 * is effectively flat), but they must still be encoded correctly
 * for the descriptor to be considered valid. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GDT_ENTRIES 3

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access      = access;
}

/* Defined in gdt_flush.asm: loads gdtp via lgdt, then far-jumps to
 * reload CS with the new code selector and reloads the data segment
 * registers with the new data selector. */
extern void gdt_flush(uint64_t gdtp_addr);

void gdt_init(void) {
    gdtp.limit = (uint16_t)(sizeof(struct gdt_entry) * GDT_ENTRIES - 1);
    gdtp.base  = (uint64_t)&gdt;

    /* Null descriptor — required by the architecture. */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: base 0, limit ignored in long mode (using
     * 0xFFFFF here per convention), access 0x9A = present, ring 0,
     * code segment, executable, readable. granularity 0xAF sets
     * the long-mode (L) bit plus 4KiB granularity. */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xAF);

    /* Kernel data: access 0x92 = present, ring 0, data segment,
     * writable. granularity 0xCF = 4KiB granularity, 32-bit (data
     * segments don't use the L bit). */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

    gdt_flush((uint64_t)&gdtp);
}
