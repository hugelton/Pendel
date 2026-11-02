#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Make the installer site (after Felucca's make_site.py):

  index.html                  a redirect to installer/
  installer/index.html        web/index.html, self-contained (papkg.js, paota.js and the metadata inlined)
  firmware/pendel-VER.fwsc    the package, with LICENSE, LICENSING.md and LICENSES/ (the package holds JieLi SDK
                              files under Apache-2.0, see LICENSING.md)

  web/make_site.py PACKAGE.fwsc VERSION OUT_DIR

The package must be one made by tools/pa_pkg_make.py (Pendel's own loader); its identity (Pocket AMP_9xx) is read
from it, and the device must report it after the install. The licence files are this checkout's.
"""
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (papkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    lic = [ROOT / "LICENSE", ROOT / "LICENSING.md", *sorted((ROOT / "LICENSES").glob("*.txt"))]
    if not all(p.is_file() for p in lic) or not (ROOT / "LICENSES" / "Apache-2.0.txt").is_file():
        raise SystemExit("make_site.py: LICENSE, LICENSING.md and LICENSES/Apache-2.0.txt are needed")
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"Pocket AMP_9\d\d", product):
        raise SystemExit(f"make_site.py: {pkg}: identity {product!r}, not a Pendel package (Pocket AMP_9xx)")
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", version):
        raise SystemExit(f"make_site.py: version {version!r}: X.Y.Z or X.Y.Z-tag")

    name = f"pendel-{version}.fwsc"
    meta = {"pkg": f"../firmware/{name}", "version": version, "product": product,
            "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    lib = "\n".join(strip_module((HERE / f).read_text()) for f in ("papkg.js", "paota.js"))
    page = (HERE / "index.html").read_text()
    if page.count("/*LIB*/") != 1 or page.count("/*META*/") != 1:
        raise SystemExit("make_site.py: web/index.html needs one /*LIB*/ and one /*META*/")
    page = page.replace("/*LIB*/", lib).replace("/*META*/", json.dumps(meta))

    (out / "installer").mkdir(parents=True, exist_ok=True)
    (out / "firmware" / "LICENSES").mkdir(parents=True, exist_ok=True)
    for old in (out / "firmware").glob("pendel-*.fwsc"):
        old.unlink()
    (out / "installer" / "index.html").write_text(page)
    shutil.copyfile(pkg, out / "firmware" / name)
    for p in lic:
        shutil.copyfile(p, out / "firmware" / p.relative_to(ROOT))
    (out / "index.html").write_text('<!doctype html>\n<meta charset="utf-8">\n<title>Pendel</title>\n'
                                    '<meta http-equiv="refresh" content="0; url=installer/">\n'
                                    '<a href="installer/">Pendel installer</a>\n')
    print(f"{out}: installer for {product} ({version}), {name} {meta['sha256'][:16]}…")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
