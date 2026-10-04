#!/usr/bin/env python3
"""Verify or prepare clean source checkouts named in config/dependencies.lock.toml.

The PS2Recomp dependency is a compatible reconstruction: one content-addressed
patch is applied to the locked upstream base and committed with fixed metadata.
It is deliberately not described as the unavailable historical d52 checkout.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
from typing import Optional

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.9, supplied by the macOS toolchain.
    tomllib = None


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK_PATH = REPO_ROOT / "config" / "dependencies.lock.toml"


def run(*args: str, cwd: Optional[pathlib.Path] = None,
        environment: Optional[dict[str, str]] = None) -> str:
    completed = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=environment)
    if completed.returncode:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"{' '.join(args)} failed: {detail}")
    return completed.stdout.strip()


def repo_state(path: pathlib.Path, revision: str, tree: Optional[str] = None) -> list[str]:
    problems: list[str] = []
    if not (path / ".git").exists():
        return [f"not a Git checkout: {path}"]
    try:
        actual = run("git", "rev-parse", "HEAD", cwd=path)
        run("git", "rev-parse", "--verify", f"{revision}^{{commit}}", cwd=path)
        dirty = run("git", "status", "--porcelain=v1", cwd=path)
    except RuntimeError as error:
        return [str(error)]
    if actual != revision:
        problems.append(f"HEAD is {actual}, expected {revision}")
    if tree:
        actual_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=path)
        if actual_tree != tree:
            problems.append(f"tree is {actual_tree}, expected {tree}")
    if dirty:
        problems.append("checkout has local edits")
    return problems


def verify_patch_set(lock: dict[str, object], name: str) -> list[str]:
    problems: list[str] = []
    for item in lock.get(name, []):
        assert isinstance(item, dict)
        path = REPO_ROOT / str(item["path"])
        expected = str(item["sha256"])
        if not path.is_file():
            problems.append(f"missing patch: {path.relative_to(REPO_ROOT)}")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            problems.append(f"patch hash mismatch: {path.relative_to(REPO_ROOT)}")
    return problems


def reconstruct_compatible_ps2recomp(component: dict[str, object], destination: pathlib.Path) -> None:
    patch = REPO_ROOT / str(component["compatible_patch"])
    expected_patch_hash = str(component["compatible_patch_sha256"])
    if hashlib.sha256(patch.read_bytes()).hexdigest() != expected_patch_hash:
        raise RuntimeError(f"PS2Recomp compatible patch hash mismatch: {patch}")
    run("git", "apply", "--check", str(patch), cwd=destination)
    run("git", "apply", "--index", str(patch), cwd=destination)
    actual_tree = run("git", "write-tree", cwd=destination)
    expected_tree = str(component["compatible_tree"])
    if actual_tree != expected_tree:
        raise RuntimeError(f"PS2Recomp reconstructed tree is {actual_tree}, expected {expected_tree}")
    fixed = os.environ.copy()
    fixed.update({
        "GIT_AUTHOR_NAME": "RRV M0R Reconstruction",
        "GIT_AUTHOR_EMAIL": "rrv-m0r@invalid.local",
        "GIT_AUTHOR_DATE": str(component["compatible_commit_date"]),
        "GIT_COMMITTER_NAME": "RRV M0R Reconstruction",
        "GIT_COMMITTER_EMAIL": "rrv-m0r@invalid.local",
        "GIT_COMMITTER_DATE": str(component["compatible_commit_date"]),
    })
    run("git", "commit", "-m", str(component["compatible_name"]),
        cwd=destination, environment=fixed)
    actual_revision = run("git", "rev-parse", "HEAD", cwd=destination)
    expected_revision = str(component["compatible_revision"])
    if actual_revision != expected_revision:
        raise RuntimeError(
            f"PS2Recomp compatible commit is {actual_revision}, expected {expected_revision}")


def reconstruct_ps2recomp_from_snapshot(component: dict[str, object], destination: pathlib.Path) -> None:
    """Rebuild the compatible-v2 checkout without the (unreachable) upstream base.

    Upstream cf9d22b is no longer reachable.  The RRV repository keeps a
    parentless snapshot of the compatible-v2 tree on a branch.  The original
    commit object is rewritten byte-for-byte (fixed author, date, message and a
    parent that stays absent behind a shallow boundary), so HEAD is again
    compatible_revision.
    """
    run("git", "init", "-q", str(destination))
    run("git", "fetch", "-q", str(component["snapshot_url"]), str(component["snapshot_branch"]),
        cwd=destination)
    expected_tree = str(component["compatible_tree"])
    fetched_tree = run("git", "rev-parse", "FETCH_HEAD^{tree}", cwd=destination)
    if fetched_tree != expected_tree:
        raise RuntimeError(f"snapshot tree is {fetched_tree}, expected {expected_tree}")
    who = f"RRV M0R Reconstruction <rrv-m0r@invalid.local> {component['compatible_commit_epoch']} +0000"
    commit = (f"tree {expected_tree}\nparent {component['revision']}\n"
              f"author {who}\ncommitter {who}\n\n{component['compatible_name']}\n")
    completed = subprocess.run(
        ["git", "hash-object", "-t", "commit", "--literally", "-w", "--stdin"],
        cwd=destination, input=commit, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        raise RuntimeError(f"git hash-object failed: {completed.stderr.strip()}")
    actual_revision = completed.stdout.strip()
    expected_revision = str(component["compatible_revision"])
    if actual_revision != expected_revision:
        raise RuntimeError(
            f"PS2Recomp compatible commit is {actual_revision}, expected {expected_revision}")
    (destination / ".git" / "shallow").write_text(f"{component['revision']}\n", encoding="ascii")
    run("git", "checkout", "-q", "--detach", expected_revision, cwd=destination)


def clone_clean(name: str, component: dict[str, object], destination: pathlib.Path) -> None:
    if destination.exists():
        raise RuntimeError(
            f"refusing to replace existing path: {destination}; move it aside or use --verify")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if name == "ps2recomp":
        # Prefer the upstream base plus the locked patch; fall back to the
        # snapshot branch when upstream no longer has the revision.
        try:
            run("git", "clone", "--no-checkout", str(component["url"]), str(destination))
            run("git", "cat-file", "-e", f"{component['revision']}^{{commit}}", cwd=destination)
            run("git", "checkout", "--detach", str(component["revision"]), cwd=destination)
            reconstruct_compatible_ps2recomp(component, destination)
            return
        except Exception:
            if destination.exists():
                shutil.rmtree(destination)
        try:
            reconstruct_ps2recomp_from_snapshot(component, destination)
        except Exception:
            if destination.exists():
                shutil.rmtree(destination)
            raise
        return
    run("git", "clone", "--no-checkout", str(component["url"]), str(destination))
    try:
        run("git", "checkout", "--detach", str(component["revision"]), cwd=destination)
    except Exception:
        shutil.rmtree(destination)
        raise


def load_toml(path: pathlib.Path) -> dict[str, object]:
    """Read the small TOML subset used by RRV manifests on Python 3.9."""
    if tomllib is not None:
        with path.open("rb") as stream:
            return tomllib.load(stream)

    result: dict[str, object] = {}
    current: dict[str, object] = result
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[[") and line.endswith("]]" ):
            name = line[2:-2]
            entries = result.setdefault(name, [])
            assert isinstance(entries, list)
            current = {}
            entries.append(current)
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            current = {}
            result[name] = current
            continue
        if "=" not in line:
            raise RuntimeError(f"unsupported lock syntax: {raw_line}")
        key, value = (part.strip() for part in line.split("=", 1))
        if value.startswith('"') and value.endswith('"'):
            current[key] = value[1:-1]
        elif value in ("true", "false"):
            current[key] = value == "true"
        else:
            raise RuntimeError(f"unsupported lock value: {raw_line}")
    return result


def verify_metal_resources(lock: dict[str, object], resource_dir: pathlib.Path,
                           manifest_path: pathlib.Path) -> list[str]:
    problems: list[str] = []
    if not manifest_path.is_file():
        return [f"Metal resource manifest is unavailable: {manifest_path}"]
    try:
        manifest = load_toml(manifest_path)
    except (OSError, RuntimeError, ValueError) as error:
        return [f"cannot read Metal resource manifest {manifest_path}: {error}"]

    pcsx2 = lock["pcsx2"]
    assert isinstance(pcsx2, dict)
    expected_revision = str(pcsx2["revision"])
    if manifest.get("pcsx2_revision") != expected_revision:
        problems.append(
            f"Metal resource manifest revision is {manifest.get('pcsx2_revision')!r}, expected {expected_revision}")
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        return problems + ["Metal resource manifest must contain one or more [[files]] entries"]

    root = resource_dir.resolve()
    required = {"default.metallib", "Metal22.metallib", "Metal23.metallib"}
    seen: set[str] = set()
    for item in entries:
        if not isinstance(item, dict):
            problems.append("Metal resource manifest has a malformed [[files]] entry")
            continue
        relative = item.get("path")
        expected_hash = item.get("sha256")
        if not isinstance(relative, str) or not isinstance(expected_hash, str):
            problems.append("Metal resource manifest entries require quoted path and sha256 values")
            continue
        candidate = (root / relative).resolve()
        if root not in candidate.parents:
            problems.append(f"Metal resource path escapes its directory: {relative}")
            continue
        seen.add(relative)
        if not candidate.is_file():
            problems.append(f"missing Metal resource: {relative}")
            continue
        if hashlib.sha256(candidate.read_bytes()).hexdigest() != expected_hash:
            problems.append(f"Metal resource hash mismatch: {relative}")
    missing = required - seen
    if missing:
        problems.append("Metal resource manifest omits: " + ", ".join(sorted(missing)))
    return problems


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="verify checkouts and patch hashes only")
    parser.add_argument("--prepare", action="store_true", help="clone missing clean checkouts at locked revisions")
    parser.add_argument("--verify-patches", action="store_true", help="verify locked PCSX2 patch hashes only")
    parser.add_argument("--verify-metal-resources", metavar="DIRECTORY",
                        help="verify Metal resources against an explicit reviewed manifest")
    parser.add_argument("--metal-resource-manifest", metavar="PATH",
                        help="TOML manifest with pcsx2_revision and [[files]] SHA-256 entries")
    parser.add_argument("--component", choices=("all", "ps2recomp", "pcsx2", "sse2neon"), default="all")
    parser.add_argument("--destination", metavar="PATH",
                        help="checkout path instead of the locked default (needs --component ps2recomp or pcsx2)")
    return parser.parse_args()


def verify_ps2recomp_compatible_patch(component: dict[str, object]) -> list[str]:
    path = REPO_ROOT / str(component["compatible_patch"])
    if not path.is_file():
        return [f"missing patch: {path.relative_to(REPO_ROOT)}"]
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    expected = str(component["compatible_patch_sha256"])
    return [] if actual == expected else [f"patch hash mismatch: {path.relative_to(REPO_ROOT)}"]


def main() -> int:
    args = parse_args()
    if sum((args.verify, args.prepare, args.verify_patches, bool(args.verify_metal_resources))) != 1:
        print("error: choose exactly one verification or preparation action", file=sys.stderr)
        return 2
    lock = load_toml(LOCK_PATH)
    marker = REPO_ROOT / "tools" / "pcsx2-gs-bridge" / "PINNED_REVISION"
    if marker.read_text().strip() != lock["pcsx2"]["revision"]:
        print("error: PCSX2 PINNED_REVISION disagrees with dependencies.lock.toml", file=sys.stderr)
        return 1

    if args.verify_metal_resources:
        if not args.metal_resource_manifest:
            print("error: --verify-metal-resources requires --metal-resource-manifest", file=sys.stderr)
            return 2
        problems = verify_metal_resources(
            lock, pathlib.Path(args.verify_metal_resources), pathlib.Path(args.metal_resource_manifest))
        if problems:
            print("Metal resource verification failed:", file=sys.stderr)
            for problem in problems:
                print(f"  - {problem}", file=sys.stderr)
            return 1
        print("Metal resource verification passed")
        return 0

    if args.verify_patches:
        problems: list[str] = []
        ps2recomp = lock["ps2recomp"]
        assert isinstance(ps2recomp, dict)
        problems.extend(verify_ps2recomp_compatible_patch(ps2recomp))
        for patch_set in ("pcsx2_bridge_patches", "pcsx2_gt_capture_patches", "pcsx2_gt_debug_server_patch"):
            problems.extend(verify_patch_set(lock, patch_set))
        if problems:
            print("locked patch verification failed:", file=sys.stderr)
            for problem in problems:
                print(f"  - {problem}", file=sys.stderr)
            return 1
        print(f"locked patch verification passed: {LOCK_PATH.relative_to(REPO_ROOT)}")
        return 0

    if args.destination and args.component == "all":
        print("error: --destination needs --component ps2recomp or pcsx2", file=sys.stderr)
        return 2
    components = ("ps2recomp", "pcsx2", "sse2neon") if args.component == "all" else (args.component,)
    problems: list[str] = []
    for name in components:
        component = lock[name]
        assert isinstance(component, dict)
        if component.get("vendored_in_tree"):
            # The public repository carries this producer in tree; there is no checkout to fetch or verify.
            where = REPO_ROOT / str(component["vendored_in_tree"])
            if not where.is_dir():
                problems.append(f"{name}: vendored directory is missing: {where}")
            else:
                print(f"{name}: vendored in {component['vendored_in_tree']} (nothing to prepare)")
            continue
        destination = (pathlib.Path(args.destination).resolve() if args.destination
                       else REPO_ROOT / str(component["default_checkout"]))
        if args.prepare and not destination.exists():
            try:
                clone_clean(name, component, destination)
                print(f"prepared {name}: {destination}")
            except RuntimeError as error:
                problems.append(f"{name}: {error}")
                continue
        expected_revision = str(component.get("compatible_revision", component["revision"]))
        expected_tree = str(component["compatible_tree"]) if "compatible_tree" in component else None
        for problem in repo_state(destination, expected_revision, expected_tree):
            problems.append(f"{name}: {problem}")

    if args.component in ("all", "pcsx2"):
        for patch_set in ("pcsx2_bridge_patches", "pcsx2_gt_capture_patches", "pcsx2_gt_debug_server_patch"):
            problems.extend(verify_patch_set(lock, patch_set))

    if "ps2recomp" in components:
        ps2recomp = lock["ps2recomp"]
        assert isinstance(ps2recomp, dict)
        problems.extend(verify_ps2recomp_compatible_patch(ps2recomp))
        status = lock["ps2recomp"].get("historic_dirty_tree")
        print(f"ps2recomp historical local state: {status}")

    if problems:
        print("dependency verification failed:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(f"source/patch identity verification passed: {LOCK_PATH.relative_to(REPO_ROOT)}")
    if "ps2recomp" in components and not ps2recomp.get("vendored_in_tree"):
        print(f"{ps2recomp['compatible_name']} source identity verified; exact historical d52 identity remains unavailable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
