#!/usr/bin/env python3
# retired from the source-owned product 2026-10-01 (third_party/ps2recomp + src/product hold its effect);
# still used by the legacy/diagnostic configurations (RRV_PRODUCT_OWNED_SOURCE=OFF) through cmake/Rrv*.cmake.
"""Apply the approved fromSPR pending-payload fix to the selected runtime copy."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from m2_causal_overlay import PINNED, patch_memory
from m2_dma_overlay import memory as dma_memory, require_locked_clean_producer


MEMORY_SOURCE = "src/lib/ps2_memory.cpp"
PATCH = Path(__file__).resolve().parents[1] / "tools/patches/ps2recomp-runtime-spr-pending-chain.patch"
PATCH_SHA256 = "18a77c796b99b7e8b131e1c92fe8064703241aef2aeb16c9c323689377feee26"


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def apply_patch(raw: bytes, patch: bytes) -> bytes:
    # Check and apply the exact historical patch in an isolated, disposable
    # source copy. git apply requires full context (no patch fuzz) and changes
    # neither the producer checkout nor an existing generated overlay.
    if sha256(patch) != PATCH_SHA256:
        raise RuntimeError("SPR pending-chain patch SHA-256 mismatch")
    with tempfile.TemporaryDirectory(prefix="rrv-spr-patch-") as directory:
        root = Path(directory)
        source = root / MEMORY_SOURCE
        source.parent.mkdir(parents=True)
        source.write_bytes(raw)
        for options in (("--check",), ()):
            result = subprocess.run(
                ["git", "apply", "--whitespace=nowarn", *options, "-"],
                cwd=root, input=patch, capture_output=True)
            if result.returncode:
                raise RuntimeError("SPR pending-chain patch does not apply: " +
                                   result.stderr.decode("utf-8", errors="replace"))
        return source.read_bytes()


def known_inputs(original: bytes) -> dict[str, bytes]:
    causal = patch_memory(original.decode("utf-8"))[0]
    # Initial-state and guest-provenance options do not further change memory.
    return {
        "locked-producer": original,
        "m2-causal": causal.encode("utf-8"),
        "m2-causal-dma-provenance": dma_memory(causal).encode("utf-8"),
    }


def generate(runtime_source: Path, source: Path, output_dir: Path) -> None:
    require_locked_clean_producer(runtime_source)
    original = (runtime_source / MEMORY_SOURCE).read_bytes()
    if sha256(original) != PINNED[MEMORY_SOURCE]:
        raise RuntimeError("SPR pending-chain requires the SHA-pinned ps2_memory.cpp producer")
    raw = source.read_bytes()
    input_kind = next((kind for kind, expected in known_inputs(original).items()
                       if raw == expected), None)
    if input_kind is None:
        raise RuntimeError("SPR pending-chain input is neither the pinned producer nor a known diagnostic overlay")
    patch = PATCH.read_bytes()
    patched = apply_patch(raw, patch)
    # Composing the fix with observation in either order must give identical
    # bytes. This also guards against future diagnostic hooks being lost.
    expected = known_inputs(apply_patch(original, patch))[input_kind]
    if patched != expected:
        raise RuntimeError("SPR pending-chain fix does not commute with the selected diagnostic overlay")
    destination = output_dir / MEMORY_SOURCE
    if destination.resolve() in (source.resolve(), (runtime_source / MEMORY_SOURCE).resolve()):
        raise RuntimeError("SPR pending-chain output must be a separate source copy")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patched)
    manifest = {
        "schema_version": 1,
        "overlay": "GAME-001 fromSPR pending-chain correction",
        "input_kind": input_kind,
        "patch": {"path": "tools/patches/" + PATCH.name, "sha256": sha256(patch)},
        "files": [{
            "path": MEMORY_SOURCE,
            "producer_sha256": sha256(original),
            "input_sha256": sha256(raw),
            "overlay_sha256": sha256(patched),
        }],
    }
    (output_dir / "spr-pending-chain-overlay-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    generate(args.runtime_source, args.source, args.output_dir)


if __name__ == "__main__":
    main()
