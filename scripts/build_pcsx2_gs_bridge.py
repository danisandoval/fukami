#!/usr/bin/env python3
"""Build the PCSX2 GS bridge on Windows without ever changing the pinned PCSX2 checkout.

The Python counterpart of scripts/build_pcsx2_gs_bridge.sh for Windows (clang-cl, Vulkan). The steps are the shell
script's: check the locked patch series, make a clean detached clone of the pinned PCSX2 revision, stage the locked
patches (the assembled target series, then the Windows stack of config/dependencies.lock.toml), configure and build
`rrv-pcsx2-gs-bridge`, stage the dependency DLLs beside it and write the source- and build-bound manifest that
cmake/RrvProduct.cmake verifies.

Run from a shell that has run vcvars64 (clang-cl, cmake, ninja, git and python on PATH; BUILDING.md), after
    python scripts/prepare_dependencies.py --prepare --component pcsx2
    python scripts/build_pcsx2_windows_deps.py

Environment (the names the shell script uses): PCSX2_SOURCE_DIR, PCSX2_GS_BRIDGE_BUILD_DIR,
PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR, PCSX2_REVISION (must equal the lock). Extra CMake arguments follow `--`.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from prepare_dependencies import load_toml

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK = ROOT / "config" / "dependencies.lock.toml"
BRIDGE_DLL = "rrv-pcsx2-gs-bridge.dll"
# Dependency DLLs PCSX2's Windows build loads next to its executable (the DEPS_TO_COPY list of pcsx2/CMakeLists.txt,
# without Qt). They are staged beside the bridge DLL, which LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH) resolves.
DEPENDENCY_DLLS = ("freetype.dll", "harfbuzz.dll", "jpeg62.dll", "libpng16.dll", "libsharpyuv.dll", "libwebp.dll",
                   "lz4.dll", "SDL3.dll", "shaderc_shared.dll", "z.dll", "zstd.dll", "plutovg.dll", "plutosvg.dll",
                   "ryml.dll")


class Failure(RuntimeError):
    pass


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], *, cwd: pathlib.Path | None = None, capture: bool = False) -> str:
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip() if capture else ""
        raise Failure(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}".rstrip())
    return (result.stdout or "").strip() if capture else ""


def git(source: pathlib.Path, *args: str) -> str:
    return run(["git", "-C", str(source), *args], capture=True)


def locked_entries(lock: dict, name: str) -> list[dict]:
    entries = lock.get(name, [])
    if not isinstance(entries, list):
        raise Failure(f"lock entry {name} must be an array of tables")
    return entries


def staged_patch_files(lock: dict) -> list[tuple[pathlib.Path, str]]:
    """The Windows stack after the target patch: (file, locked sha256), in order; every digest checked."""
    stack = []
    for entry in locked_entries(lock, "pcsx2_bridge_windows_patches"):
        path = ROOT / str(entry["path"])
        if sha256(path) != str(entry["sha256"]):
            raise Failure(f"{entry['path']} does not match its locked SHA-256")
        stack.append((path, str(entry["sha256"])))
    return stack


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--jobs", type=int, default=0, help="parallel build jobs (default: all cores)")
    parser.add_argument("--no-manifest", action="store_true",
                        help="stop after the build (the manifest needs a clean, committed RRV source)")
    parser.add_argument("--manifest-only", action="store_true",
                        help="reuse the existing build and retained patched source and only write the manifest "
                             "(a new RRV commit with unchanged bridge sources needs no rebuild)")
    parser.add_argument("cmake_args", nargs="*", help="extra CMake arguments after --")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise Failure("this script is the Windows build; macOS and Linux use scripts/build_pcsx2_gs_bridge.sh")
    for tool in ("clang-cl", "cmake", "ninja", "git"):
        if not shutil.which(tool):
            raise Failure(f"{tool} is not on PATH (run from a shell that has run vcvars64)")

    lock = load_toml(LOCK)
    pcsx2 = lock["pcsx2"]
    revision = str(pcsx2["revision"])
    if os.environ.get("PCSX2_REVISION") not in (None, "", revision):
        raise Failure(f"PCSX2_REVISION must match the locked revision {revision}")
    source = pathlib.Path(os.environ.get("PCSX2_SOURCE_DIR") or ROOT / str(pcsx2["default_checkout"])).resolve()
    build_dir = pathlib.Path(os.environ.get("PCSX2_GS_BRIDGE_BUILD_DIR") or ROOT / "build-pcsx2-gs-bridge").resolve()
    target_patch_sha = str(locked_entries(lock, "pcsx2_bridge_patches")[0]["sha256"])
    target_patch = ROOT / "tools" / "patches" / "pcsx2-gs-bridge-target.patch"
    windows_stack = staged_patch_files(lock)
    suffix = f"{revision[:12]}-{target_patch_sha[:12]}" + "".join(f"-{sha[:12]}" for _, sha in windows_stack)
    clean_source = pathlib.Path(os.environ.get("PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR") or
                                build_dir.parent / f"{build_dir.name}-source-{suffix}").resolve()
    explicit_clean_source = bool(os.environ.get("PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR"))
    deps = pathlib.Path(os.environ.get("RRV_PCSX2_DEPS_PREFIX") or source / "deps").resolve()

    run([sys.executable, str(ROOT / "scripts" / "prepare_dependencies.py"), "--verify-patches"])
    # The series is the source of the locked target patch: its concatenation must equal the artifact, byte for byte.
    with tempfile.TemporaryDirectory() as scratch:
        assembled = pathlib.Path(scratch) / "bridge-target.patch"
        run([sys.executable, str(ROOT / "scripts" / "pcsx2_patch_series.py"), "assemble", "--output", str(assembled),
             "--expect-sha256", target_patch_sha])
        if assembled.read_bytes() != target_patch.read_bytes():
            raise Failure("tools/patches/pcsx2-gs-bridge-target.patch differs from the concatenated series")

    if not (source / ".git").exists():
        raise Failure(f"PCSX2 source checkout is unavailable: {source}\n"
                      "       python scripts/prepare_dependencies.py --prepare --component pcsx2")
    if not (deps / "lib" / "cmake").is_dir() and not (deps / "include").is_dir():
        raise Failure(f"PCSX2 dependency prefix is missing: {deps}\n       python scripts/build_pcsx2_windows_deps.py")
    git(source, "rev-parse", "--verify", f"{revision}^{{commit}}")

    if args.manifest_only:
        if not clean_source.is_dir() or not (build_dir / "CMakeCache.txt").is_file():
            raise Failure(f"--manifest-only needs the finished build {build_dir} and its retained source {clean_source}")
        return finish(args, build_dir, clean_source, revision, target_patch, target_patch_sha, windows_stack)

    if not clean_source.exists():
        # --no-local forces upload-pack so the retained checkout owns its object database (no alternates). LF line
        # ends: the patches are LF, and Git for Windows would check text=auto files out as CRLF.
        run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "clone", "--no-local", "--no-checkout",
             str(source), str(clean_source)])
        git(clean_source, "config", "core.autocrlf", "false")
        git(clean_source, "config", "core.eol", "lf")
        git(clean_source, "checkout", "--detach", revision)
    elif not explicit_clean_source:
        raise Failure(f"retained bridge source already exists: {clean_source}\n"
                      "       reuse the existing manifest, or select a new PCSX2_GS_BRIDGE_BUILD_DIR")
    if git(clean_source, "rev-parse", "HEAD") != revision:
        raise Failure(f"clean bridge source is not pinned revision {revision}")
    alternates = pathlib.Path(git(clean_source, "rev-parse", "--git-path", "objects/info/alternates"))
    alternates = alternates if alternates.is_absolute() else clean_source / alternates
    if alternates.is_file() and alternates.stat().st_size:
        raise Failure(f"clean bridge source borrows Git objects through alternates: {alternates}")
    if git(clean_source, "status", "--porcelain=v1"):
        # A rerun on a source that already carries the staged patches is fine only if exactly those are staged.
        raise Failure(f"clean bridge source has local edits (or is already patched): {clean_source}")

    patches = [target_patch, *(path for path, _ in windows_stack)]
    for patch in patches:
        git(clean_source, "apply", "--index", "--check", str(patch))
        git(clean_source, "apply", "--index", str(patch))
    git(clean_source, "diff", "--cached", "--check")

    # Release binaries must not carry the builder's directory names (assert messages and PDB paths).
    maps = [f"{clean_source.as_posix()}=/pcsx2", f"{ROOT.as_posix()}=/fukami", f"{build_dir.as_posix()}=/bridge-build"]
    prefix_map = " ".join(f"/clang:-ffile-prefix-map={entry}" for entry in maps)
    # Multi-ISA GS (SSE4/AVX/AVX2 selected at run time), as the Linux build; not the build machine's own ISA.
    configure = ["cmake", "-S", str(clean_source), "-B", str(build_dir), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
                 "-DCMAKE_LINKER=lld-link", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
                 f"-DCMAKE_PREFIX_PATH={deps.as_posix()}", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                 "-DENABLE_QT_UI=OFF", "-DENABLE_TESTS=OFF", "-DUSE_VULKAN=ON", "-DUSE_OPENGL=OFF",
                 "-DDISABLE_ADVANCE_SIMD=ON",
                 f"-DCMAKE_C_FLAGS={prefix_map}", f"-DCMAKE_CXX_FLAGS={prefix_map}",
                 "-DCMAKE_SHARED_LINKER_FLAGS=/Brepro /PDBALTPATH:%_PDB%",
                 f"-DRRV_PCSX2_GS_BRIDGE_SOURCE_DIR={(ROOT / 'tools' / 'pcsx2-gs-bridge').as_posix()}",
                 f"-DRRV_PCSX2_DEPS_BIN={(deps / 'bin').as_posix()}", *args.cmake_args]
    run(configure)
    build = ["cmake", "--build", str(build_dir), "--target", "rrv-pcsx2-gs-bridge"]
    if args.jobs:
        build += ["--parallel", str(args.jobs)]
    else:
        build += ["--parallel"]
    run(build)
    return finish(args, build_dir, clean_source, revision, target_patch, target_patch_sha, windows_stack)


def finish(args: argparse.Namespace, build_dir: pathlib.Path, clean_source: pathlib.Path, revision: str,
           target_patch: pathlib.Path, target_patch_sha: str, windows_stack: list[tuple[pathlib.Path, str]]) -> int:
    bridge_dir = build_dir / "pcsx2-gs-bridge"
    library = bridge_dir / BRIDGE_DLL
    if not library.is_file():
        raise Failure(f"bridge build reported success but {library} is missing")
    missing = [name for name in DEPENDENCY_DLLS if not (bridge_dir / name).is_file()]
    if missing:
        raise Failure(f"dependency DLLs were not staged beside the bridge: {', '.join(missing)}")

    manifest = build_dir / "rrv-pcsx2-gs-bridge-manifest.json"
    if not args.no_manifest:
        command = [sys.executable, str(ROOT / "scripts" / "pcsx2_bridge_manifest.py"), "create",
                   "--library", str(library), "--pcsx2-source", str(clean_source), "--build-dir", str(build_dir),
                   "--bridge-patch", str(target_patch), "--bridge-patch-sha256", target_patch_sha,
                   "--pcsx2-commit", revision, "--output", str(manifest)]
        for path, sha in windows_stack:
            command += ["--windows-bridge-patch", f"{path}={sha}"]
        run(command)
    print(f"built in: {bridge_dir}")
    print(f"retained patched source: {clean_source}")
    print(f"pinned PCSX2 revision: {revision}")
    if not args.no_manifest:
        print(f"stamp: {build_dir / 'RRV_BRIDGE_STAMP.json'}")
        print(f"manifest: {manifest}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
