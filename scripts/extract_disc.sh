#!/usr/bin/env bash
# extract_disc.sh — Extract RRV disc files from CHD to local/.
# Requirements: chdman (brew install mame), 7z (brew install p7zip), python3
# Usage: ./scripts/extract_disc.sh [path/to/disc.chd]
#
# Produces local/R5.ALL, local/SYSTEM.CNF, local/*.IRX — everything the
# runtime needs to boot. Skips R5.STR (FMV stream, ~160MB, not needed yet).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHD="${1:-$REPO_ROOT/rom/Ridge Racer V (USA).chd}"
WORK_DIR="$(mktemp -d)"
LOCAL_DIR="$REPO_ROOT/local"

trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== RRV Disc Extractor ==="
echo "  CHD   : $CHD"
echo "  Output: $LOCAL_DIR"
echo ""

# ── Sanity checks ──────────────────────────────────────────────────────────
[[ -f "$CHD" ]] || { echo "ERROR: CHD not found at $CHD"; exit 1; }
command -v chdman &>/dev/null || { echo "ERROR: chdman not found. brew install mame"; exit 1; }
command -v 7z    &>/dev/null || { echo "ERROR: 7z not found. brew install p7zip"; exit 1; }

# ── Step 1: CHD → BIN (MODE2/2352) ───────────────────────────────────────
BIN="$WORK_DIR/rrv.bin"
CUE="$WORK_DIR/rrv.cue"
echo "=== Extracting CHD → BIN (this may take ~30s) ==="
chdman extractcd -i "$CHD" -o "$CUE" -ob "$BIN"
echo "  BIN: $(du -sh "$BIN" | cut -f1)"

# ── Step 2: MODE2/2352 → raw ISO (strip 2352-byte sector headers) ─────────
ISO="$WORK_DIR/rrv.iso"
echo "=== Stripping MODE2 headers → raw ISO ==="
python3 - <<'PY' "$BIN" "$ISO"
import sys
BIN, ISO = sys.argv[1], sys.argv[2]
SECTOR, OFFSET, DATA = 2352, 24, 2048
n = 0
with open(BIN, 'rb') as f, open(ISO, 'wb') as g:
    while True:
        s = f.read(SECTOR)
        if len(s) < SECTOR: break
        g.write(s[OFFSET:OFFSET+DATA])
        n += 1
print(f"  {n} sectors → {n*DATA//1024//1024} MB")
PY

# ── Step 3: Extract game files → local/ ───────────────────────────────────
mkdir -p "$LOCAL_DIR"
echo "=== Extracting disc files to local/ (skipping R5.STR) ==="
7z e "$ISO" -o"$LOCAL_DIR" -x'!R5.STR' -y | grep -E 'Extracting|Everything'

echo ""
echo "=== Done ==="
ls -lh "$LOCAL_DIR"
echo ""
echo "Now run: ./build/rrv-recomp local/rrv_boot.elf"
