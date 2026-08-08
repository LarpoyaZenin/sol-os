/* Sol OS kernel linker script.
 *
 * Links the kernel to the higher half (-2GiB), the conventional
 * Limine kernel load address, so kernel and user address spaces
 * don't collide once paging for user processes exists.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* R+X */
    rodata  PT_LOAD    FLAGS((1 << 2))             ; /* R   */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2))  ; /* R+W */
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . += CONSTANT(MAXPAGESIZE);

    .rodata : {
        *(.limine_requests_start)
        *(.limine_requests)
        *(.limine_requests_end)
        *(.rodata .rodata.*)
    } :rodata

    . += CONSTANT(MAXPAGESIZE);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
