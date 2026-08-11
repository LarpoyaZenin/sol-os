#include "limine_requests.h"
#include <stddef.h>

/* Tell Limine we speak base revision 3 of the boot protocol. */
LIMINE_BASE_REVISION(3);

/* Markers so the linker script can bracket the requests section;
 * Limine scans this region of the ELF for request structs. */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] = { LIMINE_COMMON_MAGIC, 0, 0 };

__attribute__((used, section(".limine_requests")))
struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] = { LIMINE_COMMON_MAGIC };
