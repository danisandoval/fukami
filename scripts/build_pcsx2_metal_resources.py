#!/usr/bin/env python3
"""Build the pinned PCSX2 Metal shader libraries from clean source.

Outputs are generated artifacts. The output directory must be supplied
explicitly and must be new or empty; this script never reads a PCSX2.app or a
prebuilt metallib.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.9 supplied by older macOS toolchains.
    tomllib = None


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK_PATH = REPO_ROOT / "config" / "dependencies.lock.toml"
PIN_MARKER = REPO_ROOT / "tools" / "pcsx2-gs-bridge" / "PINNED_REVISION"
SHADERS = (
    "pcsx2/GS/Renderers/Metal/cas.metal",
    "pcsx2/GS/Renderers/Metal/convert.metal",
    "pcsx2/GS/Renderers/Metal/present.metal",
    "pcsx2/GS/Renderers/Metal/merge.metal",
    "pcsx2/GS/Renderers/Metal/misc.metal",
    "pcsx2/GS/Renderers/Metal/interlace.metal",
    "pcsx2/GS/Renderers/Metal/tfx.metal",
    "pcsx2/GS/Renderers/Metal/fxaa.metal",
)
SHADER_HEADERS = (
    "pcsx2/GS/Renderers/Metal/GSMTLSharedHeader.h",
    "pcsx2/GS/Renderers/Metal/GSMTLShaderCommon.h",
)
LIBRARIES = (
    ("default", "macos-metal2.0", "air64-apple-macos10.13"),
    ("Metal22", "macos-metal2.2", "air64-apple-macos10.15"),
    ("Metal23", "macos-metal2.3", "air64-apple-macos11.0"),
)


def run(command: list[str], *, cwd: pathlib.Path | None = None,
        environment: dict[str, str] | None = None) -> str:
    completed = subprocess.run(command, cwd=cwd, env=environment, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        sys.stderr.write(completed.stdout)
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}")
    return completed.stdout


def select_metallib_linker(*, environment: dict[str, str] | None = None) -> tuple[list[str], str, str]:
    """Select the available AIR linker and return argv, version, and kind.

    Xcode 27 ships ``metal`` with AIR linking support but may omit the
    dedicated ``metallib`` executable.  Keep the preferred tool when it is
    present; otherwise use ``metal`` for the same AIR-to-metallib operation.
    The ``kind`` value is recorded so a fallback is never mislabelled as a
    dedicated metallib invocation.
    """
    discovery = subprocess.run(
        ["xcrun", "--find", "metallib"], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if discovery.returncode:
        version = run(["xcrun", "metal", "-v"], environment=environment)
        return ["xcrun", "metal"], version, "metal-air-linker"
    # Discovery succeeded, so a version/invocation failure is a real
    # toolchain error and must not be reclassified as tool absence.
    version = run(["xcrun", "metallib", "-v"], environment=environment)
    return ["xcrun", "metallib"], version, "metallib"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_lock() -> tuple[str, pathlib.Path, str]:
    """Return PCSX2 revision and the sole locked bridge patch identity."""
    if tomllib is not None:
        with LOCK_PATH.open("rb") as stream:
            lock = tomllib.load(stream)
    else:
        # The lock deliberately uses only quoted string values in the two
        # sections consumed here, so retain the repository's Python-3.9 path.
        lock: dict[str, object] = {}
        section: str | None = None
        for raw in LOCK_PATH.read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[[") and line.endswith("]]" ):
                section = line[2:-2]
                lock.setdefault(section, []).append({})
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                lock[section] = {}
                continue
            if not section or "=" not in line:
                continue
            key, value = (part.strip() for part in line.split("=", 1))
            if not (value.startswith('"') and value.endswith('"')):
                continue
            if isinstance(lock[section], list):
                lock[section][-1][key] = value[1:-1]
            else:
                lock[section][key] = value[1:-1]
    pcsx2 = lock.get("pcsx2")
    patches = lock.get("pcsx2_bridge_patches")
    if not isinstance(pcsx2, dict) or not isinstance(patches, list) or len(patches) != 1:
        raise RuntimeError("dependencies.lock.toml has no unambiguous PCSX2 bridge patch")
    revision = pcsx2.get("revision")
    patch = patches[0]
    if not isinstance(revision, str) or not isinstance(patch, dict):
        raise RuntimeError("malformed PCSX2 dependency lock")
    patch_name = patch.get("path")
    patch_hash = patch.get("sha256")
    if not isinstance(patch_name, str) or not isinstance(patch_hash, str):
        raise RuntimeError("malformed PCSX2 bridge patch lock entry")
    patch_path = (REPO_ROOT / patch_name).resolve()
    if REPO_ROOT not in patch_path.parents or not patch_path.is_file():
        raise RuntimeError(f"locked bridge patch is unavailable: {patch_name}")
    return revision, patch_path, patch_hash


def ensure_clean_source(source: pathlib.Path, revision: str, patch: pathlib.Path,
                        patch_hash: str) -> None:
    if not (source / ".git").exists():
        raise RuntimeError(f"not a Git checkout: {source}")
    if run(["git", "rev-parse", "HEAD"], cwd=source).strip() != revision:
        raise RuntimeError(f"source HEAD is not pinned PCSX2 revision {revision}")
    if run(["git", "status", "--porcelain=v1"], cwd=source):
        raise RuntimeError("PCSX2 source checkout is dirty")
    if PIN_MARKER.read_text(encoding="utf-8").strip() != revision:
        raise RuntimeError("tools/pcsx2-gs-bridge/PINNED_REVISION disagrees with the dependency lock")
    if sha256(patch) != patch_hash:
        raise RuntimeError(f"locked bridge patch hash mismatch: {patch}")
    run(["git", "apply", "--check", str(patch)], cwd=source)
    for relative in (*SHADERS, *SHADER_HEADERS):
        if not (source / relative).is_file():
            raise RuntimeError(f"pinned PCSX2 source is missing required Metal input: {relative}")


def write_verifier_manifest(output: pathlib.Path, revision: str,
                            hashes: dict[str, str]) -> None:
    lines = [f'pcsx2_revision = "{revision}"', ""]
    for name in ("default.metallib", "Metal22.metallib", "Metal23.metallib"):
        lines.extend(("[[files]]", f'path = "{name}"', f'sha256 = "{hashes[name]}"', ""))
    (output / "metal-resources-manifest.toml").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=pathlib.Path,
                        help="clean detached PCSX2 checkout at the locked revision")
    parser.add_argument("--output", required=True, type=pathlib.Path,
                        help="new or empty directory for all generated resources and manifests")
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise RuntimeError(f"output directory must be new or empty: {output}")
    else:
        output.mkdir(parents=True)

    revision, patch, patch_hash = read_lock()
    ensure_clean_source(source, revision, patch, patch_hash)

    # Keep Clang's module cache in the declared output tree. `metal` needs both
    # the environment and explicit Clang flag on current Apple toolchains.
    # This affects only compiler cache placement, not shader source/options.
    environment = os.environ.copy()
    module_cache = output / "clang-module-cache"
    environment["CLANG_MODULE_CACHE_PATH"] = str(module_cache)
    metal_version = run(["xcrun", "metal", "-v"], environment=environment)
    metallib_linker, metallib_linker_version, metallib_linker_kind = select_metallib_linker(
        environment=environment)
    toolchain = {
        "metal_version": metal_version,
        # Retain this key for consumers of schema 1.  It carries the selected
        # linker's version; the fields below disambiguate the fallback identity.
        "metallib_version": metallib_linker_version,
        "metallib_linker": {
            "command": metallib_linker,
            "kind": metallib_linker_kind,
            "version": metallib_linker_version,
        },
        "sdk_path": run(["xcrun", "--show-sdk-path"], environment=environment),
        "xcode_version": run(["xcodebuild", "-version"], environment=environment),
    }
    input_hashes = {name: sha256(source / name) for name in (*SHADERS, *SHADER_HEADERS)}
    commands: list[list[str]] = []
    output_hashes: dict[str, str] = {}
    air_root = output / "air"

    for library, language, triple in LIBRARIES:
        air_files: list[pathlib.Path] = []
        for relative in SHADERS:
            air = air_root / library / f"{relative}.air"
            air.parent.mkdir(parents=True, exist_ok=True)
            command = [
                "xcrun", "metal", "-ffast-math", f"-std={language}", "-target", triple,
                f"-fmodules-cache-path={module_cache}",
                "-o", str(air), "-c", str(source / relative),
            ]
            run(command, environment=environment)
            commands.append(command)
            air_files.append(air)
        metallib = output / f"{library}.metallib"
        command = [*metallib_linker, "-o", str(metallib), *map(str, air_files)]
        run(command, environment=environment)
        commands.append(command)
        output_hashes[metallib.name] = sha256(metallib)

    manifest = {
        "schema": 1,
        "pcsx2_revision": revision,
        "bridge_patch": {"path": str(patch.relative_to(REPO_ROOT)), "sha256": patch_hash},
        "source": str(source),
        "source_status": "clean detached",
        "toolchain": toolchain,
        "source_inputs_sha256": input_hashes,
        "commands": commands,
        "files": [{"path": name, "sha256": digest} for name, digest in sorted(output_hashes.items())],
    }
    (output / "metal-resources-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_verifier_manifest(output, revision, output_hashes)
    print(f"built PCSX2 Metal resources in: {output}")
    print(f"manifest for verifier: {output / 'metal-resources-manifest.toml'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
