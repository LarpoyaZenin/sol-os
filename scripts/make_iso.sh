#!/usr/bin/env bash
# Assembles build/sol-os.iso from the compiled kernel, boot/limine.conf,
# and a local Limine checkout. Run via `ninja iso` (wired up in
# CMakeLists.txt), not directly, so paths line up.
#
# Expects a sibling `limine/` directory (see scripts/fetch_limine.sh)
# containing the prebuilt BIOS/UEFI stage files from the pinned
# release in LIMINE_VERSION.md.

set -euo pipefail

BUILD_DIR="$1"
SRC_DIR="$2"
LIMINE_DIR="${SRC_DIR}/limine"

if [ ! -d "$LIMINE_DIR" ]; then
    echo "error: ${LIMINE_DIR} not found." >&2
    echo "Run scripts/fetch_limine.sh first (see LIMINE_VERSION.md for the pinned tag)." >&2
    exit 1
fi

ISO_ROOT="${BUILD_DIR}/iso_root"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot" "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"

cp "${BUILD_DIR}/kernel.elf" "$ISO_ROOT/boot/kernel.elf"
cp "${SRC_DIR}/boot/limine.conf" "$ISO_ROOT/boot/limine/limine.conf"

# BIOS boot stage files
cp "$LIMINE_DIR/limine-bios.sys" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-bios-cd.bin" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_ROOT/boot/limine/"

# UEFI boot stage files
cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true
cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true

xorriso -as mkisofs \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "${BUILD_DIR}/sol-os.iso"

# limine-deploy embeds the BIOS stage 1 in the ISO's boot sector so
# BIOS machines that don't read El Torito boot catalogs correctly
# can still boot it.
"$LIMINE_DIR/limine" bios-install "${BUILD_DIR}/sol-os.iso"

echo "Built ${BUILD_DIR}/sol-os.iso"
