#!/usr/bin/env python3
"""Create or verify the source- and build-bound M0R PCSX2 GS bridge manifest."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import os
import shutil
import subprocess
import sys
import tempfile
from typing import Optional, Union


ROOT = pathlib.Path(__file__).resolve().parents[1]
BRIDGE_SOURCE = ROOT / "tools" / "pcsx2-gs-bridge"
# Linux (Gate 5): the Vulkan bridge applies one extra patch on top of the locked
# target patch and has no Metal resources. macOS manifests are unchanged.
LINUX_BRIDGE = sys.platform.startswith("linux")


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(command: list[str], cwd: pathlib.Path, *, text: bool = True) -> Union[str, bytes]:
    completed = subprocess.run(command, cwd=cwd, text=text,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        stderr = completed.stderr.strip()
        stdout = completed.stdout.strip()
        raise RuntimeError(stderr or stdout or f"command failed: {' '.join(command)}")
    return completed.stdout.strip() if text else completed.stdout


def git(*args: str, cwd: pathlib.Path = ROOT) -> str:
    return str(run(["git", *args], cwd))


def portable_path(path: pathlib.Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(ROOT).as_posix()
    except ValueError:
        return str(resolved)


def manifest_path(value: str) -> pathlib.Path:
    path = pathlib.Path(value)
    return path if path.is_absolute() else ROOT / path


def bridge_source_files() -> list[pathlib.Path]:
    names = git("ls-files", "tools/pcsx2-gs-bridge").splitlines()
    paths = [ROOT / name for name in names]
    if not paths or any(not path.is_file() for path in paths):
        raise RuntimeError("tracked PCSX2 bridge source inventory is incomplete")
    return paths


def inventory(directory: pathlib.Path) -> list[dict[str, object]]:
    files = [path for path in sorted(directory.rglob("*")) if path.is_file()]
    if not files:
        raise RuntimeError(f"runtime resource directory is empty: {directory}")
    return [{
        "path": path.relative_to(directory).as_posix(),
        "size": path.stat().st_size,
        "sha256": sha256(path),
    } for path in files]


def inventory_fingerprint(entries: list[dict[str, object]]) -> str:
    encoded = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return sha256_bytes(encoded)


def validate_metal_source_manifest(path: pathlib.Path, resource_dir: pathlib.Path,
                                   pcsx2_commit: str, patch_sha256: str) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("pcsx2_revision") != pcsx2_commit:
        raise RuntimeError("Metal source manifest has the wrong PCSX2 revision")
    if data.get("bridge_patch", {}).get("sha256") != patch_sha256:
        raise RuntimeError("Metal source manifest has the wrong bridge patch")
    entries = data.get("files", [])
    if {entry.get("path") for entry in entries} != {
            "default.metallib", "Metal22.metallib", "Metal23.metallib"}:
        raise RuntimeError("Metal source manifest has the wrong resource set")
    for entry in entries:
        resource = resource_dir / entry["path"]
        if not resource.is_file() or sha256(resource) != entry.get("sha256"):
            raise RuntimeError(f"Metal resource does not match source manifest: {resource}")
    return data


def reverse_check_patch_stack(source: pathlib.Path, patches: list[pathlib.Path]) -> None:
    """Prove the staged index is exactly HEAD plus `patches` applied in order.

    Reverse-applies the stack last-first on a private copy of the index (the
    checkout's own index is never touched) and requires HEAD's tree to remain.
    """
    index = pathlib.Path(git("rev-parse", "--git-path", "index", cwd=source))
    if not index.is_absolute():
        index = source / index
    with tempfile.TemporaryDirectory() as scratch:
        scratch_index = pathlib.Path(scratch) / "index"
        shutil.copyfile(index, scratch_index)
        env = dict(os.environ, GIT_INDEX_FILE=str(scratch_index))
        for patch in reversed(patches):
            completed = subprocess.run(
                ["git", "apply", "--cached", "--reverse", str(patch)], cwd=source, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if completed.returncode:
                raise RuntimeError(f"staged PCSX2 source does not end with {patch.name}: "
                                   f"{completed.stderr.strip()}")
        completed = subprocess.run(["git", "write-tree"], cwd=source, env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if completed.returncode or completed.stdout.strip() != git("rev-parse", "HEAD^{tree}", cwd=source):
            raise RuntimeError("staged PCSX2 source is not exactly HEAD plus the bridge patches")


def pcsx2_source_state(source: pathlib.Path, base_commit: str,
                       patch: pathlib.Path,
                       extra_patches: Optional[list[pathlib.Path]] = None) -> dict[str, object]:
    if not (source / ".git").exists():
        raise RuntimeError(f"PCSX2 source is not a Git checkout: {source}")
    head = git("rev-parse", "HEAD", cwd=source)
    if head != base_commit:
        raise RuntimeError(f"PCSX2 source HEAD is {head}, expected {base_commit}")
    alternates = pathlib.Path(git("rev-parse", "--git-path", "objects/info/alternates",
                                 cwd=source))
    if not alternates.is_absolute():
        alternates = source / alternates
    if alternates.is_file() and alternates.read_text(encoding="utf-8").strip():
        raise RuntimeError("PCSX2 source borrows Git objects through alternates")
    status = git("status", "--porcelain=v1", "--untracked-files=all", cwd=source)
    lines = status.splitlines()
    if not lines:
        raise RuntimeError("PCSX2 source does not contain the staged bridge patch")
    invalid = [line for line in lines if len(line) < 2 or line.startswith("??") or line[1] != " "]
    if invalid:
        raise RuntimeError("PCSX2 source contains unstaged or untracked changes: " + "; ".join(invalid))
    run(["git", "diff", "--cached", "--check"], source)
    if extra_patches:
        reverse_check_patch_stack(source, [patch, *extra_patches])
    else:
        run(["git", "apply", "--cached", "--reverse", "--check", str(patch)], source)
    staged_diff = run(["git", "diff", "--cached", "--binary", "HEAD", "--"],
                      source, text=False)
    assert isinstance(staged_diff, bytes)
    return {
        "path": portable_path(source),
        "base_commit": head,
        "patched_tree": git("write-tree", cwd=source),
        "staged_diff_sha256": sha256_bytes(staged_diff),
        "status": lines,
    }


def cmake_cache_values(cache: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line or ":" not in line:
            continue
        key_type, value = line.split("=", 1)
        key, _ = key_type.split(":", 1)
        values[key] = value
    return values


def validate_build_cache(build_dir: pathlib.Path, source: pathlib.Path,
                         resource_dir: Optional[pathlib.Path]) -> dict[str, object]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"bridge CMake cache is missing: {cache}")
    values = cmake_cache_values(cache)
    checks = {
        "CMAKE_HOME_DIRECTORY": source.resolve(),
        "RRV_PCSX2_GS_BRIDGE_SOURCE_DIR": BRIDGE_SOURCE.resolve(),
    }
    if resource_dir is not None:
        checks["RRV_PCSX2_GS_METAL_RESOURCE_DIR"] = resource_dir.resolve()
    elif values.get("RRV_PCSX2_GS_METAL_RESOURCE_DIR"):
        raise RuntimeError("Linux bridge CMake cache unexpectedly names Metal resources")
    for key, expected in checks.items():
        actual = pathlib.Path(values.get(key, "")).resolve()
        if actual != expected:
            raise RuntimeError(f"CMake cache {key} is {actual}, expected {expected}")
    if values.get("CMAKE_BUILD_TYPE") != "Release":
        raise RuntimeError("bridge CMake cache is not a Release build")
    generator = values.get("CMAKE_GENERATOR", "")
    if not generator:
        raise RuntimeError("bridge CMake cache does not identify its generator")
    build: dict[str, object] = {
        "directory": portable_path(build_dir),
        "cmake_cache": portable_path(cache),
        "cmake_cache_sha256": sha256(cache),
        "cmake_home": portable_path(source),
        "generator": generator,
        "build_type": values["CMAKE_BUILD_TYPE"],
        "bridge_source": portable_path(BRIDGE_SOURCE),
    }
    if resource_dir is not None:
        build["metal_resource_source"] = portable_path(resource_dir)
    return build


def create(args: argparse.Namespace) -> int:
    if LINUX_BRIDGE:
        return create_linux(args)
    if args.metal_resource_dir is None or args.metal_verifier_manifest is None:
        raise RuntimeError("--metal-resource-dir and --metal-verifier-manifest are required on macOS")
    library = args.library.resolve()
    pcsx2_source = args.pcsx2_source.resolve()
    build_dir = args.build_dir.resolve()
    resource_dir = args.metal_resource_dir.resolve()
    verifier_manifest = args.metal_verifier_manifest.resolve()
    source_manifest = resource_dir / "metal-resources-manifest.json"
    runtime_resource_dir = library.parent / "resources"
    patch = args.bridge_patch.resolve()
    for required in (library, verifier_manifest, source_manifest, patch):
        if not required.is_file():
            raise RuntimeError(f"required bridge provenance input is missing: {required}")
    if git("status", "--porcelain=v1"):
        raise RuntimeError("RRV source must be clean before creating a bridge manifest")
    patch_hash = sha256(patch)
    if patch_hash != args.bridge_patch_sha256:
        raise RuntimeError("bridge patch does not match its locked SHA-256")
    validate_metal_source_manifest(source_manifest, resource_dir,
                                   args.pcsx2_commit, patch_hash)
    source_state = pcsx2_source_state(pcsx2_source, args.pcsx2_commit, patch)
    build = validate_build_cache(build_dir, pcsx2_source, resource_dir)
    runtime_resources = inventory(runtime_resource_dir)
    sources = [
        {"path": path.relative_to(ROOT).as_posix(), "sha256": sha256(path)}
        for path in bridge_source_files()
    ]
    library_data = {
        "path": portable_path(library),
        "size": library.stat().st_size,
        "sha256": sha256(library),
    }
    receipt = {
        "schema": 1,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "architecture": platform.machine(),
        "rrv_commit": git("rev-parse", "HEAD"),
        "pcsx2_source": source_state,
        "bridge_patch_sha256": patch_hash,
        "bridge_source_inventory_sha256": inventory_fingerprint(sources),
        "build": build,
        "library": library_data,
    }
    stamp_path = build_dir / "RRV_BRIDGE_STAMP.json"
    stamp_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    data = {
        "schema": 2,
        "created_utc": receipt["created_utc"],
        "architecture": platform.machine(),
        "rrv_commit": receipt["rrv_commit"],
        "rrv_source_clean": True,
        "pcsx2_commit": args.pcsx2_commit,
        "pcsx2_source": source_state,
        "bridge_patch": {
            "path": patch.relative_to(ROOT).as_posix(),
            "sha256": patch_hash,
        },
        "bridge_source": {
            "inventory_sha256": receipt["bridge_source_inventory_sha256"],
            "files": sources,
        },
        "build_receipt": {
            "path": portable_path(stamp_path),
            "sha256": sha256(stamp_path),
            "data": receipt,
        },
        "metal_resources": {
            "directory": portable_path(resource_dir),
            "verifier_manifest": portable_path(verifier_manifest),
            "verifier_manifest_sha256": sha256(verifier_manifest),
            "source_manifest": portable_path(source_manifest),
            "source_manifest_sha256": sha256(source_manifest),
        },
        "runtime_resources": {
            "directory": portable_path(runtime_resource_dir),
            "file_count": len(runtime_resources),
            "files": runtime_resources,
        },
        "library": library_data,
    }
    args.output.resolve().write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"bridge manifest: {args.output.resolve()}")
    return 0


def create_linux(args: argparse.Namespace) -> int:
    """Linux/Vulkan bridge manifest: same schema 2 identity as macOS, plus the
    stacked Linux patch, and no Metal resources (Vulkan compiles its GLSL from
    the staged `resources/shaders/vulkan` at run time)."""
    if args.metal_resource_dir is not None or args.metal_verifier_manifest is not None:
        raise RuntimeError("Metal resources are not part of a Linux bridge manifest")
    if args.linux_bridge_patch is None or args.linux_bridge_patch_sha256 is None:
        raise RuntimeError("--linux-bridge-patch and --linux-bridge-patch-sha256 are required on Linux")
    library = args.library.resolve()
    pcsx2_source = args.pcsx2_source.resolve()
    build_dir = args.build_dir.resolve()
    runtime_resource_dir = library.parent / "resources"
    patch = args.bridge_patch.resolve()
    linux_patch = args.linux_bridge_patch.resolve()
    for required in (library, patch, linux_patch):
        if not required.is_file():
            raise RuntimeError(f"required bridge provenance input is missing: {required}")
    if git("status", "--porcelain=v1"):
        raise RuntimeError("RRV source must be clean before creating a bridge manifest")
    patch_hash = sha256(patch)
    if patch_hash != args.bridge_patch_sha256:
        raise RuntimeError("bridge patch does not match its locked SHA-256")
    linux_patch_hash = sha256(linux_patch)
    if linux_patch_hash != args.linux_bridge_patch_sha256:
        raise RuntimeError("Linux bridge patch does not match its expected SHA-256")
    source_state = pcsx2_source_state(pcsx2_source, args.pcsx2_commit, patch, [linux_patch])
    build = validate_build_cache(build_dir, pcsx2_source, None)
    runtime_resources = inventory(runtime_resource_dir)
    sources = [
        {"path": path.relative_to(ROOT).as_posix(), "sha256": sha256(path)}
        for path in bridge_source_files()
    ]
    library_data = {
        "path": portable_path(library),
        "size": library.stat().st_size,
        "sha256": sha256(library),
    }
    receipt = {
        "schema": 1,
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "architecture": platform.machine(),
        "platform": "linux",
        "rrv_commit": git("rev-parse", "HEAD"),
        "pcsx2_source": source_state,
        "bridge_patch_sha256": patch_hash,
        "linux_bridge_patch_sha256": linux_patch_hash,
        "bridge_source_inventory_sha256": inventory_fingerprint(sources),
        "build": build,
        "library": library_data,
    }
    stamp_path = build_dir / "RRV_BRIDGE_STAMP.json"
    stamp_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    data = {
        "schema": 2,
        "platform": "linux",
        "created_utc": receipt["created_utc"],
        "architecture": platform.machine(),
        "rrv_commit": receipt["rrv_commit"],
        "rrv_source_clean": True,
        "pcsx2_commit": args.pcsx2_commit,
        "pcsx2_source": source_state,
        "bridge_patch": {
            "path": patch.relative_to(ROOT).as_posix(),
            "sha256": patch_hash,
        },
        "linux_bridge_patch": {
            "path": linux_patch.relative_to(ROOT).as_posix(),
            "sha256": linux_patch_hash,
        },
        "bridge_source": {
            "inventory_sha256": receipt["bridge_source_inventory_sha256"],
            "files": sources,
        },
        "build_receipt": {
            "path": portable_path(stamp_path),
            "sha256": sha256(stamp_path),
            "data": receipt,
        },
        "runtime_resources": {
            "directory": portable_path(runtime_resource_dir),
            "file_count": len(runtime_resources),
            "files": runtime_resources,
        },
        "library": library_data,
    }
    args.output.resolve().write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"bridge manifest: {args.output.resolve()}")
    return 0


def verify(args: argparse.Namespace) -> int:
    manifest = args.manifest.resolve()
    data = json.loads(manifest.read_text(encoding="utf-8"))
    problems: list[str] = []
    if data.get("schema") != 2:
        problems.append("unsupported bridge manifest schema")
    if data.get("pcsx2_commit") != args.expected_pcsx2_commit:
        problems.append("bridge manifest PCSX2 commit mismatch")
    if data.get("rrv_commit") != args.expected_rrv_commit or not data.get("rrv_source_clean"):
        problems.append("bridge manifest RRV source identity mismatch")
    if data.get("architecture") != platform.machine():
        problems.append("bridge manifest architecture mismatch")
    patch = data.get("bridge_patch", {})
    patch_path = ROOT / patch.get("path", "")
    if (patch.get("sha256") != args.expected_bridge_patch_sha256 or
            not patch_path.is_file() or sha256(patch_path) != patch.get("sha256")):
        problems.append("bridge patch identity mismatch")
    linux_manifest = data.get("platform") == "linux"
    if linux_manifest != LINUX_BRIDGE:
        problems.append("bridge manifest platform mismatch")
    extra_patches: Optional[list[pathlib.Path]] = None
    if linux_manifest:
        linux_patch = data.get("linux_bridge_patch", {})
        linux_patch_path = ROOT / linux_patch.get("path", "")
        if (not linux_patch.get("sha256") or not linux_patch_path.is_file() or
                sha256(linux_patch_path) != linux_patch.get("sha256") or
                (args.expected_linux_bridge_patch_sha256 is not None and
                 linux_patch.get("sha256") != args.expected_linux_bridge_patch_sha256)):
            problems.append("Linux bridge patch identity mismatch")
        extra_patches = [linux_patch_path]
    source_expected = data.get("pcsx2_source", {})
    source_dir = manifest_path(source_expected.get("path", ""))
    if not problems:
        try:
            source_actual = pcsx2_source_state(source_dir, args.expected_pcsx2_commit,
                                               patch_path, extra_patches)
            if source_actual != source_expected:
                problems.append("PCSX2 patched source tree or staged diff mismatch")
        except RuntimeError as error:
            problems.append(str(error))
    library = data.get("library", {})
    library_path = manifest_path(library.get("path", ""))
    if (not library_path.is_file() or library_path.stat().st_size != library.get("size") or
            sha256(library_path) != library.get("sha256")):
        problems.append("bridge library hash/size mismatch")
    source_entries = data.get("bridge_source", {}).get("files", [])
    actual_source_entries = [
        {"path": path.relative_to(ROOT).as_posix(), "sha256": sha256(path)}
        for path in bridge_source_files()
    ]
    if (source_entries != actual_source_entries or
            inventory_fingerprint(actual_source_entries) !=
            data.get("bridge_source", {}).get("inventory_sha256")):
        problems.append("bridge source inventory mismatch")
    resource_dir: Optional[pathlib.Path] = None
    if linux_manifest:
        if "metal_resources" in data:
            problems.append("Linux bridge manifest unexpectedly names Metal resources")
    else:
        metal = data.get("metal_resources", {})
        resource_dir = manifest_path(metal.get("directory", ""))
        verifier_manifest = manifest_path(metal.get("verifier_manifest", ""))
        source_manifest = manifest_path(metal.get("source_manifest", ""))
        if (not verifier_manifest.is_file() or
                sha256(verifier_manifest) != metal.get("verifier_manifest_sha256")):
            problems.append("Metal verifier manifest mismatch")
        if (not source_manifest.is_file() or
                sha256(source_manifest) != metal.get("source_manifest_sha256")):
            problems.append("Metal source manifest mismatch")
        else:
            try:
                validate_metal_source_manifest(source_manifest, resource_dir,
                                               args.expected_pcsx2_commit,
                                               args.expected_bridge_patch_sha256)
            except RuntimeError as error:
                problems.append(str(error))
    runtime_resources = data.get("runtime_resources", {})
    runtime_resource_dir = manifest_path(runtime_resources.get("directory", ""))
    runtime_entries = runtime_resources.get("files", [])
    actual_paths = ({path.relative_to(runtime_resource_dir).as_posix()
                     for path in runtime_resource_dir.rglob("*") if path.is_file()}
                    if runtime_resource_dir.is_dir() else set())
    expected_paths = {entry.get("path") for entry in runtime_entries}
    if actual_paths != expected_paths or len(runtime_entries) != runtime_resources.get("file_count"):
        problems.append("bridge runtime resource inventory mismatch")
    else:
        for entry in runtime_entries:
            path = runtime_resource_dir / entry["path"]
            if (path.stat().st_size != entry.get("size") or
                    sha256(path) != entry.get("sha256")):
                problems.append(f"bridge runtime resource mismatch: {path}")
    receipt_wrapper = data.get("build_receipt", {})
    receipt_path = manifest_path(receipt_wrapper.get("path", ""))
    receipt_data = receipt_wrapper.get("data", {})
    if (not receipt_path.is_file() or sha256(receipt_path) != receipt_wrapper.get("sha256")):
        problems.append("bridge build receipt hash mismatch")
    else:
        on_disk_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if on_disk_receipt != receipt_data:
            problems.append("bridge build receipt content mismatch")
    try:
        actual_build = validate_build_cache(
            manifest_path(receipt_data.get("build", {}).get("directory", "")),
            source_dir, resource_dir)
        if actual_build != receipt_data.get("build"):
            problems.append("bridge CMake build identity mismatch")
    except RuntimeError as error:
        problems.append(str(error))
    if (receipt_data.get("pcsx2_source") != source_expected or
            receipt_data.get("library") != library or
            receipt_data.get("rrv_commit") != data.get("rrv_commit") or
            receipt_data.get("bridge_patch_sha256") != patch.get("sha256") or
            receipt_data.get("bridge_source_inventory_sha256") !=
            data.get("bridge_source", {}).get("inventory_sha256") or
            (linux_manifest and receipt_data.get("linux_bridge_patch_sha256") !=
             data.get("linux_bridge_patch", {}).get("sha256"))):
        problems.append("bridge build receipt does not bind manifest inputs")
    if problems:
        print(*dict.fromkeys(problems), sep="\n", file=sys.stderr)
        return 1
    print(f"verified source-built bridge {library['sha256']} from PCSX2 tree "
          f"{source_expected['patched_tree']}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--library", type=pathlib.Path, required=True)
    create_parser.add_argument("--pcsx2-source", type=pathlib.Path, required=True)
    create_parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    # Required on macOS; must be absent on Linux (Vulkan has no Metal resources).
    create_parser.add_argument("--metal-resource-dir", type=pathlib.Path)
    create_parser.add_argument("--metal-verifier-manifest", type=pathlib.Path)
    # Linux only: tools/patches/pcsx2-gs-bridge-linux.patch, stacked after the target patch.
    create_parser.add_argument("--linux-bridge-patch", type=pathlib.Path)
    create_parser.add_argument("--linux-bridge-patch-sha256")
    create_parser.add_argument("--bridge-patch", type=pathlib.Path, required=True)
    create_parser.add_argument("--bridge-patch-sha256", required=True)
    create_parser.add_argument("--pcsx2-commit", required=True)
    create_parser.add_argument("--output", type=pathlib.Path, required=True)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--manifest", type=pathlib.Path, required=True)
    verify_parser.add_argument("--expected-rrv-commit", required=True)
    verify_parser.add_argument("--expected-pcsx2-commit", required=True)
    verify_parser.add_argument("--expected-bridge-patch-sha256", required=True)
    verify_parser.add_argument("--expected-linux-bridge-patch-sha256")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return create(args) if args.mode == "create" else verify(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
