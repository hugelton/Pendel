#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Build Pendel for the Pocket AMP: build/pa/pendel.bin (an app.bin for tools/pa_make_image.py).
#   JIELI_TOOLCHAIN=~/.fm1/jieli/toolchain firmware/pa/build.sh
# Needs the generated headers first: make ui (build/gen/*.h, build/pd_tables.h).
# PENDEL_CFLAGS adds flags (e.g. -DPENDEL_PA_VOL=0 for a test build).
# PENDEL_RELEASE=X.Y.Z[-suffix] makes a release build (the version string vX.Y.Z[-suffix]) and build/release-X.Y.Z[-suffix]/:
# the app, SHA256SUMS and the licences. No flash image: an image holds the unit's own boot loader and data, so each
# user makes theirs from their own backup (tools/pa_make_image.py).
set -e
if [ -n "$PENDEL_RELEASE" ]; then
    echo "$PENDEL_RELEASE" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$' ||
        { echo "PENDEL_RELEASE=$PENDEL_RELEASE: use X.Y.Z or X.Y.Z-suffix" >&2; exit 1; }
    PENDEL_CFLAGS="$PENDEL_CFLAGS -DPENDEL_VERSION=\"v$PENDEL_RELEASE\""
fi
TC="${JIELI_TOOLCHAIN:?JIELI_TOOLCHAIN must point at the JieLi Linux toolchain}"
BSP="${PENDEL_BSP:-$(cd "$(dirname "$0")/../bsp" && pwd)}"   # the submodule firmware/bsp, or PENDEL_BSP
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
O="$ROOT/build/pa"
rm -rf "$O"; mkdir -p "$O"
CC="$TC/pi32v2/bin/clang -target pi32v2"
CFLAGS="$PENDEL_CFLAGS -ffunction-sections -fno-builtin -Wall -Wno-unused-function -I$BSP/chip -I$BSP/boards/pocketamp \
 -I$BSP/boards/fm1 -I$ROOT/src/app -I$ROOT/src/ui -I$ROOT/src/dsp -I$ROOT/build -I$ROOT/build/gen"
$CC -c "$BSP/chip/crt0.S" -o "$O/crt0.o"
$CC -c "$BSP/chip/fm1_vec.S" -o "$O/vec.o"
$CC -c "$BSP/chip/fm1_isr.S" -o "$O/isr.o"
OBJS="$O/crt0.o $O/vec.o $O/isr.o"
for f in firmware/pa/main.c src/app/app.c src/ui/ui.c src/dsp/pd_core.c src/dsp/pd_osc.c src/dsp/pd_modules.c src/dsp/pd_effects.c \
         src/dsp/pd_rack.c src/dsp/pd_looper.c; do
    o="$O/$(basename "$f" .c).o"
    case "$f" in
        firmware/*) OPT=${PENDEL_FW_OPT:--Os} ;;    # the glue and the BSP it includes, as Felucca builds them
        *) OPT=-O2 ;;             # the DSP and the UI
    esac
    $CC $OPT $CFLAGS -c "$ROOT/$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TC/pi32v2/bin/ld" --gc-sections -T "$HERE/app.ld" $OBJS -o "$O/pendel.elf"
"$TC/common/bin/objcopy" -O binary -j .text "$O/pendel.elf" "$O/text.bin"
"$TC/common/bin/objcopy" -O binary -j .data "$O/pendel.elf" "$O/data.bin"
"$TC/common/bin/objcopy" -O binary -j .ram_text "$O/pendel.elf" "$O/rt.bin"
"$TC/common/bin/objdump" -t "$O/pendel.elf" > "$O/syms.txt"
"$TC/common/bin/objdump" -h "$O/pendel.elf" > "$O/sections.txt"
python3 - "$O" <<'PY'
import re, sys
out = sys.argv[1]
syms = open(f"{out}/syms.txt").read()
sym = lambda n: int(re.search(r"^([0-9a-f]+) .*\s" + n + "$", syms, re.M).group(1), 16)
assert sym("_start") == 0x02000120, hex(sym("_start"))
img = bytearray(open(f"{out}/text.bin", "rb").read())
for at, part in sorted([(sym("_rt_load"), open(f"{out}/rt.bin", "rb").read()), (sym("_data_load"), open(f"{out}/data.bin", "rb").read())]):
    if part:
        assert at - 0x02000120 >= len(img), "sections overlap"
        img += b"\xff" * (at - 0x02000120 - len(img)) + part
img += b"\xff" * (-len(img) % 4)
open(f"{out}/pendel.bin", "wb").write(img)
print(f"pendel.bin: {len(img)} bytes; bss {sym('_bss_end') - sym('_bss_start')} B (of 72 KiB), "
      f"pool {sym('_pool_end') - sym('_pool_start')} B (of {0x5A000} B)")
PY
if [ -n "$PENDEL_RELEASE" ]; then
    python3 - "$ROOT" "$BSP" "$PENDEL_RELEASE" <<'PY'
import hashlib, shutil, sys
from pathlib import Path
root, bsp, ver = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
rel = root / "build" / f"release-{ver}"
shutil.rmtree(rel, ignore_errors=True)
(rel / "LICENSES").mkdir(parents=True)
app = f"pendel-{ver}-app.bin"
data = (root / "build" / "pa" / "pendel.bin").read_bytes()
(rel / app).write_bytes(data)
(rel / "SHA256SUMS").write_text(f"{hashlib.sha256(data).hexdigest()}  {app}\n")
for f in sorted((root / "LICENSES").glob("*.txt")):
    shutil.copy(f, rel / "LICENSES" / f.name)
for need in ("MPL-2.0-pendel-bsp.txt", "OFL-InterTight.txt", "MIT-Fukiai.txt"):
    if not (rel / "LICENSES" / need).exists():
        raise SystemExit(f"release: LICENSES/{need} is missing")
shutil.copy(bsp / "NOTICE", rel / "LICENSES" / "NOTICE-pendel-bsp.txt")      # the hardware layer's authors and origins
for doc in ("LICENSE", "LICENSING.md"):
    shutil.copy(root / doc, rel / doc)
print(f"release  {rel}/  {app} {len(data)} B")
PY
fi
