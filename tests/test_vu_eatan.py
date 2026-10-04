#!/usr/bin/env python3
"""Asset-free test of the committed EATAN helpers (RRV_VU_EATAN_MVU).

Extracts the helper block from third_party/ps2recomp/ps2xRuntime/src/lib/ps2_vu1.cpp,
compiles it and checks them bit for bit against a
float32 model of PCSX2 microVU's mVU_EATAN_ / mVU_EATANxy (pcsx2 @ d5f75c9e4),
plus the semantic property the fix is for: EATANxy is a real arctangent of
y/x, not the interpreter's series(y/x) + pi/4 (known_issues VIS-007).
RRV_VU_EATAN_MVU=0 must report the switch off. The call sites in ps2_vu1.cpp
are asserted too.
"""
from __future__ import annotations

import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VU1 = ROOT / "third_party/ps2recomp/ps2xRuntime/src/lib/ps2_vu1.cpp"


def helpers() -> str:
    """From the RRV_VU_EATAN_MVU comment block through the end of vuMvuEatanRatio."""
    text = VU1.read_text()
    start = text.index("// RRV_VU_EATAN_MVU")
    sig = text.index("static float vuMvuEatanRatio(float a, float x)")
    end = text.index("\n}\n", sig) + 3
    return text[start:end]

T = [0x3f7ffff5, 0x3e4c40a6, 0xbe0e6c63, 0x3dc577df,
     0xbeaaa61c, 0xbd6501c4, 0x3cb31652, 0xbb84d7e7]
PI4 = 0x3f490fdb


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fb(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def mvu_eatan_ratio(a: float, x: float) -> int:
    """microVU: t = (a - x) / (x + a); PQ = t*T1; t2 *= t twice, PQ += t2*Tn; + Pi4."""
    t = f32(f32(f32(a) - f32(x)) / f32(f32(x) + f32(a)))
    pq = f32(t * fb(T[0]))
    t2 = t
    for n in range(1, 8):
        t2 = f32(t2 * t)
        t2 = f32(t2 * t)
        pq = f32(pq + f32(t2 * fb(T[n])))
    return bits(f32(pq + fb(PI4)))


# (a, x): EATANxy(x, y=a); EATAN(v) is (v, 1.0).
CASES = [(1.0, 1.0), (0.0, 2.0), (0.25, 1.0), (40.0, 3000.0), (3000.0, 40.0),
         (12.5, 7502.5), (-3.0, 5.0), (0.001, 1.0), (7.0, 7.5)]

MAIN = r'''
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
  std::printf("%d\n", vuEatanMicroVu() ? 1 : 0);
  for (int i = 1; i + 1 < argc; i += 2) {
    const float a = std::strtof(argv[i], nullptr), x = std::strtof(argv[i + 1], nullptr);
    const float p = vuMvuEatanRatio(a, x);
    uint32_t b; std::memcpy(&b, &p, 4); std::printf("%08x\n", b);
  }
  return 0;
}
'''

PRELUDE = r'''
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
static float vuFlushDenorm(float f) { return f; }
'''


class VuEatanTests(unittest.TestCase):
    def build(self, directory: str) -> Path:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")
        cpp = Path(directory) / "t.cpp"
        exe = Path(directory) / "t"
        cpp.write_text(PRELUDE + helpers() + MAIN)
        subprocess.run([compiler, "-std=c++20", "-O2", "-ffp-contract=off", str(cpp), "-o", str(exe)],
                       check=True)
        return exe

    def test_helpers_match_microvu_bit_for_bit(self):
        with tempfile.TemporaryDirectory(prefix="rrv-vu-eatan-") as directory:
            exe = self.build(directory)
            args = [str(exe)] + [f"{v:.9g}" for case in CASES for v in case]
            out = subprocess.run(args, capture_output=True, text=True, check=True).stdout.split()
            self.assertEqual(out[0], "1", "default must be on")
            for (a, x), got in zip(CASES, out[1:]):
                self.assertEqual(int(got, 16), mvu_eatan_ratio(a, x), f"EATAN ratio a={a} x={x}")
            off = subprocess.run([str(exe)], capture_output=True, text=True, check=True,
                                 env=dict(os.environ, RRV_VU_EATAN_MVU="0")).stdout.split()
            self.assertEqual(off[0], "0", "RRV_VU_EATAN_MVU=0 is the rollback")

    def test_is_close_to_a_real_arctangent(self):
        # The defect: the interpreter shape, series(y/x) + pi/4, is ~pi/4 out
        # for small angles. microVU applies its constants one slot shifted
        # (T2 = 0.1995 on x^3, T5 = -1/3 on x^9), which leaves up to ~0.1 rad;
        # that is what the reference renders with, so it is kept.
        self.assertEqual(mvu_eatan_ratio(1.0, 1.0), PI4)
        for a, x in ((40.0, 3000.0), (0.25, 1.0), (3000.0, 40.0), (0.8, 1.0)):
            err = abs(fb(mvu_eatan_ratio(a, x)) - math.atan(a / x))
            self.assertLess(err, 0.1, (a, x))
        self.assertLess(abs(fb(mvu_eatan_ratio(40.0, 3000.0)) - math.atan(40.0 / 3000.0)), 0.06)

    def test_call_sites_present_once(self):
        vu1 = VU1.read_text()
        self.assertEqual(vu1.count("vuMvuEatanRatio(vuFlushDenorm(m_state.vf[is][1]), x)"), 1)
        self.assertEqual(vu1.count("vuMvuEatanRatio(vuFlushDenorm(m_state.vf[is][2]), x)"), 1)
        self.assertEqual(vu1.count("vuMvuEatanRatio(v, 1.0f)"), 1)
        self.assertEqual(vu1.count("static float vuMvuEatanRatio("), 1)


if __name__ == "__main__":
    unittest.main()
