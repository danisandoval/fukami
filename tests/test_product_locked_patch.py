#!/usr/bin/env python3
"""Ensure the direct-product guard checks the currently locked producer patch."""
from __future__ import annotations

import importlib.util
import pathlib
import re
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("product_guard", ROOT / "scripts/check_product_direct.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class LockedProducerPatchTest(unittest.TestCase):
    def test_selected_patch_is_guarded_even_with_valid_historical_patch(self):
        original_lock = (ROOT / "config/dependencies.lock.toml").read_text()
        original_path = re.search(r'(?m)^compatible_patch\s*=\s*"([^"]+)"', original_lock).group(1)
        with tempfile.TemporaryDirectory(prefix="rrv-locked-patch-") as tmp:
            root = pathlib.Path(tmp)
            for directory in ("src", "cmake"):
                (root / directory).symlink_to(ROOT / directory, target_is_directory=True)
            (root / "tools/patches").mkdir(parents=True)
            (root / "tools/pcsx2-gs-bridge").symlink_to(ROOT / "tools/pcsx2-gs-bridge", target_is_directory=True)
            # Keep the old hardcoded filename valid, so examining it would miss
            # the intentionally invalid selected patch.
            original = (ROOT / original_path).read_text()
            (root / "tools/patches/ps2recomp-d52-compatible-v1.patch").write_text(original)
            selected = "tools/patches/selected-producer.patch"
            (root / "config").mkdir()
            (root / "config/dependencies.lock.toml").write_text(re.sub(
                r'(?m)^compatible_patch\s*=\s*"[^"]+"',
                f'compatible_patch = "{selected}"', original_lock))
            (root / selected).write_text("")
            errors = guard.source_gate(root)
            self.assertTrue(any(selected in error and "serviceDirectPresentation" in error for error in errors), errors)
            (root / selected).write_text(original)
            self.assertEqual(guard.source_gate(root), [])
            (root / selected).unlink()
            self.assertIn(f"missing required product source: {selected}", guard.source_gate(root))


if __name__ == "__main__":
    unittest.main()
