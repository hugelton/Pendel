#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Talk to a Pocket AMP without root: where it is, the soft key, pa-hwtest's serial commands.

  tools/pa_dev.py state            original / hwtest / pendel / uboot / none
  tools/pa_dev.py uboot            into UBOOT from the original firmware (SysEx soft key) or from pa-hwtest / Pendel ('u'),
                                   and wait
  tools/pa_dev.py cmd LETTERS [-t SECONDS]   send console commands (pa-hwtest or Pendel), print what comes back
  tools/pa_dev.py wait STATE [-t SECONDS]    wait until the unit is in STATE

The soft key needs mido and python-rtmidi (pip install mido python-rtmidi).
The original firmware enumerates with the same USB IDs and port name as an FM-1's, so the soft key goes only to a port
named exactly "USB Composite Device", and only when there is one. pa-hwtest (v3 on) is 1209:0001 named "PA-HWTEST"; a Felucca
FM-1 has the same IDs, so the name decides, and its serial port is found by USB location.
PA_PORT=/dev/cu.usbmodemNNNN overrides the serial port (pa-hwtest v1 / v2 still said "Felucca").
"""
import argparse, glob, os, re, select, subprocess, sys, termios, time

ORIGINAL, UBOOT = (0x4C4A, 0xC755), (0x4C4A, 0x8057)
HWTEST = (0x1209, 0x0001)                  # pa-hwtest
PENDEL = (0x1209, 0x000A)                  # Pendel in development (pid.codes test PID)


def devices():
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True).stdout
    found = []
    for blk in out.split("+-o ")[1:]:
        v = re.search(r'"idVendor" = (\d+)', blk)
        p = re.search(r'"idProduct" = (\d+)', blk)
        loc = re.search(r'"locationID" = (\d+)', blk)
        if v and p:
            found.append(((int(v.group(1)), int(p.group(1))), blk.split("<")[0].strip(), int(loc.group(1)) if loc else 0))
    return found


def state():
    ids = [d[0] for d in devices()]
    if UBOOT in ids:
        return "uboot"
    if ORIGINAL in ids:
        return "original"
    names = [name for ids, name, _ in devices() if ids in (HWTEST, PENDEL)]
    if any(n.startswith("PA-HWTEST") for n in names):
        return "hwtest"
    if any(n.startswith("Pendel") for n in names):
        return "pendel"
    if os.environ.get("PA_PORT"):
        return "hwtest"
    return "none"


def hwtest_port():
    """pa-hwtest's /dev/cu.usbmodem*: the one whose number matches a 1209:0001 device's location."""
    if os.environ.get("PA_PORT"):
        return os.environ["PA_PORT"]
    hw = [loc for ids, name, loc in devices() if ids in (HWTEST, PENDEL) and name.startswith(("PA-HWTEST", "Pendel"))]
    ports = glob.glob("/dev/cu.usbmodem*")
    for loc in hw:
        tag = f"{loc >> 16:x}"     # macOS names CDC ports after the location ID's top digits
        for p in ports:
            if p.removeprefix("/dev/cu.usbmodem").startswith(tag.rstrip("0")):
                return p
    sys.exit(f"cannot tell pa-hwtest's serial port apart: devices {hw}, ports {ports}")


def serial(letters, secs):
    fd = os.open(hwtest_port(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[3] &= ~(termios.ICANON | termios.ECHO)
    a[0] = a[1] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    buf = b""
    try:
        for c in letters:
            os.write(fd, c.encode())
            time.sleep(0.25)
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                buf += os.read(fd, 65536)
    except OSError:
        pass                        # the unit left (u, r)
    finally:
        os.close(fd)
    return buf.decode("latin1")


def softkey():
    import mido
    outs = [n for n in mido.get_output_names() if n == "USB Composite Device"]
    if len(outs) != 1:
        sys.exit(f"want exactly one 'USB Composite Device' MIDI port, got {outs}")
    with mido.open_output(outs[0]) as o:
        time.sleep(0.3)             # CoreMIDI drops a message sent right after the port opens
        for _ in range(3):
            o.send(mido.Message("sysex", data=[0x22, 0x24, 0x35, 0x7D]))
            time.sleep(0.02)


def wait(want, secs):
    end = time.time() + secs
    while time.time() < end:
        if state() == want:
            return True
        time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["state", "uboot", "cmd", "wait"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("-t", type=float, default=1.0)
    a = ap.parse_args()
    if a.what == "state":
        print(state())
    elif a.what == "uboot":
        s = state()
        if s == "original":
            softkey()
        elif s in ("hwtest", "pendel"):
            serial("u", 0.3)
        elif s != "uboot":
            sys.exit(f"no Pocket AMP ({s})")
        ok = wait("uboot", 5)
        print("uboot" if ok else f"not in UBOOT ({state()})")
        return 0 if ok else 1
    elif a.what == "cmd":
        print(serial(a.arg or "p", a.t), end="")
    elif a.what == "wait":
        ok = wait(a.arg, a.t)
        print(state())
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
