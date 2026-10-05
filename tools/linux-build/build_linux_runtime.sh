#!/usr/bin/env bash
# Build and package a Linux x86-64 (Steam Deck) product runtime from one
# commit: the Linux twin of scripts/build_fukami_runtime.sh.
#
# Run on the Mac host; every step runs in the rrv-linux-build:1 container
# with the repository mounted at the same absolute path (absolute symlinks
# keep resolving):
#   tools/linux-build/build_linux_runtime.sh COMMIT NAME
# e.g. NAME = game001-gate5-v145-1234567-linux-x86_64
#
# Source-owned build (like the macOS recipe): the runtime, the override layer and the game code are
# compiled straight from the committed third_party/ps2recomp, src/product and generated/rr5 of COMMIT.
# No stage directory, no overlay, no local/gate8 generation.
#
# Steps: private clone at COMMIT (build/fukami/linux-root-COMMIT; the bridge manifest binds the bridge to a
# clean root and its HEAD) -> bridge (Vulkan; ~25 min under Rosetta) -> links -> product
# (scripts/build_product_runtime.py; no sse2neon on x86-64) -> runtime/NAME.
# Remaining external input: PCSX2 2.8.2 (build-deps). Outputs hold generated code: ignored folders only.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
(( $# == 2 )) || { echo "usage: $0 COMMIT NAME" >&2; exit 2; }
commit=$(git -C "$repo" rev-parse --short=7 "$1")
name=$2
# Everything lands in one ignored directory, build/fukami/ (FUKAMI_BUILD_DIR overrides it): linux-root-COMMIT (a
# clean clone), linux-bridge-COMMIT and beside it linux-bridge-COMMIT-source-* (the patched PCSX2 source the
# bridge manifest records). Nothing in it is shipped.
work="${FUKAMI_BUILD_DIR:-$repo/build/fukami}"
mkdir -p "$work"
root="$work/linux-root-$commit"
bridge="$work/linux-bridge-$commit"
build="$root/build-product-linux-x86_64-$commit"
[[ -d "$repo/runtime/$name" ]] && { echo "runtime/$name already exists" >&2; exit 1; }

in_box() {
    docker run --rm --platform linux/amd64 -v "$repo":"$repo" -w "$1" -e HOME=/tmp/rrv-home \
        -e LDFLAGS=-fuse-ld=lld rrv-linux-build:1 \
        bash -c 'mkdir -p "$HOME/.config"; git config --global --add safe.directory "*"; '"$2"
}

echo "== clone $root"
if [[ ! -d "$root" ]]; then
    git clone -q --no-local --no-checkout "$repo" "$root"
    git -C "$root" checkout -q --detach "$commit"
fi

echo "== bridge $bridge (about 25 minutes)"
if [[ ! -f "$bridge/rrv-pcsx2-gs-bridge-manifest.json" ]]; then
    rm -rf "$bridge" "$bridge"-source-*
    # RRV_BRIDGE_ARCH_FLAGS (optional) reaches the bridge build inside the box: see scripts/build_pcsx2_gs_bridge.sh.
    arch_env=
    [[ -n "${RRV_BRIDGE_ARCH_FLAGS+x}" ]] && arch_env="RRV_BRIDGE_ARCH_FLAGS='$RRV_BRIDGE_ARCH_FLAGS' "
    in_box "$root" "${arch_env}CMAKE_BUILD_TYPE=Release PCSX2_GS_BRIDGE_BUILD_DIR='$bridge' PCSX2_SOURCE_DIR='$repo/build-deps/pcsx2-2.8.2' scripts/build_pcsx2_gs_bridge.sh"
fi

echo "== links"
# The bridge manifest refuses a dirty root, so these links come after it.
# `generated` is linked only where it is not tracked (a public checkout keeps it ignored, scripts/fukami_generate.py).
for link in build-deps runtime-deps generated; do
    [[ -e "$root/$link" ]] || ln -s "$repo/$link" "$root/$link"
done
for src in "$bridge" "$bridge"-source-*; do
    [[ -e "$src" && ! -e "$root/$(basename "$src")" ]] && ln -s "$src" "$root/$(basename "$src")"
done

echo "== product $build"
in_box "$root" "python3 scripts/build_product_runtime.py \
    --build-root '$build' --cmake-source-root '$root' \
    --bridge-manifest '$bridge/rrv-pcsx2-gs-bridge-manifest.json' \
    --pcsx2-spu2-source '$repo/build-deps/pcsx2-2.8.2' --parallel 11"

echo "== package runtime/$name"
in_box "$repo" "python3 scripts/package_gate4_product.py --build '$build' --name '$name' && \
    python3 scripts/package_gate4_product.py --verify --name '$name'"
echo "== done: runtime/$name"
