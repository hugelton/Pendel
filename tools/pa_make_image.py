#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Make a whole-flash image for this unit: its own backup with app.bin replaced, nothing else changed.

  python3 tools/pa_make_image.py BACKUP.bin APP.bin OUT.bin

The app is padded with 0xFF to the installed app's size, so nothing after it moves and the head
[0, 0x4000) (SPL, isd_config), VM, BTIF, OTP, USR and key_mac stay byte-identical to the backup.
The result is read back (tools/pa_flash.py) and checked before it is written. Standard library only.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pa_flash import FlashImage  # noqa: E402

SECTOR = 0x1000
HEAD = 0x4000


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ref = Path(sys.argv[1]).read_bytes()
    app = Path(sys.argv[2]).read_bytes()
    dev = FlashImage(ref)
    size = dev.app.size
    if len(app) > size:
        sys.exit(f"app is {len(app)} B, larger than the installed {size} B")
    padded = app + b"\xff" * (size - len(app))
    img = dev.set_app(padded)
    chk = FlashImage(img)
    assert chk.app_bytes() == padded, "app does not read back"
    assert img[:HEAD] == ref[:HEAD], "head changed"
    end = dev.base + dev.head.size
    diff = [s for s in range(0, len(img), SECTOR) if img[s:s + SECTOR] != ref[s:s + SECTOR]]
    assert all(HEAD <= s < end for s in diff), "a sector outside the app area changed"
    Path(sys.argv[3]).write_bytes(img)
    print(f"app {len(app)} B padded to {size} B; app area [{HEAD:#x}, {end:#x}); {len(diff)} sectors differ "
          f"({diff[0]:#x}..{diff[-1]:#x}); wrote {sys.argv[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
