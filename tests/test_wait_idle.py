#!/usr/bin/env python3
"""Asset-free test of the RR5 job-table wait idle skip in src/product/patches.cpp.

The wrapper around the poll 0x298F28 (called by the wait 0x298EB0) must be
defined before registerPatches, registered exactly once inside it, and may only
skip time for the wait's own call site.
"""
from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / "src/product/patches.cpp"
FN_ANCHOR = "void registerPatches(PS2Runtime& runtime)\n"


def wrapper(text: str) -> str:
    start = text.index("static void patch_0x298f28(")
    end = text.index("\n}\n", start) + 3
    return text[start:end]


class WaitIdleTests(unittest.TestCase):
    def setUp(self):
        self.src = PATCHES.read_text()

    def test_wrapper_defined_before_and_registered_once_in_register_patches(self):
        s = self.src
        self.assertEqual(s.count("static void patch_0x298f28("), 1)
        self.assertEqual(s.count("runtime.registerFunction(0x298F28u, patch_0x298f28);"), 1)
        self.assertEqual(s.count(FN_ANCHOR), 1)
        self.assertLess(s.index("static void patch_0x298f28("), s.index(FN_ANCHOR))
        self.assertGreater(s.index("runtime.registerFunction(0x298F28u, patch_0x298f28);"), s.index(FN_ANCHOR))
        self.assertIn("g_jobPoll298f28 = runtime.hasFunction(0x298F28u) ? runtime.lookupFunction(0x298F28u) : nullptr;", s)
        self.assertIn("constexpr uint64_t kMaxIdleSkip = 4096;", s)

    def test_skip_is_bounded_to_the_wait_call_site(self):
        fn = wrapper(self.src)
        self.assertIn("GPR_U32(ctx, 31) == 0x298EE0u", fn)
        self.assertIn("GPR_U32(ctx, 2) != 0u", fn)
        self.assertIn("std::min<uint64_t>(next - now, kMaxIdleSkip)", fn)
        self.assertIn("gate3TemporalCheckpointV1(ctx)", fn)
        # The generated poll always runs first, unchanged, before any skip.
        self.assertLess(fn.index("g_jobPoll298f28(rdram, ctx, runtime)"), fn.index("gate3ChargeHleV1"))
        # Callers other than the wait return without skipping.
        self.assertRegex(fn, r"if \(!fromWait\b")

    def test_single_application(self):
        self.assertEqual(len(re.findall(r"0x298f28", self.src, re.I)) >= 3, True)
        self.assertEqual(self.src.count("g_jobPoll298f28 = "), 2)  # initialiser + registration lookup


if __name__ == "__main__":
    unittest.main()
