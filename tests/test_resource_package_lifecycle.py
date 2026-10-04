#!/usr/bin/env python3
"""Native ADR-0006 lifecycle regressions: qualification families 4 and 5 only."""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE = ROOT / "cmake/RrvResourcePackage.cmake"
NATIVE = ROOT / "tests/resource_package_lifecycle_tests.cpp"
BUILD = ROOT / "build/adr0006-lifecycle"


def invoke(args: list[str], cwd: pathlib.Path, ok: bool = True, **kwargs: object) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, **kwargs)
    if ok and result.returncode:
        raise AssertionError(" ".join(args) + "\n" + result.stdout + result.stderr)
    if not ok and not result.returncode:
        raise AssertionError("unexpected success: " + " ".join(args))
    return result


def wait_for(path: pathlib.Path, process: subprocess.Popen[str] | None = None) -> None:
    deadline = time.monotonic() + 15
    while not path.exists():
        if process is not None and process.poll() is not None:
            stdout, stderr = process.communicate()
            raise AssertionError(f"process ended before {path.name}: {stdout}{stderr}")
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {path}")
        time.sleep(0.01)


def tree_snapshot(root: pathlib.Path, *, inodes: bool) -> list[tuple[object, ...]]:
    if not root.exists() and not root.is_symlink():
        return [("absent",)]
    rows: list[tuple[object, ...]] = []
    for path in sorted([root, *root.rglob("*")], key=lambda value: value.relative_to(root).as_posix()):
        relative = "." if path == root else path.relative_to(root).as_posix()
        stat = path.lstat()
        if path.is_symlink():
            row: tuple[object, ...] = (relative, "symlink", os.readlink(path))
        elif path.is_dir():
            row = (relative, "directory")
        elif path.is_file():
            row = (relative, "file", hashlib.sha256(path.read_bytes()).hexdigest())
        else:
            row = (relative, "other")
        if inodes:
            row += (stat.st_dev, stat.st_ino)
        rows.append(row)
    return rows


@unittest.skipUnless(sys.platform == "darwin" and os.uname().machine == "arm64", "requires native macOS arm64")
class ResourcePackageLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.run_id = f"run-{time.strftime('%Y%m%dT%H%M%S')}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
        cls.build = BUILD / cls.run_id
        cls.work = tempfile.TemporaryDirectory(prefix="rrv-adr0006-lifecycle-source-")
        cls.source = pathlib.Path(cls.work.name)
        cls.evidence = cls.build / "evidence"
        cls.source.joinpath("bridge.cpp").write_text('''#include <cstdio>
#include <cstdlib>
extern "C" int bridge_value(){ return 17; }
extern "C" __attribute__((destructor)) void bridge_dlclose_marker(){
    const char* marker = std::getenv("RRV_LIFECYCLE_DLCLOSE_MARKER");
    if (marker) { if (FILE* file = std::fopen(marker, "w")) { std::fputs("bridge-dlclose\\n", file); std::fclose(file); } }
}
''')
        invoke(["xcrun", "clang++", "-dynamiclib", "bridge.cpp", "-o", "bridge.dylib"], cls.source)
        cls.source.joinpath("metal.metallib").write_bytes(b"adr0006 lifecycle metal resource\n")
        cls.source.joinpath("input.json").write_text(json.dumps({
            "schema": "rrv-resource-package-input-v1", "bridge": "bridge.dylib",
            "entries": [{"path": "bridge.dylib", "source": "bridge.dylib"},
                        {"path": "resources/metal.metallib", "source": "metal.metallib"}],
            "provenance": {"fixture": "adr0006-lifecycle-native"},
        }))
        bad_sha = hashlib.sha256(b"not a Mach-O dylib\n").hexdigest()
        cls.source.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(adr0006_lifecycle LANGUAGES CXX)
include("{MODULE}")
rrv_select_gs_resource_package(PACKAGE fixture MANIFEST "${{CMAKE_CURRENT_SOURCE_DIR}}/input.json")
add_executable(lifecycle_reader "{NATIVE}")
target_compile_definitions(lifecycle_reader PRIVATE RRV_LIFECYCLE_BAD_BRIDGE_SHA="{bad_sha}")
rrv_use_gs_resource_package(TARGET lifecycle_reader PACKAGE fixture)
''')
        invoke(["cmake", "-S", str(cls.source), "-B", str(cls.build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], ROOT)
        invoke(["cmake", "--build", str(cls.build), "--target", "lifecycle_reader"], ROOT)
        cls.reader = cls.build / "bin/lifecycle_reader"
        cls.spec_digest = hashlib.sha256((cls.build / "CMakeFiles/rrv-resource-package/spec.json").read_bytes()).hexdigest()
        cls.current = cls.build / ".rrv-resource-packages/current"
        cls.temporary = cls.build / ".rrv-resource-packages/temporary"
        cls.lock = cls.build / ".rrv-resource-package.lock"
        cls.evidence.mkdir(parents=True)
        sources = (ROOT / "scripts/resource_package.py", ROOT / "scripts/resource_package_fs.py", MODULE,
                   ROOT / "src/host/rrv_resource_package.h", ROOT / "src/host/rrv_resource_package.cpp", NATIVE,
                   pathlib.Path(__file__))
        cls.evidence.joinpath("source-manifest.json").write_text(json.dumps({
            "run": cls.run_id, "build": str(cls.build),
            "sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
        }, indent=2) + "\n")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.work.cleanup()

    def setUp(self) -> None:
        self.ensure()
        self.assertFalse(self.temporary.exists(), "fixture setup must begin without temporary")
        self.assertEqual(invoke([str(self.reader), "read"], ROOT).returncode, 0)

    def ensure(self) -> subprocess.CompletedProcess[str]:
        return invoke([sys.executable, "-B", str(self.build / "CMakeFiles/rrv-resource-package/ensure.py")], ROOT)

    def producer_barrier(self, phase: str, event: pathlib.Path, release: pathlib.Path) -> subprocess.Popen[str]:
        script = f'''import os, pathlib, sys, time
