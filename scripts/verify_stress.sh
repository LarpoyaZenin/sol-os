#!/bin/bash
#
# Sol OS graphics stress test.
#
# Drives the VirtIO mouse over QMP through a punishing session:
#   - rapid cursor sweeps to all four corners and back (trail check)
#   - window dragging across the screen (trail / stale-pixel check)
#   - dragging windows off the screen edges (clamp check)
#   - opening several windows, dragging them over each other
#   - maximize / restore / minimize / close cycles
#   - open/close reopen cycles
#   - text left-to-right verification after all of the above
# After every stage it screendumps and asserts pixels, and it verifies
# the serial log shows: graphics selftest PASS, zero integrity
# failures, zero exceptions, a stable heap free count, and zero
# dropped VirtIO input events.
#
# Usage: scripts/verify_stress.sh
# Exit code 0 = all gates pass, 1 = failure.

set -u
cd "$(dirname "$0")/.."
ISO=build/sol-os.iso
LOG=/tmp/sol-stress.log
SOCK=/tmp/sol-stress.sock

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
echo "[stress] QEMU pid $QPID"

for _ in $(seq 1 60); do
  grep -q "\[desktop\].*up" "$LOG" 2>/dev/null && break
  kill -0 $QPID 2>/dev/null || { echo "[stress] QEMU died during boot"; exit 1; }
  sleep 0.5
done
if ! grep -q "\[desktop\].*up" "$LOG" 2>/dev/null; then
  echo "[stress] FAIL: kernel never reached desktop init"
  tail -5 "$LOG"
  kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exit 1
fi
echo "[stress] desktop is up, running session"

python3 - "$SOCK" "$LOG" <<'EOF'
import json, socket, sys, time, re

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

cur_x, cur_y = 1920 // 2 - 6, 1080 // 2 - 10

# Kernel clamps the cursor to the screen (desktop.c): x in [0, g_w-CURSOR_W],
# y in [0, g_h-CURSOR_H]. We must mirror that so the model never drifts from
# the kernel's actual position (e.g. after pushing the cursor far off-screen).
XMAX, YMAX = 1920 - 12, 1080 - 20

def rel(axis, val):
    global cur_x, cur_y
    qmp({"execute": "input-send-event",
         "arguments": {"events": [{"type": "rel",
                                   "data": {"axis": axis, "value": val}}]}})
    if axis == "x":
        cur_x = min(XMAX, max(0, cur_x + val))
    else:
        cur_y = min(YMAX, max(0, cur_y + val))

def btn(down):
    qmp({"execute": "input-send-event",
         "arguments": {"events": [{"type": "btn",
                                   "data": {"down": down, "button": "left"}}]}})

def move_to(tx, ty, steps=1):
    global cur_x, cur_y
    if steps > 1:
        dx_total = tx - cur_x
        dy_total = ty - cur_y
        for i in range(1, steps + 1):
            rel("x", int(round(dx_total * i / steps)) - int(round(dx_total * (i - 1) / steps)))
            rel("y", int(round(dy_total * i / steps)) - int(round(dy_total * (i - 1) / steps)))
            time.sleep(0.005)
    else:
        rel("x", tx - cur_x)
        rel("y", ty - cur_y)
    time.sleep(0.2)

def click():
    btn(True); time.sleep(0.05); btn(False); time.sleep(0.2)

def press():
    btn(True); time.sleep(0.05)

def release():
    btn(False); time.sleep(0.2)

def rbtn(down):
    qmp({"execute": "input-send-event",
         "arguments": {"events": [{"type": "btn",
                                   "data": {"down": down, "button": "right"}}]}})

def rclick():
    rbtn(True); time.sleep(0.05); rbtn(False); time.sleep(0.2)

def shot(path):
    qmp({"execute": "screendump",
         "arguments": {"filename": path}})

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

def gradient(y, h=1080):
    r0, g0, b0 = 58, 92, 138
    r1, g1, b1 = 18, 27, 46
    t = 0 if h <= 1 else y * 255 / (h - 1)
    def mix(a0, a1):
        return int(round(a0 + (a1 - a0) * t / 255))
    return (mix(r0, r1), mix(g0, g1), mix(b0, b1))

ORANGE = (245, 166, 35)      # TITLE_ACTIVE
INACT = (80, 94, 110)        # TITLE_INACT
BODY_BG = (244, 246, 249)
TXT = (27, 42, 74)
BLACK = (0, 0, 0)

