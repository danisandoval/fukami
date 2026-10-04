#!/usr/bin/env python3
"""Generate RR5 C++ out of tree with a content-addressed local manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK = ROOT / "config" / "dependencies.lock.toml"
CONFIG = ROOT / "config" / "rrv.toml"


def run(args: list[str], *, capture: bool = True) -> str:
    completed = subprocess.run(args, cwd=ROOT, text=True,
                               stdout=subprocess.PIPE if capture else None,
                               stderr=subprocess.STDOUT if capture else None)
    if completed.returncode:
        detail = (completed.stdout or "").strip()
        raise SystemExit(f"{' '.join(args)} failed ({completed.returncode}):\n{detail}")
    return completed.stdout or ""


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(path: pathlib.Path, *args: str) -> str:
    completed = subprocess.run(["git", *args], cwd=path, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        raise SystemExit(completed.stderr.strip() or completed.stdout.strip())
    return completed.stdout.strip()


def display_path(path: pathlib.Path) -> str:
    path = path.resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return os.path.relpath(path, ROOT)


def locked_revision(section: str) -> str:
    text = LOCK.read_text(encoding="utf-8")
    match = re.search(
        rf"(?ms)^\[{re.escape(section)}\]\s.*?^revision\s*=\s*\"([0-9a-f]{{40}})\"",
        text,
    )
    if not match:
        raise SystemExit(f"cannot find locked {section} revision in {LOCK}")
    return match.group(1)


def locked_value(section: str, key: str) -> str:
    text = LOCK.read_text(encoding="utf-8")
    match = re.search(
        rf"(?ms)^\[{re.escape(section)}\]\s.*?^{re.escape(key)}\s*=\s*\"([^\"]+)\"",
        text,
    )
    if not match:
        raise SystemExit(f"cannot find locked {section}.{key} in {LOCK}")
    return match.group(1)


def cmake_cache_value(cache: pathlib.Path, key: str) -> str:
    match = re.search(rf"(?m)^{re.escape(key)}:[^=]*=(.*)$",
                      cache.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(f"cannot find {key} in {cache}")
    return match.group(1)


def materialize_config(input_path: pathlib.Path, output_path: pathlib.Path,
                       destination: pathlib.Path) -> None:
    text = CONFIG.read_text(encoding="utf-8")
    text, input_count = re.subn(
        r'(?m)^input\s*=\s*"[^"]*"$',
        f'input = "{display_path(input_path)}"', text, count=1)
    text, output_count = re.subn(
        r'(?m)^output\s*=\s*"[^"]*"$',
        f'output = "{display_path(output_path)}/"', text, count=1)
    if input_count != 1 or output_count != 1:
        raise SystemExit("tracked RR5 config must contain exactly one input and output assignment")
    destination.write_text(text, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--producer-source", required=True, type=pathlib.Path)
    parser.add_argument("--producer-build", required=True, type=pathlib.Path)
    parser.add_argument("--input", required=True, type=pathlib.Path,
                        help="user-owned RR5 ELF")
    parser.add_argument("--run-dir", required=True, type=pathlib.Path,
                        help="new local or temporary directory for config, output, logs and manifest")
    parser.add_argument("--input-id", help="local identifier recorded in addition to the input hash")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    producer_source = args.producer_source.resolve()
    producer_build = args.producer_build.resolve()
    input_path = args.input.resolve()
    run_dir = args.run_dir.resolve()
    output_dir = run_dir / "output"
    analyzer = producer_build / "ps2xAnalyzer" / "ps2_analyzer"
    recompiler = producer_build / "ps2xRecomp" / "ps2_recomp"
    cache = producer_build / "CMakeCache.txt"
    ninja = producer_build / "build.ninja"

    for required in (input_path, analyzer, recompiler, cache, ninja):
        if not required.is_file():
            raise SystemExit(f"required input is missing: {required}")
    if run_dir.exists() and any(run_dir.iterdir()):
        raise SystemExit(f"run directory must be new or empty: {run_dir}")
    forbidden_output = (ROOT / "config" / "output").resolve()
    if output_dir == forbidden_output or forbidden_output in output_dir.parents:
        raise SystemExit("refusing to generate into ignored config/output")
    if producer_source == output_dir or producer_source in output_dir.parents:
        raise SystemExit("refusing to generate inside the producer source tree")

    producer_commit = git(producer_source, "rev-parse", "HEAD")
    producer_tree = git(producer_source, "rev-parse", "HEAD^{tree}")
    producer_clean = not bool(git(producer_source, "status", "--porcelain=v1"))
    expected_commit = locked_value("ps2recomp", "compatible_revision")
    expected_tree = locked_value("ps2recomp", "compatible_tree")
    configured_source = pathlib.Path(cmake_cache_value(cache, "CMAKE_HOME_DIRECTORY")).resolve()
    if (producer_commit != expected_commit or producer_tree != expected_tree or not producer_clean):
        raise SystemExit("producer source is not the clean locked compatible commit/tree")
    if configured_source != producer_source:
        raise SystemExit(
            f"producer build was configured from {configured_source}, expected {producer_source}")

    producer_tools = {
        "analyzer": {
            "path": display_path(analyzer),
            "size": analyzer.stat().st_size,
            "sha256": sha256(analyzer),
        },
        "recompiler": {
            "path": display_path(recompiler),
            "size": recompiler.stat().st_size,
            "sha256": sha256(recompiler),
        },
    }

    run_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir()
    before = git(ROOT, "status", "--porcelain=v1")
    analyzer_toml = run_dir / "analyzer.toml"
    analyzer_log = run([
        display_path(analyzer), display_path(input_path), display_path(analyzer_toml)
    ])
    (run_dir / "analyzer.log").write_text(analyzer_log, encoding="utf-8")

    effective_config = run_dir / "rrv.toml"
    materialize_config(input_path, output_dir, effective_config)
    recompiler_log = run([display_path(recompiler), display_path(effective_config)])
    (run_dir / "recompiler.log").write_text(recompiler_log, encoding="utf-8")
    after = git(ROOT, "status", "--porcelain=v1")
    if before != after:
        raise SystemExit("generation changed the Git working tree")

    generated: list[dict[str, object]] = []
    addresses: list[int] = []
    function_pattern = re.compile(r"^sub_[0-9A-Fa-f]+_0x([0-9A-Fa-f]+)\.cpp$")
    for path in sorted(output_dir.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(output_dir).as_posix()
        generated.append({"path": relative, "size": path.stat().st_size, "sha256": sha256(path)})
        match = function_pattern.match(path.name)
        if match:
            addresses.append(int(match.group(1), 16))
    if not generated or not (output_dir / "register_functions.cpp").is_file():
        raise SystemExit("generation did not produce the expected register_functions.cpp")

    hook_paths = [
        ROOT / "src" / "ir" / "rrv_ir_hooks.h",
        ROOT / "tools" / "pcsx2-gs-bridge" / "rrv_pcsx2_gs_bridge.h",
    ]
    hook_hashes = {display_path(path): sha256(path) for path in hook_paths}
    hook_fingerprint = hashlib.sha256(
        "".join(f"{path}:{digest}\n" for path, digest in sorted(hook_hashes.items())).encode()
    ).hexdigest()

    manifest = {
        "schema": 1,
        "producer_name": locked_value("ps2recomp", "compatible_name"),
        "producer": {
            "commit": producer_commit,
            "tree": producer_tree,
            "clean": producer_clean,
            "upstream_base": locked_revision("ps2recomp"),
        },
        "producer_build": {
            "generator": cmake_cache_value(cache, "CMAKE_GENERATOR"),
            "configured_source": display_path(configured_source),
            "cmake_cache_sha256": sha256(cache),
            "build_ninja_sha256": sha256(ninja),
            "tools": producer_tools,
        },
        "rrv_commit": git(ROOT, "rev-parse", "HEAD"),
        "pcsx2_commit": locked_revision("pcsx2"),
        "bridge_abi": 5,
        "config": {
            "template": display_path(CONFIG),
            "template_sha256": sha256(CONFIG),
            "effective_sha256": sha256(effective_config),
            "analyzer_sha256": sha256(analyzer_toml),
        },
        "game_input": {
            "identifier": args.input_id or input_path.name,
            "sha256": sha256(input_path),
        },
        "commands": {
            "analyze": [display_path(analyzer), display_path(input_path), display_path(analyzer_toml)],
            "recompile": [display_path(recompiler), display_path(effective_config)],
        },
        "generated": {
            "directory": display_path(output_dir),
            "file_count": len(generated),
            "files": generated,
            "function_count": len(addresses),
            "lowest_function_address": f"0x{min(addresses):08x}" if addresses else None,
            "highest_function_address": f"0x{max(addresses):08x}" if addresses else None,
            "address_census_sha256": hashlib.sha256(
                "".join(f"{address:08x}\n" for address in sorted(addresses)).encode()
            ).hexdigest(),
        },
        "hook_interface": {
            "files": hook_hashes,
            "fingerprint_sha256": hook_fingerprint,
            "firstChunkOfTransfer": "absent",
        },
        "working_tree_unchanged": True,
    }
    manifest_path = run_dir / "generation-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"generation passed: {len(generated)} files, {len(addresses)} functions")
    print(f"manifest: {display_path(manifest_path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
