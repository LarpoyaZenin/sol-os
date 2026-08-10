#!/usr/bin/env bash
# Boot the Rust Sol OS ISO in QEMU headlessly with serial on stdio.
# Same QEMU configuration as the C build (see the `run` target in
# CMakeLists.txt), pointed at build/sol-os-rust.iso.
#
# Usage: scripts/run-rust.sh
# Requires: scripts/build-rust.sh has been run.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${ROOT_DIR}/build/sol-os-rust.iso"

if [ ! -f "$ISO" ]; then
    echo "error: ${ISO} not found." >&2
    echo "Run scripts/build-rust.sh first." >&2
    exit 1
fi

exec qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 256M \
    -serial stdio \
    -vga none \
    -device VGA,edid=on,xres=1920,yres=1080 \
    -nic user,model=virtio-net-pci \
    -no-reboot \
    -no-shutdown
