#!/usr/bin/env bash
# Build the Rust Sol OS kernel and produce a bootable ISO, independent
# of the C build.
#
#   1. cargo builds the Rust kernel for the custom x86_64-sol target.
#   2. The result is copied to build/kernel-rust.elf.
#   3. build/sol-os-rust.iso is assembled with the same Limine stages
#      and limine.conf the C build uses (see scripts/make_iso.sh).
#
# Usage: scripts/build-rust.sh
# Requires: cargo/rustc (stable, rust-src component), and a local
# `limine/` checkout (see scripts/fetch_limine.sh).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUST_DIR="${ROOT_DIR}/rust"
BUILD_DIR="${ROOT_DIR}/build"
LIMINE_DIR="${ROOT_DIR}/limine"

if [ ! -d "$LIMINE_DIR" ]; then
    echo "error: ${LIMINE_DIR} not found." >&2
    echo "Run scripts/fetch_limine.sh first (see LIMINE_VERSION.md for the pinned tag)." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"

# 1. Build the Rust kernel against the builtin x86_64-unknown-none
#    triple (stable toolchain; spec identical to x86_64-sol.json).
#    Run from inside rust/ so cargo picks up rust/.cargo/config.toml.
echo "[build-rust] cargo build (release)"
(
    cd "$RUST_DIR"
    cargo build --release
)

# 2. Copy the kernel into build/.
KERNEL="$(find "${RUST_DIR}/target" -maxdepth 3 -type f -name 'sol-os-rust' -path '*/release/*' | head -1)"
if [ -z "$KERNEL" ]; then
    echo "error: cargo produced no kernel binary under ${RUST_DIR}/target" >&2
    exit 1
fi
cp "$KERNEL" "${BUILD_DIR}/kernel-rust.elf"
echo "[build-rust] kernel -> ${BUILD_DIR}/kernel-rust.elf"

# 3. Assemble the Rust ISO (mirror of scripts/make_iso.sh).
ISO_ROOT="${BUILD_DIR}/iso_root_rust"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"

cp "${BUILD_DIR}/kernel-rust.elf" "$ISO_ROOT/boot/kernel.elf"
cp "${ROOT_DIR}/boot/limine.conf" "$ISO_ROOT/boot/limine/limine.conf"
cp "${ROOT_DIR}/assets/wallpaper.png" "$ISO_ROOT/boot/wallpaper.png"

cp "$LIMINE_DIR/limine-bios.sys" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-bios-cd.bin" "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_ROOT/boot/limine/"

cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true
cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true

xorriso -as mkisofs \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "${BUILD_DIR}/sol-os-rust.iso"

"$LIMINE_DIR/limine" bios-install "${BUILD_DIR}/sol-os-rust.iso"

echo "Built ${BUILD_DIR}/sol-os-rust.iso"
