# Pendel

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

## Overview

Pendel is open-source firmware for the M-VAVE Pocket AMP: an effects unit with a tape loop.

```
IN → EFX1 → EFX2 → EFX3 → AMP → TAPE
```

- **EFX1–3**: drive, fuzz, filter, auto-wah, crush, phaser, flanger, tremolo, wobble, pitch, delay, room, noise, gain
- **AMP**: clean, crunch or lead, with bass, middle, treble and a cabinet
- **TAPE**: a looper that also sets the tempo

A sibling of [Felucca](https://github.com/hugelton/Felucca) (the M-VAVE FM-1 firmware), on the same chip and the same
look. Hardware layer: [pendel-bsp](https://github.com/hugelton/pendel-bsp) (MPL-2.0).

- Build: [BUILDING.md](BUILDING.md)
- Try: the browser simulator in `sim/`

## Platform Support

| Platform | Status | Notes |
| --- | --- | --- |
| M-VAVE Pocket AMP | Beta | 1.0.0 beta. Install: [BUILDING.md](BUILDING.md). |
| Browser | In progress | The simulator in `sim/`. |

## AI disclaimer

Pendel is developed with the assistance of AI coding agents (code, tests, documentation, maintenance). The design,
features, sound design and direction are the maintainer's, and no generative AI is used for music, icons or artwork.
See [On AI-Assisted Development and Responsibility](https://github.com/hugelton/Felucca/discussions/166).

## Credits and licence

[GPL-3.0-only](LICENSE); pendel-bsp is MPL-2.0; the bundled fonts keep their own licences ([LICENSING.md](LICENSING.md)).
Thanks: [CREDITS.md](CREDITS.md).

M-VAVE and Pocket AMP are trademarks of their respective owners. Pendel is not affiliated with or endorsed by them.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
