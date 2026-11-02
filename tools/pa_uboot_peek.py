#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Read RAM a firmware left behind, from mask-ROM UBOOT1.00. Read-only: nothing is written or run.

  sudo JLTOOL=~/.fm1/jl-uboot-tool $JLTOOL/.venv/bin/python tools/pa_uboot_peek.py [ADDR:LEN ...]

With no arguments it reads pa-hwtest's .noinit (crash record, crash contexts, boot guard) and a few .bss words.
"""
import os, struct, sys
from pathlib import Path

JLTOOL = Path(os.environ.get("JLTOOL", Path.home() / ".fm1/jl-uboot-tool"))
sys.path.insert(0, str(JLTOOL))
from jltech.uboot import JL_MSCDevice, JL_UBOOT  # noqa: E402
from jltech.cipher import cipher_bytes, jl_crc_cipher  # noqa: E402

DEVICE = "usb:4c4a:8057"
BLOCK = 512
DEFAULT = [(0x1C7C000, 0x190), (0x1C09380, 0x80)]   # .noinit (fm1_crash, fm1_crash_ctx, guard); .bss (a_halves, ms, jedec, boots_ok)
CRASH = "magic count vec pc rets emu dbg sp psr icfg uptime_ms etm0 etm1 etm2 etm3 early".split()


def main():
    spans = [tuple(int(x, 0) for x in s.split(":")) for s in sys.argv[1:]] or DEFAULT
    with JL_MSCDevice(DEVICE) as dev:
        vendor, prod, _ = dev.inquiry()
        if prod != "UBOOT1.00":
            sys.exit(f"unexpected device {vendor} {prod}")
        rom = JL_UBOOT(dev.dev)
        got = {}
        for addr, n in spans:
            data = b"".join(cipher_bytes(jl_crc_cipher, rom.mem_read(addr + off, min(BLOCK, n - off)))
                            for off in range(0, n, BLOCK))
            got[addr] = data
            for off in range(0, n, 16):
                print(f"  {addr + off:08x}: {data[off:off + 16].hex(' ')}")
    if 0x1C7C000 in got:
        d = got[0x1C7C000]
        c = dict(zip(CRASH, struct.unpack_from("<16I", d, 0)))
        print("fm1_crash:", " ".join(f"{k}={v:#x}" for k, v in c.items()), "(valid)" if c["magic"] == 0x43525348 else "(no CRSH magic)")
        print("guard: magic=%#x pending=%d failed=%d" % struct.unpack_from("<3I", d, 0x180))
    return 0


if __name__ == "__main__":
    sys.exit(main())
