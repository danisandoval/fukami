#!/usr/bin/env python3
"""Provenance and diagnostic-composition gates for the approved SPR fix."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from m2_causal_overlay import patch_memory
from m2_dma_overlay import locked_producer_source, memory as dma_memory
import spr_pending_chain_overlay as spr


class SprPendingChainOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime = Path(os.environ.get("RRV_SPR_TEST_RUNTIME_SOURCE_DIR",
                                         locked_producer_source() / "ps2xRuntime"))
        cls.source = cls.runtime / spr.MEMORY_SOURCE
        cls.original = cls.source.read_bytes()
        cls.patch = spr.PATCH.read_bytes()
        cls.fixed = spr.apply_patch(cls.original, cls.patch)

    def test_original_and_diagnostic_composition(self):
        # Independent calls to the existing diagnostic transforms prove that
        # SPR correction preserves all observer bytes and their ordering.
        transforms = {
            "locked-producer": lambda text: text,
            "m2-causal": lambda text: patch_memory(text)[0],
            "m2-causal-dma-provenance": lambda text: dma_memory(patch_memory(text)[0]),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for kind, transform in transforms.items():
                with self.subTest(kind=kind):
                    raw = transform(self.original.decode()).encode()
                    source = root / f"{kind}.cpp"
                    source.write_bytes(raw)
                    output = root / kind
                    spr.generate(self.runtime, source, output)
                    expected = transform(self.fixed.decode()).encode()
                    self.assertEqual((output / spr.MEMORY_SOURCE).read_bytes(), expected)
                    self.assertEqual(source.read_bytes(), raw)
                    manifest = json.loads((output / "spr-pending-chain-overlay-manifest.json").read_text())
                    self.assertEqual(manifest["input_kind"], kind)
                    self.assertEqual(manifest["patch"]["sha256"], spr.PATCH_SHA256)
                    self.assertEqual(manifest["files"], [{
                        "path": spr.MEMORY_SOURCE,
                        "producer_sha256": spr.sha256(self.original),
                        "input_sha256": spr.sha256(raw),
                        "overlay_sha256": spr.sha256(expected),
                    }])
            self.assertEqual(self.source.read_bytes(), self.original)

    def test_unknown_partial_and_already_fixed_inputs_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "ps2_memory.cpp"
            inputs = [self.original + b"\n// unknown overlay\n", self.fixed,
                      dma_memory(self.original.decode()).encode(),
                      self.original.replace(b"uint32_t finalMadr = madr;",
                                            b"uint32_t finalMadr = 0u;", 1)]
            for raw in inputs:
                with self.subTest(input_sha256=spr.sha256(raw)):
                    source.write_bytes(raw)
                    with self.assertRaisesRegex(RuntimeError, "neither the pinned producer"):
                        spr.generate(self.runtime, source, root / "output")
                    self.assertFalse((root / "output").exists())

    def test_modified_patch_fails_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            changed_patch = root / spr.PATCH.name
            changed_patch.write_bytes(self.patch + b"\n")
            with patch.object(spr, "PATCH", changed_patch):
                with self.assertRaisesRegex(RuntimeError, "patch SHA-256 mismatch"):
                    spr.generate(self.runtime, self.source, root / "output")
            self.assertFalse((root / "output").exists())

    def test_patch_requires_exact_context(self):
        raw = self.original.replace(b"uint32_t finalMadr = madr;",
                                    b"uint32_t finalMadr = 0u;", 1)
        with self.assertRaisesRegex(RuntimeError, "patch does not apply"):
            spr.apply_patch(raw, self.patch)

    def test_producer_identity_guard_runs_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            with patch.object(spr, "require_locked_clean_producer",
                              side_effect=RuntimeError("dirty producer")) as guard:
                with self.assertRaisesRegex(RuntimeError, "dirty producer"):
                    spr.generate(self.runtime, self.source, output)
                guard.assert_called_once_with(self.runtime)
            self.assertFalse(output.exists())

    def test_wrong_producer_bytes_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / spr.MEMORY_SOURCE
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original + b"\n")
            # Isolate the independent per-file pin gate from the Git guard.
            with patch.object(spr, "require_locked_clean_producer"):
                with self.assertRaisesRegex(RuntimeError, "SHA-pinned ps2_memory.cpp"):
                    spr.generate(root, source, root / "output")
            self.assertFalse((root / "output").exists())

    def test_output_cannot_replace_the_producer_or_selected_input(self):
        with self.assertRaisesRegex(RuntimeError, "separate source copy"):
            spr.generate(self.runtime, self.source, self.runtime)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / spr.MEMORY_SOURCE
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original)
            with self.assertRaisesRegex(RuntimeError, "separate source copy"):
                spr.generate(self.runtime, source, root)
            self.assertEqual(source.read_bytes(), self.original)
        self.assertEqual(self.source.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
