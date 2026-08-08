#ifndef SOL_VBE_H
#define SOL_VBE_H

#include <stdint.h>

/* Bochs/BGA VBE interface via the standard IO ports. Reads and writes
 * are 16-bit to the index/data ports 0x01CE / 0x01CF. QEMU's stdvga,
 * bochs-display and secondary-vga all implement this register set, as
 * does real Bochs/BGA hardware. */

#define VBE_DISPI_IOPORT_INDEX  0x01CEu
#define VBE_DISPI_IOPORT_DATA   0x01CFu

#define VBE_DISPI_INDEX_ID              0x0000u
#define VBE_DISPI_INDEX_XRES            0x0001u
#define VBE_DISPI_INDEX_YRES            0x0002u
#define VBE_DISPI_INDEX_BPP             0x0003u
#define VBE_DISPI_INDEX_ENABLE          0x0004u
#define VBE_DISPI_INDEX_BANK            0x0005u
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x0006u
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x0007u
#define VBE_DISPI_INDEX_X_OFFSET        0x0008u
#define VBE_DISPI_INDEX_Y_OFFSET        0x0009u
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0x000Au

#define VBE_DISPI_DISABLED              0x00u
#define VBE_DISPI_ENABLED               0x01u
#define VBE_DISPI_GETCAPS               0x02u
#define VBE_DISPI_8BIT_DAC              0x20u
#define VBE_DISPI_LFB_ENABLED           0x40u
#define VBE_DISPI_NOCLEARMEM            0x80u

/* VGA input status register #1 (3DAh in color mode): bit 3 is the
 * vertical retrace bit. Polling it lets us flip pages during vblank
 * so the scanout never shows a partially-rendered frame. */
#define VGA_IN_STATUS_1         0x3DAu
#define VGA_IN_STATUS_VR        0x08u

/* Reads/writes the 16-bit register at `index`. */
uint16_t vbe_read(uint16_t index);
void     vbe_write(uint16_t index, uint16_t value);

/* Returns 1 when the Bochs VBE interface answered the ID query. */
int vbe_present(void);

/* Returns 1 while the display is in vertical retrace. */
int vga_vblank_active(void);

#endif /* SOL_VBE_H */
