#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Materialize the opt-in RPC memory-safety source closure in one build root."""

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
PATCH = ROOT / "tools/patches/ps2recomp-runtime-rpc-memory-safety.patch"
PATCH_SHA256 = "a89c8fddedac3fd7741b4409c68087ce2c7e081b12ce2b8506576f9dd2e9fc04"
RPC = "src/lib/Kernel/Syscalls/RPC.cpp"
SYSTEM = "src/lib/Kernel/Syscalls/System.cpp"
THREAD = "src/lib/Kernel/Syscalls/Thread.cpp"
COMMON = "src/lib/Kernel/Syscalls/Common.h"
HELPER = "src/lib/Kernel/Syscalls/Helpers/Runtime.h"
FILES = (RPC, SYSTEM, THREAD, COMMON, HELPER)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def generate(runtime_source: Path, callback_source: Path, output: Path,
             allowed_output_root: Path) -> None:
    runtime_source = runtime_source.resolve()
    callback_source = callback_source.resolve()
    output = output.resolve()
    allowed_output_root = allowed_output_root.resolve()
    require_locked_clean_producer(runtime_source)
    if not allowed_output_root.is_dir() or output == allowed_output_root or allowed_output_root not in output.parents:
        raise RuntimeError("RPC overlay output must be a strict descendant of its build root")
    for source in (runtime_source, callback_source):
        if output == source or output in source.parents or source in output.parents:
            raise RuntimeError("RPC overlay output overlaps an input")
    sources = {RPC: runtime_source, THREAD: runtime_source,
               SYSTEM: callback_source, COMMON: callback_source, HELPER: callback_source}
    inputs = {relative: (sources[relative] / relative).read_bytes() for relative in FILES}
    patch = PATCH.read_bytes()
    if sha256(patch) != PATCH_SHA256:
        raise RuntimeError("RPC overlay patch SHA-256 mismatch")
    with tempfile.TemporaryDirectory(prefix="rrv-rpc-memory-safety-") as directory:
        staging = Path(directory)
        for relative, data in inputs.items():
            path = staging / "ps2xRuntime" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for options in (("--check",), ()):
            result = subprocess.run(["git", "apply", "--whitespace=nowarn", *options, str(PATCH)],
                                    cwd=staging, text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError("RPC overlay patch does not apply: " + result.stderr.strip())
        outputs = {relative: (staging / "ps2xRuntime" / relative).read_bytes() for relative in FILES}
    if output.exists():
        if not output.is_dir():
            raise RuntimeError("RPC overlay output is not a directory")
        shutil.rmtree(output)
    for relative, data in outputs.items():
        path = output / "ps2xRuntime" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    manifest = {
        "schema_version": 1,
        "overlay": "GAME-001 RPC memory safety",
        "patch": {"path": "tools/patches/" + PATCH.name, "sha256": sha256(patch)},
        "files": [{"path": relative, "input_sha256": sha256(inputs[relative]),
                   "overlay_sha256": sha256(outputs[relative])} for relative in sorted(FILES)],
        "effective_inputs": {relative: str(sources[relative] / relative) for relative in FILES},
    }
    (output / "rpc-memory-safety-overlay-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--callback-source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--allowed-output-root", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.callback_source, args.output_dir, args.allowed_output_root)


if __name__ == "__main__":
    main()
