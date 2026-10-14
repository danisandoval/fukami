#!/usr/bin/env python3
"""Windows reader against the real producer (ADR-0006).

Builds a package with scripts/resource_package.py, compiles src/host/rrv_resource_package_win.cpp together with the
native test (tests/resource_package_native_tests.cpp) and a tiny bridge DLL, and runs the reader from B/bin/native.exe.
It must open and verify the package, load the bridge and check its identity, and refuse a package that is tampered
with, or holds a hard link or a junction.
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from cxx_compiler import find_compiler
from fs_support import make_dir_link, remove_dir_link

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRODUCER = ROOT / "scripts/resource_package.py"
BRIDGE_SOURCE = 'extern "C" __declspec(dllexport) int bridge_value() { return 17; }\n'


def run(args, **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


@unittest.skipUnless(sys.platform == "win32" and find_compiler(), "needs Windows and clang-cl (a vcvars shell)")
class WindowsReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix="rrv-rp-win-")
        root = pathlib.Path(cls.temp.name)
        cls.root, cls.build = root, root / "build"
        (cls.build / "bin").mkdir(parents=True)
        cl = find_compiler()
        (root / "bridge.cpp").write_text(BRIDGE_SOURCE)
        done = run([cl, "/nologo", "/LD", "/O1", str(root / "bridge.cpp"), f"/Fo{root}\\", f"/Fe{root / 'bridge.dll'}"])
        assert done.returncode == 0, done.stdout + done.stderr
        (root / "extra.txt").write_text("payload\n")
        manifest = root / "input.json"
        manifest.write_text(json.dumps({
            "schema": "rrv-resource-package-input-v1", "bridge": "bridge.dll",
            "entries": [{"path": "bridge.dll", "source": str(root / "bridge.dll")},
                        {"path": "resources/extra.txt", "source": str(root / "extra.txt")}],
            "directories": ["resources/empty"], "provenance": {"fixture": "windows-reader"},
            "provenance_sources": {"note": str(root / "extra.txt")}}))
        cls.spec = cls.build / "CMakeFiles/rrv-resource-package/spec.json"
        done = run([sys.executable, "-B", str(PRODUCER), "configure", "--input", str(manifest),
                    "--build-root", str(cls.build), "--spec", str(cls.spec)])
        assert done.returncode == 0, done.stderr
        cls.digest = done.stdout.strip()
        cls.ensure()
        spec = json.loads(cls.spec.read_text())
        entries = "".join(
            '    {"%s", rrv::resource_package::EntryType::%s, %dULL, "%s"},\n'
            % (row["path"], row["type"], row.get("size", 0), row.get("sha256", "")) for row in spec["inventory"])
        template = (ROOT / "cmake/rrv_resource_package_binding.cpp.in").read_text()
        (root / "binding.cpp").write_text(template
            .replace("@RRV_RESOURCE_PACKAGE_ENTRIES@", entries).replace("@RRV_RESOURCE_PACKAGE_VERSION@", spec["version"])
            .replace("@RRV_RESOURCE_PACKAGE_COMPOSITION@", spec["composition"]).replace("@RRV_RESOURCE_PACKAGE_BRIDGE@", spec["bridge"]))
        cls.exe = cls.build / "bin/native.exe"
        done = run([cl, "/nologo", "/std:c++20", "/EHsc", "/O1", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", f"/I{ROOT / 'src/host'}",
                    str(ROOT / "tests/resource_package_native_tests.cpp"), str(ROOT / "src/host/rrv_resource_package_win.cpp"),
                    str(root / "binding.cpp"), f"/Fo{root}\\", f"/Fe{cls.exe}"])
        assert done.returncode == 0, done.stdout + done.stderr
        cls.current = cls.build / ".rrv-resource-packages/current"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    @classmethod
    def ensure(cls) -> None:
        script = ("import pathlib, sys\nsys.path.insert(0, %r)\nimport resource_package\n"
                  "resource_package.ensure_configured(pathlib.Path(%r), %r)\n" % (str(ROOT / "scripts"), str(cls.build), cls.digest))
        done = run([sys.executable, "-B", "-c", script])
        assert done.returncode == 0, done.stderr

    def reader(self) -> subprocess.CompletedProcess[str]:
        return run([str(self.exe)], cwd=str(self.root))

    def setUp(self) -> None:
        self.ensure()  # every case starts from a verified package

    def test_opens_verifies_and_loads_the_bridge(self) -> None:
        done = self.reader()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("typed-package-reader-pass", done.stdout)

    def test_a_modified_file_is_refused_and_repaired(self) -> None:
        with open(self.current / "resources/extra.txt", "ab") as handle:
            handle.write(b"tamper")
        done = self.reader()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("digest mismatch", done.stderr)
        self.ensure()
        self.assertEqual(self.reader().returncode, 0)

    def test_an_extra_file_is_refused(self) -> None:
        (self.current / "resources/stray.txt").write_text("x")
        done = self.reader()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("unexpected entries", done.stderr)

    def test_a_hard_link_is_refused(self) -> None:
        linked = self.current / "resources/linked.txt"
        os.link(self.current / "resources/extra.txt", linked)
        try:
            done = self.reader()
            self.assertNotEqual(done.returncode, 0)
        finally:
            linked.unlink()  # the producer never repairs a tree with a hard link, by design

    def test_a_junction_is_refused(self) -> None:
        link = self.current / "resources/junction"
        make_dir_link(link, self.root)
        try:
            done = self.reader()
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("unsafe node", done.stderr)
        finally:
            remove_dir_link(link)

    def test_a_junction_as_the_package_root_is_refused(self) -> None:
        packages = self.build / ".rrv-resource-packages"
        moved = packages / "moved-current"
        self.current.rename(moved)
        make_dir_link(self.current, moved)
        try:
            self.assertNotEqual(self.reader().returncode, 0)
        finally:
            remove_dir_link(self.current)
            moved.rename(self.current)


if __name__ == "__main__":
    unittest.main()
