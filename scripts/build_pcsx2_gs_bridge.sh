#!/usr/bin/env bash
# Build the optional bridge without ever changing the user's dirty PCSX2 tree.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lock_file="$root_dir/config/dependencies.lock.toml"
pcsx2_source=${PCSX2_SOURCE_DIR:-"$root_dir/build-deps/pcsx2-2.8.2"}
# The lock is authoritative; PINNED_REVISION remains a source-level provenance
# marker for the bridge ABI header.
locked_pcsx2_revision=$(awk '
    $0 == "[pcsx2]" { in_pcsx2 = 1; next }
    in_pcsx2 && /^revision = / { gsub(/"/, "", $3); print $3; exit }
' "$lock_file")
if [[ -n "${PCSX2_REVISION:-}" && "${PCSX2_REVISION:-}" != "$locked_pcsx2_revision" ]]; then
    echo "error: PCSX2_REVISION must match the locked revision $locked_pcsx2_revision" >&2
    exit 1
fi
pcsx2_revision="$locked_pcsx2_revision"
# Build products live IN THE REPO (gitignored via build-*/), never in /tmp: a
# bridge under /private/tmp disappears on reboot, and the runtime then silently
# rendered through the legacy rasterizer for a whole session before anyone
# noticed (docs/TESTING.md T-BACKEND-LEGACY-GAP). The clean patched source is
# also part of the schema-2 provenance record, so it must remain available for
# later verification; it is retained in an ignored directory beside the build.
build_dir=${PCSX2_GS_BRIDGE_BUILD_DIR:-"$root_dir/build-pcsx2-gs-bridge"}
# The bridge patch is the ordered series in tools/patches/pcsx2/; its concatenation is the one locked
# patch. tools/patches/pcsx2-gs-bridge-target.patch is that assembled artifact (read by the lock,
# CMake and the bridge manifest); it must equal the concatenation, byte for byte.
patch_file="$root_dir/tools/patches/pcsx2-gs-bridge-target.patch"
locked_bridge_patch_sha256=$(awk '
    $0 == "[[pcsx2_bridge_patches]]" { in_patch = 1; next }
    in_patch && /^sha256 = / { gsub(/"/, "", $3); print $3; exit }
' "$lock_file")
build_parent=$(dirname "$build_dir")
clean_source_suffix="${pcsx2_revision:0:12}-${locked_bridge_patch_sha256:0:12}"
# Linux (Gate 5, Steam Deck): a second, Linux-only patch is stacked after the
# locked target patch (Vulkan direct-present accounting). The target patch and
# its locked SHA stay exactly the macOS ones.
linux_patch_file="$root_dir/tools/patches/pcsx2-gs-bridge-linux.patch"
linux_patch_sha256=
if [[ $(uname -s) == Linux ]]; then
    linux_patch_sha256=$(sha256sum "$linux_patch_file" | awk '{ print $1 }')
    # Optional lock entry; when present it is authoritative.
    locked_linux_patch_sha256=$(awk '
        $0 == "[[pcsx2_bridge_linux_patches]]" { in_patch = 1; next }
        in_patch && /^sha256 = / { gsub(/"/, "", $3); print $3; exit }
    ' "$lock_file")
    if [[ -n "$locked_linux_patch_sha256" && "$locked_linux_patch_sha256" != "$linux_patch_sha256" ]]; then
        echo "error: $linux_patch_file does not match its locked SHA-256" >&2
        exit 1
    fi
    clean_source_suffix="$clean_source_suffix-${linux_patch_sha256:0:12}"
fi
clean_source_dir=${PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR:-"$build_parent/$(basename "$build_dir")-source-$clean_source_suffix"}
cmake_arch_args=()
python3 "$root_dir/scripts/prepare_dependencies.py" --verify-patches
assembled_patch=$(mktemp "${TMPDIR:-/tmp}/rrv-bridge-target.XXXXXX")
trap 'rm -f "$assembled_patch"' EXIT
python3 "$root_dir/scripts/pcsx2_patch_series.py" assemble --output "$assembled_patch" \
    --expect-sha256 "$locked_bridge_patch_sha256"
cmp -s "$assembled_patch" "$patch_file" || {
    echo "error: tools/patches/pcsx2-gs-bridge-target.patch differs from the concatenated series" >&2
    exit 1
}

if [[ -z "$pcsx2_revision" ]]; then
    echo "error: could not read the PCSX2 revision from $lock_file" >&2
    exit 1
fi

if [[ ! -d "$pcsx2_source/.git" ]]; then
    echo "error: PCSX2 source checkout is unavailable: $pcsx2_source" >&2
    echo "       prepare the pinned clean checkout with:" >&2
    echo "       python3 scripts/prepare_dependencies.py --prepare --component pcsx2" >&2
    echo "       or set PCSX2_SOURCE_DIR to an explicit checkout containing $pcsx2_revision" >&2
    exit 1
fi

if [[ $(uname -s) == Darwin ]]; then
    # Follow the repository's native-first policy. A cross-architecture bridge
    # cannot be dlopened by the native RRV process, so override this only when
    # the matching RRV runtime is also built for that architecture.
    cmake_arch_args=(-DCMAKE_OSX_ARCHITECTURES="${PCSX2_GS_BRIDGE_ARCH:-$(uname -m)}")
    # Release builds name the oldest macOS they support (the same value must be used for the product build).
    if [[ -n "${FUKAMI_MACOS_TARGET:-}" ]]; then
        cmake_arch_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$FUKAMI_MACOS_TARGET")
    fi

    # The compiled libraries are produced by PCSX2's Metal build. They are not
    # reconstructible from this source checkout on hosts without a Metal
    # toolchain, so require an explicit, provenance-reviewed resource directory.
    # M0 deliberately does not substitute a prebuilt app or add a presentation
    # path; M1 owns direct GPU presentation.
    metal_resource_dir=${PCSX2_METAL_RESOURCE_DIR:-}
    metal_resource_manifest=${PCSX2_METAL_RESOURCE_MANIFEST:-}
    if [[ -z "$metal_resource_dir" || -z "$metal_resource_manifest" ]]; then
        echo "error: PCSX2 Metal resources require an explicit reviewed manifest" >&2
        echo "       set PCSX2_METAL_RESOURCE_DIR and PCSX2_METAL_RESOURCE_MANIFEST" >&2
        echo "       the manifest must pin $pcsx2_revision and SHA-256 every required .metallib" >&2
        echo "       no trusted Metal-resource manifest is checked in for M0; do not trust a local app automatically" >&2
        exit 1
    fi
    python3 "$root_dir/scripts/prepare_dependencies.py" \
        --verify-metal-resources "$metal_resource_dir" \
        --metal-resource-manifest "$metal_resource_manifest"
    cmake_arch_args+=("-DRRV_PCSX2_GS_METAL_RESOURCE_DIR=$metal_resource_dir")
elif [[ $(uname -s) == Linux ]]; then
    # PCSX2's Linux CI configuration minus Qt (not needed by the bridge) and
    # tests. DISABLE_ADVANCE_SIMD builds the GS in PCSX2's multi-ISA form
    # (SSE4/AVX/AVX2 selected at run time), as PCSX2's Linux release does,
    # instead of -march=native of whatever machine happens to build it; the
    # Steam Deck (Zen 2) takes the AVX2 path. OpenGL is off: the product
    # renderer is Vulkan and AUTO resolves to it. Caller arguments follow and
    # may override any of these.
    cmake_arch_args=(
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DENABLE_QT_UI=OFF
        -DENABLE_TESTS=OFF
        -DUSE_VULKAN=ON
        -DUSE_OPENGL=OFF
        -DX11_API=ON
        -DWAYLAND_API=ON
        -DDISABLE_ADVANCE_SIMD=ON
        -DENABLE_SETCAP=OFF
    )
fi

if ! git -C "$pcsx2_source" rev-parse --verify "$pcsx2_revision^{commit}" >/dev/null; then
    echo "error: pinned PCSX2 revision is unavailable in: $pcsx2_source" >&2
    exit 1
fi

if [[ ! -e "$clean_source_dir" ]]; then
    # Force the local source through upload-pack so the retained checkout owns
    # its object database. A shared clone would leave objects/info/alternates
    # pointing back to the input checkout and would not be durable provenance.
    git clone --no-local --no-checkout "$pcsx2_source" "$clean_source_dir"
    git -C "$clean_source_dir" checkout --detach "$pcsx2_revision"
elif [[ -z "${PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR:-}" ]]; then
    echo "error: retained bridge source already exists: $clean_source_dir" >&2
    echo "       reuse the existing manifest, or select a new PCSX2_GS_BRIDGE_BUILD_DIR" >&2
    exit 1
fi

if ! git -C "$clean_source_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
   [[ $(git -C "$clean_source_dir" rev-parse HEAD) != "$pcsx2_revision" ]]; then
    echo "error: clean bridge source is not pinned revision $pcsx2_revision" >&2
    exit 1
fi
alternates_file=$(git -C "$clean_source_dir" rev-parse --git-path objects/info/alternates)
if [[ -s "$clean_source_dir/$alternates_file" ]]; then
    echo "error: clean bridge source borrows Git objects through alternates: $clean_source_dir/$alternates_file" >&2
    echo "       provide an independent checkout; shared clones are not durable manifest inputs" >&2
    exit 1
fi

if [[ -n $(git -C "$clean_source_dir" status --porcelain=v1) ]]; then
    echo "error: clean bridge source has local edits; provide a clean detached source" >&2
    exit 1
fi
git -C "$clean_source_dir" apply --check "$assembled_patch"
# Stage the one reviewed patch (the concatenated series) while applying it. The resulting index tree is a
# stable content address for the exact PCSX2 source compiled by CMake; unstaged
# or untracked source would make manifest creation fail.
git -C "$clean_source_dir" apply --index "$assembled_patch"
if [[ -n "$linux_patch_sha256" ]]; then
    git -C "$clean_source_dir" apply --check --cached "$linux_patch_file"
    git -C "$clean_source_dir" apply --index "$linux_patch_file"
fi
git -C "$clean_source_dir" diff --cached --check

# Release binaries must not carry the builder's directory names (assert messages and debug info embed the paths of the
# sources): the patched PCSX2 tree, the bridge sources and the build directory are mapped to fixed names.
prefix_map="-ffile-prefix-map=$clean_source_dir=/pcsx2 -ffile-prefix-map=$root_dir=/fukami -ffile-prefix-map=$build_dir=/bridge-build"
cmake_arch_args+=("-DCMAKE_C_FLAGS=$prefix_map" "-DCMAKE_CXX_FLAGS=$prefix_map")
if [[ $(uname -s) == Darwin ]]; then
    cmake_arch_args+=("-DCMAKE_OBJC_FLAGS=$prefix_map" "-DCMAKE_OBJCXX_FLAGS=$prefix_map")
fi

cmake -S "$clean_source_dir" -B "$build_dir" \
    -DRRV_PCSX2_GS_BRIDGE_SOURCE_DIR="$root_dir/tools/pcsx2-gs-bridge" \
    "${cmake_arch_args[@]}" \
    ${@+"$@"}
cmake --build "$build_dir" --target rrv-pcsx2-gs-bridge

if [[ $(uname -s) == Darwin ]]; then
    bridge_suffix=dylib
else
    bridge_suffix=so
fi
bridge_lib="$build_dir/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.$bridge_suffix"
if [[ ! -f "$bridge_lib" ]]; then
    echo "error: bridge build reported success but $bridge_lib is missing" >&2
    exit 1
fi

if [[ $(uname -s) == Darwin ]]; then
    python3 "$root_dir/scripts/pcsx2_bridge_manifest.py" create \
        --library "$bridge_lib" \
        --pcsx2-source "$clean_source_dir" \
        --build-dir "$build_dir" \
        --metal-resource-dir "$metal_resource_dir" \
        --metal-verifier-manifest "$metal_resource_manifest" \
        --bridge-patch "$patch_file" \
        --bridge-patch-sha256 "$locked_bridge_patch_sha256" \
        --pcsx2-commit "$pcsx2_revision" \
        --output "$build_dir/rrv-pcsx2-gs-bridge-manifest.json"
elif [[ $(uname -s) == Linux ]]; then
    python3 "$root_dir/scripts/pcsx2_bridge_manifest.py" create \
        --library "$bridge_lib" \
        --pcsx2-source "$clean_source_dir" \
        --build-dir "$build_dir" \
        --bridge-patch "$patch_file" \
        --bridge-patch-sha256 "$locked_bridge_patch_sha256" \
        --linux-bridge-patch "$linux_patch_file" \
        --linux-bridge-patch-sha256 "$linux_patch_sha256" \
        --pcsx2-commit "$pcsx2_revision" \
        --output "$build_dir/rrv-pcsx2-gs-bridge-manifest.json"
else
    echo "error: bridge manifest creation is implemented for macOS and Linux only" >&2
    exit 1
fi

echo "built in: $build_dir/pcsx2-gs-bridge"
echo "retained patched source: $clean_source_dir"
echo "pinned PCSX2 revision: $pcsx2_revision"
echo "stamp: $build_dir/RRV_BRIDGE_STAMP.json"
echo "manifest: $build_dir/rrv-pcsx2-gs-bridge-manifest.json"
