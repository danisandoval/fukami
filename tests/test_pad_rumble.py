#!/usr/bin/env python3
"""Asset-free test of the DualShock 2 vibration decode in the committed runtime.

Extracts dualShockRumble from third_party/ps2recomp/ps2xRuntime/src/lib/Kernel/Stubs/Pad.cpp,
compiles it and checks it against PCSX2's PadDualshock2 decode (pcsx2 @ d5f75c9e4,
PadDualshock2.cpp VibrationMap and Poll case 4): align byte 0/1 pick the
direct-data byte for the small/large motor, 0xff leaves a motor off, the small
motor is its byte's LSB at full power and the large motor is byte / 255.
Also asserts the actuator handling is wired in the committed sources.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "third_party/ps2recomp/ps2xRuntime"
PAD = RT / "src/lib/Kernel/Stubs/Pad.cpp"
PAD_H = RT / "include/runtime/ps2_pad.h"
PAD_CPP = RT / "src/lib/ps2_pad.cpp"


def extract_function(text: str, signature: str) -> str:
    """Return the comment block above `signature` through its closing brace."""
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    else:
        raise AssertionError("unbalanced braces")
    lines = text[:start].splitlines(keepends=True)
    head = ""
    while lines and lines[-1].lstrip().startswith("//"):
        head = lines.pop() + head
    return head + text[start:end]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct HostPadRumble { float lowFrequency = 0.0f; float highFrequency = 0.0f; };
namespace {
%s
}
int main(int argc, char **argv)
{
    const uint8_t data[6] = {uint8_t(std::strtoul(argv[3], nullptr, 0)), uint8_t(std::strtoul(argv[4], nullptr, 0)), 0, 0, 0, 0};
    const HostPadRumble r = dualShockRumble(uint8_t(std::strtoul(argv[1], nullptr, 0)),
                                            uint8_t(std::strtoul(argv[2], nullptr, 0)), data);
    std::printf("%%.6f %%.6f\n", r.lowFrequency, r.highFrequency);
    return argc == 5 ? 0 : 1;
}
'''


def pcsx2(small_align: int, large_align: int, d0: int, d1: int) -> tuple[float, float]:
    motors = [d0, d1]
    large = motors[large_align] if large_align in (0, 1) else 0
    small = motors[small_align] & 1 if small_align in (0, 1) else 0
    return large / 255.0, 1.0 if small else 0.0


CASES = [(0xff, 0xff, 1, 255), (0, 1, 1, 255), (0, 1, 0, 128), (0, 1, 2, 0),
         (0, 1, 3, 64), (1, 0, 200, 1), (0, 0xff, 1, 255), (0xff, 1, 1, 17)]


class PadRumbleTests(unittest.TestCase):
    def test_decode_matches_pcsx2(self):
        cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        self.assertIsNotNone(cxx, "a C++ compiler is required")
        helper = extract_function(PAD.read_text(), "HostPadRumble dualShockRumble(")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "rumble.cpp"
            binary = Path(directory) / "rumble"
            source.write_text(HARNESS % helper)
            subprocess.run([cxx, "-std=c++17", "-O1", str(source), "-o", str(binary)], check=True)
            for case in CASES:
                out = subprocess.run([str(binary), *(str(v) for v in case)], check=True,
                                     capture_output=True, text=True).stdout.split()
                low, high = (float(v) for v in out)
                want_low, want_high = pcsx2(*case)
                self.assertAlmostEqual(low, want_low, places=5, msg=case)
                self.assertEqual(high, want_high, msg=case)

    def test_actuator_handling_is_wired(self):
        pad = PAD.read_text()
        self.assertEqual(pad.count("dualShockRumble(smallAlign, largeAlign, data.data())"), 1)
        self.assertIn("uint8_t smallMotorAlign = 0xffu;", pad)
        self.assertIn("uint8_t largeMotorAlign = 0xffu;", pad)
        self.assertIn("portState->smallMotorAlign = align[0];", pad)
        self.assertIn("portState->largeMotorAlign = align[1];", pad)
        self.assertIn("setRumble(port, HostPadRumble{})", pad)  # scePadEnd stops vibration
        self.assertNotIn("I4 deferred", pad)
        self.assertIn("void setRumble(int port, HostPadRumble rumble);", PAD_H.read_text())
        cpp = PAD_CPP.read_text()
        self.assertEqual(cpp.count("void PSPadBackend::setRumble(int port, HostPadRumble rumble)"), 1)
        self.assertIn("backend->stopRumble(static_cast<unsigned>(port));", cpp)
        self.assertIn("backend->setRumble(static_cast<unsigned>(port), rumble);", cpp)


if __name__ == "__main__":
    unittest.main()
