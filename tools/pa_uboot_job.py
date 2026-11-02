#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Submit a job to tools/pa_uboot_daemon.py and print its output.

  tools/pa_uboot_job.py dump [--out build/backup]
  tools/pa_uboot_job.py peek [ADDR:LEN ...]
  tools/pa_uboot_job.py write --ref A.bin --image B.bin [--was C.bin] [--write]
"""
import json, sys, time
from pathlib import Path

QUEUE = Path(__file__).resolve().parents[1] / "build" / "uboot-queue"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    name = time.strftime("%Y%m%d-%H%M%S") + f"-{sys.argv[1]}"
    QUEUE.mkdir(parents=True, exist_ok=True)
    tmp = QUEUE / f"{name}.tmp"
    tmp.write_text(json.dumps({"tool": sys.argv[1], "args": sys.argv[2:]}))
    tmp.rename(QUEUE / f"{name}.job")
    done, out = QUEUE / f"{name}.done", QUEUE / f"{name}.out"
    t0 = time.time()
    while not done.exists():
        if time.time() - t0 > 660:
            sys.exit("no answer from the daemon (is it running?)")
        time.sleep(0.2)
    text = out.read_text()
    print(text, end="")
    return 0 if "[exit 0]" in text else 1


if __name__ == "__main__":
    sys.exit(main())
