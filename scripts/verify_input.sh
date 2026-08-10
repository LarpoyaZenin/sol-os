#!/bin/bash
# Verify the Milestone 4 centralized input subsystem end-to-end.
#
# Boots the Rust ISO in QEMU (shell-backgrounded), injects keyboard and
# mouse events over one persistent QMP connection, waits for heartbeats,
# then parses the serial log and checks every gate:
#   * PS/2 keyboard + mouse initialized
#   * VirtIO input initialized, zero dropped events
#   * shift / caps-lock modifier behavior for every required example
#     (; -> :, 1 -> !, 2 -> @, , -> <, . -> >, / -> ?, a -> A,
#      caps+a -> A, caps+shift+a -> a, shift release restores layout)
#   * mouse buttons, relative motion, and wheel from VirtIO
#   * heartbeats still running, zero exceptions
#
# Usage: scripts/verify_input.sh
# Exit code 0 = all gates passed, 1 = failure.

set -u
cd "$(dirname "$0")/.."

ISO=build/sol-os-rust.iso
LOG=/tmp/sol-input-verify.log
SOCK=/tmp/sol-input-verify.sock

pkill -f qemu-system-x86_64 2>/dev/null
sleep 1
rm -f "$LOG" "$SOCK"

qemu-system-x86_64 -cdrom "$ISO" -m 256M \
  -serial "file:$LOG" \
  -vga none -device VGA,edid=on,xres=1920,yres=1080 \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -qmp "unix:$SOCK,server,nowait" \
  -no-reboot -display none &
QPID=$!
echo "[verify] QEMU pid $QPID"

for _ in $(seq 1 60); do
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

def key(sock, qcode, down):
    event(sock, {"type": "key", "data": {"down": down,
          "key": {"type": "qcode", "data": qcode}}})
    time.sleep(0.04)

def tap(sock, qcode):
    key(sock, qcode, True)
    key(sock, qcode, False)

def hold(sock, qcode):
    key(sock, qcode, True)

def release(sock, qcode):
    key(sock, qcode, False)

def rel(sock, axis, value):
    event(sock, {"type": "rel", "data": {"axis": axis, "value": value}})
    time.sleep(0.04)

def btn(sock, b, down):
    event(sock, {"type": "btn", "data": {"down": down, "button": b}})
    time.sleep(0.04)

s = connect()

# -- plain (unmodified) keys --
tap(s, "semicolon")     # ; -> ;
tap(s, "1")             # 1 -> 1
tap(s, "2")             # 2 -> 2
tap(s, "comma")         # , -> ,
tap(s, "dot")           # . -> .
tap(s, "slash")         # / -> /
tap(s, "a")             # a -> a

# -- shift-modified keys --
hold(s, "shift")
tap(s, "semicolon")     # shift+; -> :
tap(s, "1")             # shift+1 -> !
tap(s, "2")             # shift+2 -> @
tap(s, "comma")         # shift+, -> <
tap(s, "dot")           # shift+. -> >
tap(s, "slash")         # shift+/ -> ?
tap(s, "a")             # shift+a -> A
release(s, "shift")
tap(s, "a")             # release shift -> a again

# -- caps lock --
tap(s, "caps_lock")     # caps on
tap(s, "a")             # caps+a -> A
hold(s, "shift")
tap(s, "a")             # caps+shift+a -> a
release(s, "shift")
tap(s, "a")             # caps+a -> A
tap(s, "caps_lock")     # caps off
tap(s, "a")             # -> a

# -- mouse: buttons, motion, wheel --
for b in ["left", "right", "middle", "side", "extra"]:
    btn(s, b, True)
    btn(s, b, False)
rel(s, "x", 5); rel(s, "y", -3)
rel(s, "x", 2); rel(s, "y", 1)
rel(s, "x", -1); rel(s, "y", 2)
for b in ["wheel-down", "wheel-down", "wheel-up", "wheel-up"]:
    btn(s, b, True)
    btn(s, b, False)

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

def has(p):
    return re.search(p, content) is not None

gates = []

