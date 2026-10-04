#!/usr/bin/env python3
"""Verify the G1 control overlay receipt binds its result and StoreImage inputs."""

import argparse
import hashlib
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--producer-gs", type=Path, required=True)
    args = parser.parse_args()
    receipt = json.loads(args.manifest.read_text(encoding="utf-8"))
    expected_path = "src/gs-control/rrv_gs_result_boundary.h"
    expected_hash = hashlib.sha256(args.header.read_bytes()).hexdigest()
    matches = [entry for entry in receipt.get("transforms", [])
               if entry.get("path") == expected_path]
    if len(matches) != 1 or matches[0].get("sha256") != expected_hash:
        raise SystemExit("GS control receipt does not bind the result-boundary header hash")
    expected_gs = "src/lib/Kernel/Stubs/GS.cpp"
    producer_hash = hashlib.sha256(args.producer_gs.read_bytes()).hexdigest()
    copies = [entry for entry in receipt.get("files", []) if entry.get("path") == expected_gs]
    if len(copies) != 1 or copies[0].get("producer_sha256") != producer_hash:
        raise SystemExit("GS control receipt does not bind the selected StoreImage producer source")
    overlay = args.manifest.parent / expected_gs
    if (not overlay.is_file() or
            copies[0].get("overlay_sha256") != hashlib.sha256(overlay.read_bytes()).hexdigest()):
        raise SystemExit("GS control receipt does not bind the copied StoreImage overlay output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
