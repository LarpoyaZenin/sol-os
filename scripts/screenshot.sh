#!/bin/bash
# Boot Sol OS headless in QEMU and capture a screenshot of the
# framebuffer via the QMP screendump command. Used to visually verify
# the desktop during development.
#
# Usage: scripts/screenshot.sh [out.png]
# Output: /tmp/sol-screen.png by default.

set -u
cd "$(dirname "$0")/.."

ISO=build/sol-os.iso
OUT=${1:-/tmp/sol-screen.png}
PPM=/tmp/sol-screen.ppm
LOG=/tmp/sol-screen.log
SOCK=/tmp/sol-screen.sock

pkill -f qemu-system-x86_64 2>/dev/null
sleep 1
rm -f "$PPM" "$LOG" "$SOCK"

qemu-system-x86_64 -cdrom "$ISO" -m 256M \
  -serial "file:$LOG" \
  -vga none -device VGA,edid=on,xres=1920,yres=1080 \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -qmp "unix:$SOCK,server,nowait" \
  -no-reboot -display none &
QPID=$!
echo "[shot] QEMU pid $QPID"

for _ in $(seq 1 60); do
  grep -q "\[desktop\].*up" "$LOG" 2>/dev/null && break
  kill -0 $QPID 2>/dev/null || { echo "[shot] QEMU died during boot"; exit 1; }
  sleep 0.5
done
if ! grep -q "\[desktop\].*up" "$LOG" 2>/dev/null; then
  echo "[shot] FAIL: kernel never reached desktop init"
  tail -5 "$LOG"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi
sleep 1
echo "[shot] desktop is up, capturing"

python3 - "$SOCK" "$PPM" <<'EOF'
import json, socket, sys, time

SOCK, PPM = sys.argv[1], sys.argv[2]

def read_line(s):
    buf = b""
    while b"\n" not in buf:
        buf += s.recv(65536)
    line, _ = buf.split(b"\n", 1)
    return line.strip()

def read_response(s):
    while True:
        line = read_line(s)
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if "return" in obj or "error" in obj:
            return obj

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(100):
    try:
        s.connect(SOCK)
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("[shot] could not connect to QMP socket")
s.settimeout(30)
read_line(s)  # greeting
s.sendall(b'{"execute": "qmp_capabilities"}\n')
read_response(s)
s.sendall(json.dumps({"execute": "screendump",
                      "arguments": {"filename": PPM, "format": "ppm"}}).encode() + b"\n")
resp = read_response(s)
s.close()
if "error" in resp:
    print("[shot] QMP ERROR:", json.dumps(resp["error"]))
    raise SystemExit(1)
print("[shot] screendump ok")
EOF
rc=$?
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null

if [ $rc -ne 0 ]; then
  exit $rc
fi

ffmpeg -y -loglevel error -i "$PPM" "$OUT" || exit 1
echo "[shot] wrote $OUT"
