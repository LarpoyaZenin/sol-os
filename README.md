# Sol OS

A from-scratch x86_64 kernel booted via the Limine protocol, developed
in QEMU. The project is currently being migrated from C to Rust.

## Status

Boots under QEMU and is exercised end-to-end by scripted harnesses:
`scripts/verify_virtio.sh` (input path),
`scripts/verify_desktop.sh` (UI path), and
`scripts/verify_browser.sh` (browser path).

Verified features:

- Limine higher-half boot, framebuffer (VGA/EDID), serial logging
- GDT, IDT, PIC remap (IRQs 32-47), PIT timer at 100 Hz
- Physical memory manager (bitmap) + kernel heap, both with selftests
- PCI bus enumeration
- PS/2 keyboard + mouse (IRQ1/IRQ12)
- VirtIO input (virtio-keyboard-pci / virtio-mouse-pci), shared-IRQ
  safe, with event counters and drop tracking
- VirtIO networking (virtio-net-pci) with ARP, IPv4, UDP, DNS, TCP,
  and HTTP/1.0 client
- CMOS RTC reader for the taskbar clock/date (Indian Standard Time,
  UTC+05:30)
- Desktop UI: gradient background, mouse cursor, taskbar with Start
  button and live clock, desktop icons, and a window system
  (z-order focus, drag-to-move title bars, minimize / maximize /
  close buttons, taskbar window buttons)
- Terminal window with command input (help, clear, about, echo, time,
  date, mem, reboot)
- Browser window with address bar, keyboard input, DNS resolution,
  TCP connection, HTTP fetch, and basic HTML/text rendering
- Heartbeat diagnostics every 5 s on the serial log

## Rust kernel

The kernel is being rewritten in Rust (`no_std`, `x86_64-unknown-none`).
The C implementation remains as a reference until the Rust replacement
is fully verified.

Build the Rust kernel:

```bash
cd rust
cargo build --release
```

Build the Rust ISO:

```bash
bash scripts/build-rust.sh
```

## Running

```bash
qemu-system-x86_64 -cdrom build/sol-os-rust.iso -m 256M \
  -serial stdio \
  -vga none \
  -device VGA,edid=on,xres=1920,yres=1080 \
  -device virtio-keyboard-pci \
  -device virtio-mouse-pci \
  -device virtio-net-pci
```

## Verifying

```bash
bash scripts/verify_virtio.sh     # input path: keys, buttons, motion
bash scripts/verify_desktop.sh    # UI path: windows, taskbar, icons
bash scripts/verify_browser.sh    # browser: window, nav bar, search results
```

## Roadmap

- [x] Rust kernel boot (Limine, GDT/IDT/PIC/timer, PCI, PS/2, VirtIO input)
- [x] Rust desktop (backbuffer, wallpaper, cursor, taskbar, windows, terminal)
- [x] Rust VirtIO networking driver
- [x] Rust netstack (ARP, IPv4, UDP, DNS, TCP, HTTP/1.0)
- [x] Browser window with address bar and keyboard input
- [x] Basic HTML/text rendering
- [ ] HTTPS/TLS support
- [ ] JavaScript support
- [ ] CSS support
- [ ] Multi-window tabbed browsing
- [ ] Bookmark/history persistence
- [ ] Rust scheduler and preemptive multitasking
- [ ] Remove C implementation after Rust is fully verified
