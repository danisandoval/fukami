#!/usr/bin/env python3
"""Export the public Fukami tree from this private repository.

The private repository (rrv-recomp) stays the source of truth. The public repository is a curated
export of one *committed* revision: a top-level allowlist, per-directory exclusions, the `public/`
overlay (README, docs, CI, licences), a few text transforms, and then safety gates that fail the export
if anything game-derived or personal slipped through. It never pushes anywhere.

    python3 scripts/export_public.py --out ~/Downloads/Fukami              # export only
    python3 scripts/export_public.py --out ~/Downloads/Fukami --commit     # export and commit locally
    python3 scripts/export_public.py --check-tree DIR                      # run the gates on any tree

Rules this script enforces (AGENTS.md "Hard constraints"):
  * nothing under generated/, no ELF/IRX/CHD/ISO/captures/BIOS, no disc data;
  * no personal paths, addresses, key names or e-mail addresses;
  * the export has a fresh, parentless history (--commit on an empty directory) and no remote.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# --- what goes in -----------------------------------------------------------------------------------

ROOT_FILES = ["CMakeLists.txt", "CMakePresets.json", "rrv.ini", "LICENSE", "VERSION"]
ROOT_DIRS = ["src", "cmake", "config", "third_party", "tools", "scripts", "tests"]

# Patches the product build, the dependency lock or the asset-free suite actually use. Every other
# tools/patches/* file is a historical diagnostic and stays private.
KEPT_PATCHES = [
    "tools/patches/pcsx2/*",
    "tools/patches/pcsx2-gs-bridge-target.patch",
    "tools/patches/pcsx2-gs-bridge-linux.patch",
    # The lock also pins the PCSX2 reference-capture patches (scripts/build_pcsx2_gt_capture.sh, pcsx2_debug.py).
    "tools/patches/pcsx2-gt-capture.patch",
    "tools/patches/pcsx2-gt-guest-trace.patch",
    "tools/patches/pcsx2-vu1-input-trace.patch",
    "tools/patches/pcsx2-gt-write-watch.patch",
    "tools/patches/pcsx2-debug-server.patch",
    "tools/patches/ps2recomp-d52-compatible-v2.patch",   # the lock's producer patch (checked by check_product_direct.py)
    "tools/patches/ps2recomp-runtime-callback-stack-main-reservation.patch",
    "tools/patches/ps2recomp-runtime-iop-heap-isolation.patch",
    "tools/patches/ps2recomp-runtime-rpc-memory-safety.patch",
    "tools/patches/ps2recomp-runtime-spr-pending-chain.patch",
]

# Excluded inside the allowed directories (fnmatch on the POSIX path relative to the repo root).
EXCLUDE = [
    "**/__pycache__/**", "**/*.pyc", "**/.DS_Store",
    "scripts/historical/**",
    "scripts/diag_run.command",
    "scripts/ghidra/**", "scripts/ghidra_rrv.sh", "scripts/m2_ghidra/**",
    "scripts/scripts/**",
    "tools/deck-profile/**",          # Steam Deck address and key of the owner
    "tools/PS2Recomp/**", "tools/PCSX2.app/**",
    # PS2Recomp's GUI: not used by the product, and its bundled fonts carry no licence file.
    "third_party/ps2recomp/ps2xStudio/**",
    # Upstream parts the product and the recompiler configuration never build or use.
    "third_party/ps2recomp/ps2xTest/**", "third_party/ps2recomp/.github/**", "third_party/ps2recomp/ps2xRecomp/tools/**",
    "third_party/ps2recomp/RRV_STAGE_PROVENANCE.md", "tools/rrv/**",
    # The pre-M0 diagnostic runtime, the F7 oracle and the development-phase harness tests (their own cmake files).
    "cmake/RrvDiagnosticRuntime.cmake", "cmake/RrvOracleF7.cmake", "cmake/RrvHarnessTests.cmake",
    # Offline renderer-IR / GS-replay / F7 oracle code (only the removed diagnostic runtime and the harness tests used it;
    # the product takes src/ir/rrv_present_dump_ticks.h, and RrvProduct.cmake names tools/gs-replay/m1_present_replay.cpp).
    "src/gs-frame/**", "src/main.cpp", "src/ir/*.cpp", "src/ir/rrv_ir_[a-qs-z]*.h", "src/ir/rrv_ir_resident_*.h",
    "tools/gs-frame-f3/**", "tools/gs-frame-f4/**", "tools/gs-frame-f5/**", "tools/gs-replay/gs_replay_main.cpp", "tools/gs-replay/gs_synth_stress.cpp", "tools/gs2gsr/**",
    "tools/gsr2gs/**", "tools/gstrace-replay/**", "tools/ir-metal/**", "tools/ir_shared/**",
    # Development-phase harness: M2 / Gate-3 workload tooling, the F7 oracle runner, doc gates of the private repository.
    "scripts/m2_*.py", "scripts/m2p_*.py", "scripts/gate3_*.py", "scripts/make_replay.py", "scripts/rrv_list_boundary.py",
    "scripts/run_f7_oracle.py", "scripts/f5_fullframe_seed.py", "scripts/build.sh", "scripts/extract_elf.sh",
    "scripts/check_docs.py", "scripts/check_docs_baseline.json", "scripts/check_docs_preservation.py",
    "scripts/pcsx2_anchor.md",
    "tests/dual_lattice_epoch_plan.h", "tests/gif_path_latency_config_tests.cpp", "tests/gs_dual_lattice_*",
    "tests/gs_frame_command_tests.cpp", "tests/gs_full_frame_*", "tests/gs_sprite_sampling_tests.cpp", "tests/ir_*",
    "tests/rrv_ir_resident_extractor_tests.cpp", "tests/rrv_present_dump_ticks_tests.cpp", "tests/vu0_math_tests.cpp",
    "tests/test_callback_stack_main_reservation_overlay.py", "tests/test_check_docs.py", "tests/test_f5_fullframe_seed.py",
    "tests/test_gate3_*.py", "tests/test_gstrace_to_gsr.py", "tests/test_m2_*.py", "tests/test_m2p_*.py",
    "tests/test_make_replay.py",
    # Investigation tooling of the development phases, referenced by nothing the product, the packaging or the
    # asset-free suite builds or runs (it works on the owner's captures and recordings).
    "scripts/gsr_*.py", "scripts/gstrace_*.py", "scripts/gs_vram_splice.py", "scripts/gs_transfer_census.py",
    "scripts/gate3_audit_hle_charges.py", "scripts/gate4_*.py",
    "scripts/gate6_*.py", "scripts/gate8_*.py", "scripts/cadence_by_phase.py", "scripts/ee_chainwalk.py",
    "scripts/ee_gsreg_scan.py", "scripts/fm_overlay_white.py", "scripts/ghidra_recomp_progress.py",
    "scripts/ir_catalogue.py", "scripts/nan_census_overlay.py", "scripts/ofy_frame_census.py",
    "scripts/pcsx2_drawtrace.py", "scripts/run_bounded_observation.py",
    "scripts/run_gold_car_baseline.sh", "scripts/run_gs_control_candidate.sh", "scripts/run_gs_worker_candidate.sh",
    "scripts/run_product.sh", "scripts/snapshot_equiv.py", "scripts/vram_fb.py", "scripts/compile_check.sh",
    "scripts/diag_instrumentation.md", "scripts/generate_current.py", "scripts/check_provenance.py",
    "scripts/check_ps2recomp_d52_contract.py", "scripts/statefile.py", "scripts/test_*.py",
    "tools/diagnostics/**", "tools/gate3_*.py", "tools/ir-diff/**", "tools/ir-dump-stats/**", "tools/tests/**",
    "tests/gate3_scheduler_runtime_tests.cpp", "tests/gate3_temporal_owner_tests.cpp",
    "tests/gate3_temporal_runtime_tests.cpp", "tests/iop_heap_native_tests.cpp", "tests/pcsx2_gif_boundary_tests.cpp",
    "tests/pcsx2_gif_packed_partial_exhaustion_tests.cpp", "tests/pcsx2_metal_lifetime_diag_tests.cpp",
    "tests/pcsx2_source_scratch_tests.cpp", "tests/pcsx2_target_scratch_tests.cpp", "tests/ps2recomp_d52_contract_probe.cpp",
    "tests/run_gate4_vif_unpack_tests.py", "tests/spr_pending_chain_tests.cpp", "tests/test_build_pcsx2_metal_resources.py",
    "tests/test_check_provenance.py", "tests/test_gate1_producer_control_comparator.py", "tests/test_gate4_*.py",
    "tests/test_ghidra_recomp_progress.py", "tests/test_gs_owner_surface.mm", "tests/test_gs_transfer_census.py",
    "tests/test_iop_heap_overlay.py", "tests/test_m2v_rrv_gs_gamefix_backport.py", "tests/test_rpc_memory_safety_overlay.py",
]

# Text rewrites applied to exported files: (path, old, new). Keep this list short and explicit.
TEXT_REPLACEMENTS: list[tuple[str, str, str]] = []

# --- gates ------------------------------------------------------------------------------------------

FORBIDDEN_PATH_GLOBS = [
    "generated/**", "**/generated/rr5/**",
    "*.elf", "*.irx", "*.chd", "*.iso", "*.cue", "*.img", "*.nrg", "*.mdf", "*.bin", "*.rom",
    "*.gsr", "*.gs", "*.rrvsnap", "*.irf", "*.texblob", "*.gstrace", "*.p2s", "*.vram",
    "**/SLUS_*", "**/SCES_*", "**/R5.ALL", "**/*.IRX", "**/IOPRP*.IMG",
    "local/**", "runtime/**", "rom/**", "images/**", "videos/**", "build*/**",
    ".claude/**", ".codex/**", ".agents/**", ".mcp.json",
    "docs/evidence/**", "docs/archive/**", "old_docs/**",
]

# (name, compiled regex, files allowed to match, as POSIX globs)
TEXT_GATES = [
    ("personal path", re.compile(rb"/Users/" + b"dani"), []),
    ("home address", re.compile(rb"192\.168\.\d+\.\d+"), []),
    ("ssh key name", re.compile(rb"id_rrv_" + b"deck"), []),
    ("personal e-mail", re.compile(rb"dani\.sandoval" + b"@"), []),
    ("private key", re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"), []),
    ("token", re.compile(rb"\b(?:ghp_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{10,})\b"), []),
]

MAX_FILE_BYTES = 5 * 1024 * 1024
# Known large, reviewed files (path -> reason).
LARGE_FILE_ALLOW = {
    "third_party/ps2recomp/ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h":
        "upstream ran-j/PS2Recomp symbol database (12,074,936 bytes upstream)",
}

# PNG/JPEG are allowed only here (README screenshots); never as game assets elsewhere.
IMAGE_ALLOWED_DIRS = ("docs/img/", "public/img/", "tools/fukami-app/fukami.png")


# Ignored directories of a public checkout that a re-export must never delete.
LOCAL_ONLY = {"generated", "runtime", "runtime-deps", "local", "rom", "tmp"}


def git(*args: str, cwd: Path = ROOT, text: bool = True) -> str:
    out = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=text)
    return out.stdout


def matches(path: str, patterns: list[str]) -> bool:
    """fnmatch on the repo-relative POSIX path, anchored at the root; a pattern starting with `**/` matches at any depth."""
    for p in patterns:
        if fnmatch.fnmatchcase(path, p) or (p.startswith("**/") and fnmatch.fnmatchcase(path, p[3:])):
            return True
    return False


def wanted(rel: str) -> bool:
    top = rel.split("/", 1)[0]
    if rel in ROOT_FILES:
        return True
    if top not in ROOT_DIRS:
        return False
    if matches(rel, EXCLUDE):
        return False
    if rel.startswith("tools/patches/") and rel != "tools/patches/INDEX.md":
        return matches(rel, KEPT_PATCHES)
    if rel == "tools/patches/INDEX.md":
        return False
    return True


def extract_revision(rev: str, dest: Path) -> None:
    """Materialise one committed revision (never the working tree) with `git archive`."""
    dest.mkdir(parents=True, exist_ok=True)
    archive = subprocess.Popen(["git", "archive", "--format=tar", rev], cwd=ROOT, stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", str(dest)], stdin=archive.stdout, check=True)
    if archive.wait() != 0:
        raise SystemExit("git archive failed")


def copy_selected(src: Path, out: Path) -> int:
    count = 0
    for path in sorted(src.rglob("*")):
        if path.is_dir() or path.is_symlink():
            if path.is_symlink():
                raise SystemExit(f"symlink in tracked tree: {path.relative_to(src)} (refusing to export)")
            continue
        rel = path.relative_to(src).as_posix()
        if not wanted(rel):
            continue
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        count += 1
    return count


# Hash-only reference manifests: what scripts/fukami_generate.py must reproduce from the user's ELF.
# (path inside generated/rr5 of the private revision -> name in the export's config/rr5)
REFERENCE_MANIFESTS = {
    "source-manifest.json": "source-manifest.json",
    "gate3-accounted-generation-manifest.json": "gate3-accounted-generation-manifest.json",
    "native/manifest.json": "native-manifest.json",
    "vu/manifest.json": "vu-manifest.json",
    "vu/programs.json": "vu-programs.json",
}


def add_reference_manifests(stage: Path, out: Path) -> int:
    dest = out / "config/rr5"
    dest.mkdir(parents=True, exist_ok=True)
    for src_rel, name in REFERENCE_MANIFESTS.items():
        src = stage / "generated/rr5" / src_rel
        if not src.is_file():
            raise SystemExit(f"reference manifest missing in the revision: generated/rr5/{src_rel}")
        shutil.copy2(src, dest / name)
    return len(REFERENCE_MANIFESTS)


def apply_overlay(stage: Path, out: Path) -> int:
    """`public/` (of the exported revision) is laid over the root of the export: README, docs, CI, .gitignore."""
    overlay = stage / "public"
    count = 0
    if not overlay.is_dir():
        return 0
    for path in sorted(overlay.rglob("*")):
        if path.is_dir() or path.name == ".DS_Store":
            continue
        rel = path.relative_to(overlay)
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        count += 1
    return count


def prune_suite_manifest(stage: Path, out: Path) -> int:
    """scripts/asset_free_suite.json lists every required test; the harness tests (cmake/RrvHarnessTests.cmake) are not in
    the export, so their entries (and the targets of the header contracts) leave the manifest too. Derived, not listed."""
    import json
    harness = (stage / "cmake/RrvHarnessTests.cmake").read_text()
    dropped = set(re.findall(r"add_test\(NAME ([\w-]+)", harness))
    dropped |= set(re.findall(r"rrv_add_header_contract_test\([\w-]+\s+[\w/.]+\s+([\w-]+)\)", harness))
    path = out / "scripts/asset_free_suite.json"
    manifest = json.loads(path.read_text())
    removed = 0
    for key in ("required", "input_dependent", "platform_native"):
        kept = [e for e in manifest.get(key, []) if e["name"] not in dropped]
        removed += len(manifest.get(key, [])) - len(kept)
        manifest[key] = kept
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return removed


def transform_lock(out: Path) -> None:
    """The private repository reconstructs the compatible PS2Recomp from upstream (which no longer has the revision)
    through a parentless snapshot branch of this repository. The public one carries the producer in tree, so the
    snapshot keys go and the lock says so; prepare_dependencies.py then has nothing to fetch for it."""
    path = out / "config/dependencies.lock.toml"
    text = path.read_text()
    block = re.search(r"# Upstream no longer has `revision`.*?# The same tree is also tracked in this repository at third_party/ps2recomp\.\n", text, re.S)
    if not block or "snapshot_url" not in block.group(0):
        raise SystemExit("lock transform: the snapshot block of config/dependencies.lock.toml was not found")
    text = text.replace(block.group(0),
        "# Upstream no longer has `revision`, so this repository carries the producer in tree (third_party/ps2recomp: the\n"
        "# compatible tree below plus the changes listed in its RRV_CHANGES.md). There is nothing to fetch for it:\n"
        "# prepare_dependencies.py skips this component.\n"
        'vendored_in_tree = "third_party/ps2recomp"\n')
    path.write_text(text)


def apply_replacements(out: Path) -> None:
    for rel, old, new in TEXT_REPLACEMENTS:
        path = out / rel
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if old not in text:
            continue
        path.write_text(text.replace(old, new), encoding="utf-8")


def iter_files(tree: Path):
    for path in sorted(tree.rglob("*")):
        if ".git" in path.relative_to(tree).parts[:1]:
            continue
        if path.is_file() or path.is_symlink():
            yield path


def run_gates(tree: Path) -> list[str]:
    problems: list[str] = []
    for path in iter_files(tree):
        rel = path.relative_to(tree).as_posix()
        if path.is_symlink():
            problems.append(f"{rel}: symlink")
            continue
        if matches(rel, FORBIDDEN_PATH_GLOBS):
            problems.append(f"{rel}: forbidden path")
            continue
        size = path.stat().st_size
        if size > MAX_FILE_BYTES and rel not in LARGE_FILE_ALLOW:
            problems.append(f"{rel}: {size} bytes exceeds {MAX_FILE_BYTES} (not on the reviewed large-file list)")
        data = path.read_bytes()
        head = data[:16]
        if head.startswith(b"\x7fELF"):
            problems.append(f"{rel}: ELF magic")
        if head.startswith(b"MComprHD"):
            problems.append(f"{rel}: CHD magic")
        if len(data) > 0x8006 and data[0x8001:0x8006] == b"CD001":
            problems.append(f"{rel}: ISO9660 magic")
        if head.startswith(b"\x89PNG") or head.startswith(b"\xff\xd8\xff"):
            if not rel.startswith(IMAGE_ALLOWED_DIRS):
                problems.append(f"{rel}: image outside the reviewed screenshot directory")
        if b"\x00" in data[:4096]:
            continue  # binary: the text gates below do not apply
        for name, rx, allowed in TEXT_GATES:
            if rx.search(data) and not matches(rel, allowed):
                line = next(i for i, ln in enumerate(data.split(b"\n"), 1) if rx.search(ln))
                problems.append(f"{rel}:{line}: {name}")
    return problems


def local_git_identity() -> tuple[str, str]:
    name = git("config", "user.name").strip()
    email = git("config", "user.email").strip()
    return name, email


def commit_export(out: Path, message: str, author: tuple[str, str]) -> str:
    env = dict(os.environ, GIT_AUTHOR_NAME=author[0], GIT_AUTHOR_EMAIL=author[1],
               GIT_COMMITTER_NAME=author[0], GIT_COMMITTER_EMAIL=author[1])
    def g(*a: str) -> str:
        return subprocess.run(["git", *a], cwd=out, check=True, capture_output=True, text=True, env=env).stdout
    if not (out / ".git").exists():
        g("init", "-q", "-b", "main")
    # Local safety net: this repository must not be pushed until the owner says so.
    hook = out / ".git/hooks/pre-push"
    hook.parent.mkdir(parents=True, exist_ok=True)
    hook.write_text("#!/bin/sh\n[ \"$FUKAMI_ALLOW_PUSH\" = 1 ] && exit 0\n"
                    "echo 'pre-push: blocked. The public Fukami repository is not authorised for push yet.' >&2\nexit 1\n")
    hook.chmod(0o755)
    if g("remote").strip():
        print("note: the export repository has a remote configured:", g("remote", "-v").strip(), file=sys.stderr)
    g("add", "-A")
    if not g("status", "--porcelain").strip():
        return "unchanged"
    g("commit", "-q", "-m", message)
    return g("rev-parse", "--short", "HEAD").strip()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, help="export directory (created; its non-.git contents are replaced)")
    ap.add_argument("--rev", default="HEAD", help="private revision to export (default HEAD)")
    ap.add_argument("--commit", action="store_true", help="commit the export locally (no remote, no push)")
    ap.add_argument("--orphan", action="store_true",
                    help="with --commit: drop the checkout's old git history first, so the export is ONE parentless commit")
    ap.add_argument("--message", help="commit message (default: 'Fukami <VERSION>' from the VERSION file)")
    ap.add_argument("--author", help="'Name <email>' for the commit (default: this repository's git identity)")
    ap.add_argument("--check-tree", type=Path, help="only run the safety gates on this directory")
    args = ap.parse_args()

    if args.check_tree:
        problems = run_gates(args.check_tree)
        for p in problems:
            print("GATE FAIL:", p, file=sys.stderr)
        print("gates:", "FAIL" if problems else "PASS", f"({args.check_tree})")
        return 1 if problems else 0

    if not args.out:
        ap.error("--out is required")
    out = args.out.expanduser().resolve()
    if out == ROOT or ROOT in out.parents:
        ap.error("--out must be outside the private repository")
    rev = git("rev-parse", args.rev).strip()
    if git("status", "--porcelain", "--untracked-files=no").strip() and args.rev == "HEAD":
        print("warning: tracked changes in the working tree are NOT exported (only the committed revision is).", file=sys.stderr)

    with tempfile.TemporaryDirectory(prefix="fukami-export-") as tmp:
        stage = Path(tmp) / "src"
        extract_revision(rev, stage)
        staged = Path(tmp) / "out"
        staged.mkdir()
        copied = copy_selected(stage, staged)
        overlaid = apply_overlay(stage, staged) + add_reference_manifests(stage, staged)
        pruned = prune_suite_manifest(stage, staged)
        print(f"suite manifest: {pruned} harness entries left out")
        transform_lock(staged)
        apply_replacements(staged)
        problems = run_gates(staged)
        if problems:
            for p in problems:
                print("GATE FAIL:", p, file=sys.stderr)
            print(f"export refused: {len(problems)} gate failure(s); nothing was written to {out}", file=sys.stderr)
            return 1

        out.mkdir(parents=True, exist_ok=True)
        for child in out.iterdir():
            if child.name == ".git" or child.name in LOCAL_ONLY or child.name.startswith("build"):
                continue  # the user's own ignored work in the export checkout (generated game code, builds, runtimes)
            shutil.rmtree(child) if child.is_dir() else child.unlink()
        for child in staged.iterdir():
            dest = out / child.name
            shutil.copytree(child, dest, symlinks=True) if child.is_dir() else shutil.copy2(child, dest)

    print(f"exported {copied} files + {overlaid} overlay files from {rev[:10]} to {out}; gates PASS")
    if args.commit:
        if args.orphan and (out / ".git").exists():
            shutil.rmtree(out / ".git")
        version = (out / "VERSION").read_text().strip() if (out / "VERSION").exists() else "dev"
        author = (args.author and tuple(re.match(r"(.*?)\s*<(.*)>", args.author).groups())) or local_git_identity()
        message = args.message or f"Fukami {version}\n\nExport of private revision {rev[:10]}."
        print("local commit:", commit_export(out, message, author), "(not pushed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
