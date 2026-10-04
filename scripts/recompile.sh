#!/usr/bin/env bash
# recompile.sh — Wrap ps2_recomp; guard for missing ELF and TOML.
# Usage: ./scripts/recompile.sh [toml_path]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

TOML="${1:-config/rrv.toml}"
ELF="local/rrv_boot.elf"

# ── Locate the recompiler binary ────────────────────────────────────────────
find_binary() {
    for candidate in \
        "$REPO_ROOT/tools/PS2Recomp/build-arm64/ps2xRecomp/ps2_recomp" \
        "$REPO_ROOT/build-tools/ps2xRecomp/ps2_recomp" \
        "$(command -v ps2_recomp 2>/dev/null || true)"; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

RECOMP="$(find_binary)" || {
    echo ""
    echo "ERROR: ps2_recomp binary not found."
    echo "  Build it first (same cmake invocation as analyze.sh)."
    echo ""
    exit 1
}

# ── Guards ──────────────────────────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo ""
    echo "ERROR: local/rrv_boot.elf not found."
    echo "  The recompiler needs the original ELF at runtime."
    echo "  See README.md for extraction instructions."
    echo ""
    exit 1
fi

if [[ ! -f "$TOML" ]]; then
    echo ""
    echo "ERROR: TOML config not found at: $TOML"
    echo "  Run analyze.sh first to generate it."
    echo ""
    exit 1
fi

echo "=== RRV Recompiler ==="
echo "  TOML     : $TOML"
echo "  Recomp   : $RECOMP"
echo ""

"$RECOMP" "$TOML"

# Re-apply diagnostics that intentionally live outside derivative generated C++.
python3 "$REPO_ROOT/scripts/apply_generated_probes.py" "$REPO_ROOT/config/output"

echo ""
echo "Done. Generated C++ is in: config/output/"
echo "NOTE: config/output/ is gitignored — do not commit it."
