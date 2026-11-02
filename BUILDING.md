# Building Pendel

## Host: tests, screens, simulator

Needs a C11 compiler, Python 3 with Pillow and fontTools, and (for the simulator) Emscripten.

| Command | What it does |
| --- | --- |
| `make` | builds and runs the DSP tests (`tests/test_dsp.c`, ASan and UBSan) |
| `make wav` | the same, and writes their renders to `build/wav/` |
| `make ui` | renders a scripted session's screens to `build/ui/*.png` in each palette, with the layout checks |
| `make sim` | builds `sim/pendel.wasm`; serve `sim/` over HTTP and open `index.html` |
| `make FS=48000` | the tables for another sample rate (the Pocket AMP runs at 44118 Hz) |

## Pocket AMP firmware

Needs the JieLi pi32v2 toolchain (Linux; on macOS through Docker) and the submodule `firmware/bsp` (pendel-bsp):
clone with `--recurse-submodules`, or run `git submodule update --init`.

    make ui                                   # the generated headers (build/gen/*.h, build/pd_tables.h)
    JIELI_TOOLCHAIN=/path/to/toolchain firmware/pa/build.sh

The result is `build/pa/pendel.bin`, an app image linked at 0x02000120. `firmware/` and the BSP are built at `-Os`
(as Felucca builds them), the DSP and the screen at `-O2`. `PENDEL_CFLAGS` adds flags:

| Flag | Default | What it does |
| --- | --- | --- |
| `PENDEL_PA_VOL` | 1 | read the volume knob (PA10) every 20 ms |
| `PENDEL_PA_FRAME` | 1 | draw the screen |

`PENDEL_FW_OPT` sets the optimisation of `firmware/` (default `-Os`; `-O2` works too).

## Releases

    PENDEL_RELEASE=1.0.0-beta JIELI_TOOLCHAIN=/path/to/toolchain firmware/pa/build.sh

A release build sets the version (`v1.0.0-beta`, on the splash and in MENU > VERSION) and writes
`build/release-1.0.0-beta/`: `pendel-1.0.0-beta-app.bin`, `SHA256SUMS`, `LICENSE`, `LICENSING.md` and `LICENSES/`
(with pendel-bsp's `NOTICE`). A release holds the app only, never a flash image: an image carries the unit's own boot
loader and data, so each user makes theirs from their own backup (below). Tags are `vX.Y.Z` (`v1.0.0-beta`).

## Installing on a Pocket AMP

At your own risk. Pendel never writes the flash itself; installing goes through the chip's USB boot loader with
[jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (installed separately, in its own Python environment) and
root. Below, `JL` stands for `sudo JLTOOL=/path/to/jl-uboot-tool /path/to/jl-uboot-tool/.venv/bin/python`, and APP for
the app: a release's `pendel-X-app.bin` (check it against `SHA256SUMS`) or your own `build/pa/pendel.bin`.

1. Enter the boot loader: `tools/pa_dev.py uboot` (a SysEx message from the computer; it works from the original firmware
   and from Pendel; needs `pip install mido python-rtmidi`).
2. Back up the whole flash first: `JL tools/pa_uboot_dump.py` (two reads, compared). Keep this file: it is the only way
   back to your unit's own firmware.
3. Make the image: `python3 tools/pa_make_image.py BACKUP.bin APP OUT.bin` (standard library only). Only the app area
   changes; the boot loader, the settings and the data stay those of your backup.
4. Write it: `JL tools/pa_uboot_write.py --ref BACKUP.bin --image OUT.bin --write` (the app area only, every sector read
   back). Over an earlier Pendel, add `--was EARLIER.bin` (the image you wrote then). `--image BACKUP.bin` puts your own
   firmware back.

For a bench session, `tools/pa_uboot_daemon.py` (run once with `JL`) takes the same jobs from `tools/pa_uboot_job.py`
without root.

A crash, a main loop that stops for 3 s, or two boots in a row that never reach 2 s drop the unit into the boot loader,
from where an image can always be written again. A power-on or the power key's long press starts the count afresh:
the Pocket AMP's battery keeps the RAM through a power-off, and a count left there would send the next power-on to the
boot loader.

## Controls

| Control | What it does |
| --- | --- |
| PARAM (turn) | the value in focus: on HOME an effect's first value, the AMP's gain, the TAPE level or the input gain (the scope); in a list its row |
| PARAM (push) | opens what is in focus (the scope: MENU at the input's rows); on the TAPE band it works the tape: a tap records, closes the take, overdubs; held 0.7 s it stops, 2 s it clears |
| LEFT / RIGHT | the focus, on every page: on HOME EFX1, EFX2, EFX3, AMP, the TAPE band, the scope; a row in a list |
| HOME | home; held 0.7 s: MENU (input gain, gate, low cut, colours, levels); in MENU a tap closes it |
| VOL- / VOL+ | the master level, repeating while held |

## Console

Pendel opens a USB serial console: `p` status (audio and frame times, keys, volume, the page), `z` clear the maxima,
`8 2 4 6 5 0 7` the keys (LEFT, RIGHT, the knob down, the knob up, PUSH, the tape, HOME; `+` before one makes it a
long press), `a` every ADC channel, `e` the encoder's line changes (state and time) and the timer's longest gap,
`u` the boot loader, `r` reboot. The status line also gives the frame's and the panel's times, the strips sent, the
battery's reading, and the boot's phases with the previous boot's trace (`prev_*`: how far it got, why it left for
the boot loader, the reset's cause).
