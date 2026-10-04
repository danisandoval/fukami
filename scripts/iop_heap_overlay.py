#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Materialize the opt-in CD/SIF IOP heap overlay after callback-stack reservation."""

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
PATCH = ROOT / "tools/patches/ps2recomp-runtime-iop-heap-isolation.patch"
PATCH_SHA256 = "dac26fd31cd18bb62161f3ce92dec5cfcc474636eec123df8b98b1763c21178d"
CD = "src/lib/Kernel/Stubs/CD.cpp"
SIF = "src/lib/Kernel/Stubs/SIF.cpp"
SUPPORT = "src/lib/Kernel/Stubs/Helpers/Support.h"
COMMON = "src/lib/Kernel/Stubs/Common.h"


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
        raise RuntimeError("IOP overlay output must be a strict descendant of its build root")
    for source in (runtime_source, callback_source):
        if output == source or output in source.parents or source in output.parents:
            raise RuntimeError("IOP overlay output overlaps an input")
    inputs = {
        CD: (callback_source / CD).read_bytes(),
        SIF: (runtime_source / SIF).read_bytes(),
        SUPPORT: (runtime_source / SUPPORT).read_bytes(),
        COMMON: (runtime_source / COMMON).read_bytes(),
    }
    patch = PATCH.read_bytes()
    if sha256(patch) != PATCH_SHA256:
        raise RuntimeError("IOP overlay patch SHA-256 mismatch")
    with tempfile.TemporaryDirectory(prefix="rrv-iop-heap-") as directory:
        staging = Path(directory)
        for relative, data in inputs.items():
            path = staging / "ps2xRuntime" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for options in (("--check",), ()):
            result = subprocess.run(["git", "apply", "--whitespace=nowarn", *options, str(PATCH)],
                                    cwd=staging, text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError("IOP overlay patch does not apply: " + result.stderr.strip())
        outputs = {relative: (staging / "ps2xRuntime" / relative).read_bytes() for relative in inputs}
    if output.exists():
        if not output.is_dir():
            raise RuntimeError("IOP overlay output is not a directory")
        shutil.rmtree(output)
    for relative, data in outputs.items():
        path = output / "ps2xRuntime" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    manifest = {
        "schema_version": 1,
        "overlay": "GAME-001 isolated synthetic IOP heap",
        "patch": {"path": "tools/patches/" + PATCH.name, "sha256": sha256(patch)},
        "files": [{"path": relative, "input_sha256": sha256(inputs[relative]),
                   "overlay_sha256": sha256(outputs[relative])} for relative in sorted(inputs)],
        "effective_inputs": {
            CD: str(callback_source / CD),
            **{relative: str(runtime_source / relative) for relative in (SIF, SUPPORT, COMMON)},
        },
    }
    (output / "iop-heap-overlay-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--callback-source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--allowed-output-root", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.callback_source, args.output_dir,
             args.allowed_output_root)


if __name__ == "__main__":
    main()