sys.path.insert(0, {str(ROOT / "scripts")!r})
import resource_package as rp
build = pathlib.Path({str(self.build)!r})
event = pathlib.Path({str(event)!r})
release = pathlib.Path({str(release)!r})
def pause(label):
    event.write_text(label + "\\n")
    while not release.exists(): time.sleep(0.01)
if {phase!r} == "pre-swap":
    original = rp.atomic_swap
    def hooked(fd):
        pause("before-renameatx-np")
        original(fd)
    rp.atomic_swap = hooked
elif {phase!r} == "post-swap":
    original = rp.atomic_swap
    def hooked(fd):
        original(fd)
        pause("after-renameatx-np")
    rp.atomic_swap = hooked
elif {phase!r} == "pre-first-publication":
    original = rp.os.rename
    def hooked(source, destination, *args, **kwargs):
        if source == "temporary" and destination == "current":
            pause("before-renameat-first-publication")
        return original(source, destination, *args, **kwargs)
    rp.os.rename = hooked
elif {phase!r} == "before-temporary-cleanup":
    original = rp.remove_tree_beneath
    def hooked(fd, name):
        if name == "temporary": pause("exclusive-before-temporary-cleanup")
        return original(fd, name)
    rp.remove_tree_beneath = hooked
else: raise RuntimeError("unknown test barrier")
rp.ensure_configured(build, {self.spec_digest!r})
'''
        return subprocess.Popen([sys.executable, "-B", "-c", script], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def lock_probe(self) -> str:
        script = f'''import fcntl, os
fd = os.open({str(self.lock)!r}, os.O_RDONLY | os.O_CLOEXEC)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    print("exclusive-holder-acquired")
finally:
    os.close(fd)
