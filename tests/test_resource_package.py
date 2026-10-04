#!/usr/bin/env python3
"""Focused producer regressions for ADR-0006 fixed publication."""
from __future__ import annotations
import hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRODUCER = ROOT / "scripts/resource_package.py"

def run(*args: str, ok: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([sys.executable, "-B", str(PRODUCER), *args], text=True, capture_output=True)
    if ok and result.returncode:
        raise AssertionError(result.stderr)
    if not ok and not result.returncode:
        raise AssertionError("producer unexpectedly succeeded")
    return result

class ResourcePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="rrv-adr0006-")
        self.root = pathlib.Path(self.temp.name); self.build = self.root / "build"; self.build.mkdir()
        self.bridge = self.root / "bridge.dylib"; self.bridge.write_bytes(b"bridge-v1")
        self.metal = self.root / "default.metallib"; self.metal.write_bytes(b"metal-v1")
        self.provenance = self.root / "bridge-provenance.json"; self.provenance.write_text('{"bridge":"fixture"}')
        self.verifier = self.root / "bridge-verifier.py"; self.verifier.write_text("import sys\nsys.exit(0)\n")
        self.manifest = self.root / "input.json"
        self.manifest.write_text(json.dumps({"schema":"rrv-resource-package-input-v1", "bridge":"bridge.dylib",
            "entries":[{"path":"bridge.dylib","source":str(self.bridge)}, {"path":"resources/default.metallib","source":str(self.metal)}],
            "directories":["resources/empty"], "provenance":{"fixture":"adr0006"}, "provenance_sources":{"bridge-manifest":str(self.provenance)}}))
        self.spec = self.build / "CMakeFiles/rrv-resource-package/spec.json"
    def tearDown(self) -> None: self.temp.cleanup()
    def configure(self) -> str:
        sha = run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec)).stdout.strip()
        self.entry = self.spec.with_name("ensure.py")
        self.entry.write_text("import pathlib, sys\nsys.path.insert(0, %r)\nimport resource_package\nresource_package.ensure_configured(pathlib.Path(__file__).resolve().parents[2], %r)\n" % (str(ROOT / "scripts"), sha))
        return sha
    def ensure(self, sha: str, ok: bool = True) -> subprocess.CompletedProcess[str]:
        self.entry.write_text("import pathlib, sys\nsys.path.insert(0, %r)\nimport resource_package\nresource_package.ensure_configured(pathlib.Path(__file__).resolve().parents[2], %r)\n" % (str(ROOT / "scripts"), sha))
        result = subprocess.run([sys.executable, "-B", str(self.entry)], text=True, capture_output=True)
        if ok and result.returncode: raise AssertionError(result.stderr)
        if not ok and not result.returncode: raise AssertionError("producer unexpectedly succeeded")
        return result
    def test_fixed_current_atomic_repair_and_identity(self) -> None:
        sha = self.configure(); self.ensure(sha)
        current = self.build / ".rrv-resource-packages/current"
        self.assertEqual((current / "identity").read_text().splitlines()[0], "rrv-resource-package-v1")
        before = (current / "bridge.dylib").read_bytes(); self.ensure(sha)
        self.assertFalse((self.build / ".rrv-resource-packages/temporary").exists())
        (current / "resources/default.metallib").write_bytes(b"corrupt")
        self.ensure(sha)
        self.assertEqual((current / "bridge.dylib").read_bytes(), before)
        self.assertEqual((current / "resources/default.metallib").read_bytes(), b"metal-v1")
        self.assertFalse((self.build / ".rrv-resource-packages/generation").exists())
        self.assertFalse((self.build / "CMakeFiles/rrv-resource-package/plans").exists())
    def test_spec_and_pinned_provenance_fail_closed(self) -> None:
        sha = self.configure(); self.ensure("0" * 64, ok=False)
        self.spec.write_text("{}")
        self.ensure(sha, ok=False)
        sha = self.configure(); self.ensure(sha)
        current = self.build / ".rrv-resource-packages/current/bridge.dylib"
        before = current.read_bytes()
        self.bridge.write_bytes(b"changed")
        self.ensure(sha, ok=False)
        self.assertEqual(current.read_bytes(), before)
        self.bridge.write_bytes(before); self.provenance.write_text('{"bridge":"changed"}')
        self.ensure(sha, ok=False)
    def test_explicit_bridge_verifier_is_pinned_and_required(self) -> None:
        raw = json.loads(self.manifest.read_text())
        raw["bridge_provenance"] = {
            "manifest": str(self.provenance), "verifier": str(self.verifier),
            "expected_rrv_commit": "r" * 40, "expected_pcsx2_commit": "p" * 40,
            "expected_bridge_patch_sha256": "h" * 64,
        }
        self.manifest.write_text(json.dumps(raw))
        sha = self.configure(); self.ensure(sha)
        self.verifier.write_text("import sys\nsys.exit(1)\n")
        self.ensure(sha, ok=False)
        bridge_provenance = raw.pop("bridge_provenance")
        bridge_provenance.pop("verifier")
        raw["bridge_provenance"] = bridge_provenance
        self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec), ok=False).returncode, 0)
    def test_unsafe_current_is_never_swapped_or_cleaned(self) -> None:
        sha = self.configure(); self.ensure(sha)
        packages = self.build / ".rrv-resource-packages"; current = packages / "current"
        outside = self.root / "outside"; outside.mkdir(); sentinel = outside / "sentinel"; sentinel.write_text("preserve")
        def restore() -> None:
            if current.is_symlink() or current.exists():
                if current.is_symlink(): current.unlink()
                else: shutil.rmtree(current)
            self.ensure(sha)
        def reject(mutate) -> None:
            mutate()
            before = os.lstat(current)
            self.ensure(sha, ok=False)
            self.assertEqual(os.lstat(current).st_ino, before.st_ino)
            self.assertEqual(sentinel.read_text(), "preserve")
            self.assertFalse((packages / "temporary").exists())
            restore()
        old = packages / "old-current"; current.rename(old); current.symlink_to(outside, target_is_directory=True)
        before = os.lstat(current); self.ensure(sha, ok=False)
        self.assertTrue(current.is_symlink()); self.assertEqual(os.lstat(current).st_ino, before.st_ino)
        self.assertEqual(sentinel.read_text(), "preserve"); self.assertFalse((packages / "temporary").exists())
        current.unlink(); old.rename(current)
        reject(lambda: (current / "resources/leak").symlink_to(sentinel))
        reject(lambda: os.link(current / "bridge.dylib", current / "resources/hard-link"))
        reject(lambda: os.mkfifo(current / "resources/fifo"))
    def test_illegal_names_collisions_and_output_escape_reject(self) -> None:
        raw = json.loads(self.manifest.read_text()); raw["entries"][1]["path"] = "a/../b"; self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec), ok=False).returncode, 0)
        raw["entries"][1]["path"] = "Bridge.dylib"; raw["entries"][1]["source"] = str(self.metal); self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec), ok=False).returncode, 0)
        raw["entries"][1]["path"] = "resources/default.metallib"; self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.root / "outside.json"), ok=False).returncode, 0)
        linked = self.root / "linked.dylib"; linked.symlink_to(self.bridge)
        raw["entries"][0]["source"] = str(linked); self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec), ok=False).returncode, 0)
        raw["entries"][0]["source"] = str(self.bridge); hard = self.root / "hard.metallib"; os.link(self.metal, hard); raw["entries"][1]["source"] = str(hard); self.manifest.write_text(json.dumps(raw))
        self.assertNotEqual(run("configure", "--input", str(self.manifest), "--build-root", str(self.build), "--spec", str(self.spec), ok=False).returncode, 0)

if __name__ == "__main__": unittest.main()
