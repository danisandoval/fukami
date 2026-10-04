#!/usr/bin/env python3
"""Verify every file in an out-of-tree RR5 generation result."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--expected-producer-tree", required=True)
    parser.add_argument("--expected-producer-commit", required=True)
    parser.add_argument("--expected-pcsx2-commit", required=True)
    args = parser.parse_args()

    data = json.loads(args.manifest.read_text())
    problems: list[str] = []
    producer = data.get("producer", {})
    if producer.get("commit") != args.expected_producer_commit:
        problems.append("generation producer commit does not match compatible producer")
    if producer.get("tree") != args.expected_producer_tree:
        problems.append("generation producer tree does not match compatible producer")
    if not producer.get("clean"):
        problems.append("generation producer was not clean")
    if data.get("pcsx2_commit") != args.expected_pcsx2_commit:
        problems.append("generation PCSX2 commit does not match the lock")
    if data.get("bridge_abi") != 5:
        problems.append("generation does not declare bridge ABI 5")
    if data.get("working_tree_unchanged") is not True:
        problems.append("generation did not preserve the RRV working tree")
    hook = data.get("hook_interface", {})
    if hook.get("firstChunkOfTransfer") != "absent":
        problems.append("generation hook contract contains firstChunkOfTransfer")

    producer_build = data.get("producer_build", {})
    tools = producer_build.get("tools", {})
    for name in ("analyzer", "recompiler"):
        tool = tools.get(name, {})
        path_value = tool.get("path")
        if not isinstance(path_value, str) or not path_value:
            problems.append(f"generation omits the {name} executable identity")
            continue
        path = pathlib.Path(path_value)
        if not path.is_absolute():
            path = ROOT / path
        if not path.is_file():
            problems.append(f"generation {name} executable is unavailable: {path}")
        elif path.stat().st_size != tool.get("size") or sha256(path) != tool.get("sha256"):
            problems.append(f"generation {name} executable hash/size mismatch")
    if not producer_build.get("cmake_cache_sha256") or not producer_build.get("build_ninja_sha256"):
        problems.append("generation omits its producer build-system identity")

    expected_files = data.get("generated", {}).get("files", [])
    expected_paths = {entry["path"] for entry in expected_files}
    actual_paths = {str(path.relative_to(args.output_dir))
                    for path in args.output_dir.rglob("*") if path.is_file()}
    if expected_paths != actual_paths:
        missing = sorted(expected_paths - actual_paths)
        extra = sorted(actual_paths - expected_paths)
        problems.append(f"generated file set differs: missing={missing} extra={extra}")
    for entry in expected_files:
        path = args.output_dir / entry["path"]
        if not path.is_file():
            continue
        if path.stat().st_size != entry["size"] or sha256(path) != entry["sha256"]:
            problems.append(f"generated file hash/size mismatch: {entry['path']}")

    if len(expected_files) != data.get("generated", {}).get("file_count"):
        problems.append("manifest generated file count is inconsistent")
    if problems:
        print(*problems, sep="\n", file=sys.stderr)
        return 1
    print(f"verified {len(expected_files)} generated files; producer tree {producer['tree']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
