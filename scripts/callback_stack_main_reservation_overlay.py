#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Materialize the opt-in callback-stack reservation overlay without editing the producer."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from m2_dma_overlay import require_locked_clean_producer


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "tools/patches/ps2recomp-runtime-callback-stack-main-reservation.patch"
PATCH_SHA256 = "460d1e3019a35fa15ecea4e43a581f8db8e6d096dad6a8d27747170f26bf6b9a"
RUNTIME = "src/lib/ps2_runtime.cpp"
HEADER = "include/ps2_runtime.h"
CANONICAL_FILES = (
    "src/lib/Kernel/Syscalls/System.cpp",
    "src/lib/Kernel/Syscalls/Interrupt.cpp",
    "src/lib/Kernel/Syscalls/Helpers/Runtime.h",
    "src/lib/Kernel/Stubs/CD.cpp",
    "src/lib/Kernel/Stubs/GS.cpp",
)
CLOSURE_FILES = (
    "src/lib/Kernel/Syscalls/Sync.cpp",
    "src/lib/Kernel/Syscalls/Common.h",
    "src/lib/Kernel/Stubs/Common.h",
)
OVERLAY_SOURCE_FILES = CANONICAL_FILES + CLOSURE_FILES


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes:
    if not path.is_file():
        raise RuntimeError(f"callback-stack overlay input is unavailable: {path}")
    return path.read_bytes()


def input_files(runtime_source: Path, header_input: Path, runtime_input: Path) -> dict[str, bytes]:
    values = {HEADER: read(header_input), RUNTIME: read(runtime_input)}
    for relative in OVERLAY_SOURCE_FILES:
        values[relative] = read(runtime_source / relative)
    return values


def apply_patch(inputs: dict[str, bytes]) -> dict[str, bytes]:
    patch = read(PATCH)
    if sha256(patch) != PATCH_SHA256:
        raise RuntimeError("callback-stack overlay patch SHA-256 mismatch")
    with tempfile.TemporaryDirectory(prefix="rrv-callback-stack-overlay-") as directory:
        root = Path(directory)
        staged = root / "ps2xRuntime"
        for relative, raw in inputs.items():
            destination = staged / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(raw)
        for options in (("--check",), ()):
            result = subprocess.run(
                ["git", "apply", "--whitespace=nowarn", *options, str(PATCH)], cwd=root,
                text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError("callback-stack overlay patch does not apply: " + result.stderr.strip())
        return {relative: (staged / relative).read_bytes() for relative in inputs}


def _clear_output(output: Path, allowed_output_root: Path, inputs: tuple[Path, ...]) -> None:
    resolved = output.resolve()
    allowed_root = allowed_output_root.resolve()
    if not allowed_root.is_dir():
        raise RuntimeError("callback-stack overlay allowed output root is not a directory")
    if resolved == allowed_root or allowed_root not in resolved.parents:
        raise RuntimeError("callback-stack overlay output must be a strict descendant of its allowed output root")
    for input_path in inputs:
        resolved_input = input_path.resolve()
        if (resolved == resolved_input or resolved in resolved_input.parents or
                resolved_input in resolved.parents):
            raise RuntimeError("callback-stack overlay output must be separate from every input")
    if output.exists():
        if not output.is_dir():
            raise RuntimeError("callback-stack overlay output is not a directory")
        shutil.rmtree(output)


def generate(runtime_source: Path, header_input: Path, runtime_input: Path, output: Path,
             allowed_output_root: Path) -> None:
    runtime_source = runtime_source.resolve()
    header_input = header_input.resolve()
    runtime_input = runtime_input.resolve()
    output = output.resolve()
    allowed_output_root = allowed_output_root.resolve()
    require_locked_clean_producer(runtime_source)
    inputs = input_files(runtime_source, header_input, runtime_input)
    outputs = apply_patch(inputs)
    _clear_output(output, allowed_output_root, (runtime_source, header_input, runtime_input))
    # Preserve a complete include closure for later opt-in overlays. Only the
    # public runtime header is changed; the other producer headers are copied
    # byte-for-byte so quoted includes cannot silently fall through to a
    # divergent include root.
    for original in sorted((runtime_source / "include").rglob("*")):
        if original.is_file():
            destination = output / "ps2xRuntime" / "include" / original.relative_to(runtime_source / "include")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(original.read_bytes())
    host_backend = header_input.parent / "ps2_host_backend.h"
    if host_backend.is_file():
        destination = output / "ps2xRuntime" / "include" / host_backend.name
        destination.write_bytes(host_backend.read_bytes())
    # Sync.cpp, System.cpp and Interrupt.cpp include their adjacent Common.h
    # with quotes.  Replacing all three translation units plus that closure is
    # required for their compiled Runtime.h consumer to resolve to the patched
    # overlay header rather than the canonical sibling.
    for relative, raw in outputs.items():
        destination = output / "ps2xRuntime" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
    manifest = {
        "schema_version": 1,
        "overlay": "GAME-001 callback-stack main reservation",
        "patch": {"path": "tools/patches/" + PATCH.name, "sha256": sha256(read(PATCH))},
        "files": [{"path": relative, "input_sha256": sha256(inputs[relative]),
                   "overlay_sha256": sha256(outputs[relative])}
                  for relative in sorted(inputs)],
        "effective_inputs": {
            HEADER: str(header_input),
            RUNTIME: str(runtime_input),
            **{relative: str((runtime_source / relative).resolve()) for relative in OVERLAY_SOURCE_FILES},
        },
    }
    (output / "callback-stack-main-reservation-overlay-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--header-input", type=Path, required=True)
    parser.add_argument("--runtime-input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--allowed-output-root", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.header_input, args.runtime_input, args.output_dir,
             args.allowed_output_root)


if __name__ == "__main__":
    main()