# -- init gates --
gates.append(("ps2_kbd_init", has(r"PS/2 keyboard initialized")))
gates.append(("ps2_mouse_init", has(r"PS/2 mouse initialized")))
gates.append(("virtio_init", has(r"VirtIO input initialized")))

# -- modifier proofs --
gates.append(("semi_plain", has(r"keydown code=39 key=Char\(';'\) ch=';' shift=false caps=false")))
gates.append(("semi_shift", has(r"keydown code=39 key=Char\(';'\) ch=':' shift=true caps=false")))
gates.append(("1_plain", has(r"keydown code=2 key=Char\('1'\) ch='1' shift=false caps=false")))
gates.append(("1_shift", has(r"keydown code=2 key=Char\('1'\) ch='!' shift=true caps=false")))
gates.append(("2_plain", has(r"keydown code=3 key=Char\('2'\) ch='2' shift=false caps=false")))
gates.append(("2_shift", has(r"keydown code=3 key=Char\('2'\) ch='@' shift=true caps=false")))
gates.append(("comma_plain", has(r"keydown code=51 key=Char\(','\) ch=',' shift=false caps=false")))
gates.append(("comma_shift", has(r"keydown code=51 key=Char\(','\) ch='<' shift=true caps=false")))
gates.append(("dot_plain", has(r"keydown code=52 key=Char\('.'\) ch='.' shift=false caps=false")))
gates.append(("dot_shift", has(r"keydown code=52 key=Char\('.'\) ch='>' shift=true caps=false")))
gates.append(("slash_plain", has(r"keydown code=53 key=Char\('/'\) ch='/' shift=false caps=false")))
gates.append(("slash_shift", has(r"keydown code=53 key=Char\('/'\) ch='\?' shift=true caps=false")))
gates.append(("a_plain", has(r"keydown code=30 key=Char\('a'\) ch='a' shift=false caps=false")))
gates.append(("a_shift", has(r"keydown code=30 key=Char\('a'\) ch='A' shift=true caps=false")))
gates.append(("caps_a", has(r"keydown code=30 key=Char\('a'\) ch='A' shift=false caps=true")))
gates.append(("caps_shift_a", has(r"keydown code=30 key=Char\('a'\) ch='a' shift=true caps=true")))
gates.append(("shift_release", has(r"keyup code=42 key=ShiftL ch=none shift=false caps=false")))

# -- mouse gates --
for b in ["left", "right", "middle", "side", "extra"]:
    gates.append((f"btn_{b}_down", has(rf"button {b} down")))
    gates.append((f"btn_{b}_up", has(rf"button {b} up")))
gates.append(("mouse_x5", has(r"mouse x=5 y=0")))
gates.append(("mouse_yneg3", has(r"mouse x=0 y=-3")))
gates.append(("mouse_x2", has(r"mouse x=2 y=0")))
gates.append(("mouse_yp1", has(r"mouse x=0 y=1")))
gates.append(("mouse_xneg1", has(r"mouse x=-1 y=0")))
gates.append(("mouse_yp2", has(r"mouse x=0 y=2")))
gates.append(("wheel_down", has(r"wheel delta=-1")))
gates.append(("wheel_up", has(r"wheel delta=1")))

# -- liveness / health gates --
heartbeats = re.findall(
    r"\[heartbeat\] ticks=(\d+).*?dropped=(\d+)",
    content)
gates.append(("heartbeats", len(heartbeats) >= 2))
gates.append(("dropped_zero", all(int(d) == 0 for _, d in heartbeats)))
gates.append(("no_exceptions", not re.search(r"PANIC|exception|unhandled", content)))

print("---- gates ----")
ok = True
for name, passed in gates:
    print(f"  {name:20s} {'OK' if passed else 'FAIL'}")
    ok = ok and passed

print("---- samples ----")
for line in re.findall(r"\[input\].*", content):
    print("  " + line)
print("heartbeats:", heartbeats)

sys.exit(0 if ok else 1)
EOF
rc=$?
echo "[verify] exit $rc"
exit $rc
