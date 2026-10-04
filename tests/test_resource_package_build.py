#!/usr/bin/env python3
"""Focused CMake/native-reader regressions for the six ADR-0006 families."""
from __future__ import annotations
import pathlib, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE = ROOT / "cmake/RrvResourcePackage.cmake"
NATIVE = ROOT / "tests/resource_package_native_tests.cpp"

def invoke(args: list[str], cwd: pathlib.Path, ok: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if ok and result.returncode: raise AssertionError(" ".join(args) + "\n" + result.stdout + result.stderr)
    if not ok and not result.returncode: raise AssertionError("unexpected success: " + " ".join(args))
    return result

class ResourcePackageBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="rrv-adr0006-build-"); self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "source"; self.build = self.root / "build"; self.source.mkdir()
        (self.source / "bridge.cpp").write_text('extern "C" int bridge_value(){ return 17; }\n')
        invoke(["xcrun", "clang++", "-dynamiclib", "bridge.cpp", "-o", "bridge.dylib"], self.source)
        (self.source / "metal.metallib").write_bytes(b"metal-resource")
        (self.source / "input.json").write_text('{"schema":"rrv-resource-package-input-v1","bridge":"bridge.dylib","entries":[{"path":"bridge.dylib","source":"bridge.dylib"},{"path":"resources/metal.metallib","source":"metal.metallib"}],"provenance":{"fixture":"native"}}')
        (self.source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(adr0006 LANGUAGES CXX)
include("{MODULE}")
rrv_select_gs_resource_package(PACKAGE fixture MANIFEST "${{CMAKE_CURRENT_SOURCE_DIR}}/input.json")
add_executable(reader "{NATIVE}")
rrv_use_gs_resource_package(TARGET reader PACKAGE fixture)
add_executable(reader_two "{NATIVE}")
rrv_use_gs_resource_package(TARGET reader_two PACKAGE fixture)
''')
    def tearDown(self) -> None: self.temp.cleanup()
    def configure_build(self) -> pathlib.Path:
        invoke(["cmake", "-S", str(self.source), "-B", str(self.build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], self.root)
        invoke(["cmake", "--build", str(self.build), "--target", "reader", "reader_two"], self.root)
        return self.build / "bin/reader"
    def test_native_reader_and_authoritative_fixed_layout(self) -> None:
        reader = self.configure_build(); result = invoke([str(reader)], self.root)
        self.assertIn("typed-package-reader-pass", result.stdout)
        self.assertIn("typed-package-reader-pass", invoke([str(self.build / "bin/reader_two")], self.root).stdout)
        relocated = self.root / "relocated-build"; shutil.copytree(self.build, relocated)
        self.assertIn("typed-package-reader-pass", invoke([str(relocated / "bin/reader")], self.root).stdout)
        copied = self.root / "copied-reader"; shutil.copy2(reader, copied)
        invoke([str(copied)], self.root, ok=False)
    def test_no_relink_healthy_and_same_composition_repair(self) -> None:
        reader = self.configure_build(); before = reader.stat().st_mtime_ns
        invoke(["ninja", "-C", str(self.build), "reader"], self.root)
        self.assertEqual(reader.stat().st_mtime_ns, before)
        (self.build / ".rrv-resource-packages/current/resources/metal.metallib").write_bytes(b"corrupt")
        invoke(["ninja", "-C", str(self.build), "reader"], self.root)
        self.assertEqual(reader.stat().st_mtime_ns, before)
        self.assertEqual((self.build / ".rrv-resource-packages/current/resources/metal.metallib").read_bytes(), b"metal-resource")
        dry = invoke(["ninja", "-C", str(self.build), "-n", "reader"], self.root).stdout
        self.assertNotIn("clang++", dry)
    def test_missing_extra_wrong_type_and_wrong_identity_fail_closed(self) -> None:
        reader = self.configure_build(); current = self.build / ".rrv-resource-packages/current"
        for mutate in (lambda: (current / "bridge.dylib").unlink(), lambda: (current / "extra").write_text("x"), lambda: ((current / "resources/metal.metallib").unlink(), (current / "resources/metal.metallib").mkdir()), lambda: (current / "identity").write_text("rrv-resource-package-v1\n0" * 64)):
            mutate(); invoke([str(reader)], self.root, ok=False)
            invoke(["ninja", "-C", str(self.build), "reader"], self.root)
    def test_symlinked_current_rejects_fixed_nofollow_reader(self) -> None:
        reader = self.configure_build(); current = self.build / ".rrv-resource-packages/current"; safe = self.build / ".rrv-resource-packages/safe"
        current.rename(safe); current.symlink_to(safe, target_is_directory=True)
        invoke([str(reader)], self.root, ok=False)
    def test_quoted_unicode_module_and_build_roots(self) -> None:
        # The module derives its trusted source root from its own path.  Copy the
        # minimal core tree under a name that would break the former raw Python
        # single-quoted generated entrypoint.
        copied = self.root / "repo O'Brien ü"
        for relative in ("cmake/RrvResourcePackage.cmake", "cmake/rrv_resource_package_binding.cpp.in",
                         "scripts/resource_package.py", "scripts/resource_package_fs.py", "scripts/pcsx2_bridge_manifest.py",
                         "src/host/rrv_resource_package.h", "src/host/rrv_resource_package.cpp",
                         "tests/resource_package_native_tests.cpp"):
            destination = copied / relative; destination.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / relative, destination)
        source = self.root / "source O'Brien ü"; build = self.root / "build O'Brien ü"; source.mkdir()
        (source / "bridge.cpp").write_text('extern "C" int bridge_value(){ return 17; }\n')
        invoke(["xcrun", "clang++", "-dynamiclib", "bridge.cpp", "-o", "bridge.dylib"], source)
        (source / "metal.metallib").write_bytes(b"metal-resource")
        (source / "input.json").write_text('{"schema":"rrv-resource-package-input-v1","bridge":"bridge.dylib","entries":[{"path":"bridge.dylib","source":"bridge.dylib"},{"path":"resources/metal.metallib","source":"metal.metallib"}]}')
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(quoted_adr0006 LANGUAGES CXX)
include("{copied / 'cmake/RrvResourcePackage.cmake'}")
rrv_select_gs_resource_package(PACKAGE fixture MANIFEST "${{CMAKE_CURRENT_SOURCE_DIR}}/input.json")
add_executable(reader "{copied / 'tests/resource_package_native_tests.cpp'}")
rrv_use_gs_resource_package(TARGET reader PACKAGE fixture)
''')
        invoke(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], self.root)
        invoke(["cmake", "--build", str(build), "--target", "reader"], self.root)
        self.assertIn("typed-package-reader-pass", invoke([str(build / "bin/reader")], self.root).stdout)

if __name__ == "__main__": unittest.main()