'''
        return invoke([sys.executable, "-B", "-c", script], ROOT).stdout.strip()

    def terminate(self, process: subprocess.Popen[str]) -> tuple[str, str]:
        process.terminate()
        stdout, stderr = process.communicate(timeout=15)
        self.assertNotEqual(process.returncode, 0, "interrupted producer unexpectedly completed")
        return stdout, stderr

    def test_concurrent_reader_writer_ordering_and_full_lease(self) -> None:
        trace: list[dict[str, object]] = []
        stale = self.temporary
        stale.mkdir()
        stale.joinpath("stale").write_text("private remainder")
        ready, release, done, dlclose = (self.evidence / "reader-ready", self.evidence / "reader-release", self.evidence / "reader-done", self.evidence / "reader-dlclose")
        for path in (ready, release, done, dlclose):
            path.unlink(missing_ok=True)
        reader_env = os.environ.copy()
        reader_env["RRV_LIFECYCLE_DLCLOSE_MARKER"] = str(dlclose)
        reader = subprocess.Popen([str(self.reader), "hold", str(ready), str(release), str(done)], cwd=ROOT, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=reader_env)
        wait_for(ready, reader)
        trace.append({"event": "reader_ready", "text": ready.read_text().strip()})
        mutation = self.evidence / "writer-mutation"
        mutation.unlink(missing_ok=True)
        writer_release = self.evidence / "writer-release"
        writer_release.unlink(missing_ok=True)
        writer = self.producer_barrier("before-temporary-cleanup", mutation, writer_release)
        time.sleep(0.25)
        self.assertFalse(mutation.exists(), "writer reached mutation while live reader held the shared lease")
        release.write_text("release reader\n")
        wait_for(dlclose, reader)
        wait_for(done, reader)
        reader_stdout, reader_stderr = reader.communicate(timeout=15)
        self.assertEqual(reader.returncode, 0, reader_stdout + reader_stderr)
        wait_for(mutation, writer)
        self.assertGreaterEqual(mutation.stat().st_mtime_ns, dlclose.stat().st_mtime_ns)
        trace.extend(({"event": "reader_dlclose", "text": dlclose.read_text().strip()},
                      {"event": "reader_destroyed", "text": done.read_text().strip()},
                      {"event": "writer_mutation", "text": mutation.read_text().strip()}))
        writer_release.write_text("finish writer\n")
        writer_stdout, writer_stderr = writer.communicate(timeout=15)
        self.assertEqual(writer.returncode, 0, writer_stdout + writer_stderr)
        self.assertFalse(self.temporary.exists())

        self.temporary.mkdir()
        self.temporary.joinpath("stale").write_text("private remainder")
        writer_locked = self.evidence / "writer-locked"
        writer_gate = self.evidence / "writer-gate"
        reader_two_ready = self.evidence / "reader-two-ready"
        reader_two_done = self.evidence / "reader-two-done"
        for path in (writer_locked, writer_gate, reader_two_ready, reader_two_done):
            path.unlink(missing_ok=True)
        writer = self.producer_barrier("before-temporary-cleanup", writer_locked, writer_gate)
        wait_for(writer_locked, writer)
        reader_two = subprocess.Popen([str(self.reader), "hold", str(reader_two_ready), str(self.evidence / "reader-two-release"), str(reader_two_done)], cwd=ROOT, text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.25)
        self.assertFalse(reader_two_ready.exists(), "reader crossed a live writer's exclusive lease")
        trace.append({"event": "writer_exclusive", "text": writer_locked.read_text().strip()})
        writer_gate.write_text("release writer\n")
        writer_stdout, writer_stderr = writer.communicate(timeout=15)
        self.assertEqual(writer.returncode, 0, writer_stdout + writer_stderr)
        wait_for(reader_two_ready, reader_two)
        trace.append({"event": "reader_after_writer", "text": reader_two_ready.read_text().strip()})
        (self.evidence / "reader-two-release").write_text("release reader\n")
        reader_stdout, reader_stderr = reader_two.communicate(timeout=15)
        self.assertEqual(reader_two.returncode, 0, reader_stdout + reader_stderr)
        self.ensure()
        (self.evidence / "lifecycle-concurrency.log").write_text(json.dumps(trace, indent=2) + "\n")

    def test_interruption_preserves_observed_publication_cuts_and_recovers(self) -> None:
        trace: list[dict[str, object]] = []
        def cut(phase: str, expected: list[tuple[object, ...]], label: str) -> None:
            if phase == "before-temporary-cleanup":
                self.temporary.mkdir()
                self.temporary.joinpath("stale").write_text("pre-mutation remainder\n")
            event = self.evidence / f"{label}-event"; release = self.evidence / f"{label}-release"
            event.unlink(missing_ok=True); release.unlink(missing_ok=True)
            producer = self.producer_barrier(phase, event, release)
            wait_for(event, producer)
            self.terminate(producer)
            observed = tree_snapshot(self.current, inodes=True)
            self.assertEqual(observed, expected, label)
            self.assertEqual(self.lock_probe(), "exclusive-holder-acquired")
            trace.append({"cut": label, "barrier": event.read_text().strip(), "current": observed,
                          "temporary_exists": self.temporary.exists(), "subsequent_holder": "acquired"})
            self.ensure()
            self.assertFalse(self.temporary.exists())

        healthy = tree_snapshot(self.current, inodes=True)
        cut("before-temporary-cleanup", healthy, "pre-mutation-valid-current")
        shutil.rmtree(self.current)
        cut("pre-first-publication", [("absent",)], "pre-publication-missing-current")
        self.ensure()
        self.current.joinpath("identity").write_text("invalid original current\n")
        invalid = tree_snapshot(self.current, inodes=True)
        cut("pre-swap", invalid, "pre-publication-invalid-current")

        self.current.joinpath("resources/metal.metallib").write_text("force replacement\n")
        expected_new = tree_snapshot(self.current, inodes=False)
        self.ensure()
        expected_new = tree_snapshot(self.current, inodes=False)
        self.current.joinpath("resources/metal.metallib").write_text("force post-swap replacement\n")
        event = self.evidence / "post-publication-event"; release = self.evidence / "post-publication-release"
        event.unlink(missing_ok=True); release.unlink(missing_ok=True)
        producer = self.producer_barrier("post-swap", event, release)
        wait_for(event, producer)
        self.terminate(producer)
        observed_new = tree_snapshot(self.current, inodes=False)
        self.assertEqual(observed_new, expected_new, "post-publication current must be the complete new package")
        self.assertEqual(self.lock_probe(), "exclusive-holder-acquired")
        trace.append({"cut": "post-publication", "barrier": event.read_text().strip(), "current": observed_new,
                      "temporary_exists": self.temporary.exists(), "subsequent_holder": "acquired"})
        self.ensure()
        self.assertFalse(self.temporary.exists())
        (self.evidence / "lifecycle-interruption.log").write_text(json.dumps(trace, indent=2) + "\n")

    def test_repeated_in_process_failure_ownership(self) -> None:
        result = invoke([str(self.reader), "failure-loop", str(self.build / "failure-scratch")], ROOT)
        self.assertIn("failure-loop-pass iterations=101", result.stdout)
        self.assertEqual(self.lock_probe(), "exclusive-holder-acquired")
        self.ensure()
        (self.evidence / "lifecycle-failure-ownership.log").write_text(result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
