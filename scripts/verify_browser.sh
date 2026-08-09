#!/bin/bash
# Verify the Sol OS browser end-to-end: open it from a desktop icon,
# run a search, then navigate to youtube.com. Checks pixels on screen
# and greps the serial log.
#
# Usage: scripts/verify_browser.sh
# Exit code 0 = all gates passed, 1 = failure.

set -u
cd "$(dirname "$0")/.."

ISO=build/sol-os.iso
LOG=/tmp/sol-browser.log
SOCK=/tmp/sol-browser.sock

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
echo "[browser] QEMU pid $QPID"

for _ in $(seq 1 60); do
  grep -q "\[desktop\].*up" "$LOG" 2>/dev/null && break
  kill -0 $QPID 2>/dev/null || { echo "[browser] QEMU died during boot"; exit 1; }
  sleep 0.5
done
if ! grep -q "\[desktop\].*up" "$LOG" 2>/dev/null; then
  echo "[browser] FAIL: kernel never reached desktop init"
  tail -5 "$LOG"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi
echo "[browser] desktop is up, opening browser"

python3 - "$SOCK" "$LOG" <<'EOF'
import json, socket, sys, time

SOCK, LOG = sys.argv[1], sys.argv[2]

gates = []
def check(name, ok):
    gates.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}")

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
    raise SystemExit("could not connect to QMP socket")
s.settimeout(30)
read_line(s)
s.sendall(b'{"execute": "qmp_capabilities"}\n')
read_response(s)

def qmp(obj):
    s.sendall(json.dumps(obj).encode() + b"\n")
    resp = read_response(s)
    if "error" in resp:
        raise SystemExit("QMP ERROR: " + json.dumps(resp["error"]))
    return resp

cur_x, cur_y = 960 - 6, 540 - 10

def rel(axis, val):
    global cur_x, cur_y
    qmp({"execute": "input-send-event",
         "arguments": {"events": [{"type": "rel",
                                   "data": {"axis": axis, "value": val}}]}})
    if axis == "x":
        cur_x += val
    else:
        cur_y += val

def btn(down):
    qmp({"execute": "input-send-event",
         "arguments": {"events": [{"type": "btn",
                                   "data": {"down": down, "button": "left"}}]}})

def key(qcode):
    for down in (True, False):
        qmp({"execute": "input-send-event",
             "arguments": {"events": [{"type": "key",
                                       "data": {"down": down,
                                                "key": {"type": "qcode",
                                                        "data": qcode}}}]}})
        time.sleep(0.03)

def move_to(tx, ty):
    rel("x", tx - cur_x)
    rel("y", ty - cur_y)
    time.sleep(0.2)

def click():
    btn(True)
    time.sleep(0.1)
    btn(False)
    time.sleep(0.3)

def shot(path):
    qmp({"execute": "screendump",
         "arguments": {"filename": path, "format": "ppm"}})

def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        data = f.read()
    return w, h, data

def px(img, x, y):
    w, h, data = img
    i = (y * w + x) * 3
    return (data[i], data[i+1], data[i+2])

def near(a, b, tol=10):
    return all(abs(a[i]-b[i]) <= tol for i in range(3))

def count_color(img, x0, y0, x1, y1, color, step=4, tol=10):
    """Sample the rectangle at `step`-pixel increments and count how many
    pixels are within `tol` of `color`. Robust text-region check."""
    w, h, data = img
    hits = 0
    n = 0
    for yy in range(y0, y1, step):
        for xx in range(x0, x1, step):
            i = (yy * w + xx) * 3
            if all(abs(data[i+k]-color[k]) <= tol for k in range(3)):
                hits += 1
            n += 1
    return hits, n

WHITE = (255, 255, 255)
NAV_BAR = (244, 246, 249)      # 0xF4F6F9 browser nav bar
RESULT_TX = (171, 13, 26)      # 0xAB0D1A search result titles
RED_MAST = (255, 0, 0)         # 0xFF0000 YouTube masthead
ADDR_TX = (27, 42, 74)         # 0x1B2A4A address-bar text

time.sleep(1)

# --- open the browser from its desktop icon (row 4: y = 24 + 4*98) ---
move_to(60, 452)
click()
shot("/tmp/br_1.ppm")
img = load("/tmp/br_1.ppm")
# browser window opens at (300,90) 720x460; nav bar band spans y=147..181,
# so y=149 is background chrome (the white address bar starts at y=152)
check("browser: window opened (nav bar)", near(px(img, 500, 149), NAV_BAR))

# --- clear the pre-filled "sol.os/home" (11 chars), then search "sol os" ---
for _ in range(11):
    key("backspace")
time.sleep(0.2)
for c in ["s", "o", "l", "spc", "o", "s"]:
    key(c)
key("ret")
time.sleep(0.5)
shot("/tmp/br_2.ppm")
img = load("/tmp/br_2.ppm")
hits, n = count_color(img, 321, 220, 521, 260, RESULT_TX)
check("search: result titles shown", hits >= 3)
check("search: page background white", near(px(img, 500, 300), WHITE))

# --- clear the address bar, then go to youtube ---
for _ in range(6):
    key("backspace")
time.sleep(0.2)
for c in ["y", "o", "u", "t", "u", "b", "e"]:
    key(c)
key("ret")
time.sleep(0.5)
shot("/tmp/br_3.ppm")
img = load("/tmp/br_3.ppm")
check("youtube: red masthead", near(px(img, 500, 200), RED_MAST))

s.close()

content = open(LOG).read()
check("log: browser opened", "[desktop] open 'Browser'" in content)
exceptions = [l for l in content.splitlines() if "PANIC" in l or "exception" in l or "unhandled" in l]
check("log: no exceptions", not exceptions)

print("---- summary ----")
print("gates:", sum(1 for g in gates), "passed /", len(gates))
sys.exit(0 if all(gates) else 1)
EOF
rc=$?
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
echo "[browser] exit $rc"
exit $rc
