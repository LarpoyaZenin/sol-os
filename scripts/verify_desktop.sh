#!/bin/bash
# Verify the Sol OS desktop window system end-to-end.
#
# Boots QEMU headless, then drives the VirtIO mouse over QMP through a
# scripted session: drag a window by its title bar, minimize, restore
# from the taskbar, maximize/un-maximize, and close it. After each
# stage it captures a QMP screendump and checks the pixels, and it
# greps the serial log for the desktop's interaction messages.
#
# Usage: scripts/verify_desktop.sh
# Exit code 0 = all gates passed, 1 = failure.

set -u
cd "$(dirname "$0")/.."

ISO=build/sol-os.iso
LOG=/tmp/sol-desk.log
SOCK=/tmp/sol-desk.sock

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
echo "[desk] QEMU pid $QPID"

for _ in $(seq 1 60); do
  grep -q "\[desktop\].*up" "$LOG" 2>/dev/null && break
  kill -0 $QPID 2>/dev/null || { echo "[desk] QEMU died during boot"; exit 1; }
  sleep 0.5
done
if ! grep -q "\[desktop\].*up" "$LOG" 2>/dev/null; then
  echo "[desk] FAIL: kernel never reached desktop init"
  tail -5 "$LOG"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi
echo "[desk] desktop is up, running session"

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

def near(a, b, tol=12):
    return all(abs(a[i]-b[i]) <= tol for i in range(3))

# cursor tip starts at desktop center minus half the cursor bitmap
cur_x, cur_y = 1920 // 2 - 6, 1080 // 2 - 10

def move_to(tx, ty):
    global cur_x, cur_y
    rel("x", tx - cur_x)
    rel("y", ty - cur_y)
    time.sleep(0.2)

def click():
    btn(True)
    time.sleep(0.1)
    btn(False)
    time.sleep(0.25)

def press():
    btn(True)
    time.sleep(0.1)

def release():
    btn(False)
    time.sleep(0.25)

ORANGE = (245, 166, 35)
BTN_BG = (217, 142, 23)
BODY_BG = (244, 246, 249)
TASKBAR_BG = (31, 42, 60)
TASKBAR_INACT = (49, 62, 85)
TITLE_TX = (27, 42, 74)

time.sleep(1)

# --- stage 1: initial window renders ---
shot("/tmp/desk_1.ppm")
img = load("/tmp/desk_1.ppm")
check("initial: title bar orange", near(px(img, 130, 95), ORANGE))
check("initial: body light", near(px(img, 500, 300), BODY_BG))
check("initial: close btn bg", near(px(img, 556, 95), BTN_BG))
check("initial: taskbar About btn active", near(px(img, 187, 1043), ORANGE))
# text reads left-to-right: the 'A' of "About Sol OS" (title origin 127,99)
# has its top pixels at glyph columns 2,3 (LSB-first), i.e. x=129,130
check("text: 'A' peak left-of-center", near(px(img, 129, 99), TITLE_TX))
check("text: 'A' peak present at 130", near(px(img, 130, 99), TITLE_TX))
check("text: 'A' col0 empty", not near(px(img, 127, 99), TITLE_TX))
check("text: 'A' col7 empty", not near(px(img, 134, 99), TITLE_TX))
check("text: no glyph at mirrored pos", not near(px(img, 131, 99), TITLE_TX))

# --- stage 2: drag window by title bar (120,90) -> (240,150) ---
move_to(250, 100)
press()
rel("x", 120)
rel("y", 60)
time.sleep(0.2)
release()
shot("/tmp/desk_2.ppm")
img = load("/tmp/desk_2.ppm")
check("drag: old title spot is background", not near(px(img, 130, 95), ORANGE))
check("drag: new title spot orange", near(px(img, 250, 155), ORANGE))

# --- stage 3: minimize via title button ---
# min button center for window at (240,150): spans x 626..648, y 153..173
move_to(637, 163)
click()
shot("/tmp/desk_3.ppm")
img = load("/tmp/desk_3.ppm")
check("min: window gone", not near(px(img, 250, 155), ORANGE))
check("min: taskbar About btn inactive", near(px(img, 187, 1043), TASKBAR_INACT))

# --- stage 4: restore from taskbar ---
move_to(187, 1056)
click()
shot("/tmp/desk_4.ppm")
img = load("/tmp/desk_4.ppm")
check("restore: window back", near(px(img, 250, 155), ORANGE))

# --- stage 5: maximize, then un-maximize ---
move_to(661, 163)
click()
shot("/tmp/desk_5.ppm")
img = load("/tmp/desk_5.ppm")
check("max: title bar at top-right", near(px(img, 1000, 13), ORANGE))
check("max: body fills screen", near(px(img, 1000, 600), BODY_BG))

move_to(1881, 13)
click()
shot("/tmp/desk_6.ppm")
img = load("/tmp/desk_6.ppm")
check("unmax: window restored", near(px(img, 250, 155), ORANGE))

# --- stage 6: close ---
move_to(685, 163)
click()
shot("/tmp/desk_7.ppm")
img = load("/tmp/desk_7.ppm")
check("close: window gone", not near(px(img, 250, 155), ORANGE))
check("close: taskbar About btn removed", near(px(img, 187, 1043), TASKBAR_BG))

# --- stage 7: desktop icons ---
shot("/tmp/desk_8.ppm")
img = load("/tmp/desk_8.ppm")
check("icons: Terminal tile", near(px(img, 26, 26), (69, 139, 217)))     # 0x00458BD9
check("icons: Files tile", near(px(img, 26, 124), ORANGE))
check("icons: Settings tile", near(px(img, 26, 222), (58, 175, 169)))    # 0x003AAFA9

move_to(60, 60)   # Terminal icon tile
click()
shot("/tmp/desk_9.ppm")
img = load("/tmp/desk_9.ppm")
check("icon: Terminal window opened", near(px(img, 320, 95), ORANGE))

s.close()

# --- serial log gates ---
content = open(LOG).read()
for tag in ["[desktop] drag 'About Sol OS'",
            "[desktop] min 'About Sol OS'",
            "[desktop] taskbar 'About Sol OS'",
            "[desktop] close 'About Sol OS'",
            "[desktop] open 'Terminal'"]:
    check(f"log: {tag}", tag in content)
check("log: max fired twice", content.count("[desktop] max 'About Sol OS'") >= 2)
exceptions = [l for l in content.splitlines() if "PANIC" in l or "exception" in l or "unhandled" in l]
check("log: no exceptions", not exceptions)

print("---- summary ----")
print("gates:", sum(1 for g in gates), "passed /", len(gates))
sys.exit(0 if all(gates) else 1)
EOF
rc=$?
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
echo "[desk] exit $rc"
exit $rc
