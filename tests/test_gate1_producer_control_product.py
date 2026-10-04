#!/usr/bin/env python3
"""Compare source-bound actual rrv-product Gate-1 producer-control receipts."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


PREFIXES_TO_CLEAR = ("RRV_", "DYLD_", "PCSX2", "SDL_", "METAL_", "CMAKE_", "FETCHCONTENT_")
RECEIPT_PREFIX = "GATE1_PRODUCER_CONTROL_RECEIPT="
SCENARIOS = ("signal", "vif-finish", "field", "cleanup")
MODES = ("inline", "worker-sync")
# CSR.FIELD (bit 13) follows the free-running host VSync worker, so in the
# signal and vif-finish rows it depends on scheduling (known issue TIMING-003).
# Their CSR values are compared without it; the field row checks FIELD
# against the serviced field at the VBlank-start handler.
CSR_FIELD = 1 << 13
CSR_KEYS = ("first_signal_csr", "second_signal_csr", "finish_csr", "final_csr")


def control_value(key: str, value):
    return value & ~CSR_FIELD if key in CSR_KEYS and isinstance(value, int) else value
REQUIRED = {
    "signal": {"signals": 2, "signal_acks": 2, "irq_dispatches": 2,
               "first_signal_csr": 1, "second_signal_csr": 1,
               "final_siglblid": (3 << 32) | 2, "final_csr": 0},
    "vif-finish": {"finishes": 1, "finish_deliveries": 1, "finish_acks": 1,
                    "irq_dispatches": 3, "vif_input_bytes": 64,
                    "finish_csr": 2, "final_siglblid": (4 << 32) | 2, "final_csr": 0},
    "field": {"guest_field_flag": 1, "field_advanced": 1, "field_tick_matches": 1,
              "field_parity_matches": 1},
    "cleanup": {"cleanup_positive_control": 1, "cleanup_normal_removed": 1,
                "cleanup_early_exit_removed": 1},
}
# Scheduling-dependent values (host VSync ticks) are reported under this key
# and never take part in the inline/worker-sync equivalence.
DIAGNOSTIC_KEY = "diagnostic"


def environment(mode: str) -> dict[str, str]:
    result = {key: value for key, value in os.environ.items() if not key.startswith(PREFIXES_TO_CLEAR)}
    result.update(
        {
            "RRV_GS_BACKEND": "pcsx2",
            "RRV_GS_RENDER_MODE": "field",
            "RRV_GS_EXECUTION": mode,
            "RRV_GS_CONTROL": "producer",
        }
    )
    return result


def run(product: Path, scenario: str, mode: str) -> dict:
    completed = subprocess.run(
        [str(product), f"--gate1-producer-control={scenario}"],
        cwd=product.parent,
        env=environment(mode),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False, timeout=90,
    )
    print(f"--- rrv-product scenario={scenario} mode={mode} exit={completed.returncode} ---")
    print(completed.stdout, end="")
    if completed.returncode != 0:
        raise RuntimeError(f"rrv-product {mode} exited {completed.returncode}")
    receipts = [line[len(RECEIPT_PREFIX):] for line in completed.stdout.splitlines()
                if line.startswith(RECEIPT_PREFIX)]
    if len(receipts) != 1:
        raise RuntimeError(f"rrv-product {mode} emitted {len(receipts)} producer-control receipts")
    receipt = json.loads(receipts[0])
    if receipt.get("mode") != mode or receipt.get("scenario") != scenario:
        raise RuntimeError(f"rrv-product receipt selector mismatch: {receipt!r}")
    for key, expected in REQUIRED[scenario].items():
        if control_value(key, receipt.get(key)) != expected:
            raise RuntimeError(f"rrv-product {mode} receipt {key}={receipt.get(key)!r}, expected {expected!r}")
    if scenario == "signal" and (not isinstance(receipt.get("irq_requests"), int) or receipt["irq_requests"] < 2):
        raise RuntimeError(f"rrv-product {mode} has no nonempty IRQ request receipt")
    return receipt


def logical(receipt: dict) -> dict:
    """The receipt without its mode, scheduling-dependent diagnostics and CSR.FIELD."""
    return {key: control_value(key, value) for key, value in receipt.items()
            if key not in ("mode", DIAGNOSTIC_KEY)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--product", required=True, type=Path)
    args = parser.parse_args()
    product = args.product.resolve()
    if not product.is_file():
        raise RuntimeError(f"rrv-product is absent: {product}")
    matrix, failed = {}, False
    for scenario in SCENARIOS:
        matrix[scenario] = {}
        for mode in MODES:
            try:
                matrix[scenario][mode] = {"status": "PASS", "receipt": run(product, scenario, mode)}
            except Exception as error:
                print(f"MATRIX scenario={scenario} mode={mode} status=FAIL cause={error}")
                matrix[scenario][mode] = {"status": "FAIL", "cause": str(error)}
                failed = True
        rows = matrix[scenario]
        if all(rows[m]["status"] == "PASS" for m in MODES):
            left, right = (logical(rows[m]["receipt"]) for m in MODES)
            if left != right:
                print(f"MATRIX scenario={scenario} equivalence=FAIL")
                failed = True
            else:
                print(f"MATRIX scenario={scenario} equivalence=PASS")
        else:
            print(f"MATRIX scenario={scenario} equivalence=FAIL missing-valid-mode")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
