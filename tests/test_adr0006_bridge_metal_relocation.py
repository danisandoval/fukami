#!/usr/bin/env python3
"""Run the real package bridge smoke after copying it to a distinct B root."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", type=pathlib.Path, required=True)
    parser.add_argument("--executable", type=pathlib.Path, required=True)
    parser.add_argument("--raw-log", type=pathlib.Path, required=True)
    args = parser.parse_args()
    build_root = args.build_root.resolve()
    executable = args.executable.resolve()
    if executable.parent != build_root / "bin":
        raise SystemExit("smoke executable must be laid out at B/bin/<consumer>")
    package = build_root / ".rrv-resource-packages" / "current"
    lock = build_root / ".rrv-resource-package.lock"
    if not package.is_dir() or not lock.is_file():
        raise SystemExit("source B does not contain a published canonical package and lock")
    with tempfile.TemporaryDirectory(prefix="rrv-adr0006-metal-relocation-") as temporary:
        relocated = pathlib.Path(temporary) / "B"
        relocated_bin = relocated / "bin"
        relocated_package = relocated / ".rrv-resource-packages" / "current"
        relocated_bin.mkdir(parents=True)
        relocated_package.parent.mkdir(parents=True)
        shutil.copy2(executable, relocated_bin / executable.name)
        shutil.copy2(lock, relocated / lock.name)
        shutil.copytree(package, relocated_package, symlinks=False)
        completed = subprocess.run(
            [str(relocated_bin / executable.name)], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False)
        args.raw_log.parent.mkdir(parents=True, exist_ok=True)
        args.raw_log.write_text(completed.stdout, encoding="utf-8")
        print(completed.stdout, end="")
        if completed.returncode:
            raise SystemExit(completed.returncode)
        expected_root = str(relocated_package.resolve()) + "/"
        package_line = next((line for line in completed.stdout.splitlines()
                             if line.startswith("ADR0006_METAL_SMOKE package_bridge=")), "")
        loaded_line = next((line for line in completed.stdout.splitlines()
                            if line.startswith("ADR0006_METAL_SMOKE dladdr_image=")), "")
        metal_line = next((line for line in completed.stdout.splitlines()
                           if line.startswith("RRV: selected packaged Metal library ")), "")
        if not package_line.removeprefix("ADR0006_METAL_SMOKE package_bridge=").strip('"').startswith(expected_root):
            raise SystemExit("relocated smoke did not report the relocated package bridge")
        if not loaded_line.removeprefix("ADR0006_METAL_SMOKE dladdr_image=").strip('"').startswith(expected_root):
            raise SystemExit("relocated smoke did not report the relocated dladdr image")
        selected_metal = metal_line.removeprefix("RRV: selected packaged Metal library ")
        if not selected_metal.startswith(expected_root + "resources/") or not selected_metal.endswith(".metallib"):
            raise SystemExit("relocated smoke did not report a successfully selected packaged Metal library")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