time.sleep(1)

# --- stage 1: rapid cursor sweeps over empty background, trail check ---
clean = (1500, 500)
move_to(*clean)
shot("/tmp/stress_1.ppm"); img = load("/tmp/stress_1.ppm")
check("sweep: cursor tip at clean spot", near(px(img, *clean), BLACK, 6))

move_to(10, 500, steps=8)
shot("/tmp/stress_2.ppm"); img = load("/tmp/stress_2.ppm")
check("sweep: corner-left cursor", near(px(img, 10, 500), BLACK, 6))
check("sweep: no trail at clean spot", near(px(img, 1506, 506), gradient(506)))

move_to(1800, 40, steps=8)
shot("/tmp/stress_3.ppm"); img = load("/tmp/stress_3.ppm")
check("sweep: top-right corner cursor", near(px(img, 1800, 40), BLACK, 6))
check("sweep: no trail at left corner", near(px(img, 16, 506), gradient(506)))

move_to(20, 1040, steps=8)
shot("/tmp/stress_4.ppm"); img = load("/tmp/stress_4.ppm")
check("sweep: bottom-left (taskbar) cursor", near(px(img, 20, 1040), BLACK, 6))

move_to(1900, 1040, steps=8)
shot("/tmp/stress_5.ppm"); img = load("/tmp/stress_5.ppm")
check("sweep: bottom-right corner cursor", near(px(img, 1900, 1040), BLACK, 6))
check("sweep: taskbar edge intact after sweep",
      all(near(px(img, x, 1032), ORANGE, 6) for x in (300, 800, 1400, 1800)))

# --- stage 2: drag the About window away and back (trail check) ---
# About window at (120,90), size 460x240; title y 90..116. Safe title
# probe point: (x+200, y+4) = (320,94), clear of the title text and the
# close/max/min buttons.
move_to(250, 100); press()
rel("x", 480); rel("y", 300)     # drag to (600, 390)
release()
shot("/tmp/stress_6.ppm"); img = load("/tmp/stress_6.ppm")
check("drag: new title bar orange", near(px(img, 800, 394), ORANGE))
check("drag: no stale window pixels at old spot", near(px(img, 320, 94), gradient(94)))
check("drag: no stale window pixels old body", near(px(img, 500, 300), gradient(300)))

move_to(800, 394); press()
rel("x", -480); rel("y", -300)   # drag back to (120,90)
release()
shot("/tmp/stress_7.ppm"); img = load("/tmp/stress_7.ppm")
# Cursor sits at (320,94) after the drag-back, so probe the title bar
# clear of it (text ends ~x+95, buttons start ~x+384).
check("drag back: title bar restored", near(px(img, 240, 94), ORANGE))
check("drag back: no trail at vacated spot", near(px(img, 800, 394), gradient(394)))

# --- stage 3: drag window off-screen edges (clamp) ---
move_to(250, 100); press()
rel("x", 5000)                   # push far right
release()
shot("/tmp/stress_8.ppm"); img = load("/tmp/stress_8.ppm")
check("clamp: window stays on screen (right edge orange)",
      near(px(img, 1660, 94), ORANGE))
check("clamp: window did not wrap to left",
      not near(px(img, 50, 100), ORANGE))

# --- stage 4: open several windows and drag them over each other ---
# icon_open uses a persistent counter n. First three opens land at
# (300,90), (336,118), (372,146), size 460x220, TITLE_H=26.
move_to(60, 60); click()                     # Terminal n=0 -> (300,90)
move_to(60, 158); click()                    # Files    n=1 -> (336,118)
move_to(60, 256); click()                    # Settings n=2 -> (372,146)
shot("/tmp/stress_9.ppm"); img = load("/tmp/stress_9.ppm")
# Settings is topmost (active orange title); the other two are inactive gray.
check("multiwin: Terminal title bar", near(px(img, 500, 94), INACT))
check("multiwin: Files title bar", near(px(img, 536, 122), INACT))
check("multiwin: Settings title bar", near(px(img, 572, 150), ORANGE))

