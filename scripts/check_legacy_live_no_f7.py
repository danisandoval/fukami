#!/usr/bin/env python3
"""Fail when the isolated legacy-live binary contains F7/P3 renderer code."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


FORBIDDEN_SOURCE_PARTS = (
    "/src/ir/",
    "/src/gs-frame/",
    "/tools/ir-metal/",
    "rrv_live_full_frame_adapter",
)
FORBIDDEN_SYMBOL_PARTS = (
    "rrv::ir::",
    "rrv_ir_",
    "FullFrame",
    "full_frame",
    "LiveFullFrame",
    "RRV_P3_",
    "rrvNoteP3Boundary",
)


def output(*args: str) -> str:
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--producer", type=pathlib.Path, required=True)
    parser.add_argument("--expected-producer-commit", required=True)
    parser.add_argument("--expected-producer-tree", required=True)
    args = parser.parse_args()

    compile_commands = json.loads((args.build_dir / "compile_commands.json").read_text())
    sources = [entry["file"] for entry in compile_commands]
    bad_sources = sorted({source for source in sources
                          if any(part in source for part in FORBIDDEN_SOURCE_PARTS)})

    symbols = output("nm", "-a", str(args.binary))
    strings = output("strings", str(args.binary))
    bad_symbols = sorted({part for part in FORBIDDEN_SYMBOL_PARTS
                          if part in symbols or part in strings})
    absolute_dependency = any(prefix in strings for prefix in ("/Users/", "/home/"))
    runtime_commands = [entry.get("command", "") for entry in compile_commands
                        if "/ps2_runtime.dir/" in entry.get("output", "")]
    field_only_missing = not runtime_commands or any(
        "PS2X_RRV_FIELD_ONLY=1" not in command for command in runtime_commands)
    link_commands = output("ninja", "-C", str(args.build_dir), "-t", "commands",
                           "rrv-legacy-live")
    forbidden_link = any(part in link_commands for part in FORBIDDEN_SOURCE_PARTS)
    producer_commit = output("git", "-C", str(args.producer), "rev-parse", "HEAD").strip()
    producer_tree = output("git", "-C", str(args.producer), "rev-parse", "HEAD^{tree}").strip()
    producer_dirty = bool(output("git", "-C", str(args.producer),
                                 "status", "--porcelain=v1").strip())
    producer_mismatch = (producer_commit != args.expected_producer_commit or
                         producer_tree != args.expected_producer_tree or producer_dirty)

    if (bad_sources or bad_symbols or absolute_dependency or field_only_missing or
            forbidden_link or producer_mismatch):
        if bad_sources:
            print("forbidden configured source paths:", *bad_sources, sep="\n  ", file=sys.stderr)
        if bad_symbols:
            print("forbidden linked symbols/strings: " + ", ".join(bad_symbols), file=sys.stderr)
        if absolute_dependency:
            print("binary embeds an absolute user-home dependency", file=sys.stderr)
        if field_only_missing:
            print("PS2 runtime compile graph is not uniformly field-only", file=sys.stderr)
        if forbidden_link:
            print("legacy-live build commands include a forbidden renderer source", file=sys.stderr)
        if producer_mismatch:
            print("producer HEAD/tree/clean state does not match the lock", file=sys.stderr)
        return 1
    print("rrv-legacy-live no-F7/no-absolute-path gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
