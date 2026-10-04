#!/usr/bin/env python3
"""Keep the fake feedback regression in an isolated ADR-0006 build root."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def call(*args: str) -> None:
    subprocess.run(args, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root.resolve()
    build = args.build_root.resolve()
    call("cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
         "-DRRV_BUILD_PRODUCT=OFF")
    call("cmake", "--build", str(build), "--target",
         "rrv-test-fake-pcsx2-gs-bridge", "rrv-gs-backend-tests")
    bridge = build / "librrv-test-fake-pcsx2-gs-bridge.dylib"
    if not bridge.is_file():
        raise SystemExit("isolated feedback fixture did not build its fake bridge")
    control = build / "CMakeFiles" / "rrv-resource-package"
    control.mkdir(parents=True, exist_ok=True)
    package_input = control / "feedback-input.json"
    package_input.write_text(json.dumps({
        "schema": "rrv-resource-package-input-v1",
        "bridge": bridge.name,
        "entries": [{"path": bridge.name, "source": str(bridge)}],
        "provenance": {"fixture": "checked-in fake feedback bridge"},
    }, sort_keys=True) + "\n", encoding="ascii")
    spec = control / "spec.json"
    call(sys.executable, "-B", str(source / "scripts" / "resource_package.py"), "configure",
         "--input", str(package_input), "--build-root", str(build), "--spec", str(spec))
    expected = hashlib.sha256(spec.read_bytes()).hexdigest()
    call(sys.executable, "-B", "-c",
         "import pathlib, sys; sys.path.insert(0, sys.argv[3]); import resource_package; resource_package.ensure_configured(pathlib.Path(sys.argv[1]), sys.argv[2])",
         str(build), expected, str(source / "scripts"))
    package = build / ".rrv-resource-packages" / "current" / bridge.name
    if not package.is_file():
        raise SystemExit("isolated feedback package was not published")
    call("ctest", "--test-dir", str(build), "--output-on-failure", "-R", "^rrv-gs-backend$")
    print(f"ADR0006_FEEDBACK_FIXTURE build_root={build}")
    print(f"ADR0006_FEEDBACK_FIXTURE package_bridge={package}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
