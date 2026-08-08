#!/usr/bin/env python3
"""Inject input events into a running QEMU instance over QMP.

Usage:
  python3 scripts/qmp_input.py <event> [<event> ...]

Each <event> is one of:
  key <qcode> down|up        e.g. key a down, key ret up, key esc down
  btn <qcode> down|up        e.g. btn btn-left down, btn btn-side down
  rel <x|y|z|wheel> <int>    e.g. rel x 5, rel y -3

Connects to the unix QMP socket (default /tmp/sol-qmp.sock, override
with QMP_SOCK env var), negotiates capabilities, then sends each
event with a small delay so the guest sees them as distinct packets.
"""
import json
import os
import socket
import sys
import time

SOCK = os.environ.get("QMP_SOCK", "/tmp/sol-qmp.sock")
DELAY = float(os.environ.get("QMP_DELAY", "0.12"))


def read_object(sock):
    """Read one QMP response/event line; returns the first object that
    carries a 'return' or 'error' key (skipping unsolicited events)."""
    buf = b""
    while True:
        buf += sock.recv(65536)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if "return" in obj or "error" in obj:
                return obj


def connect_with_retry(path, attempts=20, delay=0.5):
    """QEMU may not have its QMP server up yet; retry the connect."""
    for _ in range(attempts):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            sock.settimeout(15)
            return sock
        except OSError:
            time.sleep(delay)
    raise SystemExit("could not connect to QMP socket %s" % path)


def send(sock, obj):
    sock.sendall(json.dumps(obj).encode("utf-8") + b"\n")


def parse_event(spec):
    parts = spec.split()
    if not parts:
        raise SystemExit("empty event spec")
    kind = parts[0]
    if kind == "key":
        if len(parts) != 3 or parts[2] not in ("down", "up"):
            raise SystemExit("usage: key <qcode> down|up")
        return {"type": "key", "data": {
            "down": parts[2] == "down",
            "key": {"type": "qcode", "data": parts[1]}}}
    if kind == "btn":
        if len(parts) != 3 or parts[2] not in ("down", "up"):
            raise SystemExit("usage: btn <button> down|up")
        button = parts[1]
        if button.startswith("btn-"):
            button = button[4:]      # accept "btn-side" as a shorthand
        return {"type": "btn", "data": {
            "down": parts[2] == "down",
            "button": button}}
    if kind == "rel":
        if len(parts) != 3:
            raise SystemExit("usage: rel <axis> <int>")
        return {"type": "rel", "data": {"axis": parts[1], "value": int(parts[2])}}
    raise SystemExit("unknown event kind: %s" % kind)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    sock = connect_with_retry(SOCK)

    read_object(sock)                # greeting
    send(sock, {"execute": "qmp_capabilities"})
    read_object(sock)

    for spec in sys.argv[1:]:
        ev = parse_event(spec)
        send(sock, {"execute": "input-send-event", "arguments": {"events": [ev]}})
        resp = read_object(sock)
        if "error" in resp:
            print("ERROR %s: %s" % (spec, resp["error"]))
        time.sleep(DELAY)

    sock.close()


if __name__ == "__main__":
    main()
