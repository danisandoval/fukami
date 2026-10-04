#!/usr/bin/env bash
# Build the PCSX2 reference emulator with the ground-truth capture trigger.
#
# This is the OTHER PCSX2 build in this project. Keep them straight:
#   * the GS bridge (scripts/build_pcsx2_gs_bridge.sh) builds PCSX2's GS as a
#     dylib that our runtime feeds — that is the CONSUMER side, the oracle we
#     render through;
#   * this one builds the whole emulator, which runs the game and PRODUCES the
#     hardware reference stream we compare against.
#
# Same discipline as the bridge script: the pinned revision is cloned into a
# scratch tree and the user's dirty PCSX2 working tree is never touched.
#
# Output: $BUILD/pcsx2-qt/PCSX2.app — an x86_64 (Rosetta) build, deliberately.
# PCSX2 has no EE/VU/IOP recompilers on Apple Silicon, so an arm64 build runs
# the interpreter and would take hours to reach a mid-attract checkpoint; the
# trusted 2026-08-02 references were captured x86_64 as well.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lock_file="$root_dir/config/dependencies.lock.toml"
pcsx2_source=${PCSX2_SOURCE_DIR:-"$root_dir/build-deps/pcsx2-2.8.2"}
locked_pcsx2_revision=$(awk '
    $0 == "[pcsx2]" { in_pcsx2 = 1; next }
    in_pcsx2 && /^revision = / { gsub(/"/, "", $3); print $3; exit }
' "$lock_file")
if [[ -n "${PCSX2_REVISION:-}" && "${PCSX2_REVISION:-}" != "$locked_pcsx2_revision" ]]; then
    echo "error: PCSX2_REVISION must match the locked revision $locked_pcsx2_revision" >&2
    exit 1
fi
pcsx2_revision="$locked_pcsx2_revision"
scratch=${RRV_GT_CAPTURE_SCRATCH:-"$root_dir/build-pcsx2-gt-capture"}
deps=${PCSX2_X64_DEPS:-}
prebuilt_metal=${PCSX2_PREBUILT_METAL_DIR:-}
prebuilt_metal_manifest=${PCSX2_PREBUILT_METAL_MANIFEST:-}
patch_file="$root_dir/tools/patches/pcsx2-gt-capture.patch"
trace_patch_file="$root_dir/tools/patches/pcsx2-gt-guest-trace.patch"
vu1_patch_file="$root_dir/tools/patches/pcsx2-vu1-input-trace.patch"
watch_patch_file="$root_dir/tools/patches/pcsx2-gt-write-watch.patch"
# Opt-in fifth patch: the MCP/TCP debug server. Off by default so the standard
# GT binary keeps exactly the four probes the trusted references were built
# with; set RRV_GT_DEBUG_SERVER_BUILD=1 to include it. The server itself is
# additionally gated at runtime on RRV_GT_DEBUG_SERVER.
debug_server_patch_file="$root_dir/tools/patches/pcsx2-debug-server.patch"
build_debug_server=${RRV_GT_DEBUG_SERVER_BUILD:-0}
src="$scratch/src"
build="$scratch/build"
jobs=${JOBS:-10}

if [[ $(uname -s) != Darwin ]]; then
    echo "error: ground-truth capture builds the macOS x86_64 PCSX2 app and must run on macOS" >&2
    exit 1
fi
python3 "$root_dir/scripts/prepare_dependencies.py" --verify-patches

if [[ -z "$pcsx2_revision" ]]; then
    echo "error: could not read the PCSX2 revision from $lock_file" >&2
    exit 1
fi
if [[ ! -d "$pcsx2_source/.git" ]]; then
    echo "error: PCSX2 source checkout is unavailable: $pcsx2_source" >&2
    echo "       run: python3 scripts/prepare_dependencies.py --prepare --component pcsx2" >&2
    echo "       or set PCSX2_SOURCE_DIR to an explicit checkout containing $pcsx2_revision" >&2
    exit 1
fi

if ! git -C "$pcsx2_source" rev-parse --verify "$pcsx2_revision^{commit}" >/dev/null; then
    echo "error: pinned PCSX2 revision is unavailable in: $pcsx2_source" >&2
    exit 1
fi
if [[ -z "$deps" || ! -d "$deps" ]]; then
    echo "error: x86_64 PCSX2 dependency prefix is required" >&2
    echo "       set PCSX2_X64_DEPS to a pinned dependency prefix" >&2
    exit 1
fi

# ── Clean source tree at the pinned revision ────────────────────────────────
if [[ ! -d $src/.git ]] || [[ $(git -C "$src" rev-parse HEAD 2>/dev/null) != "$pcsx2_revision" ]]; then
    rm -rf "$src"
    mkdir -p "$scratch"
    # A shared clone reads objects from the pinned checkout but never writes to
    # its worktree or .git metadata.
    git clone --shared --no-checkout "$pcsx2_source" "$src"
    git -C "$src" checkout --detach "$pcsx2_revision"
fi
git -C "$src" checkout -- .
if [[ -n $(git -C "$src" status --porcelain=v1) ]]; then
    echo "error: reusable GT source is not clean; refusing to skip or stack patches" >&2
    exit 1
fi

# ── Patch, and prove the patch is reversible before trusting the build ──────
git -C "$src" apply --check "$patch_file"
git -C "$src" apply "$patch_file"
git -C "$src" apply --reverse --check "$patch_file"
echo "patch applied and reverse-validated: $(basename "$patch_file")"
git -C "$src" apply --check "$trace_patch_file"
git -C "$src" apply "$trace_patch_file"
git -C "$src" apply --reverse --check "$trace_patch_file"
echo "patch applied and reverse-validated: $(basename "$trace_patch_file")"
git -C "$src" apply --check "$vu1_patch_file"
git -C "$src" apply "$vu1_patch_file"
git -C "$src" apply --reverse --check "$vu1_patch_file"
echo "patch applied and reverse-validated: $(basename "$vu1_patch_file")"
git -C "$src" apply --check "$watch_patch_file"
git -C "$src" apply "$watch_patch_file"
git -C "$src" apply --reverse --check "$watch_patch_file"
echo "patch applied and reverse-validated: $(basename "$watch_patch_file")"
if [[ $build_debug_server == 1 ]]; then
    git -C "$src" apply --check "$debug_server_patch_file"
    git -C "$src" apply "$debug_server_patch_file"
    git -C "$src" apply --reverse --check "$debug_server_patch_file"
    echo "patch applied and reverse-validated: $(basename "$debug_server_patch_file")"
fi

# ── Metal shaders ───────────────────────────────────────────────────────────
# macOS 26 moved the Metal compiler into a separately downloadable Xcode
# component, and this host does not have it. Our patch changes one C++ file and
# no shader, so the pinned build-x64 tree's shader binaries are byte-correct
# here. Shim `xcrun metal`/`metallib` to copy them rather than stubbing the
# renderer out — the SW renderer is what captures use, but PCSX2 still creates a
# Metal presentation device on macOS.
shim_path=""
if ! xcrun metal -v >/dev/null 2>&1; then
    if [[ -z "$prebuilt_metal" || -z "$prebuilt_metal_manifest" ]]; then
        echo "error: no Metal toolchain and no reviewed prebuilt shader manifest" >&2
        echo "       install the toolchain (xcodebuild -downloadComponent MetalToolchain)" >&2
        echo "       or set PCSX2_PREBUILT_METAL_DIR and PCSX2_PREBUILT_METAL_MANIFEST" >&2
        echo "       no trusted Metal-resource manifest is checked in for M0; do not trust a local app automatically" >&2
        exit 1
    fi
    python3 "$root_dir/scripts/prepare_dependencies.py" \
        --verify-metal-resources "$prebuilt_metal" \
        --metal-resource-manifest "$prebuilt_metal_manifest"
    shim_path="$scratch/shim"
    mkdir -p "$shim_path"
    cat > "$shim_path/xcrun" <<EOF
#!/bin/bash
prebuilt=$prebuilt_metal
build=$build/pcsx2-qt
case "\$1" in
  metal|metallib)
    out=""; prev=""
    for a in "\$@"; do [[ \$prev == "-o" ]] && out=\$a; prev=\$a; done
    [[ -z \$out ]] && exit 1
    rel=\${out#\$build/}
    if [[ -f \$prebuilt/\$rel ]]; then
      mkdir -p "\$(dirname "\$out")"; cp "\$prebuilt/\$rel" "\$out"; exit 0
    fi
    echo "xcrun shim: no prebuilt shader for \$rel" >&2; exit 1;;
esac
exec /usr/bin/xcrun "\$@"
EOF
    chmod +x "$shim_path/xcrun"
    echo "using prebuilt Metal shaders from $prebuilt_metal"
fi

# ── Configure and build ─────────────────────────────────────────────────────
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_PREFIX_PATH="$deps" \
    -DDISABLE_ADVANCE_SIMD=ON \
    -DLTO_PCSX2_CORE=OFF \
    -DPACKAGE_MODE=OFF \
    -DUSE_LINKED_FFMPEG=ON

if [[ -n $shim_path ]]; then
    PATH="$shim_path:$PATH" cmake --build "$build" -j "$jobs"
else
    cmake --build "$build" -j "$jobs"
fi

app="$build/pcsx2-qt/PCSX2.app"
if [[ ! -x $app/Contents/MacOS/PCSX2 ]]; then
    echo "error: build did not produce $app" >&2
    exit 1
fi
# CMake's bundle step can leave unsigned bundled dylibs. Sign the successfully
# built app ad hoc so the local capture executable is launchable.
codesign --force --deep --sign - "$app" >/dev/null 2>&1 || true

echo
echo "PCSX2 (GT capture): $app"
"$app/Contents/MacOS/PCSX2" -version | head -1
