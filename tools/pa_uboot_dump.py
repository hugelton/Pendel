#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Back up a Pocket AMP's whole flash from mask-ROM UBOOT1.00. Read-only: no erase or write call is made.

  sudo JLTOOL=~/.fm1/jl-uboot-tool $JLTOOL/.venv/bin/python tools/pa_uboot_dump.py [--out DIR]

Enter UBOOT first with the soft key (F0 22 24 35 7D F7 on the "USB Composite Device" MIDI port).
Steps: upload wl82loader to RAM and run it, print the chip key and the flash ID, take the size from the
ID's capacity byte, read the whole flash twice, save both reads and require them to be equal.
Power-cycle afterwards to boot the installed firmware again.
"""
import argparse, hashlib, os, sys, time
from pathlib import Path

JLTOOL = Path(os.environ.get("JLTOOL", Path.home() / ".fm1/jl-uboot-tool"))
sys.path.insert(0, str(JLTOOL))
from jltech.uboot import JL_MSCDevice, JL_UBOOT, JL_LoaderV2  # noqa: E402

DEVICE = "usb:4c4a:8057"
LOADER = JLTOOL / "data/loaderblobs/usb/wl82loader.bin"
LOADER_ADDR = 0x1C02000
LOADER_ARG = 0x0001
IO = 512


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def read_all(ldr, size):
    out = bytearray()
    for a in range(0, size, IO):
        out += ldr.flash_read(a, IO)
        if a % 0x40000 == 0:
            log(f"  {a:#08x}")
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/backup")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with JL_MSCDevice(DEVICE) as dev:
        vendor, prod, _ = dev.inquiry()
        log(f"device {vendor} {prod}")
        if prod != "UBOOT1.00":
            sys.exit("not UBOOT1.00; aborting")
        rom = JL_UBOOT(dev.dev)
        blob = LOADER.read_bytes()
        for off in range(0, len(blob), 512):
            rom.mem_write(LOADER_ADDR + off, blob[off:off + 512])
        rom.mem_jump(LOADER_ADDR, LOADER_ARG)
        ldr = JL_LoaderV2(dev.dev)
        key, od = ldr.chip_key(), ldr.online_device()
        cap = od["id"] & 0xFF
        log(f"loader up: chip key {key:#06x}, flash id {od['id']:#08x} type {od['type']:#x}")
        if not 0x13 <= cap <= 0x18:
            sys.exit(f"capacity byte {cap:#x} out of range; aborting")
        size = 1 << cap
        log(f"flash size {size} B ({size >> 10} KiB)")
        one = read_all(ldr, size)
        two = read_all(ldr, size)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    for i, d in ((1, one), (2, two)):
        p = out / f"pa-flash-{stamp}-{i}.bin"
        p.write_bytes(d)
        log(f"saved {p} sha256 {hashlib.sha256(d).hexdigest()}")
    if one != two:
        sys.exit("the two reads DIFFER")
    log("the two reads are equal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
