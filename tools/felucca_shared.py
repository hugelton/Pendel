#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""The files Pendel shares with Felucca (https://github.com/hugelton/Felucca), its sibling: kept identical, so the two
look and draw alike and a fix in one is a fix in both.

  tools/felucca_shared.py check [FELUCCA]   compare with a Felucca checkout (default ~/Felucca); exit 1 on a difference
  tools/felucca_shared.py pull [FELUCCA]    copy Felucca's versions in (then review the diff and `make ui`)

Pendel's own deltas are listed in DELTAS: a shared file that differs only there still counts as in step.
"""
import difflib, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Pendel path -> Felucca path
SHARED = {
    "src/ui/gfx.c": "firmware/src/gfx.c",
    "tools/aa_raster.py": "tools/aa_raster.py",
    "tools/gen_aa_font.py": "tools/gen_aa_font.py",
    "tools/gen_ui_palettes.py": "tools/gen_ui_palettes.py",
    "assets/fonts/InterTight[wght].ttf": "assets/fonts/InterTight[wght].ttf",
    "assets/fonts/OFL.txt": "assets/fonts/OFL.txt",
}
# Pendel's deltas, as (file, Pendel's text, Felucca's text): the big tables placed by UI_POOL (the host tests put them in
# .bss). (The canvas's size, a 240 x 40 strip, is set by ui.c before it includes gfx.c, through Felucca's CV_MAX.)
DELTAS = {
    "src/ui/gfx.c": [
        ("/* Small-canvas renderer (no full framebuffer), from Felucca (same author), kept in step with it. Pendel: the "
         "canvas is\n * a 240 x 40 strip; a screen is drawn strip by strip with cv_oy (ui.c draw_screen), and UI_POOL "
         "places the big tables.\n * Small-canvas renderer (no full framebuffer). Draw text/lines into",
         "/* Small-canvas renderer (no full framebuffer). Draw text/lines into"),
        ("#ifndef UI_POOL\n#define UI_POOL __attribute__((section(\".pool\")))\n#endif\n#ifndef CV_MAX",
         "#ifndef CV_MAX"),
        ("static uint16_t cv_px[CV_MAX] UI_POOL;", "static uint16_t cv_px[CV_MAX] __attribute__((section(\".pool\")));"),
        ("hc_tab[HC_FONTS] UI_POOL;", "hc_tab[HC_FONTS] __attribute__((section(\".pool\")));"),
        ("} kr UI_POOL;", "} kr __attribute__((section(\".pool\")));"),
    ],
}


def as_felucca(rel, data):
    """Pendel's file with its deltas undone: what it would be in Felucca"""
    if rel not in DELTAS:
        return data
    text = data.decode()
    for ours, theirs in DELTAS[rel]:
        text = text.replace(ours, theirs)
    return text.encode()


def as_pendel(rel, data):
    if rel not in DELTAS:
        return data
    text = data.decode()
    for ours, theirs in DELTAS[rel]:
        text = text.replace(theirs, ours)
    return text.encode()


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "pull"):
        sys.exit(__doc__)
    fel = Path(sys.argv[2]).expanduser() if len(sys.argv) > 2 else Path.home() / "Felucca"
    rev = subprocess.run(["git", "-C", str(fel), "describe", "--tags", "--always"], capture_output=True,
                         text=True).stdout.strip()
    bad = 0
    for ours, theirs in SHARED.items():
        a, b = ROOT / ours, fel / theirs
        if sys.argv[1] == "pull":
            a.write_bytes(as_pendel(ours, b.read_bytes()))
            print(f"pulled {ours} from Felucca {rev}")
            continue
        same = as_felucca(ours, a.read_bytes()) == b.read_bytes()
        print(f"{'same   ' if same else 'DIFFERS'} {ours}")
        if not same:
            bad += 1
            if not ours.endswith(".ttf"):
                sys.stdout.writelines(list(difflib.unified_diff(
                    b.read_text().splitlines(True), as_felucca(ours, a.read_bytes()).decode().splitlines(True),
                    f"Felucca {rev}: {theirs}", f"Pendel: {ours}", n=1))[:40])
    if sys.argv[1] == "check":
        print(f"{len(SHARED) - bad} of {len(SHARED)} in step with Felucca {rev}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
