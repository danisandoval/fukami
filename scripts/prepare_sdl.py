#!/usr/bin/env python3
"""Install pinned SDL outside product build trees, or verify its installed receipt.

Without --from-source, --prepare clones the locked source into runtime-deps/sdl/source.
Existing sources and installation prefixes are never replaced. The build workspace
is temporary; only the installed prefix is needed to compile and run the product.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import tempfile

from prepare_dependencies import load_toml, repo_state, run

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION = "2.32.10"
REVISION = "5d249570393f7a37e037abf22cd6012a4cc56a71"
LIBRARY = "lib/libSDL2-2.0.0.dylib"
INCLUDE = "include/SDL2"
REQUIRED = {LIBRARY, "LICENSE.txt", *(f"{INCLUDE}/{name}" for name in
            ("SDL.h", "SDL_version.h", "SDL_config.h"))}


def native_platform() -> str:
    arch = platform.machine().lower()
    if platform.system() != "Darwin" or arch not in ("arm64", "x86_64"):
        raise RuntimeError("SDL product preparation currently supports native macOS arm64/x86_64 only")
    return f"macos-{arch}"


def lock_identity() -> dict:
    lock = load_toml(ROOT / "config/dependencies.lock.toml")["sdl"]
    if lock["version"] != VERSION or lock["revision"] != REVISION:
        raise RuntimeError("SDL lock differs from the accepted release-2.32.10 pin")
    return lock


def inventory(prefix: pathlib.Path) -> tuple[dict, dict]:
    files, symlinks = {}, {}
    for path in sorted(prefix.rglob("*")):
        relative = path.relative_to(prefix).as_posix()
        if relative == "manifest.json":
            continue
        if path.is_symlink():
            target = path.readlink()
            if target.is_absolute() or prefix not in path.resolve().parents or not path.exists():
                raise RuntimeError(f"SDL symlink escapes prefix or is dangling: {relative}")
            symlinks[relative] = str(target)
        elif path.is_file():
            files[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return files, symlinks


def verify(prefix: pathlib.Path, target: str) -> dict:
    lock_identity()
    manifest = json.loads((prefix / "manifest.json").read_text())
    expected = {"schema_version": 1, "name": "SDL2", "version": VERSION,
                "revision": REVISION, "platform": target, "library": LIBRARY,
                "include_dir": INCLUDE}
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise RuntimeError(f"SDL manifest {key} differs: expected {value!r}")
    files, symlinks = inventory(prefix)
    if not REQUIRED.issubset(files):
        raise RuntimeError(f"SDL install missing required files: {sorted(REQUIRED - files.keys())}")
    if files != manifest.get("files") or symlinks != manifest.get("symlinks", {}):
        raise RuntimeError("SDL installed files/symlinks differ from manifest hashes")
    arch = target.removeprefix("macos-")
    if run("lipo", "-archs", str(prefix / LIBRARY)).split() != [arch]:
        raise RuntimeError(f"SDL library must contain exactly the {arch} architecture")
    identity = run("otool", "-D", str(prefix / LIBRARY)).splitlines()[1:]
    if identity != ["@rpath/libSDL2-2.0.0.dylib"]:
        raise RuntimeError("SDL library must use the relocatable @rpath install name")
    dependencies = run("otool", "-L", str(prefix / LIBRARY)).splitlines()[2:]
    for line in dependencies:
        name = line.strip().split(" (", 1)[0]
        if not name.startswith(("/usr/lib/", "/System/Library/")):
            raise RuntimeError(f"SDL library has a non-system dependency: {name}")
    return manifest


def publish_directory(staged: pathlib.Path, prefix: pathlib.Path) -> None:
    """Atomically publish on macOS, refusing even an empty existing directory."""
    # Darwin sys/stdio.h: RENAME_EXCL = 0x00000004 (macOS 10.12+).
    libc = ctypes.CDLL(None, use_errno=True)
    rename_exclusive = libc.renamex_np
    rename_exclusive.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename_exclusive.restype = ctypes.c_int
    if rename_exclusive(os.fsencode(staged), os.fsencode(prefix), 0x00000004):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(prefix))


def prepare(prefix: pathlib.Path, source: pathlib.Path, target: str, jobs: int) -> None:
    lock = lock_identity()
    if prefix.exists():
        verify(prefix, target)
        print(f"Existing SDL installation verified; preserved: {prefix}")
        return
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "--no-checkout", str(lock["url"]), str(source))
        run("git", "checkout", "--detach", REVISION, cwd=source)
    problems = repo_state(source, REVISION)
    if problems:
        raise RuntimeError("SDL source verification failed: " + "; ".join(problems))
    arch = target.removeprefix("macos-")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    # Stage on the same filesystem for atomic publication. DESTDIR keeps CMake
    # package/config paths bound to the final prefix, not the staging directory.
    with tempfile.TemporaryDirectory(prefix=".rrv-sdl-stage-", dir=prefix.parent) as temporary:
        workspace = pathlib.Path(temporary) / "build"
        destdir = pathlib.Path(temporary) / "install"
        staged = destdir / prefix.relative_to(prefix.anchor)
        commands = [
            ["cmake", "-S", str(source), "-B", str(workspace), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
             f"-DCMAKE_OSX_ARCHITECTURES={arch}", "-DCMAKE_INSTALL_LIBDIR=lib",
             "-DCMAKE_INSTALL_NAME_DIR=@rpath", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF",
             "-DSDL_CCACHE=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF", "-DSDL_INSTALL=ON",
             # no builder paths in the shipped library (assert messages and debug info embed source paths)
             f"-DCMAKE_C_FLAGS=-ffile-prefix-map={source}=/SDL -ffile-prefix-map={workspace}=/SDL-build",
             f"-DCMAKE_OBJC_FLAGS=-ffile-prefix-map={source}=/SDL -ffile-prefix-map={workspace}=/SDL-build"],
            ["cmake", "--build", str(workspace), "--parallel", str(jobs)],
            ["cmake", "--install", str(workspace)],
        ]
        for command in commands[:2]:
            subprocess.run(command, check=True)
        subprocess.run(commands[2], check=True, env={**os.environ, "DESTDIR": str(destdir)})
        (staged / "LICENSE.txt").write_bytes((source / "LICENSE.txt").read_bytes())
        files, symlinks = inventory(staged)
        manifest = {"schema_version": 1, "name": "SDL2", "version": VERSION,
                    "revision": REVISION, "platform": target, "library": LIBRARY,
                    "include_dir": INCLUDE, "files": files, "symlinks": symlinks,
                    "provenance": {"method": "cmake-install", "source": str(source),
                                   "source_tree": run("git", "rev-parse", "HEAD^{tree}", cwd=source),
                                   "configure_command": commands[0]}}
        (staged / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        verify(staged, target)
        publish_directory(staged, prefix)
    print(f"Installed and verified SDL: {prefix}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--prepare", action="store_true", help="build/install missing prefix; verify existing prefix")
    actions.add_argument("--verify", action="store_true", help="verify installed hashes, pin and Mach-O identity")
    parser.add_argument("--from-source", type=pathlib.Path, help="existing clean pinned SDL Git checkout")
    parser.add_argument("--prefix", type=pathlib.Path, help="installation prefix (default: runtime-deps/sdl/VERSION/PLATFORM)")
    parser.add_argument("--platform", choices=("macos-arm64", "macos-x86_64"), help="expected native target")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    try:
        target = args.platform or native_platform()
        if args.prepare and target != native_platform():
            raise RuntimeError("cross compilation is not supported by this preparation helper")
        if args.jobs < 1:
            raise RuntimeError("--jobs must be positive")
        prefix = (args.prefix or ROOT / "runtime-deps" / "sdl" / VERSION / target).resolve()
        if args.verify:
            verify(prefix, target)
            print(f"SDL installation verified: {prefix}")
        else:
            source = (args.from_source or ROOT / "runtime-deps" / "sdl" / "source").resolve()
            if args.from_source and not source.is_dir():
                raise RuntimeError(f"--from-source does not exist: {source}")
            prepare(prefix, source, target, args.jobs)
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"SDL preparation/verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
