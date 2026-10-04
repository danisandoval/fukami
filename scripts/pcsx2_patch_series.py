#!/usr/bin/env python3
"""The PCSX2 bridge patch as an ordered series (tools/patches/pcsx2/).

The series is the reviewable source; its concatenation, byte for byte, is the one patch the
lock pins (config/dependencies.lock.toml [[pcsx2_bridge_patches]]) and the bridge build applies.
tools/patches/pcsx2-gs-bridge-target.patch stays tracked as that assembled artifact (the lock,
CMake and the bridge manifest read it); `check` keeps all three equal.

  pcsx2_patch_series.py assemble [--output FILE] [--expect-sha256 HEX]
  pcsx2_patch_series.py check    [--pcsx2-source DIR]
  pcsx2_patch_series.py tree     --pcsx2-source DIR

check: series files == `series` list; concatenation SHA-256 == the lock; concatenation == the
tracked artifact; and, with --pcsx2-source (a PCSX2 checkout containing the locked revision),
applying the series one patch at a time to that revision in a throw-away index yields the tree
recorded in the lock (bridge_patched_tree), identical to applying the single patch.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_dependencies import LOCK_PATH, load_toml  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SERIES_DIR = ROOT / "tools/patches/pcsx2"
ARTIFACT = ROOT / "tools/patches/pcsx2-gs-bridge-target.patch"


def series_files() -> list[Path]:
    names = [l.strip() for l in (SERIES_DIR / "series").read_text().splitlines()
             if l.strip() and not l.lstrip().startswith("#")]
    if len(set(names)) != len(names):
        raise ValueError("series lists a patch twice")
    return [SERIES_DIR / n for n in names]


def assemble() -> bytes:
    files = series_files()
    listed = {p.name for p in files}
    on_disk = {p.name for p in SERIES_DIR.glob("*.patch")}
    if listed != on_disk:
        raise ValueError(f"series and directory differ: only listed {sorted(listed - on_disk)}, "
                         f"only on disk {sorted(on_disk - listed)}")
    return b"".join(p.read_bytes() for p in files)


def lock() -> tuple[str, dict]:
    data = load_toml(LOCK_PATH)
    return data["pcsx2_bridge_patches"][0]["sha256"], data["pcsx2"]


def patched_tree(source: Path, revision: str, patches: list[bytes]) -> str:
    gitdir = subprocess.run(["git", "-C", str(source), "rev-parse", "--absolute-git-dir"],
                            check=True, capture_output=True, text=True).stdout.strip()
    with tempfile.TemporaryDirectory(prefix="rrv-pcsx2-series-") as t:
        tmp = Path(t)
        (tmp / "objects").mkdir()
        env = dict(os.environ, GIT_DIR=gitdir, GIT_INDEX_FILE=str(tmp / "index"),
                   GIT_OBJECT_DIRECTORY=str(tmp / "objects"),
                   GIT_ALTERNATE_OBJECT_DIRECTORIES=os.path.join(gitdir, "objects"))

        def git(*args, stdin=None):
            done = subprocess.run(["git", *args], env=env, cwd=str(source), input=stdin,
                                  capture_output=True)
            if done.returncode:
                raise ValueError(f"git {' '.join(args)} failed: {done.stderr.decode()[:300]}")
            return done.stdout.decode().strip()

        git("read-tree", revision)
        for patch in patches:
            git("apply", "--cached", "-", stdin=patch)
        return git("write-tree")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("assemble")
    p.add_argument("--output", type=Path)
    p.add_argument("--expect-sha256")
    p = sub.add_parser("check")
    p.add_argument("--pcsx2-source", type=Path)
    p = sub.add_parser("tree")
    p.add_argument("--pcsx2-source", type=Path, required=True)
    args = parser.parse_args()
    try:
        data = assemble()
        digest = hashlib.sha256(data).hexdigest()
        locked, pcsx2 = lock()
        if args.command == "assemble":
            if args.expect_sha256 and digest != args.expect_sha256:
                raise ValueError(f"series concatenation is {digest}, expected {args.expect_sha256}")
            (args.output.write_bytes(data) if args.output else sys.stdout.buffer.write(data))
            return 0
        if args.command == "tree":
            print(patched_tree(args.pcsx2_source, pcsx2["revision"], [p.read_bytes() for p in series_files()]))
            return 0
        if digest != locked:
            raise ValueError(f"series concatenation is {digest}, the lock pins {locked}")
        if ARTIFACT.read_bytes() != data:
            raise ValueError(f"{ARTIFACT.relative_to(ROOT)} differs from the concatenated series")
        print(f"series: {len(series_files())} patches, {len(data)} bytes, sha256 {digest} == lock == artifact")
        if args.pcsx2_source:
            expected = pcsx2["bridge_patched_tree"]
            by_series = patched_tree(args.pcsx2_source, pcsx2["revision"], [p.read_bytes() for p in series_files()])
            by_single = patched_tree(args.pcsx2_source, pcsx2["revision"], [data])
            if not (by_series == by_single == expected):
                raise ValueError(f"patched tree: series {by_series}, single patch {by_single}, lock {expected}")
            print(f"patched tree {by_series} == single patch == lock (PCSX2 {pcsx2['revision'][:12]})")
        return 0
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print(f"pcsx2 patch series check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
