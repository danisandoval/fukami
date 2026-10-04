#!/usr/bin/env bash
# analyze.sh — Wrap ps2_analyzer; guard for missing ELF.
# Usage: ./scripts/analyze.sh [elf_path] [toml_out]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

ELF="${1:-local/rrv_boot.elf}"
TOML="${2:-config/rrv.toml}"
GENERATED="config/output"

# ── Locate the analyzer binary ─────────────────────────────────────────────
find_binary() {
    for candidate in \
        "$REPO_ROOT/tools/PS2Recomp/build-arm64/ps2xAnalyzer/ps2_analyzer" \
        "$REPO_ROOT/build-tools/ps2xAnalyzer/ps2_analyzer" \
        "$(command -v ps2_analyzer 2>/dev/null || true)"; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

ANALYZER="$(find_binary)" || {
    echo ""
    echo "ERROR: ps2_analyzer binary not found."
    echo "  Build it first:"
    echo "    cmake -B tools/PS2Recomp/build-arm64 -S tools/PS2Recomp \\"
    echo "      -DCMAKE_BUILD_TYPE=Release \\"
    echo "      -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF"
    echo "    cmake --build tools/PS2Recomp/build-arm64 --parallel"
    echo ""
    exit 1
}

# ── Guard for user ELF ─────────────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo ""
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║  RRV boot ELF not found at:                                     ║"
    echo "║    $ELF"
    echo "║                                                                  ║"
    echo "║  You must supply your own legally-extracted copy:               ║"
    echo "║    1. Mount / extract your Ridge Racer V disc image.            ║"
    echo "║    2. Copy the boot ELF to:  local/rrv_boot.elf                 ║"
    echo "║       (NTSC USA: SLUS_200.02  |  PAL: SCES_500.00)             ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"
    echo ""
    exit 1
fi

# ── Prepare output dirs ────────────────────────────────────────────────────
mkdir -p "$(dirname "$TOML")" "$GENERATED"

echo "=== RRV ELF Analyzer ==="
echo "  ELF      : $ELF"
echo "  TOML out : $TOML"
echo "  Analyzer : $ANALYZER"
echo ""

"$ANALYZER" "$ELF" "$TOML"

echo ""
echo "Done. Review $TOML before running recompile.sh."
echo "The 'stubs' list = HLE backlog for Phase 2."
