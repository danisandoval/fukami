#!/usr/bin/env python3
"""Compile and execute actual signed-zero branch strings emitted by CodeGenerator.

This is intentionally opt-in: it requires an already-built, clean producer
checkout selected by config/dependencies.lock.toml.  It never reads game data.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "config" / "dependencies.lock.toml"
DRIVER = ROOT / "tests" / "signed_branch_codegen_execution_driver.cpp"


def lock_value(key: str) -> str:
    text = LOCK.read_text(encoding="utf-8")
    section = re.search(r"(?ms)^\[ps2recomp\]\s*(.*?)(?=^\[|\Z)", text)
    if section is None:
        raise SystemExit("dependencies lock lacks [ps2recomp]")
    match = re.search(rf"(?m)^{re.escape(key)}\s*=\s*\"([^\"]+)\"\s*$", section.group(1))
    if match is None:
        raise SystemExit(f"dependencies lock lacks ps2recomp.{key}")
    return match.group(1)


def git(source: Path, *args: str) -> str:
    completed = subprocess.run(["git", *args], cwd=source, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed in {source}: {completed.stderr.strip()}")
    return completed.stdout.strip()


def cache_value(cache: Path, key: str) -> str:
    pattern = re.compile(rf"^{re.escape(key)}(?::[^=]*)?=(.*)$", re.MULTILINE)
    match = pattern.search(cache.read_text(encoding="utf-8", errors="replace"))
    if match is None:
        raise SystemExit(f"producer build cache lacks {key}: {cache}")
    return match.group(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def require_locked_build(source: Path, build: Path) -> None:
    expected_source = (ROOT / lock_value("default_checkout")).resolve()
    if source != expected_source:
        raise SystemExit(f"producer source must be locked checkout {expected_source}, got {source}")
    if not source.is_dir():
        raise SystemExit(f"locked producer source is missing: {source}")
    expected_revision = lock_value("compatible_revision")
    expected_tree = lock_value("compatible_tree")
    if git(source, "rev-parse", "HEAD") != expected_revision:
        raise SystemExit("producer source is not the locked compatible revision")
    if git(source, "rev-parse", "HEAD^{tree}") != expected_tree:
        raise SystemExit("producer source tree does not match the lock")
    if git(source, "status", "--porcelain=v1"):
        raise SystemExit("producer source must be clean")

    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise SystemExit(f"producer build cache is missing: {cache}")
    configured_source = Path(cache_value(cache, "CMAKE_HOME_DIRECTORY")).resolve()
    if configured_source != source:
        raise SystemExit(f"producer build source is {configured_source}, expected {source}")


def run(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        rendered = " ".join(command)
        raise SystemExit(f"command failed ({completed.returncode}): {rendered}\n{completed.stdout}")
    return completed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args()

    source = args.source.resolve()
    build = args.build.resolve()
    output = args.output.resolve()
    require_locked_build(source, build)
    if not DRIVER.is_file():
        raise SystemExit(f"execution driver is missing: {DRIVER}")

    required_libraries = (
        build / "ps2xRecomp" / "libps2_recomp_lib.a",
        build / "ps2xRecomp" / "librabbitizer.a",
        build / "_deps" / "fmt-build" / "libfmt.a",
    )
    producer_build_command = ["cmake", "--build", str(build), "--target", "ps2_recomp_lib"]
    output.mkdir(parents=True, exist_ok=True)
    producer_build_result = run(producer_build_command, cwd=output)
    # A source-bound target rebuild is not enough on its own: re-check that it
    # neither dirtied nor silently reconfigured the locked source tree.
    require_locked_build(source, build)
    for library in required_libraries:
        if not library.is_file():
            raise SystemExit(f"required producer library is missing: {library}")

    driver = output / "signed-branch-codegen-driver"
    generated_cpp = output / "signed-branch-codegen-generated.cpp"
    generated_program = output / "signed-branch-codegen-generated"
    driver_compile = [
        args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
        f"-I{source / 'ps2xRecomp/include'}", f"-I{source / 'ps2xRuntime/include'}",
        str(DRIVER), *(str(library) for library in required_libraries), "-pthread", "-o", str(driver),
    ]
    generated_compile = [
        args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
        str(generated_cpp), "-o", str(generated_program),
    ]
    driver_result = run(driver_compile, cwd=output)
    emit_result = run([str(driver), str(generated_cpp)], cwd=output)
    generated_compile_result = run(generated_compile, cwd=output)
    execute_result = run([str(generated_program)], cwd=output)
    source_contract = run([sys.executable, str(ROOT / "tests/test_signed_branch_codegen.py"),
                           "--source", str(source)], cwd=output)

    receipt = {
        "schema": 1,
        "source": {
            "path": str(source),
            "commit": git(source, "rev-parse", "HEAD"),
            "tree": git(source, "rev-parse", "HEAD^{tree}"),
        },
        "build": {
            "path": str(build),
            "cmake_home_directory": str(source),
            "cmake_cache_sha256": sha256(build / "CMakeCache.txt"),
            "libraries": [
                {"path": str(library), "sha256": sha256(library)}
                for library in required_libraries
            ],
        },
        "synthetic_execution": {
            "driver": str(DRIVER),
            "generated_cpp": {"path": str(generated_cpp), "sha256": sha256(generated_cpp)},
            "forms": 12,
            "values_per_form": 8,
            "cases": 96,
            "exit_code": execute_result.returncode,
        },
        "commands": {
            "producer_build": producer_build_command,
            "driver_compile": driver_compile,
            "emit": [str(driver), str(generated_cpp)],
            "generated_compile": generated_compile,
            "execute": [str(generated_program)],
            "source_contract": [sys.executable, str(ROOT / "tests/test_signed_branch_codegen.py"),
                                "--source", str(source)],
        },
        "outputs": {
            "producer_build": producer_build_result.stdout,
            "driver_compile": driver_result.stdout,
            "emit": emit_result.stdout,
            "generated_compile": generated_compile_result.stdout,
            "execute": execute_result.stdout,
            "source_contract": source_contract.stdout,
        },
    }
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
