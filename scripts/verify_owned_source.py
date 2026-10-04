#!/usr/bin/env python3
"""Verify the committed game source against generated/rr5/source-manifest.json.

Replaces the executable-bound generation-manifest check for the source-owned product
build: generated/rr5/output (read-only game code), generated/rr5/legacy-stubs and the
accounted-generation manifest copy must match the recorded size and SHA-256 exactly, and
the derived native/VU manifests must name this generation.

  verify_owned_source.py [--root DIR] [--print-field FIELD]

Prints one line per summary field the CMake configure consumes. Exit 1 on any mismatch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root: Path = ROOT) -> dict:
    gen = root / "generated/rr5"
    manifest_path = gen / "source-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema_version") != 2 or not manifest.get("read_only"):
        raise ValueError("source-manifest.json is not a read-only schema-2 manifest")
    for section, directory in (("generated", gen / "output"), ("legacy_stubs", gen / "legacy-stubs")):
        listed = {e["path"]: e for e in manifest[section]["files"]}
        actual = {p.name for p in directory.iterdir() if p.is_file()}
        if actual != set(listed):
            extra, missing = sorted(actual - set(listed)), sorted(set(listed) - actual)
            raise ValueError(f"{section}: file set differs (extra {extra[:3]}, missing {missing[:3]})")
        for name, entry in listed.items():
            path = directory / name
            if path.stat().st_size != entry["size"] or sha(path) != entry["sha256"]:
                raise ValueError(f"{section}/{name} differs from source-manifest.json (generated source is read-only)")
    accounted_path = gen / "gate3-accounted-generation-manifest.json"
    accounted_sha = sha(accounted_path)
    if accounted_sha != manifest["accounted_generation_manifest"]["sha256"]:
        raise ValueError("accounted generation manifest differs from source-manifest.json")
    accounted = json.loads(accounted_path.read_text())
    if {e["path"] for e in accounted["generated"]} != {p.name for p in (gen / "output").iterdir()}:
        raise ValueError("accounted manifest file set differs from generated/rr5/output")
    for entry in accounted["generated"]:
        path = gen / "output" / entry["path"]
        if path.stat().st_size != entry["size"] or sha(path) != entry["sha256"]:
            raise ValueError(f"accounted manifest mismatch: {entry['path']}")
    if manifest["game_input"]["sha256"] != accounted["user_elf_sha256"]:
        raise ValueError("source-manifest.json game_input differs from the accounted generation's ELF")
    native = json.loads((gen / "native/manifest.json").read_text())
    if native["generation_manifest_sha256"] != accounted_sha:
        raise ValueError("native manifest names a different generation")
    vu = json.loads((gen / "vu/manifest.json").read_text())
    if vu["input_elf_sha256"] != accounted["user_elf_sha256"]:
        raise ValueError("VU catalogue was derived from a different ELF")
    programs_path = gen / "vu/programs.json"
    if programs_path.exists():  # whole-program native microprograms (tools/vu-aot/vu_prog_gen.py)
        programs = json.loads(programs_path.read_text())
        if programs["input_elf_sha256"] != accounted["user_elf_sha256"]:
            raise ValueError("VU program catalogue was derived from a different ELF")
        if programs["programs_inc_sha256"] != sha(gen / "vu/rrv_vu_aot_programs.inc"):
            raise ValueError("vu/rrv_vu_aot_programs.inc differs from vu/programs.json")
    return {
        "source_manifest_sha256": sha(manifest_path),
        "accounted_manifest_sha256": accounted_sha,
        "elf_sha256": accounted["user_elf_sha256"],
        "producer_commit": manifest["recompiler"]["producer_commit"],
        "recompiler_rrv_commit": manifest["recompiler"]["rrv_commit_at_freeze"],
        "file_count": manifest["generated"]["file_count"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--print-field", metavar="FIELD")
    args = parser.parse_args()
    try:
        summary = verify(args.root.resolve())
    except (ValueError, OSError, KeyError) as error:
        print(f"owned-source verification failed: {error}", file=sys.stderr)
        return 1
    if args.print_field:
        print(summary[args.print_field])
    else:
        print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
