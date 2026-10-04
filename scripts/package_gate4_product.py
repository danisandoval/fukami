#!/usr/bin/env python3
"""Freeze a Gate-3/4 candidate build into a relocatable product runtime.

Usage:
  python3 scripts/package_gate4_product.py --build BUILD_DIR --name NAME
  python3 scripts/package_gate4_product.py --verify --name NAME

The package keeps the candidate's fixed layout, which its resource loader
requires (the executable in B/bin, the typed package in
B/.rrv-resource-packages/current, the reader lock beside it), and adds the SDL
dylib the executable links through @rpath under B/lib. The executable is
copied unchanged, so its SHA-256 is the qualified build's; the launcher sets
DYLD_LIBRARY_PATH to B/lib instead of rewriting the binary.

runtime/NAME is local and ignored: the executable contains generated
recompiled code, which is never committed. --verify rechecks every file
against runtime-manifest.json before a launch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIND = "rrv-gate4-product-runtime"
EXE = "bin/Fukami"
# The executable's name in packages made before 2026-10-02 (v154 and older).
LEGACY_EXE = "bin/rrv-gate3-candidate"


def executable_of(package: Path) -> Path:
    """The game executable of a runtime package, whichever name it was packaged under."""
    current = package / EXE
    return current if current.is_file() else package / LEGACY_EXE


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(root: Path) -> list[dict]:
    entries = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if relative == "runtime-manifest.json":
            continue
        if path.is_symlink():
            raise SystemExit(f"package contains a symlink: {relative}")
        if path.is_file():
            entries.append({"path": relative, "size": path.stat().st_size, "sha256": sha256(path)})
    return entries


def sdl_dylib(executable: Path) -> Path:
    load = subprocess.check_output(["otool", "-l", str(executable)], text=True).splitlines()
    # "path /some dir/lib (offset 12)": the path may contain spaces.
    rpaths = [line.strip()[len("path "):].rsplit(" (offset", 1)[0]
              for line in load if line.strip().startswith("path ")]
    for rpath in rpaths:
        candidate = Path(rpath) / "libSDL2-2.0.0.dylib"
        if candidate.is_file():
            return candidate.resolve()
    raise SystemExit("cannot locate libSDL2-2.0.0.dylib through the executable's rpaths")


def sdl_shared_object(executable: Path) -> Path:
    """Linux (Gate 5): the SDL2 the executable resolves (ldd), shipped as B/lib/<soname>."""
    for line in subprocess.check_output(["ldd", str(executable)], text=True).splitlines():
        name, _, rest = line.strip().partition(" => ")
        if name.startswith("libSDL2-2.0.so") and rest:
            candidate = Path(rest.rsplit(" (0x", 1)[0])
            if candidate.is_file():
                return candidate.resolve()
    raise SystemExit("cannot locate libSDL2-2.0.so.0 through ldd")


def package(build: Path, name: str) -> None:
    dst = ROOT / "runtime" / name
    if dst.exists():
        raise SystemExit(f"{dst} exists; refusing to overwrite")
    executable = build / "candidate-bin/Fukami"
    receipt = build / "gate3-candidate-build-receipt.json"
    for required in (executable, receipt, build / ".rrv-resource-package.lock",
                     build / ".rrv-resource-packages/current/identity"):
        if not required.is_file():
            raise SystemExit(f"build is incomplete: {required}")
    temporary = dst.with_name(dst.name + ".partial")
    if temporary.exists():
        shutil.rmtree(temporary)
    (temporary / "bin").mkdir(parents=True)
    (temporary / "lib").mkdir()
    shutil.copy2(executable, temporary / EXE)
    shutil.copy2(build / ".rrv-resource-package.lock", temporary / ".rrv-resource-package.lock")
    shutil.copytree(build / ".rrv-resource-packages/current", temporary / ".rrv-resource-packages/current",
                    symlinks=False)
    if sys.platform.startswith("linux"):
        # Linux: the launcher sets LD_LIBRARY_PATH to B/lib (same rule as macOS).
        shutil.copy2(sdl_shared_object(executable), temporary / "lib/libSDL2-2.0.so.0")
    else:
        shutil.copy2(sdl_dylib(executable), temporary / "lib/libSDL2-2.0.0.dylib")
    if sha256(temporary / EXE) != sha256(executable):
        raise SystemExit("executable copy differs from the build")
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    manifest = {
        "kind": KIND, "schema_version": 1, "name": name,
        "source_build": str(build.resolve()), "source_commit": commit,
        "build_receipt_sha256": sha256(receipt),
        "executable_sha256": sha256(executable),
        "platform": "linux-x86_64" if sys.platform.startswith("linux") else "macos-arm64",
        "files": inventory(temporary),
    }
    (temporary / "runtime-manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    temporary.rename(dst)
    print(f"packaged {dst} executable_sha256={manifest['executable_sha256']}")


def verify(name: str) -> None:
    dst = ROOT / "runtime" / name
    manifest_path = dst / "runtime-manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(f"runtime package not found: {dst}")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("kind") != KIND or manifest.get("schema_version") != 1:
        raise SystemExit(f"not a {KIND} v1 package: {dst}")
    if inventory(dst) != manifest["files"]:
        raise SystemExit(f"runtime package differs from its manifest: {dst}")
    if sha256(executable_of(dst)) != manifest["executable_sha256"]:
        raise SystemExit(f"runtime executable differs from its manifest: {dst}")
    print(f"verified {dst} executable_sha256={manifest['executable_sha256']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if "/" in args.name or args.name.startswith("."):
        parser.error("--name must be a plain directory name")
    if args.verify == bool(args.build):
        parser.error("give exactly one of --build or --verify")
    if args.verify:
        verify(args.name)
    else:
        package(args.build.resolve(), args.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
