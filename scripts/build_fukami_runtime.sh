#!/bin/zsh -f
# Build and package a product runtime from one commit, following the
# candidate rebuild recipe (clean worktree -> bridge -> candidate -> package).
#
# Usage: scripts/build_fukami_runtime.sh COMMIT NAME [VERSION_TAG]
#   COMMIT  a committed revision (the worktree is made from it; dirty trees
#           are never built)
#   NAME    runtime package name, e.g. game001-gate9-v131-91b6b68
#   VERSION_TAG  build dir tag, default: the vNNN part of NAME
#
# Source-owned build (RRV_SOURCE_OWNERSHIP_PLAN.md Phase 3): the runtime, the override layer and
# the game code are compiled straight from the committed third_party/ps2recomp, src/product and
# generated/rr5 of COMMIT. No stage directory, no overlay, no local/gate8 generation.
# Remaining external inputs: PCSX2 2.8.2 (build-deps), the locked sse2neon checkout
# (build-deps/sse2neon-*; prepare_dependencies.py --prepare --component sse2neon), the Metal
# resources and SDL (runtime-deps). Everything lands in the ignored build/fukami/ and runtime/ folders
# (it holds generated code; never commit it).
set -eu
repo="${0:A:h:h}"
(( $# >= 2 )) || { print -u2 "usage: $0 COMMIT NAME [VERSION_TAG]"; exit 2; }
commit="$(git -C "$repo" rev-parse --short=7 "$1")"
name="$2"
tag="${3:-${${name#*-gate*-}%%-*}}"
# Everything this script builds lands in ONE ignored directory, build/fukami/ (FUKAMI_BUILD_DIR overrides it):
#   metal/             PCSX2's Metal shader libraries, built once from the pinned PCSX2 source
#   root-COMMIT/       a clean git worktree of COMMIT (the bridge manifest binds the bridge to a clean root)
#   bridge-TAG-COMMIT/ the PCSX2 GS bridge build, and beside it bridge-TAG-COMMIT-source-*: the patched PCSX2
#                      source the bridge manifest records and later builds re-verify (keep it)
# build-deps/ and runtime-deps/ (the pinned dependency checkouts) stay at the top level. None of it is shipped:
# the app and the Linux package take only the finished libraries from runtime/NAME.
work="${FUKAMI_BUILD_DIR:-$repo/build/fukami}"
mkdir -p "$work"
# FUKAMI_METAL_RESOURCES points at an existing Metal-resources directory; the owner's older hash-named one is reused.
metal="${FUKAMI_METAL_RESOURCES:-}"
if [[ -z "$metal" ]]; then
    if [[ -d "$repo/build-gate6-metal-resources-a5d0088d6921" ]]; then
        metal="$repo/build-gate6-metal-resources-a5d0088d6921"
    else
        metal="$work/metal"
    fi
fi
pcsx2="$repo/build-deps/pcsx2-2.8.2"
root="$work/root-$commit"
bridge="$work/bridge-$tag-$commit"
build="$root/build-product-$tag-$commit"
[[ -d "$repo/runtime/$name" ]] && { print -u2 "runtime/$name already exists"; exit 1; }

if [[ ! -f "$metal/metal-resources-manifest.toml" ]]; then
    print "== Metal resources $metal (needs the Xcode Metal toolchain)"
    python3 "$repo/scripts/build_pcsx2_metal_resources.py" --source "$pcsx2" --output "$metal"
fi

print "== worktree $root"
[[ -d "$root" ]] || git -C "$repo" worktree add --detach "$root" "$commit"

print "== bridge $bridge (about 10 minutes)"
if [[ ! -f "$bridge/rrv-pcsx2-gs-bridge-manifest.json" ]]; then
    rm -rf "$bridge" "$bridge"-source-*(N)
    (cd "$root" && CMAKE_BUILD_TYPE=Release PCSX2_GS_BRIDGE_BUILD_DIR="$bridge" PCSX2_SOURCE_DIR="$pcsx2" \
        PCSX2_METAL_RESOURCE_DIR="$metal" \
        PCSX2_METAL_RESOURCE_MANIFEST="$metal/metal-resources-manifest.toml" \
        scripts/build_pcsx2_gs_bridge.sh)
fi

print "== links"
# The bridge manifest refuses a dirty root, so these links come after it. `generated` is a link only where it is
# not tracked: a public checkout keeps the locally generated game code (scripts/fukami_generate.py) in an ignored
# directory the clean worktree does not have.
for link in build-deps runtime-deps generated; do
    [[ -e "$root/$link" ]] || ln -s "$repo/$link" "$root/$link"
done
for src in "$metal" "$bridge" "$bridge"-source-*(N); do
    [[ -e "$root/${src:t}" ]] || ln -s "$src" "$root/${src:t}"
done

print "== product $build"
(cd "$root" && python3 scripts/build_product_runtime.py \
    --build-root "$build" --cmake-source-root "$root" \
    --bridge-manifest "$bridge/rrv-pcsx2-gs-bridge-manifest.json" \
    --pcsx2-spu2-source "$pcsx2" --parallel "${FUKAMI_JOBS:-8}")

print "== package runtime/$name"
python3 "$repo/scripts/package_gate4_product.py" --build "$build" --name "$name"
python3 "$repo/scripts/package_gate4_product.py" --verify --name "$name"
print "== done: runtime/$name"
