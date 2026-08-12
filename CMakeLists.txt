cmake_minimum_required(VERSION 3.20)

# Must be invoked with:
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake
# See README.md for full instructions.

project(sol-os C ASM_NASM)

if (NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "Sol OS must be cross-compiled. Configure with:\n"
        "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake")
endif()

enable_language(ASM_NASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- Freestanding compiler flags ----
set(SOL_C_FLAGS
    -ffreestanding
    -fno-stack-protector
    -fno-stack-check
    -fno-lto
    -fno-pic
    -fno-pie
    -mcmodel=kernel
    -m64
    -march=x86-64
    -mno-80387
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-red-zone
    -Wall
    -Wextra
    -std=gnu17
    -O2
    -g
)

set(SOL_LINK_FLAGS
    -nostdlib
    -static
    -no-pie
    -Wl,-T,${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/linker.ld
    -Wl,-z,max-page-size=0x1000
)

add_executable(kernel.elf
    kernel/kmain.c
    kernel/klog.c
    kernel/framebuffer.c
    kernel/font.c
    kernel/desktop.c
    kernel/png.c
    kernel/wallpaper.c
    kernel/limine_requests.c
    kernel/mm/pmm.c
    kernel/mm/kheap.c
    kernel/arch/x86_64/entry.asm
    kernel/arch/x86_64/gdt.c
    kernel/arch/x86_64/gdt_flush.asm
    kernel/arch/x86_64/idt.c
    kernel/arch/x86_64/idt_flush.asm
    kernel/arch/x86_64/interrupts.asm
    kernel/arch/x86_64/pic.c
    kernel/arch/x86_64/timer.c
    kernel/arch/x86_64/rtc.c
    kernel/arch/x86_64/keyboard.c
    kernel/arch/x86_64/mouse.c
    kernel/arch/x86_64/paging.c
    kernel/drivers/pci/pci.c
    kernel/drivers/virtio/virtio.c
    kernel/drivers/virtio/virtio_input.c
    kernel/drivers/virtio/virtio_net.c
    kernel/netstack.c
    kernel/crypto.c
    kernel/tls.c
    kernel/drivers/vga/vbe.c
    libc/string.c
)

target_include_directories(kernel.elf PRIVATE
    include
    kernel
    libc
)

target_compile_options(kernel.elf PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${SOL_C_FLAGS}>
)
target_link_options(kernel.elf PRIVATE ${SOL_LINK_FLAGS})

set_target_properties(kernel.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ---- ISO image target ----
# Requires: xorriso, and a local copy of the Limine binaries (see
# scripts/fetch_limine.sh). Produces build/sol-os.iso bootable in QEMU
# or on real hardware.
add_custom_target(iso
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make_iso.sh
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}
    DEPENDS kernel.elf
    COMMENT "Building sol-os.iso with Limine"
)

add_custom_target(run
    COMMAND qemu-system-x86_64
        -cdrom ${CMAKE_BINARY_DIR}/sol-os.iso
        -m 256M
        -serial stdio
        -vga none
        -device VGA,edid=on,xres=1920,yres=1080
        -nic user,model=virtio-net-pci
        -no-reboot
        -no-shutdown
    DEPENDS iso
    COMMENT "Booting sol-os.iso in QEMU (1920x1080)"
)
