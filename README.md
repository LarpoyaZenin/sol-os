# Sol OS

A from-scratch x86_64 kernel booted via the Limine protocol, developed
in QEMU. Current milestone: **foundation + memory + interrupts +
input drivers + desktop UI**, with VirtIO keyboard/mouse as the
primary input path.

## Status

Boots under QEMU and is exercised end-to-end by scripted harnesses:
`scripts/verify_virtio.sh` (input path) and
`scripts/verify_desktop.sh` (UI path). The desktop harness drives the
VirtIO mouse over QMP — dragging windows by their title bars,
minimizing, restoring from the taskbar, maximizing/un-maximizing,
closing, and launching apps from desktop icons — capturing QMP
screendumps after each stage and asserting on the pixels. Verified
features:

- Limine higher-half boot, framebuffer (VGA/EDID), serial logging
- GDT, IDT, PIC remap (IRQs 32-47), PIT timer at 100 Hz
- Physical memory manager (bitmap) + kernel heap, both with selftests
- PCI bus enumeration
- PS/2 keyboard + mouse (IRQ1/IRQ12)
- VirtIO input (virtio-keyboard-pci / virtio-mouse-pci), shared-IRQ
  safe, with event counters and drop tracking
- CMOS RTC reader for the taskbar clock/date
- Desktop UI: gradient background, mouse cursor, taskbar with Start
  button and live clock, desktop icons, and a window system
  (z-order focus, drag-to-move title bars, minimize / maximize /
  close buttons, taskbar window buttons)
- Heartbeat diagnostics every 5 s on the serial log

## Layout

```
sol-os/
├── CMakeLists.txt              # top-level build (uses the toolchain file)
├── boot/limine.conf            # Limine 8.x config
├── include/limine.h            # boot protocol header
├── kernel/
│   ├── kmain.c                 # init sequence + idle loop
│   ├── klog.c / .h             # COM1 logger (IRQ-safe, %d/%u/%x/%p/%lu...)
│   ├── framebuffer.c / .h      # pixel/rect drawing
│   ├── desktop.c / .h          # desktop UI (cursor, taskbar, windows, icons)
│   ├── mm/pmm.c / kheap.c      # physical memory manager + kernel heap
│   ├── arch/x86_64/            # GDT, IDT, PIC, PIT, RTC, entry, linker script
│   ├── drivers/
│   │   ├── pci.c               # bus scan + device table
│   │   ├── ps2/                # keyboard + mouse (IRQ 1/12)
│   │   └── virtio/virtio_input.c  # VirtIO input transport + EV_* parsing
├── libc/                       # minimal string/mem ops
├── toolchain/x86_64-elf.cmake  # cross-compilation toolchain file
└── scripts/
    ├── fetch_limine.sh         # clones + builds the pinned Limine release
    ├── make_iso.sh             # assembles the bootable ISO (`ninja iso`)
    ├── qmp_input.py            # manual QMP input injection
    ├── verify_virtio.sh        # end-to-end VirtIO input verification
    ├── verify_desktop.sh       # end-to-end desktop/window-system verification
    └── screenshot.sh           # headless boot + QMP framebuffer screendump
```

## Building

Prerequisites: `x86_64-elf-gcc` (e.g. `brew install x86_64-elf-gcc`),
`nasm`, `cmake`, `ninja`, `xorriso`, `qemu-system-x86_64`, `git`.

```bash
./scripts/fetch_limine.sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake
ninja -C build
ninja -C build iso
```

## Running

```bash
qemu-system-x86_64 -cdrom build/sol-os.iso -m 256M \
  -serial file:/tmp/sol.log \
  -vga none -device VGA,edid=on,xres=1920,yres=1080 \
  -device virtio-keyboard-pci -device virtio-mouse-pci
```

Use `-serial stdio` to watch kernel logs on the terminal.

## Verifying

```bash
bash scripts/verify_virtio.sh    # input path: keys, buttons, motion
bash scripts/verify_desktop.sh   # UI path: windows, taskbar, icons
```

The VirtIO harness boots QEMU headless with
`-qmp unix:/tmp/sol-verify.sock`, waits for
`VirtIO input initialized`, injects four keys (`a`, `w`, Enter,
Space), all five mouse buttons (left, right, middle, side, extra —
press + release), and six relative-motion deltas, then checks the
serial log for each event, heartbeat continuity, zero exceptions, and
zero dropped packets. Exit code 0 = all gates passed.

The desktop harness boots a 1920x1080 headless guest, waits for the
desktop, then walks a scripted pointer session over QMP
(`screendump` + pixel asserts after each stage): it drags the About
window by its title bar, minimizes and restores it via the taskbar
button, maximizes and un-maximizes it, closes it, and clicks each
desktop icon to verify the Terminal window opens. It also greps the
serial log for the corresponding `[desktop]` action lines.

Manual injection over QMP:

```bash
# attach the QMP socket to a running QEMU, then:
python3 scripts/qmp_input.py "key a down" "key a up" "rel x 5" "rel y -3"
python3 scripts/qmp_input.py "btn side down" "btn side up" "btn extra down" "btn extra up"
```

Notes on the QMP `input-send-event` API:

- Buttons are a plain enum string, **without** a `btn-` prefix:
  `left`, `right`, `middle`, `side`, `extra`, `wheel-up`, `wheel-down`.
  (`qmp_input.py` also accepts `btn-side` as shorthand.)
- Keys use `{"type": "qcode", "data": "<qcode>"}`.
- QEMU's QMP server serves one client at a time; keep a single
  persistent connection for a sequence of events.

## Known limitations

- No scheduler / preemptive multitasking yet; the idle loop is a
  polling loop that sleeps (`hlt`) between driver polls, and the
  desktop UI is driven directly by the idle loop rather than a
  dedicated process.
- The window system is single-framebuffer: windows are not
  composited/damage-tracked; they repaint from front to back on
  changes, and each window is a static line array (no live content
  yet).
- No keyboard input path into the UI yet (no text boxes / focus
  typing).
- Serial is the only console; there is no terminal emulator app.
- PCI device support is limited to the devices needed so far
  (VirtIO input, PS/2); no mass-storage/network drivers.
