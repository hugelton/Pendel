#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Write a whole-flash image to a Pocket AMP from mask-ROM UBOOT1.00, app area only, every sector read back.

  sudo JLTOOL=~/.fm1/jl-uboot-tool $JLTOOL/.venv/bin/python tools/pa_uboot_write.py \
      --ref BACKUP.bin --image IMAGE.bin [--write]

Writing pa-hwtest:  --image build/flash/pa-hwtest.bin      (from tools/pa_make_image.py)
Restoring the backup: --image BACKUP.bin                     (the same file as --ref)
Replacing an earlier image: add --was EARLIER.bin (the app area now on the unit may then be that one too)

Steps (each one aborts on any mismatch):
  1. wait for UBOOT1.00, upload wl82loader to RAM and run it; check chip key 0x980F and flash ID 0x856014;
  2. read the whole flash; the head [0, 0x4000) must equal --ref and the app area --ref, --image or --was (VM, OTP, USR may
     have been changed by the running firmware and are left as they are);
  3. the image's head must equal --ref and the image may differ from --ref only inside the app area;
  4. with --write only: erase + write just the app-area sectors that differ from the flash, read each back;
  5. read the whole flash again and require the app area to equal the image.
Nothing outside [0x4000, APP_END) is ever erased or written.
"""
import argparse, os, sys, time
from pathlib import Path

JLTOOL = Path(os.environ.get("JLTOOL", Path.home() / ".fm1/jl-uboot-tool"))
sys.path.insert(0, str(JLTOOL))
from jltech.uboot import JL_MSCDevice, JL_UBOOT, JL_LoaderV2  # noqa: E402

DEVICE = "usb:4c4a:8057"
LOADER = JLTOOL / "data/loaderblobs/usb/wl82loader.bin"
LOADER_ADDR = 0x1C02000
LOADER_ARG = 0x0001
CHIP_KEY = 0x980F
FLASH_ID = 0x856014
FLASH_SIZE = 0x100000
SECTOR = 0x1000
IO = 512
HEAD = 0x4000
APP_END = 0xA5000        # the end of the app area on this unit (the settings region follows)


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def read_all(ldr):
    return b"".join(ldr.flash_read(a, IO) for a in range(0, FLASH_SIZE, IO))


def sectors(a, b, lo, hi):
    return [s for s in range(lo, hi, SECTOR) if a[s:s + SECTOR] != b[s:s + SECTOR]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="this unit's whole-flash backup")
    ap.add_argument("--image", required=True, help="whole-flash image to write (app area only)")
    ap.add_argument("--was", action="append", default=[], help="an image written earlier, which the app area may hold now")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    ref, img = Path(a.ref).read_bytes(), Path(a.image).read_bytes()
    was = [Path(w).read_bytes() for w in a.was]
    if len(ref) != FLASH_SIZE or len(img) != FLASH_SIZE:
        sys.exit("--ref and --image must both be 1 MiB")
    if img[:HEAD] != ref[:HEAD]:
        sys.exit("refusing: the image's head [0, 0x4000) differs from --ref")
    if sectors(img, ref, 0, HEAD) or sectors(img, ref, APP_END, FLASH_SIZE):
        sys.exit("refusing: the image differs from --ref outside the app area")
    with JL_MSCDevice(DEVICE) as dev:
        vendor, prod, _ = dev.inquiry()
        if prod != "UBOOT1.00":
            sys.exit(f"unexpected device {vendor} {prod}")
        rom = JL_UBOOT(dev.dev)
        blob = LOADER.read_bytes()
        for off in range(0, len(blob), 512):
            rom.mem_write(LOADER_ADDR + off, blob[off:off + 512])
        rom.mem_jump(LOADER_ADDR, LOADER_ARG)
        ldr = JL_LoaderV2(dev.dev)
        key, od = ldr.chip_key(), ldr.online_device()
        log(f"loader up: chip key {key:#06x}, flash id {od['id']:#08x}")
        if key != CHIP_KEY or od["id"] != FLASH_ID:
            sys.exit("chip key / flash id mismatch; aborting")
        now = read_all(ldr)
        if now[:HEAD] != ref[:HEAD]:
            sys.exit("the flash head differs from --ref (another unit?); aborting")
        known = [("--ref", ref), ("--image", img)] + [(f"--was {w}", d) for w, d in zip(a.was, was)]
        held = next((n for n, d in known if now[HEAD:APP_END] == d[HEAD:APP_END]), None)
        if held is None:
            sys.exit("the app area is none of --ref, --image, --was; aborting")
        log(f"the app area now holds {held}")
        extra = [hex(s) for s in sectors(now, ref, APP_END, FLASH_SIZE)]
        log(f"device-data sectors changed since --ref (left as they are): {extra or 'none'}")
        diff = sectors(img, now, HEAD, APP_END)
        log(f"{len(diff)} app-area sectors to write" + (f": {diff[0]:#x}..{diff[-1]:#x}" if diff else ""))
        if not a.write:
            log("dry run: nothing written")
            return 0
        for s in diff:
            ldr.flash_erase_sector(s)
            for off in range(0, SECTOR, IO):
                ldr.flash_write(s + off, img[s + off:s + off + IO])
            back = b"".join(ldr.flash_read(s + off, IO) for off in range(0, SECTOR, IO))
            if back != img[s:s + SECTOR]:
                sys.exit(f"sector {s:#07x}: VERIFY FAILED; stay in UBOOT and run again")
        log(f"{len(diff)} sectors written and read back")
        final = read_all(ldr)
        ok = final[:APP_END] == img[:APP_END]
        log("final read: app area " + ("EQUALS the image" if ok else "DIFFERS from the image"))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
