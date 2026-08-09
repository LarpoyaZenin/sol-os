#!/bin/bash
# Verify VirtIO input end-to-end for Sol OS.
#
# Boots QEMU (shell-backgrounded, the reliable pattern), injects
# keyboard/button/relative-motion events over one persistent QMP
# connection, waits for heartbeats, then parses the serial log and
# checks: all expected events received, zero exceptions, zero dropped
# virtio packets, heartbeats still running.
#
# Usage: scripts/verify_virtio.sh
# Exit code 0 = all gates passed, 1 = failure.

set -u
cd "$(dirname "$0")/.."

ISO=build/sol-os.iso
LOG=/tmp/sol-verify.log
SOCK=/tmp/sol-verify.sock

pkill -f qemu-system-x86_64 2>/dev/null
sleep 1
rm -f "$LOG" "$SOCK"

qemu-system-x86_64 -cdrom "$ISO" -m 256M \
  -serial "file:$LOG" \
  -vga none -device VGA,edid=on,xres=1920,yres=1080 \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -device virtio-net-pci,netdev=n0 -netdev user,id=n0 \
  -qmp "unix:$SOCK,server,nowait" \
  -no-reboot -display none &
QPID=$!
echo "[verify] QEMU pid $QPID"

for _ in $(seq 1 40); do
  grep -q "VirtIO input initialized" "$LOG" 2>/dev/null && break
  kill -0 $QPID 2>/dev/null || { echo "[verify] QEMU died during boot"; exit 1; }
  sleep 0.5
done
if ! grep -q "VirtIO input initialized" "$LOG" 2>/dev/null; then
  echo "[verify] FAIL: kernel never reached VirtIO init"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi
echo "[verify] kernel booted, VirtIO input ready"

python3 - "$SOCK" <<'EOF'
import json, socket, sys, time

SOCK = sys.argv[1]
KBD = ["a", "w", "ret", "spc"]
BTN = ["left", "right", "middle", "side", "extra"]
REL = [(5, -3), (2, 1), (-1, 2)]

def read_json(sock):
    buf = b""
    while True:
        buf += sock.recv(65536)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                return json.loads(line)
            except ValueError:
                continue

def read_response(sock):
    while True:
        obj = read_json(sock)
        if "return" in obj or "error" in obj:
            return obj

def connect():
    for _ in range(100):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK)
            s.settimeout(15)
            read_json(s)  # greeting
            break
        except OSError:
            time.sleep(0.1)
    else:
        raise SystemExit("could not connect to QMP socket")
    s.sendall(b'{"execute": "qmp_capabilities"}\n')
    read_response(s)
    return s

def event(sock, ev):
    sock.sendall(json.dumps({"execute": "input-send-event",
                             "arguments": {"events": [ev]}}).encode() + b"\n")
    resp = read_response(sock)
    if "error" in resp:
        print("[verify] QMP ERROR:", json.dumps(resp["error"]))

s = connect()
for code in KBD:
    for down in (True, False):
        event(s, {"type": "key", "data": {"down": down,
              "key": {"type": "qcode", "data": code}}})
        time.sleep(0.05)
for b in BTN:
    for down in (True, False):
        event(s, {"type": "btn", "data": {"down": down, "button": b}})
        time.sleep(0.05)
for x, y in REL:
    event(s, {"type": "rel", "data": {"axis": "x", "value": x}})
    event(s, {"type": "rel", "data": {"axis": "y", "value": y}})
    time.sleep(0.05)
s.close()
print("[verify] events injected")
EOF
if [ $? -ne 0 ]; then
  echo "[verify] FAIL: QMP injection error"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi

sleep 12
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null

python3 - "$LOG" <<'EOF'
import re, sys

content = open(sys.argv[1]).read()
key_codes = set(int(c) for c in re.findall(r"\[vinput0\] key code=(\d+)", content))
btn_lines = re.findall(r"\[vinput1\] button (\w+) (down|up)=(\d)", content)
rel_lines = re.findall(r"\[vinput1\] rel ([xy])=(-?\d+)", content)
heartbeats = re.findall(
    r"\[heartbeat\] ticks=(\d+).*?virtio_events=(\d+) \(\+(\d+)\)\s*"
    r"virtio_key=(\d+) virtio_rel=(\d+) virtio_dropped=(\d+)", content)
exceptions = re.findall(r"PANIC|exception|unhandled", content)

print("---- summary ----")
print("key_codes:", sorted(key_codes))
print("btn_events:", btn_lines)
print("rel_events:", rel_lines)
print("heartbeats:", heartbeats)
print("exceptions:", exceptions)

ok_keys = {30, 17, 28, 57} <= key_codes
ok_btns = {b for b, _, _ in btn_lines} == {"left", "right", "middle", "side", "extra"}
ok_rel = len(rel_lines) == 6
ok_hb = len(heartbeats) >= 2
ok_noexc = not exceptions
ok_dropped = all(int(h[5]) == 0 for h in heartbeats)

print("keys_ok=%s btns_ok=%s rel_ok=%s hb_ok=%s noexc=%s dropped0=%s" % (
    ok_keys, ok_btns, ok_rel, ok_hb, ok_noexc, ok_dropped))
sys.exit(0 if all([ok_keys, ok_btns, ok_rel, ok_hb, ok_noexc, ok_dropped]) else 1)
EOF
rc=$?
echo "[verify] exit $rc"
exit $rc
