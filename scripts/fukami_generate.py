#!/usr/bin/env python3
"""Generate the game code Fukami compiles from your own SLUS_200.02 (Ridge Racer V, USA).

The public repository holds no game code: the recompiler in third_party/ps2recomp turns *your* ELF
into C++, and the VU and hot-function generators add their inputs from the same ELF. The result goes
to the ignored directory generated/rr5/ and is checked byte for byte against the reference hashes in
config/rr5/ (the hashes are not the work; a different ELF or a different generator version fails).

    python3 scripts/fukami_generate.py [--elf PATH] [--work DIR] [--force]

--elf defaults to the SLUS_200.02 that Fukami.app unpacked from your CHD (Application Support), then
to local/rrv_boot.elf. Needs cmake, a C++20 compiler and network access for the recompiler's pinned
source dependencies (they are fetched by CMake). It takes several minutes (recompiler build plus a
1,857-file generation) and writes only under generated/rr5/ and --work (default build/fukami-generate).
Afterwards: scripts/build_fukami_runtime.sh (see BUILDING.md).

In the private repository generated/rr5 is tracked; there the command only verifies it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "generated/rr5"
REF_PUBLIC = ROOT / "config/rr5"

# Reference file names in config/rr5 (public) and where each one lives inside generated/rr5.
REF_FILES = {
    "source-manifest.json": "source-manifest.json",
    "gate3-accounted-generation-manifest.json": "gate3-accounted-generation-manifest.json",
    "native-manifest.json": "native/manifest.json",
    "vu-manifest.json": "vu/manifest.json",
    "vu-programs.json": "vu/programs.json",
}
RAND_STUB_DECL = "void sub_002D5F40_0x2d5f40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def say(msg: str) -> None:
    print(f"[fukami-generate] {msg}", flush=True)


def die(msg: str) -> "NoReturn":  # type: ignore[name-defined]
    print(f"fukami_generate: {msg}", file=sys.stderr)
    raise SystemExit(1)


def run(cmd: list[str], **kw) -> None:
    say("$ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def reference(name: str) -> Path:
    path = REF_PUBLIC / name
    if path.is_file():
        return path
    private = GEN / REF_FILES[name]
    if private.is_file():
        return private
    die(f"reference file missing: config/rr5/{name} (is this a complete checkout?)")


def find_elf(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else [
        Path.home() / "Library/Application Support/Fukami/disc/SLUS_200.02",
        Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "Fukami/disc/SLUS_200.02",
        ROOT / "local/rrv_boot.elf",
    ]
    for c in candidates:
        if c and c.is_file():
            return c.resolve()
    die("no SLUS_200.02 found: pass --elf PATH (the unpacked file from your Ridge Racer V USA disc; "
        "Fukami.app unpacks it to Application Support on first launch)")


def verify(elf: Path | None = None) -> dict:
    """verify_owned_source plus the byte-exact checks of the derived VU and native files."""
    sys.path.insert(0, str(ROOT / "scripts"))
    import verify_owned_source  # noqa: PLC0415
    summary = verify_owned_source.verify(ROOT)
    ref = {k: json.loads(reference(k).read_text()) for k in ("vu-manifest.json", "vu-programs.json", "native-manifest.json")}
    vu = json.loads((GEN / "vu/manifest.json").read_text())
    programs = json.loads((GEN / "vu/programs.json").read_text())
    native = json.loads((GEN / "native/manifest.json").read_text())
    for label, got, want, keys in (
        ("VU blocks", vu, ref["vu-manifest.json"], ("catalogue_id", "blocks_inc_sha256", "input_elf_sha256")),
        ("VU programs", programs, ref["vu-programs.json"], ("catalogue_id", "programs_inc_sha256", "input_elf_sha256")),
        ("native hot functions", native, ref["native-manifest.json"], ("inc_sha256", "generation_manifest_sha256")),
    ):
        for key in keys:
            if got.get(key) != want.get(key):
                die(f"{label}: {key} differs from the reference ({got.get(key)} != {want.get(key)})")
    if sha256(GEN / "vu/rrv_vu_aot_blocks.inc") != vu["blocks_inc_sha256"]:
        die("vu/rrv_vu_aot_blocks.inc does not match its manifest")
    if sha256(GEN / "native/rrv_ee_native.inc") != native["inc_sha256"]:
        die("native/rrv_ee_native.inc does not match its manifest")
    if elf is not None and sha256(elf) != summary["elf_sha256"]:
        die("the ELF changed during generation")
    return summary


def derive_legacy_stubs(generated_header: Path, destination: Path) -> None:
    """The legacy header is the recompiler's own stub header plus the guest `rand` declaration, which the
    accounted generation takes out of the recompiler's stub list (reextract_game_source.py)."""
    lines = generated_header.read_text().splitlines()
    decl = re.compile(r"^void sub_([0-9A-Fa-f]{8})_0x[0-9a-f]+\(")
    head = [ln for ln in lines if not decl.match(ln)]
    decls = {ln for ln in lines if decl.match(ln)} | {RAND_STUB_DECL}
    ordered = sorted(decls, key=lambda ln: int(decl.match(ln).group(1), 16))
    while head and head[-1] == "":
        head.pop()
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(head) + "\n\n" + "\n".join(ordered) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, help="your SLUS_200.02 (Ridge Racer V, USA)")
    ap.add_argument("--work", type=Path, default=ROOT / "build/fukami-generate", help="scratch directory")
    ap.add_argument("--force", action="store_true", help="replace an existing generated/rr5 (public checkouts)")
    ap.add_argument("--verify-only", action="store_true", help="only check the existing generated/rr5")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if args.verify_only or ((GEN / "output").is_dir() and not args.force):
        if not (GEN / "output").is_dir():
            die("generated/rr5/output does not exist; run without --verify-only")
        say("generated/rr5 already exists; verifying it" + ("" if args.verify_only else " (use --force to regenerate)"))
        summary = verify()
        say(f"OK: {summary['file_count']} generated files match the reference (ELF sha256 {summary['elf_sha256'][:16]}...)")
        return 0

    tracked = subprocess.run(["git", "ls-files", "--error-unmatch", "generated/rr5/source-manifest.json"],
                             cwd=ROOT, capture_output=True).returncode == 0
    if tracked:
        die("generated/rr5 is tracked in this repository; regenerate with scripts/reextract_game_source.py "
            "(see generated/rr5/README.md)")
    elf = find_elf(args.elf)
    want = json.loads(reference("source-manifest.json").read_text())["game_input"]["sha256"]
    got = sha256(elf)
    if got != want:
        die(f"{elf} is not the supported ELF (sha256 {got[:16]}... != {want[:16]}...). Only the USA release, "
            "SLUS_200.02 of Ridge Racer V, is supported.")
    say(f"ELF ok: {elf}")

    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    build = work / "recompiler"
    started = time.time()
    run(["cmake", "-S", ROOT, "-B", build, "-DRRV_BUILD_RECOMPILER=ON", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", build, "--target", "ps2_analyzer", "ps2_recomp", "--parallel", str(args.jobs)])

    extract = work / "extract"
    if extract.exists():
        shutil.rmtree(extract)
    run([sys.executable, ROOT / "scripts/reextract_game_source.py", "--recompiler-build", build,
         "--elf", elf, "--run-dir", extract])

    stage = ROOT / "generated/rr5.partial"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "vu").mkdir(parents=True)
    (stage / "native").mkdir()
    shutil.copytree(extract / "output", stage / "output")
    derive_legacy_stubs(stage / "output/ps2_recompiled_stubs.h", stage / "legacy-stubs/ps2_recompiled_stubs.h")
    for name, rel in REF_FILES.items():
        if name in ("vu-manifest.json", "vu-programs.json"):
            continue  # produced by the VU generators below, then compared with the reference
        shutil.copy2(reference(name), stage / rel)

    # The generators read and write generated/rr5, so move the staged tree into place first.
    final = GEN
    if final.exists():
        shutil.rmtree(final)
    stage.rename(final)
    try:
        run([sys.executable, ROOT / "tools/vu-aot/vu_aot_gen.py", "--elf", elf, "--output", GEN / "vu"])
        run([sys.executable, ROOT / "tools/vu-aot/vu_prog_gen.py", "--elf", elf, "--output", GEN / "vu"])
        run([sys.executable, ROOT / "tools/ee-native/ee_native_gen.py", "--generation", GEN,
             "--output", GEN / "native", "--from-manifest", reference("native-manifest.json")])
        summary = verify(elf)
    except (subprocess.CalledProcessError, SystemExit):
        broken = ROOT / "generated/rr5.failed"
        if broken.exists():
            shutil.rmtree(broken)
        GEN.rename(broken)
        say(f"generation did NOT verify; the result was moved to {broken.relative_to(ROOT)} for inspection")
        raise
    say(f"OK in {time.time() - started:.0f}s: {summary['file_count']} files, identical to the reference "
        f"(ELF sha256 {summary['elf_sha256'][:16]}...)")
    say("next: scripts/build_fukami_runtime.sh (see BUILDING.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
