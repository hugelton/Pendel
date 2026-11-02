#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""The README's contact sheet, docs/screens.png, from make ui's screens (build/ui/*.png): the pages in the default
palette, then a row of other palettes.

  make ui && python3 tools/readme_sheet.py [build/ui] [docs/screens.png]"""
import sys
from pathlib import Path

from PIL import Image

SHOTS = [
    "grey_01_home", "grey_10_home_playing", "grey_06c_home_input", "grey_03_efx1_drive",
    "grey_06_efx2_delay", "grey_06b_amp_lead", "grey_08_tape_playing", "grey_13_menu_tempo",
    "amber_10_home_playing", "amber_06_efx2_delay", "paper_10_home_playing", "paper_06b_amp_lead",
]
COLS, TILE, PAD, BG = 4, 480, 24, (27, 28, 30)        # the simulator's background


def main(src="build/ui", out="docs/screens.png"):
    src, out = Path(src), Path(out)
    missing = [s for s in SHOTS if not (src / f"{s}.png").is_file()]
    if missing:
        sys.exit(f"readme_sheet.py: not in {src}: {', '.join(missing)} (run make ui)")
    rows = (len(SHOTS) + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * (TILE + PAD) + PAD, rows * (TILE + PAD) + PAD), BG)
    for i, name in enumerate(SHOTS):
        im = Image.open(src / f"{name}.png").convert("RGB").resize((TILE, TILE), Image.NEAREST)
        sheet.paste(im, (PAD + (i % COLS) * (TILE + PAD), PAD + (i // COLS) * (TILE + PAD)))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out, optimize=True)
    print(f"{out}: {sheet.size[0]} x {sheet.size[1]}, {out.stat().st_size} B")


if __name__ == "__main__":
    main(*sys.argv[1:])
