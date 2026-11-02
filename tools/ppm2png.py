#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""PPM screens from tests/ui_render.c -> PNG (2x, nearest) and one contact sheet per palette.

  ppm2png.py DIR"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

d = Path(sys.argv[1])
shots = sorted(d.glob("*.ppm"))
by_pal = {}
for p in shots:
    im = Image.open(p).convert("RGB")
    im.resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    by_pal.setdefault(p.stem.split("_")[0], []).append((p.stem, im))
    p.unlink()
for pal, ims in by_pal.items():
    cols, pad, label = 4, 16, 18
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (480 + pad) + pad, rows * (480 + pad + label) + pad), (40, 40, 40))
    dr = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(ims):
        x, y = pad + (i % cols) * (480 + pad), pad + (i // cols) * (480 + pad + label)
        dr.text((x, y), name, fill=(200, 200, 200))
        sheet.paste(im.resize((480, 480), Image.NEAREST), (x, y + label))
    out = d / f"sheet_{pal}.png"
    sheet.save(out)
    print(" ", out)
