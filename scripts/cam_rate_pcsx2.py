#!/usr/bin/env python3
"""
cam_rate_pcsx2.py — measure PCSX2's camera-advancement rate for RRV, to compare
against our runtime's [DIAG-CAM] pathlen/s (see cont.39). Ground-truth for the
"camera slower than PCSX2" investigation.

Reads the view-inverse translation (eye world position) at EE 0x01e24e80 + 0x30,
same address our runtime probes, and reports world-units/s path length once per
wall-second plus the current eye coords (the scene fingerprint used to confirm
we're measuring the SAME flyover segment on both platforms).

Requires PCSX2 running with EnablePINE=true and a live attract flyover.
Run:  python3 scripts/cam_rate_pcsx2.py [seconds]
Hygiene: caller restores EnablePINE=false and kills PCSX2 afterward.
"""

import math
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pine import Pine  # noqa: E402

EYE = 0x01E24E80 + 0x30  # view-inverse translation column q3 (m[12..14])


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    with Pine() as p:
        prev = None
        path = 0.0
        samples = 0
        sec_start = time.monotonic()
        run_start = sec_start
        while time.monotonic() - run_start < duration:
            try:
                ex = p.read_f32(EYE + 0)
                ey = p.read_f32(EYE + 4)
                ez = p.read_f32(EYE + 8)
            except Exception as e:
                print("read error:", e)
                time.sleep(0.5)
                continue
            if prev is not None:
                dx, dy, dz = ex - prev[0], ey - prev[1], ez - prev[2]
                step = math.sqrt(dx * dx + dy * dy + dz * dz)
                # Reject scene-cut teleports: a real 60Hz camera step is <~40 units
                # even at 10x our rate; a cut jumps thousands. Count only smooth motion.
                if step < 200.0:
                    path += step
            prev = (ex, ey, ez)
            samples += 1
            now = time.monotonic()
            if now - sec_start >= 1.0:
                secs = now - sec_start
                print(f"[PCSX2-CAM] wall={secs:.2f}s eye=({ex:.2f},{ey:.2f},{ez:.2f}) "
                      f"pathlen/s={path/secs:.2f} samples={samples}")
                path = 0.0
                samples = 0
                sec_start = now
            # ~60 Hz sampling for parity with our per-frame probe
            time.sleep(1.0 / 60.0)


if __name__ == "__main__":
    main()
