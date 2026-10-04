#!/usr/bin/env bash
# pcsx2_ref.sh — regenerate PCSX2 ground-truth frames for a GS dump, headless,
# at NATIVE internal resolution so they compare to our 640x224 output with no
# resampling error.
#
# Why native matters (measured 2026-08-04): PCSX2's default dump is the
# presented 640x480 (active rows 49..436), which forces a 388 -> 180 vertical
# rescale to compare against our buffer. That rescale alone puts a floor of
# ~13-17 mean abs error between our software renderer and PCSX2 — larger than
# the Metal defect we are chasing, so frame-level comparison could not resolve
# it. Setting GSScreenshotSize = InternalResolutionUncorrected (enum value 2,
# pcsx2/Config.h) dumps 640x448 = exactly 2x our 640x224, so the comparison is
# an integer row decimation with ZERO resampling error.
#
# With that alignment the numbers become usable (frame 0, rows 22..201, the
# displayed picture):
#     RRV software renderer vs PCSX2 : 7.69   <- the floor; our SW oracle is
#                                               VALIDATED for interior draws
#     Metal                 vs PCSX2 : 17.03  <- 2.2x the floor, a real defect
# Over the full 0..223 the ranking inverts (RRV 22.25, Metal 14.48) because the
# RRV software renderer fills the cinematic-bar rows that PCSX2 does not.
#
# Reference: the clean PCSX2 checkout pinned in config/dependencies.lock.toml.
# The checkout is never modified. A direct gsrunner path is allowed only with
# an explicit matching source checkout, so its provenance remains checkable.
#
# Usage:  scripts/pcsx2_ref.sh <dump.gs> <outdir> [renderer]
#   renderer defaults to "sw" (PCSX2's most accurate path).
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LOCK_FILE="$ROOT_DIR/config/dependencies.lock.toml"
PCSX2_SRC="${PCSX2_SRC:-$ROOT_DIR/build-deps/pcsx2-2.8.2}"
LOCKED_REVISION=$(awk '
    $0 == "[pcsx2]" { in_pcsx2 = 1; next }
    in_pcsx2 && /^revision = / { gsub(/"/, "", $3); print $3; exit }
' "$LOCK_FILE")
RUNNER="${PCSX2_GSRUNNER:-$PCSX2_SRC/build/pcsx2-gsrunner/pcsx2-gsrunner}"

DUMP="${1:?usage: pcsx2_ref.sh <dump.gs> <outdir> [renderer]}"
OUTDIR="${2:?usage: pcsx2_ref.sh <dump.gs> <outdir> [renderer]}"
RENDERER="${3:-sw}"

[ -n "$LOCKED_REVISION" ] || { echo "locked PCSX2 revision missing from $LOCK_FILE" >&2; exit 1; }
[ -d "$PCSX2_SRC/.git" ] || { echo "PCSX2 source checkout missing: $PCSX2_SRC" >&2; exit 1; }
[ "$(git -C "$PCSX2_SRC" rev-parse HEAD)" = "$LOCKED_REVISION" ] || {
    echo "PCSX2 source is not locked revision $LOCKED_REVISION: $PCSX2_SRC" >&2; exit 1; }
[ -z "$(git -C "$PCSX2_SRC" status --porcelain=v1)" ] || {
    echo "PCSX2 source checkout has local edits: $PCSX2_SRC" >&2; exit 1; }
[ -x "$RUNNER" ] || { echo "pcsx2-gsrunner not found at $RUNNER; set PCSX2_GSRUNNER after building the locked source" >&2; exit 1; }
[ -f "$DUMP" ]   || { echo "GS dump not found: $DUMP" >&2; exit 1; }

INI_DIR="$(mktemp -d)"
# GSScreenshotSize = 2 == InternalResolutionUncorrected
printf '[EmuCore/GS]\nScreenshotSize = 2\n' > "$INI_DIR/PCSX2.ini"

mkdir -p "$OUTDIR"
"$RUNNER" -surfaceless -renderer "$RENDERER" -ini "$INI_DIR/PCSX2.ini" \
          -dumpdir "$OUTDIR" -loop 1 "$DUMP" >/dev/null 2>&1 || true
rm -rf "$INI_DIR"

n=$(ls -1 "$OUTDIR"/*.png 2>/dev/null | wc -l | tr -d ' ')
echo "wrote $n frame(s) to $OUTDIR (renderer=$RENDERER, native internal resolution)"
[ "$n" -gt 0 ] || { echo "no frames produced" >&2; exit 1; }

# Row mapping for consumers: PCSX2 row (2*y + parity) <-> our row y.
# Parity 1 measured marginally better on frame 0 (7.69 vs 8.76); either is
# within noise of the other, so pick one and stay consistent.
echo "row mapping: pcsx2_row = 2*our_row + 1   (640x448 -> 640x224)"
echo "displayed picture = our rows 22..201; rows 0-21 and 202-223 are cinematic bars"
