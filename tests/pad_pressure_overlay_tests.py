#!/usr/bin/env python3
"""Provenance/composition checks for the PAD-001 runtime overlay."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from m2_dma_overlay import locked_producer_source
from m2p_pad_overlay import pad_stub
from pad_pressure_overlay import PAD_SOURCE, generate, patch_pad


class PadPressureOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime = Path(os.environ.get("RRV_PAD_TEST_RUNTIME_SOURCE_DIR",
                                        locked_producer_source() / "ps2xRuntime"))
        cls.source = cls.runtime / PAD_SOURCE
        cls.original = cls.source.read_text(encoding="utf-8")

    def test_original_and_observer_compose_identically(self):
        # Pressure conversion and additive observation must commute. This
        # catches a replacement that drops the existing guest-packet receipt.
        observed = pad_stub(self.original)[0]
        self.assertEqual(patch_pad(observed), pad_stub(patch_pad(self.original))[0])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for kind, text in (("locked-producer", self.original),
                               ("m2p-pad-observer", observed)):
                source = root / f"{kind}.cpp"
                source.write_text(text, encoding="utf-8")
                output = root / kind
                generate(self.runtime, source, output)
                self.assertEqual((output / PAD_SOURCE).read_text(), patch_pad(text))
                manifest = json.loads((output / "pad-pressure-overlay-manifest.json").read_text())
                self.assertEqual(manifest["input_kind"], kind)
            self.assertEqual(self.source.read_text(), self.original)

    def test_unknown_and_already_patched_inputs_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Pad.cpp"
            for text in (self.original + "\n// unknown overlay\n", patch_pad(self.original)):
                source.write_text(text, encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "neither the pinned producer"):
                    generate(self.runtime, source, root / "output")
                self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
