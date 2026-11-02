# Pendel licensing

Pendel is free software, licensed under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). That covers the code and its own assets. A few bundled parts keep their own licences; they are listed below,
and their licence texts are in `LICENSES/`.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments

## What is GPL-3.0-only

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header:

- the application: `src/` (the DSP in `src/dsp/`, the screen in `src/ui/`, the pages and keys in `src/app/`)
- the Pocket AMP firmware glue: `firmware/` (not `firmware/bsp/`, MPL-2.0: below)
- the browser simulator: `sim/`
- the web installer: `web/`
- the host build, tests and tools: `Makefile`, `host/`, `tests/`, `tools/`

`src/ui/gfx.c`, the font, icon and palette generators in `tools/`, `tools/pa_pkg_make.py`, `tools/lz4blk.py`,
`tools/pa_install.py` and `web/` come from Felucca
(<https://github.com/hugelton/Felucca>), GPL-3.0-only, by the same copyright holder.

The hardware layer (the chip: clock, interrupts, guards, flash, USB, the boot entry and libc; and the Pocket AMP's board
code: display, keys, audio) is pendel-bsp (<https://github.com/hugelton/pendel-bsp>), a branch of felucca-bsp
(<https://github.com/hugelton/felucca-bsp>), under the Mozilla Public License 2.0 (`SPDX-License-Identifier: MPL-2.0`,
`LICENSES/MPL-2.0-pendel-bsp.txt`; its authors and origins in its `NOTICE`). The firmware built with it is
GPL-3.0-only as a whole; the MPL files stay MPL-2.0, and their source is part of Pendel's complete source.

You may use, study, change and share Pendel under the GPL. If you distribute Pendel, or firmware derived from it, you
must also give your recipients its complete corresponding source under the same licence.

## Hügelton Instruments' own work

| What | Licence | Where |
| --- | --- | --- |
| Pendel: application, firmware glue, simulator, tools, tests | GPL-3.0-only | `LICENSE` |
| The Fukiai icon font (<https://github.com/hugelton/Fukiai>): the icons, rasterised at build time by `tools/gen_icons.py` | MIT | `assets/fonts/fukiai.ttf`, `LICENSES/MIT-Fukiai.txt` |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Inter Tight font by The Inter Project Authors: the UI text, rasterised into the firmware at build time (`tools/gen_aa_font.py`; the generated tables are not offered as a font, and the font declares no Reserved Font Name) | SIL OFL 1.1 | `assets/fonts/InterTight[wght].ttf`, `LICENSES/OFL-InterTight.txt` (also `assets/fonts/OFL.txt`) |
| JieLi AC79 SDK by JieLi Technology: three of its files go into every `.fwsc` package (below); none are in this tree | Apache-2.0 | `LICENSES/Apache-2.0.txt` |
| jl-uboot-tool by Andrey Grigoryev (kagaimiq) (<https://github.com/kagaimiq/jl-uboot-tool>): the USB boot-loader protocol the `tools/pa_uboot_*.py` scripts use; not in this tree, installed separately | MIT | credit only |

## JieLi SDK files in the packages (Apache-2.0)

A `.fwsc` package made by `firmware/pa/build.sh` (with `tools/pa_pkg_make.py`; this is the package the web installer
and `tools/pa_install.py` install) holds three unmodified files from the JieLi AC79 SDK
(<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>, `cpu/wl82/tools/`). They are read from your SDK checkout at build
time (see BUILDING.md); no SDK files are in this tree.

| File in the package | What it is |
| --- | --- |
| `uboot.boot` | the first-stage boot loader (SPL) |
| `cfg_tool.bin` | the chip configuration block |
| `eq_cfg_hw.bin` | the default hardware EQ table |

They are licensed under the Apache License, Version 2.0 (`LICENSES/Apache-2.0.txt`), not under the GPL, and their
copyright stays with JieLi Technology. The SDK has no NOTICE file. Apache-2.0 files may be distributed together with
GPL-3.0 code; Pendel itself stays GPL-3.0-only.

Every distribution of a package carries the licence texts with it: the release folder (`PENDEL_RELEASE=...
firmware/pa/build.sh`) and the installer site.

## No vendor material

No M-VAVE material is part of Pendel. The firmware links no vendor code and contains no vendor data, and the packages
hold only Pendel and the three SDK files above. Returning to the official firmware takes the official file you
download from M-VAVE; Pendel holds only its size and SHA-256 to recognise it. `tools/pa_make_image.py` writes Pendel
into the backup you read from your own unit (its boot loader, settings and data stay as they were); that backup is
never part of Pendel.

## Contributions

Contributions are welcome under GPL-3.0-only. Changes to the hardware layer go to pendel-bsp, under MPL-2.0.

## Trademarks

"Pendel" and "Hügelton Instruments" are names of Hügelton Instruments.

"M-VAVE" and "Pocket AMP" are trademarks of their respective owners. Pendel is independent firmware that runs on
Pocket AMP hardware. It is not affiliated with, endorsed by or supported by those owners.

## Radio

Pendel never enables the Bluetooth / Wi-Fi radio of the hardware.