# right-click raises the clicked window; park the cursor on clear
# background before each shot so it cannot cover the probed titles.
move_to(500, 94); rclick()                   # raise Terminal
move_to(100, 400)
shot("/tmp/stress_9b.ppm"); img = load("/tmp/stress_9b.ppm")
check("rclick: Terminal raised", near(px(img, 500, 94), ORANGE))
move_to(800, 340); rclick()                  # Settings area clear of Terminal
move_to(100, 400)
shot("/tmp/stress_9c.ppm"); img = load("/tmp/stress_9c.ppm")
check("rclick: Settings re-raised", near(px(img, 572, 150), ORANGE))
check("rclick: Terminal inactive again", near(px(img, 500, 94), INACT))

# drag the topmost (Settings) window over the others by its title bar
move_to(600, 152); press()
rel("x", 300); rel("y", 400)
release()
shot("/tmp/stress_10.ppm"); img = load("/tmp/stress_10.ppm")
check("overlap: Settings moved", near(px(img, 872, 550), ORANGE))

# --- stage 5: maximize / restore / minimize cycles ---
# About was clamped at the right edge (x=1460) in stage 3. Bring it
# forward via its taskbar button (slot 0 at x 112..262).
move_to(187, 1043); click()
shot("/tmp/stress_11.ppm"); img = load("/tmp/stress_11.ppm")
check("taskbar: About raised", near(px(img, 1660, 94), ORANGE))

# maximize: buttons at close=x+w-26, max=-24, min=-24; y+3..y+23
# About at (1460,90): max button center = 1460+460-39 = 1881, y = 103
move_to(1881, 103); click()
shot("/tmp/stress_12.ppm"); img = load("/tmp/stress_12.ppm")
check("max: title bar top edge", near(px(img, 1000, 13), ORANGE))
check("max: body fills screen", near(px(img, 1000, 600), BODY_BG))

# unmaximize (window now at 0,0 so buttons sit at y 3..23)
move_to(1881, 13); click()
shot("/tmp/stress_13.ppm"); img = load("/tmp/stress_13.ppm")
check("unmax: restored", near(px(img, 1660, 94), ORANGE))

# minimize via title min button (normal window: y 93..113)
move_to(1857, 103); click()
shot("/tmp/stress_14.ppm"); img = load("/tmp/stress_14.ppm")
check("min: window gone", not near(px(img, 1660, 94), ORANGE))

# restore from taskbar
move_to(187, 1043); click()
shot("/tmp/stress_15.ppm"); img = load("/tmp/stress_15.ppm")
check("restore: window back", near(px(img, 1660, 94), ORANGE))

# --- stage 5b: dragging a maximized window restores its size and makes
# it follow the cursor (About was maximized from (1460,90), size 460x240) ---
move_to(1881, 103); click()                 # maximize About again
shot("/tmp/stress_16.ppm"); img = load("/tmp/stress_16.ppm")
check("maxdrag: maximized first", near(px(img, 1000, 13), ORANGE))
move_to(1000, 13); press()
rel("x", 300); time.sleep(0.1)              # each rel lands in its own poll
rel("y", 200); time.sleep(0.1)
release()
shot("/tmp/stress_16b.ppm"); img = load("/tmp/stress_16b.ppm")
check("maxdrag: restored size, follows cursor", near(px(img, 500, 204), ORANGE))
# (1000,600) is covered by Settings (dragged to (672,546) in stage 4), so
# probe a pixel inside the old maximized area but clear of every window.
check("maxdrag: no longer fullscreen", near(px(img, 1000, 100), gradient(100)))
# drag About back to its right-edge clamp position (1460,90)
move_to(500, 204); press()
rel("x", 1160); time.sleep(0.1)
rel("y", -110); time.sleep(0.1)
release()
shot("/tmp/stress_16c.ppm"); img = load("/tmp/stress_16c.ppm")
check("maxdrag: About parked at right edge", near(px(img, 1580, 94), ORANGE))

# --- stage 6: open / close repeatedly ---
# icon_open counter continues: opens 3,4,5 land at (408,174), (300,90),
# (336,118). Each reopened window is topmost (active orange title).
for i, (wx, wy) in enumerate([(408, 174), (300, 90), (336, 118)]):
    move_to(60, 60); click()
    cx = wx + 200; cy = wy + 4
    shot(f"/tmp/stress_open_{i}.ppm"); img = load(f"/tmp/stress_open_{i}.ppm")
    check(f"reopen {i}: Terminal window appears", near(px(img, cx, cy), ORANGE))
    ccx = wx + 460 - 26 + 11; ccy = wy + 13
    move_to(ccx, ccy); click()
    shot(f"/tmp/stress_close_{i}.ppm"); img = load(f"/tmp/stress_close_{i}.ppm")
    check(f"reclose {i}: Terminal window gone", not near(px(img, cx, cy), ORANGE))

