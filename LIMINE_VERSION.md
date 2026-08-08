# Limine Version

Sol OS is developed against **Limine 8.x**, using the `limine` boot
protocol (base revision 3) and the `limine.conf` config format.

- Repo: https://github.com/limine-bootloader/limine
- Branch to clone: `v8.x-binary` (contains prebuilt `limine` deploy
  tool and BIOS/UEFI stage files — do NOT use `trunk`, which tracks
  unreleased protocol changes and can break this config/header pair
  without warning).

Before upgrading Limine:
1. Diff `limine.h` against the new release's copy in `PROTOCOL.md`.
2. Re-check `boot/limine.conf` syntax against the new release's
   `CONFIG.md` — the config format has changed at least once before
   (`limine.cfg` → `limine.conf`) and may again.
3. Update this file with the new pinned tag.

Record here once you've actually pinned a commit:

- Pinned tag/commit: `aad3edd370955449717a334f0289dee10e2c5f01`
- Date pinned: `2026-08-08`
