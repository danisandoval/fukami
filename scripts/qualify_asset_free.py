#!/usr/bin/env python3
"""Run the declared portable asset-free CTest suite fail-closed."""
import argparse
import json
import subprocess
import sys
import re
import xml.etree.ElementTree as ET
from pathlib import Path


def registration(build_dir):
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=False, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    return json.loads(result.stdout).get("tests", [])


def validate_registration(tests, manifest):
    required = manifest.get("required", [])
    dependent = manifest.get("input_dependent", [])
    # Tests registered only by a native configure (for example the macOS ARM64
    # G1-H1E contract). Classified and label-checked when present; never run
    # or required by the portable suite.
    native = manifest.get("platform_native", [])
    if not isinstance(required, list) or not isinstance(dependent, list) or not isinstance(native, list):
        raise RuntimeError("manifest required/input_dependent/platform_native must be lists")
    if not all(isinstance(entry, dict) and isinstance(entry.get("name"), str)
               for entry in required + dependent + native):
        raise RuntimeError("manifest entries must be objects with string names")
    if not all(isinstance(entry.get("label"), str) and entry["label"] and
               entry["label"] != "portable-asset-free" for entry in native):
        raise RuntimeError("platform_native entries need a non-portable label")
    required_names = [entry["name"] for entry in required]
    dependent_names = [entry["name"] for entry in dependent]
    native_names = [entry["name"] for entry in native]
    all_names = required_names + dependent_names + native_names
    if (not all(isinstance(name, str) and name for name in all_names) or
            len(all_names) != len(set(all_names))):
        raise RuntimeError("manifest contains malformed, duplicate, or overlapping names")
    if manifest.get("label") != "portable-asset-free":
        raise RuntimeError("manifest label must be portable-asset-free")
    classified = set(all_names)
    registered = [test["name"] for test in tests]
    missing = [name for name in required_names if name not in registered]
    unexpected = [name for name in registered if name not in classified]
    if missing or unexpected or not required_names:
        raise RuntimeError(f"registration mismatch: missing={missing} unexpected={unexpected}")
    by_name = {test["name"]: test for test in tests}
    def labels(test):
        for prop in test.get("properties", []):
            if prop["name"] == "LABELS":
                value = prop["value"]
                return set(value if isinstance(value, list) else [value])
        return set()
    for entry in required:
        test = by_name[entry["name"]]
        test_labels = labels(test)
        if test_labels != {"portable-asset-free"}:
            raise RuntimeError(f"label mismatch for {entry['name']}: {test_labels}")
        target = entry.get("target")
        if not target:
            continue
        command = test.get("command", [])
        if not command or not Path(command[0]).exists():
            raise RuntimeError(f"required executable absent for {entry['name']}: {command}")
    for entry in dependent:
        test_labels = labels(by_name[entry["name"]])
        if test_labels != {entry.get("label", "input-dependent")}:
            raise RuntimeError(f"input-dependent label mismatch for {entry['name']}: {test_labels}")
    for entry in native:
        if entry["name"] in by_name and labels(by_name[entry["name"]]) != {entry["label"]}:
            raise RuntimeError(f"platform-native label mismatch for {entry['name']}: "
                               f"{labels(by_name[entry['name']])}")
    return required_names


def validate_junit(path, required_names):
    root = ET.parse(path).getroot()
    cases = {case.attrib.get("name"): case for case in root.iter("testcase")}
    missing = [name for name in required_names if name not in cases]
    skipped = [name for name in required_names if name in cases and cases[name].find("skipped") is not None]
    failed = [name for name in required_names if name in cases and
              (cases[name].find("failure") is not None or cases[name].find("error") is not None)]
    if missing or skipped or failed:
        raise RuntimeError(f"result mismatch: missing={missing} skipped={skipped} failed={failed}")


def validate_unittest_output(output):
    match = re.search(r"OK \(skipped=([1-9][0-9]*)\)", output)
    if match:
        raise RuntimeError(f"inner unittest reported skipped={match.group(1)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("asset_free_suite.json"))
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    required = validate_registration(registration(args.build_dir), manifest)
    print(f"suite={manifest['label']} required={len(required)} input-dependent={len(manifest['input_dependent'])}")
    if args.check_only:
        return 0
    junit = args.build_dir.resolve() / "asset-free-qualification.xml"
    if junit.exists():
        junit.unlink()
    result = subprocess.run([
        "ctest", "--test-dir", str(args.build_dir), "--output-on-failure",
        "--no-tests=error", "--output-junit", str(junit),
        "-R", "^(" + "|".join(re.escape(name) for name in required) + ")$"],
        check=False, capture_output=True, text=True)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        return result.returncode
    validate_unittest_output(result.stdout + result.stderr)
    if not junit.exists():
        raise RuntimeError("fresh JUnit result is absent")
    validate_junit(junit, required)
    print(f"PASS: {len(required)}/{len(required)} required tests; SKIP=0 FAIL=0")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, json.JSONDecodeError, ET.ParseError) as exc:
        print(f"asset-free qualification: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
