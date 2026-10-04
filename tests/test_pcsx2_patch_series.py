#!/usr/bin/env python3
"""The PCSX2 bridge patch series (tools/patches/pcsx2/) reproduces the locked patch.

SeriesTests (asset-free, always run): the series is complete and ordered, its concatenation equals the
lock's SHA-256 and the tracked assembled artifact, and reordering / dropping a patch is caught.
TreeTests (need a PCSX2 checkout with the locked revision: PCSX2_SOURCE_DIR or build-deps/pcsx2-2.8.2;
CI fetches it): applying the series one patch at a time gives the git tree recorded in the lock, equal to
applying the single patch.
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import pcsx2_patch_series as series  # noqa: E402

LOCKED_SHA256 = "a5d0088d69219ef4703863d38c8abb81d1fdb5b7ae7bbdcea3d9a1b0eb5ff281"
PCSX2 = Path(os.environ.get("PCSX2_SOURCE_DIR", ROOT / "build-deps/pcsx2-2.8.2"))


class SeriesTests(unittest.TestCase):
    def test_concatenation_equals_lock_and_artifact(self):
        data = series.assemble()
        self.assertEqual(hashlib.sha256(data).hexdigest(), LOCKED_SHA256)
        self.assertEqual(series.lock()[0], LOCKED_SHA256)
        self.assertEqual(series.ARTIFACT.read_bytes(), data)

    def test_check_command_passes(self):
        done = subprocess.run([sys.executable, str(ROOT / "scripts/pcsx2_patch_series.py"), "check"],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_series_is_complete_and_each_patch_starts_at_a_diff(self):
        files = series.series_files()
        self.assertEqual(len(files), 11)
        for file in files:
            self.assertTrue(file.read_bytes().startswith(b"diff --git "), file.name)

    def test_reordering_or_dropping_changes_the_hash(self):
        files = series.series_files()
        blobs = [f.read_bytes() for f in files]
        swapped = blobs[:1] + [blobs[2], blobs[1]] + blobs[3:]
        self.assertNotEqual(hashlib.sha256(b"".join(swapped)).hexdigest(), LOCKED_SHA256)
        self.assertNotEqual(hashlib.sha256(b"".join(blobs[:-1])).hexdigest(), LOCKED_SHA256)


@unittest.skipUnless((PCSX2 / ".git").exists() or (PCSX2 / "HEAD").exists(), "no PCSX2 checkout")
class TreeTests(unittest.TestCase):
    def test_series_applies_to_the_locked_tree(self):
        _, pcsx2 = series.lock()
        files = series.series_files()
        by_series = series.patched_tree(PCSX2, pcsx2["revision"], [f.read_bytes() for f in files])
        by_single = series.patched_tree(PCSX2, pcsx2["revision"], [series.assemble()])
        self.assertEqual(by_series, by_single)
        self.assertEqual(by_series, pcsx2["bridge_patched_tree"])


if __name__ == "__main__":
    unittest.main()
