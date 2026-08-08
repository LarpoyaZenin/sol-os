#!/usr/bin/env bash
# Clones the pinned Limine release (see LIMINE_VERSION.md) into
# ./limine and builds the limine deploy tool. Run once, from the
# project root, on your own machine.

set -euo pipefail

BRANCH="v8.x-binary"   # keep in sync with LIMINE_VERSION.md

git clone https://github.com/limine-bootloader/limine.git \
    --branch="$BRANCH" --depth=1 limine

make -C limine

echo "Limine ${BRANCH} fetched into ./limine"
echo "Record the exact commit in LIMINE_VERSION.md:"
git -C limine rev-parse HEAD
