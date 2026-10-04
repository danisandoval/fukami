#!/usr/bin/env python3
"""pine_watch.py — sample EE words/floats live from PCSX2 and print on change.

Why this exists (B7): reference *builder* state can only be read **live**. A
`.p2s` savestate and a `.gs` capture are different runs, and per-frame build
buffers are rewritten every frame, so anything scraped out of a savestate image
is stale (docs/TESTING.md §B6 method limit). This samples the running emulator
instead, which makes an RRV-vs-GT comparison of guest state exact.

The alignment trap this tool exists to avoid
-------------------------------------------
PCSX2 *resumes* after `-statefile`, so if you sleep before sampling you measure
a scene the anchor has already left. Measured 2026-08-12: sleeping 25 s past the
A2 anchor moved `scenePhase` from 2 to 0x1F. Two rules:

1. Start sampling immediately — this script polls from the first accepted
   connection, so the first rows *are* the anchor.
2. Slow the emulator right down so the anchor window is wide:
   `NominalScalar = 0.05` in `[Framerate]` of PCSX2.ini (5 % speed).

Setup and MANDATORY cleanup are in scripts/pcsx2_anchor.md — restore
`EnablePINE = false` and `NominalScalar = 1`, and remove $TMPDIR/pcsx2.sock.

Usage:
    python3 scripts/pine_watch.py --secs 60 \
        name=0x334EC0 phase=0x334E94 blend:f=0x334F68

Each argument is `label[:f]=addr`; `:f` reads it as a float instead of a u32.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import Pine  # noqa: E402


def parse_spec(spec):
    if "=" not in spec:
        raise argparse.ArgumentTypeError(f"expected label[:f]=addr, got {spec!r}")
    label, addr = spec.split("=", 1)
    as_float = label.endswith(":f")
    if as_float:
        label = label[:-2]
    return label, int(addr, 0), as_float


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("specs", nargs="+", type=parse_spec,
                    help="label[:f]=addr  (a ':f' suffix reads a float)")
    ap.add_argument("--secs", type=float, default=60.0)
    ap.add_argument("--interval", type=float, default=0.05)
    ap.add_argument("--all", action="store_true",
                    help="print every sample, not only changes")
    args = ap.parse_args()

    t0 = time.time()
    last = None
    p = None
    while time.time() - t0 < args.secs:
        try:
            if p is None:
                p = Pine().connect()
            vals = tuple(p.read_f32(a) if f else p.read32(a)
                         for _, a, f in args.specs)
        except Exception:
            # PCSX2 not up yet, or restarting — keep trying rather than exiting,
            # so the tool can be launched before the emulator.
            p = None
            time.sleep(0.2)
            continue
        if args.all or vals != last:
            row = "  ".join(
                f"{label}={v:g}" if f else f"{label}=0x{v:X}"
                for (label, _, f), v in zip(args.specs, vals))
            print(f"[{time.time() - t0:7.2f}s] {row}", flush=True)
            last = vals
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
