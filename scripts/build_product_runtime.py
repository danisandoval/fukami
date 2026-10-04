#!/usr/bin/env python3
"""Configure and build the source-owned product executable.

Compiles straight from the committed sources: third_party/ps2recomp (runtime),
src/product (host entry and override layer) and generated/rr5 (read-only game code).
No stage directory, no Python overlay and no recompiler run. The output is
<build-root>/candidate-bin/Fukami (the name the packaging scripts use) plus
gate3-candidate-build-receipt.json (the receipt scripts/package_gate4_product.py hashes).

Inputs that still come from outside the repository: the source-built PCSX2 bridge
(scripts/build_pcsx2_gs_bridge.sh, bound to the RRV commit), the pinned sse2neon
checkout (prepare_dependencies.py --component sse2neon) and, for Gate-8 audio, the
pinned PCSX2 source (build-deps/pcsx2-2.8.2).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# sse2neon provides the SSE intrinsics on NEON; a Linux x86-64 (Gate 5) build has native SSE.
NATIVE_ARM64 = platform.machine() in ("arm64", "aarch64")
sys.path.insert(0, str(ROOT / "scripts"))
from verify_owned_source import verify  # noqa: E402
from prepare_dependencies import LOCK_PATH, load_toml  # noqa: E402


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-root", type=Path, required=True,
                        help="absent directory named build-product-* inside the repository")
    parser.add_argument("--cmake-source-root", type=Path, default=ROOT,
                        help="clean RRV checkout whose HEAD the bridge manifest names")
    parser.add_argument("--bridge-manifest", type=Path, required=True)
    parser.add_argument("--sse2neon-source", type=Path,
                        help="default: the locked checkout (python3 scripts/prepare_dependencies.py "
                             "--prepare --component sse2neon)")
    parser.add_argument("--pcsx2-spu2-source", type=Path,
                        help="pinned PCSX2 2.8.2 checkout: links the Gate-8 SPU2 core and sound driver")
    parser.add_argument("--parallel", type=int, default=6)
    args = parser.parse_args()
    build, source = args.build_root.resolve(), args.cmake_source_root.resolve()
    if build.exists():
        raise ValueError("build root must be absent; use a new private build directory")
    if ROOT not in build.parents or not build.name.startswith("build-product-"):
        raise ValueError("build root must be build-product-* inside this repository")
    if not 1 <= args.parallel <= 32:
        raise ValueError("--parallel must be between 1 and 32")
    if NATIVE_ARM64 and not args.sse2neon_source:
        args.sse2neon_source = ROOT / str(load_toml(LOCK_PATH)["sse2neon"]["default_checkout"])
    for name in ("bridge_manifest", "sse2neon_source"):
        value = getattr(args, name)
        if value is None and name == "sse2neon_source" and not NATIVE_ARM64:
            continue
        if not value.exists():
            raise ValueError(f"{name} does not exist: {value}")
    summary = verify(source)
    command = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        "-DRRV_BUILD_PRODUCT=ON", "-DRRV_PRODUCT_OWNED_SOURCE=ON", "-DRRV_GS_PRODUCER_CONTROL=OFF",
        f"-DRRV_PCSX2_GS_BRIDGE_MANIFEST={args.bridge_manifest.resolve()}",
    ]
    if os.environ.get("FUKAMI_MACOS_TARGET") and sys.platform == "darwin":  # release builds pick the oldest macOS to support
        command.append(f"-DCMAKE_OSX_DEPLOYMENT_TARGET={os.environ['FUKAMI_MACOS_TARGET']}")
    if args.sse2neon_source:
        command.append(f"-DFETCHCONTENT_SOURCE_DIR_SSE2NEON={args.sse2neon_source.resolve()}")
    if args.pcsx2_spu2_source:
        command.append(f"-DRRV_PCSX2_SOURCE_DIR={args.pcsx2_spu2_source.resolve()}")
    subprocess.run(command, check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "Fukami",
                    "--parallel", str(args.parallel)], check=True)
    binary = build / "candidate-bin/Fukami"
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError("product executable was not linked")
    verify(source)
    receipt = {
        "schema": "rrv-product-build-v2", "source_owned": True, "runtime_executed": False,
        "candidate": str(binary), "candidate_sha256": sha256(binary),
        "cmake_source_root": str(source),
        "bridge_manifest": str(args.bridge_manifest.resolve()),
        "bridge_manifest_sha256": sha256(args.bridge_manifest),
        "source_manifest_sha256": summary["source_manifest_sha256"],
        "accounted_generation_manifest_sha256": summary["accounted_manifest_sha256"],
        "accounted_generation_file_count": summary["file_count"],
    }
    (build / "gate3-candidate-build-receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"LINK PASS: {binary} ({receipt['candidate_sha256']})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Product build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
