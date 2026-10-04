#!/usr/bin/env python3
"""Source-ownership gates (asset-free; needs only committed files, no build-deps).

(a) generated/rr5/output is read-only: it matches source-manifest.json exactly.
(b) the source-owned build edits and copies no source: the runtime and patch layer include the live
    sources and the game-derived data in place, and no stage machinery remains in the product path.
(c) game-derived text (generated/rr5 vu/native/output) does not appear under
    third_party/ or src/product/, and the native splice marker is present once.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "generated/rr5"
ACCOUNTED_MANIFEST_SHA256 = "eb1c6078f6c7ff89d28144beeebda3901225def392c5f29f8ff2780ad87880f5"
# The game code is derived from the user's own ELF: a public checkout has it only after
# scripts/fukami_generate.py, so the checks on the generated tree skip when it is absent.
HAVE_GENERATED = (GEN / "output").is_dir()
NEEDS_GENERATED = unittest.skipUnless(HAVE_GENERATED, "generated game code is absent (run scripts/fukami_generate.py)")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(root: Path) -> dict[str, str]:
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob("*")) if p.is_file()}


@NEEDS_GENERATED
class ReadOnlyGeneratedSource(unittest.TestCase):
    def test_output_matches_source_manifest(self):
        manifest = json.loads((GEN / "source-manifest.json").read_text())
        self.assertTrue(manifest["read_only"])
        for section, directory in (("generated", GEN / "output"), ("legacy_stubs", GEN / "legacy-stubs")):
            listed = {e["path"]: e for e in manifest[section]["files"]}
            actual = {p.name for p in directory.iterdir() if p.is_file()}
            self.assertEqual(actual, set(listed), f"{section}: file set differs from source-manifest.json")
            for name, entry in listed.items():
                path = directory / name
                self.assertEqual(path.stat().st_size, entry["size"], f"{section}/{name} size")
                self.assertEqual(sha(path), entry["sha256"], f"{section}/{name} was edited")
        self.assertEqual(manifest["generated"]["file_count"], len(manifest["generated"]["files"]))

    def test_accounted_manifest_binds_output(self):
        path = GEN / "gate3-accounted-generation-manifest.json"
        self.assertEqual(sha(path), ACCOUNTED_MANIFEST_SHA256)
        self.assertEqual(json.loads((GEN / "source-manifest.json").read_text())
                         ["accounted_generation_manifest"]["sha256"], ACCOUNTED_MANIFEST_SHA256)
        accounted = json.loads(path.read_text())
        for entry in accounted["generated"]:
            file = GEN / "output" / entry["path"]
            self.assertEqual((file.stat().st_size, sha(file)), (entry["size"], entry["sha256"]), entry["path"])
        self.assertEqual({e["path"] for e in accounted["generated"]},
                         {p.name for p in (GEN / "output").iterdir()})

    def test_derived_manifests_name_this_generation(self):
        accounted = json.loads((GEN / "gate3-accounted-generation-manifest.json").read_text())
        native = json.loads((GEN / "native/manifest.json").read_text())
        self.assertEqual(native["generation_manifest_sha256"], ACCOUNTED_MANIFEST_SHA256)
        for function in native["functions"]:
            self.assertEqual(sha(GEN / "output" / function["source"]), function["source_sha256"])
        vu = json.loads((GEN / "vu/manifest.json").read_text())
        self.assertEqual(vu["input_elf_sha256"], accounted["user_elf_sha256"])
        self.assertEqual(sha(GEN / "vu/rrv_vu_aot_blocks.inc"), vu["blocks_inc_sha256"])


class OwnedBuildInputs(unittest.TestCase):
    """The product build (RRV_PRODUCT_OWNED_SOURCE) compiles committed files only."""

    def test_runtime_cmake_sources_exist(self):
        text = (ROOT / "cmake/product-runtime/CMakeLists.txt").read_text()
        sources = re.findall(r'"\$\{RRV_RT\}/([^"]+)"', text)
        self.assertGreater(len(sources), 15)
        for relative in sources:
            if "*" in relative or relative.endswith("/include"):
                continue
            self.assertTrue((ROOT / "third_party/ps2recomp/ps2xRuntime" / relative).exists(), relative)

    def test_no_build_step_edits_or_copies_source(self):
        for name in ("cmake/RrvProductOwned.cmake", "cmake/RrvProductOwnedIncludes.cmake",
                     "cmake/product-runtime/CMakeLists.txt"):
            text = (ROOT / name).read_text()
            for forbidden in ("configure_file(", "file(WRITE", "file(APPEND", "file(COPY", "string(REPLACE"):
                self.assertNotIn(forbidden, text, f"{name}: {forbidden}")

    def test_in_place_includes_resolve(self):
        """Everything the runtime and patch layer include by name lives where the include dirs point."""
        runtime = (ROOT / "third_party/ps2recomp/ps2xRuntime/src/lib/ps2_runtime.cpp").read_text()
        vu1 = (ROOT / "third_party/ps2recomp/ps2xRuntime/src/lib/ps2_vu1.cpp").read_text()
        for name in ("rrv_guest_rtc.cpp", "rrv_guest_time.cpp", "rrv_ee_timers.cpp"):
            self.assertEqual(runtime.count(f'#include "{name}"'), 1, name)
            self.assertTrue((ROOT / "src/guest-time" / name).is_file(), name)
        self.assertEqual(vu1.count('#include "rrv_vu_aot_engine.inc"'), 1)
        self.assertTrue((ROOT / "src/vu-aot/rrv_vu_aot_engine.inc").is_file())
        self.assertIn('#include "rrv_vu_aot_blocks.inc"', (ROOT / "src/vu-aot/rrv_vu_aot_engine.inc").read_text())
        self.assertEqual((ROOT / "src/product/patches.cpp").read_text().count('#include "rrv_ee_native.inc"'), 1)
        if HAVE_GENERATED:
            self.assertTrue((GEN / "vu/rrv_vu_aot_blocks.inc").is_file())
            self.assertTrue((GEN / "native/rrv_ee_native.inc").is_file())

    def test_no_stage_machinery_in_product_path(self):
        for name in ("cmake/RrvProduct.cmake", "cmake/RrvLegacyLive.cmake", "cmake/RrvProductOwned.cmake"):
            text = (ROOT / name).read_text()
            self.assertNotIn("RRV_GATE3_STAGED_RUNTIME", text, name)
            self.assertNotIn("gate3-scheduler-manifest", text, name)
        self.assertFalse((ROOT / "cmake/RrvGate3Candidate.cmake").exists())

    @NEEDS_GENERATED
    def test_verify_owned_source_script(self):
        done = subprocess.run([sys.executable, str(ROOT / "scripts/verify_owned_source.py")],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(json.loads(done.stdout)["accounted_manifest_sha256"], ACCOUNTED_MANIFEST_SHA256)


@NEEDS_GENERATED
class GameTextStaysUnderGenerated(unittest.TestCase):
    @staticmethod
    def lines(path: Path, minimum: int):
        with path.open(errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if len(line) >= minimum:
                    yield line

    def test_no_generated_lines_in_owned_trees(self):
        derived: set[int] = set()
        for inc in ("vu/rrv_vu_aot_blocks.inc", "native/rrv_ee_native.inc"):
            derived.update(hash(l) for l in self.lines(GEN / inc, 50))
        for cpp in sorted((GEN / "output").iterdir()):
            derived.update(hash(l) for l in self.lines(cpp, 90))
        self.assertGreater(len(derived), 20000)
        hits = []
        roots = [ROOT / "third_party", ROOT / "src/product"]
        for root in roots:
            for path in sorted(root.rglob("*")):
                if not path.is_file() or path.suffix in (".json", ".md"):
                    continue
                for number, line in enumerate(self.lines(path, 50), 1):
                    if hash(line) in derived:
                        hits.append(f"{path.relative_to(ROOT)}: {line[:80]}")
                        break
        self.assertEqual(hits, [], "game-derived lines found outside generated/rr5")


if __name__ == "__main__":
    unittest.main()
