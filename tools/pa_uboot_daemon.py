#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Root-side runner for Pocket AMP UBOOT jobs, so sudo is typed once per bench session (after FM-1's).

  sudo JLTOOL=~/.fm1/jl-uboot-tool $JLTOOL/.venv/bin/python tools/pa_uboot_daemon.py   (Ctrl-C to stop)
  tools/pa_uboot_job.py write --ref ... --image ... [--was ...] [--write]

The daemon polls build/uboot-queue/ for NAME.job (JSON: {"tool", "args"}) and runs only these tools, with argv
(no shell), as root:
  dump  -> tools/pa_uboot_dump.py   (read only)
  peek  -> tools/pa_uboot_peek.py   (read only)
  write -> tools/pa_uboot_write.py  (app area only; writes only with --write)
Every path argument must resolve inside this repository. Output goes to NAME.out (readable by the user), then
NAME.done; files the job leaves under build/ are handed back to the user.
"""
import json, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
QUEUE = ROOT / "build" / "uboot-queue"
JLTOOL = os.environ.get("JLTOOL", str(Path.home() / ".fm1/jl-uboot-tool"))
PY = str(Path(JLTOOL) / ".venv" / "bin" / "python")
TOOLS = {"dump": ROOT / "tools" / "pa_uboot_dump.py", "peek": ROOT / "tools" / "pa_uboot_peek.py",
         "write": ROOT / "tools" / "pa_uboot_write.py"}
NUM = re.compile(r"(0x[0-9a-fA-F]+|\d+)(:(0x[0-9a-fA-F]+|\d+))?")
FLAGS = {"--out", "--ref", "--image", "--was", "--write"}


def validate(job):
    tool = TOOLS.get(job.get("tool"))
    args = job.get("args", [])
    if tool is None or not isinstance(args, list) or not all(isinstance(a, str) for a in args):
        raise ValueError("unknown tool or bad args")
    for a in args:
        if a.startswith("-"):
            if a not in FLAGS:
                raise ValueError(f"flag {a} not allowed")
        elif not NUM.fullmatch(a):
            p = (ROOT / a).resolve()
            if ROOT not in p.parents:
                raise ValueError(f"path {a} outside the repository")
    return [PY, str(tool), *args]


def give_back(uid, gid):
    for d, _, files in os.walk(ROOT / "build"):
        for f in [d, *(os.path.join(d, n) for n in files)]:
            if os.stat(f).st_uid == 0:
                os.chown(f, uid, gid)


def main():
    QUEUE.mkdir(parents=True, exist_ok=True)
    uid = int(os.environ.get("SUDO_UID", os.getuid()))
    gid = int(os.environ.get("SUDO_GID", os.getgid()))
    give_back(uid, gid)
    print(f"pa UBOOT daemon: watching {QUEUE} (Ctrl-C to stop)", flush=True)
    env = dict(os.environ, JLTOOL=JLTOOL)
    while True:
        for jf in sorted(QUEUE.glob("*.job")):
            claimed = jf.with_suffix(f".run{os.getpid()}")
            try:
                jf.rename(claimed)          # atomic: only one daemon can take a job
            except OSError:
                continue
            jf = claimed
            out = jf.with_suffix(".out")
            try:
                argv = validate(json.loads(jf.read_text()))
                print(time.strftime("%H:%M:%S"), "run", " ".join(argv[1:]), flush=True)
                r = subprocess.run(argv, cwd=ROOT, env=env, capture_output=True, text=True, timeout=600)
                text = r.stdout + r.stderr + f"\n[exit {r.returncode}]\n"
            except Exception as e:  # noqa: BLE001 - report everything to the job file
                text = f"[rejected or failed: {e}]\n"
            out.write_text(text)
            jf.rename(jf.with_suffix(".done"))
            give_back(uid, gid)
            print(text.rstrip().splitlines()[-1], flush=True)
        time.sleep(0.3)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
