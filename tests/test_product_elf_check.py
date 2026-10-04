#!/usr/bin/env python3
"""The product refuses a game ELF that is not the one the recompiled code was extracted from.

Needs a built source-owned product binary and the user's own ELF, so it is input-dependent:
  RRV_PRODUCT_BIN=<path to candidate-bin/Fukami or runtime/*/bin/Fukami>
  RRV_GAME_ELF=<path to SLUS_200.02>     (default: local/rrv_boot.elf)
The check runs before any window, audio or GS backend starts, so this needs no display. Verifies the
expected-hash source (generated/rr5/source-manifest.json) too.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("RRV_PRODUCT_BIN", ""))
ELF = Path(os.environ.get("RRV_GAME_ELF", ROOT / "local/rrv_boot.elf"))
EXPECTED = json.loads((ROOT / "generated/rr5/source-manifest.json").read_text())["game_input"]["sha256"]


@unittest.skipUnless(BIN.is_file() and ELF.is_file(), "needs RRV_PRODUCT_BIN and the user's ELF")
class ElfCheck(unittest.TestCase):
    def run_product(self, elf: Path, timeout=40):
        return subprocess.run([str(BIN), str(elf)], capture_output=True, text=True, timeout=timeout,
                              env={**os.environ, "RRV_GATE3_HEADLESS": "1"})

    def test_modified_elf_is_refused_before_anything_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "SLUS_200.02"
            data = bytearray(ELF.read_bytes())
            data[len(data) // 2] ^= 0xFF
            bad.write_bytes(bytes(data))
            done = self.run_product(bad)
            self.assertEqual(done.returncode, 3, done.stderr[-600:])
            self.assertIn("not the game ELF this build was made from", done.stderr)
            self.assertIn(EXPECTED, done.stderr)
            self.assertNotIn("SDL", done.stderr)

    def test_missing_elf_is_refused(self):
        done = self.run_product(Path("/nonexistent/SLUS_200.02"))
        self.assertEqual(done.returncode, 3)
        self.assertIn("cannot open the game ELF", done.stderr)

    def test_the_real_elf_passes_the_check(self):
        self.assertEqual(subprocess.run(["shasum", "-a", "256", str(ELF)], capture_output=True, text=True)
                         .stdout.split()[0], EXPECTED)
        # No bound workload: the run stops later for another reason, but only after the check passed.
        done = self.run_product(ELF)
        self.assertIn(f"game ELF verified sha256={EXPECTED}", done.stderr)


if __name__ == "__main__":
    unittest.main()