# --- stage 7: final integrity sweep ---
# Sweep the cursor through all four corners once more and confirm the
# desktop is still intact (taskbar, gradient, About window text).
move_to(10, 10, steps=8)
move_to(1900, 1060, steps=8)
move_to(950, 500, steps=8)
shot("/tmp/stress_17.ppm"); img = load("/tmp/stress_17.ppm")
check("final: cursor at center", near(px(img, 950, 500), BLACK, 6))
check("final: taskbar edge intact",
      all(near(px(img, x, 1032), ORANGE, 6) for x in (100, 600, 1200, 1700)))
# About window still open at (1460,90): text orientation left-to-right.
check("final: title 'A' left-of-center", near(px(img, 1469, 99), TXT))
check("final: title 'A' no mirrored pixel", not near(px(img, 1471, 99), TXT))
# icons still rendered
check("final: Terminal tile", near(px(img, 26, 26), (46, 76, 115)))
check("final: Settings tile", near(px(img, 26, 222), (58, 175, 169)))

# --- stage 8: close About last (kept open so stage 7 could check text) ---
move_to(187, 1043); click()                  # raise About again
move_to(1905, 103); click()                  # close button (x+w-15, y+13)
shot("/tmp/stress_18.ppm"); img = load("/tmp/stress_18.ppm")
check("close: window gone", not near(px(img, 1660, 94), ORANGE))
check("close: taskbar button removed", not near(px(img, 187, 1043), ORANGE))

# --- stage 8b: window covering the icons must not ghost them ---
# Regression for the stray-pixel/ghosting bug: a damaged region that
# does NOT intersect a window currently covering the desktop icons must
# never cause those icons to be stamped over the window in the
# backbuffer (scene_region must clip every element to the damage rect).
# Here a fresh About window is parked at (0,0) over the icons, then the
# Start menu is toggled (its damage rect is far from the window) and the
# cursor is swept through the covered icon area. If the icons leaked,
# their tile colors would show where the window body should be.
# icon_open counter is 6 at this point -> new window at (372,146).
move_to(60, 350); click()                    # open About icon (3rd row)
shot("/tmp/stress_19.ppm"); img = load("/tmp/stress_19.ppm")
check("cover: About opened", near(px(img, 500, 150), ORANGE))
move_to(560, 150); press()
rel("x", -550); rel("y", -146)               # cursor -> (10,4); window clamps to (0,0)
release()
shot("/tmp/stress_19b.ppm"); img = load("/tmp/stress_19b.ppm")
check("cover: window parked over icons", near(px(img, 44, 55), BODY_BG))
check("cover: title bar orange", near(px(img, 30, 13), ORANGE))
move_to(40, 1055); click()                   # open Start menu (damage far from window)
move_to(40, 1055); click()                   # close it again
move_to(40, 40)                              # cursor sweeps over the covered icons
move_to(80, 180)                             # ...and over the second icon row too
move_to(500, 500)                            # and away; leaked pixels would remain
shot("/tmp/stress_19c.ppm"); img = load("/tmp/stress_19c.ppm")
check("cover: no icon leak on window body", near(px(img, 44, 55), BODY_BG))
check("cover: no icon leak lower body", near(px(img, 80, 180), BODY_BG))

s.close()

# --- serial log gates ---
content = open(LOG).read()
check("log: gfx selftest PASS", "[gfx] selftest: PASS" in content)
check("log: no gfx integrity failures", "[gfx] INTEGRITY FAILURE" not in content)
exceptions = [l for l in content.splitlines()
              if "PANIC" in l or "exception" in l or "unhandled" in l or "out of heap" in l]
check("log: no exceptions/panics/heap-oom", not exceptions)

heap_free = re.findall(r"heap_free=(\d+)", content)
same_heap = len(set(heap_free)) <= 1
check("log: heap free stable (no OOB into heap)", same_heap)

dropped = re.findall(r"virtio_dropped=(\d+)", content)
check("log: no virtio input drops", all(int(v) == 0 for v in dropped))

print("---- summary ----")
print("gates:", sum(1 for g in gates), "passed /", len(gates))
sys.exit(0 if all(gates) else 1)
EOF
rc=$?
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
echo "[stress] exit $rc"
exit $rc
