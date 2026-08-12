# Sol OS

A from-scratch x86_64 kernel booted via the Limine protocol, developed
in QEMU. Written in C (freestanding, no libc).

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
  HTTP/1.0 client, and a partial TLS 1.2 client
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
- Writer/Notepad with New, Open, Save, Save As, and close-window
  support
- File manager window with file listing, file opening in Writer,
  rename-on-save, and delete-with-confirmation
- Heartbeat diagnostics every 5 s on the serial log

## Building

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake
cd build
ninja
```

This produces `build/sol-os.iso`. The ISO build automatically bundles
`assets/wallpaper.png` as a Limine module so the desktop can decode
and display it.

## Running

```bash
qemu-system-x86_64 -cdrom build/sol-os.iso -m 256M \
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

- [x] C kernel boot (Limine, GDT/IDT/PIC/timer, PCI, PS/2, VirtIO input)
- [x] C desktop (backbuffer, wallpaper, cursor, taskbar, windows, terminal)
- [x] VirtIO networking driver
- [x] Netstack (ARP, IPv4, UDP, DNS, TCP, HTTP/1.0)
- [x] Browser window with address bar and keyboard input
- [x] Basic HTML/text rendering
- [x] File manager with delete-with-confirmation
- [ ] Full HTTPS/TLS 1.2 support (handshake implemented, server
      acceptance not yet verified)
- [ ] Certificate validation (design in progress)
- [ ] JavaScript support
- [ ] CSS support
- [ ] Multi-window tabbed browsing
- [ ] Bookmark/history persistence
- [ ] Preemptive multitasking
- [ ] Doom integration (optional)

## HTTPS / TLS Status

### How HTTPS works in Sol OS

HTTPS in Sol OS follows the standard layered model:

```
Browser -> HTTPS client -> TLS 1.2 -> TCP -> IPv4 -> VirtIO-net
```

The netstack (`kernel/netstack.c`) already understands both `http://`
and `https://` URLs. For HTTPS it opens a TCP connection to port 443,
runs a TLS 1.2 handshake, and then sends the HTTP request inside the
encrypted TLS record layer.

### Current TLS implementation

- A custom TLS 1.2 client lives in `kernel/tls.c`.
- It sits above `kernel/netstack.c`'s single TCP client connection and
  below the browser.
- The handshake state machine covers: ClientHello, ServerHello,
  Certificate, ServerKeyExchange, ServerHelloDone, ClientKeyExchange,
  ChangeCipherSpec, and Finished.
- Key agreement is X25519; record encryption is AES-128-GCM.
- The TLS layer feeds decrypted application data back into the HTTP
  response path so the browser can render the body.

### What is verified

- TCP SYN-ACK is received after connecting to port 443.
- The ClientHello is built and serialized correctly (TLS 1.2, SNI,
  supported groups, signature algorithms, EC point formats,
  extended_master_secret, renegotiation_info).
- The server acknowledges the ClientHello (ACK with updated sequence
  numbers), confirming the TCP path is intact.

### Known limitation

Despite the handshake being well-formed, the public HTTPS test servers
used during development close the connection immediately after the
ClientHello without returning a ServerHello or TLS alert. This has
been observed consistently across multiple servers and test runs.

At this time Sol OS **does not successfully complete a TLS 1.2
handshake with a real public server**, so HTTPS fetches do not
complete. The browser still accepts `https://` URLs and will attempt
the TLS path, but the request will time out and report an HTTP error.

### Certificate validation

Certificate validation is designed as a distinct step in `tls.c` but
**is not currently enforced**. The existing code does not ship a trusted
CA certificate store, so validation cannot be performed safely. Do not
treat Sol OS HTTPS as providing identity assurance or confidentiality
until a CA store and verifier are added.

## File manager delete

The file manager (`kernel/desktop.c`, window kind 4) supports deleting
the selected file:

- Select a file with the mouse.
- Click the **Del** button in the title bar, or press `Backspace` /
  `Delete` while the file manager window is focused.
- A confirmation dialog appears. Click **Yes** or press `Y` to delete,
  **No** or press `N` / `Esc` to cancel.
- After a successful delete the file listing refreshes immediately.
- Deleting the last file leaves the manager in an empty state.
- The built-in filesystem (`g_fs[]`) supports deletion by marking the
  slot unused. No real persistent storage is involved.

## Writer / Notepad

The Writer (Notepad) window supports:

- **New** — clears the current document. If the document has unsaved
  changes it is saved automatically first.
- **Open** — cycles through existing files in the built-in filesystem.
- **Save** — writes the current text to the existing filename. If the
  document has never been saved, it defaults to `untitled.txt`.
- **Save As** — lets you choose a new filename before saving.
- **Close** — click the `X` button in the title bar to close the
  window. The document text remains in the filesystem; reopening the
  file from the file manager restores it.

### Save As flow

1. Click the **As** button in the Writer toolbar.
2. The toolbar switches to an inline filename editor showing
   `Save As: <current name>`.
3. Type the new filename. Only letters, digits, `_`, `-`, and `.` are
   accepted.
4. Press **Enter** to confirm. If a file with that name already exists
   it is overwritten after the new content is written.
5. Press **Esc** to cancel and return to normal editing.

After a successful Save As:

- The Writer window title updates to the new filename.
- The file appears immediately in the file manager.
- Opening the file from the file manager loads the saved text.

### Filename handling

- The built-in filesystem uses a flat array of named blobs (`g_fs[]`
  in `kernel/desktop.c`). There are no directories.
- Filenames are case-sensitive and limited to `NOTEPAD_MAX_FNAME`
  (64) bytes.
- Invalid characters in a Save As name are rejected; the editor stays
  open until a valid name is entered or Esc is pressed.

## File manager

- The file manager lists every file in the built-in filesystem.
- Double-click a file to open it in Writer.
- Click **Del** to delete the selected file. A confirmation dialog
  requires explicit **Yes** / **No** (or `Y` / `N` / `Esc`).
- Files created or renamed by Writer appear immediately.
- Deleted files disappear immediately.
- Closing and reopening Writer preserves the saved filename and
  content because the data lives in the global filesystem, not the
  window.

## Window closing

Every normal application window (Browser, Writer, Terminal, File
Manager, info windows) can be closed by clicking the **X** button in
its title bar. Closing a window:

- Removes it from the active window list.
- Aborts any in-flight network request.
- Clears drag/resize state.
- Repaints the area the window occupied.
- Removes the taskbar entry.

Closing one window does not affect any other window.

## Building

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/x86_64-elf.cmake
cd build
ninja
```

This produces `build/sol-os.iso`.

## Running

```bash
qemu-system-x86_64 -cdrom build/sol-os.iso -m 256M \
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
