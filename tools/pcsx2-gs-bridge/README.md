# PCSX2 GS diagnostic bridge

The accepted renderer is PCSX2 GS hardware. The tracked ABI 5 field transport
supports both the M0 diagnostic CPU snapshot path and an M1 direct-present
surface with tagged on-demand capture. On macOS the product shell owns an SDL
2.32.10 window and Metal view, then passes SDL's view/layer as opaque borrowed
handles. PCSX2 owns its Metal device, queue, final image, drawable and present.
The bridge is not a release claim until `rrv-product` and every M1 gate pass.
See [M1 baseline](../../docs/evidence/M1_PRESENTATION_PATH_BASELINE.md) and
[ADR-0001](../../docs/adr/ADR-0001-pcsx2-gs-production.md).

Source pin: PCSX2 2.8.2, `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`, from
[dependencies.lock.toml](../../config/dependencies.lock.toml).
`PINNED_REVISION` is retained as a compatibility marker and must agree with it.
Changing the source directory does not change the revision.

```sh
python3 scripts/prepare_dependencies.py --prepare --component pcsx2
python3 scripts/prepare_dependencies.py --verify-patches
# On macOS, generate reviewed resources from this exact clean source checkout. The shaders are
# compiled with the locked bridge patch applied (to an export in the output directory).
python3 scripts/build_pcsx2_metal_resources.py \
  --source build-deps/pcsx2-2.8.2 \
  --output build-pcsx2-metal-resources
python3 scripts/prepare_dependencies.py \
  --verify-metal-resources build-pcsx2-metal-resources \
  --metal-resource-manifest build-pcsx2-metal-resources/metal-resources-manifest.toml
PCSX2_METAL_RESOURCE_DIR=build-pcsx2-metal-resources \
PCSX2_METAL_RESOURCE_MANIFEST=build-pcsx2-metal-resources/metal-resources-manifest.toml \
  scripts/build_pcsx2_gs_bridge.sh
```

The manifest format has a root `pcsx2_revision` and one `[[files]]` table for
each of `default.metallib`, `Metal22.metallib`, `Metal23.metallib`, with `path`
and `sha256` strings. `build_pcsx2_metal_resources.py` writes that verifier
manifest plus a JSON manifest of every source hash, compiler command, and
toolchain identity. It requires a clean detached checkout at the pinned source
revision, validates the locked bridge-patch hash and applicability, and never
uses an app bundle or prebuilt `.metallib` as input. Its output is generated
build material and must not be committed.

The bridge builder uses a fresh pinned checkout and verifies the tracked patch.
It retains that patched checkout in an ignored, content-named directory beside
the build because the schema-2 manifest re-verifies the source tree whenever
`rrv-legacy-live` configures. A completed manifest must never point at temporary
or deleted source. The retained checkout has an independent Git object database;
schema-2 verification rejects shared clones whose object alternates point at a
different checkout. Set a new `PCSX2_GS_BRIDGE_BUILD_DIR` for an independent
rebuild; `PCSX2_GS_BRIDGE_CLEAN_SOURCE_DIR` is only for an explicit clean,
independent detached source input.

Outputs are `.dylib` on macOS, `.so` on Linux. The default renderer in tracked
code is PCSX2 SW; `RRV_PCSX2_GS_RENDERER=metal` or `auto` selects an optional
hardware path on macOS. These are diagnostic settings, not a passing hardware
gate. Source-derived shader resources do not by themselves prove a product link
or actual Metal execution. Vulkan execution is not verified in M0.

The SW rasterizer (the whole SW renderer, and CPU sprite rendering in the
Metal profile) generates no code at run time: it uses C functions specialised
ahead of time per selector (`GSDrawScanlineStaticKeys.inc` in the patch; add
selectors with `RRV_PCSX2_GS_SW_KEYS=<file>` and
`scripts/pcsx2_sw_static_keys.py`) and the generic C functions otherwise, and
the bridge maps no JIT memory. Configure with `-DRRV_PCSX2_GS_BRIDGE_SW_JIT=ON`
for a diagnostic bridge that runs PCSX2's ARM64 JIT; there
`RRV_PCSX2_GS_SW_DIFF=<file>` redraws every JIT span with the static/generic
functions into a shadow of local memory and counts mismatching spans per
selector. `RRV_PCSX2_GS_SW_TIMING=1` prints the cumulative SW draw time.

## Linux / Steam Deck (Gate 5)

On Linux the same script builds `librrv-pcsx2-gs-bridge.so`: it stacks
`tools/patches/pcsx2-gs-bridge-linux.patch` (Vulkan direct-present count)
after the locked target patch and configures PCSX2 with Ninja, Release,
`ENABLE_QT_UI=OFF`, `ENABLE_TESTS=OFF`, `USE_VULKAN=ON`, `USE_OPENGL=OFF`,
X11 + Wayland, and `DISABLE_ADVANCE_SIMD=ON` (PCSX2's multi-ISA GS, picked at
run time: SSE4/AVX/AVX2). No Metal resources: Vulkan compiles the GLSL in the
staged `resources/shaders/vulkan` at run time. Build box: `tools/linux-build/`.

```sh
docker run --rm --platform linux/amd64 -v "$PWD":"$PWD" -w "$PWD" rrv-linux-build:1 \
  scripts/build_pcsx2_gs_bridge.sh
```

- Renderer: `RRV_PCSX2_GS_RENDERER=vulkan` / ABI `RENDERER_VULKAN`; `AUTO`
  resolves to Vulkan (never PCSX2's OpenGL). The RR5 hardware profile
  (TextureInsideRt, CPU sprite BW, large-ST, paltex) applies to Vulkan exactly
  as to Metal, with the same `RRV_PCSX2_GS_*` env overrides.
- Direct present: surface kind `LINUX_X11` (Display*, Window XID) or
  `LINUX_WAYLAND` (wl_display*, wl_surface*), mapped to PCSX2 `WindowInfo`.
  `CALLER_OWNS_HANDLES` is required; there is no main-thread rule (SDL2 calls
  `XInitThreads`). The owner-thread exports exist; `pump_main_thread` is a no-op.
- Not on Vulkan: the Metal `presentDrawable:atTime:` pacer; FIFO vsync alone
  paces. `RRV_PCSX2_GS_PRESENT_PACING` (the even-pacing setting) instead decides
  where the `vkAcquireNextImageKHR` that PCSX2 issues right after each present
  runs: on a helper thread (`rrv-vk-acquire`) by default, so the GS thread goes
  on with the next field and waits for the image only at the next
  `BeginPresent` (and before anything else that touches the swap chain); `=0`
  keeps it on the GS thread. Measurements: `docs/TESTING.md` T-DECK-GS-WAIT.
  The other `RRV_PCSX2_GS_PRESENT_*` variables are ignored.
- GS-thread attribution (Linux patch): every ~2 s the bridge prints a
  `[gs-wait]` line next to the `[cpu]` log: the GSvsync thread's CPU time per
  field, and the time it was blocked on the GPU, split into forced readbacks,
  command-buffer rotation, stream-buffer exhaustion and swap-chain acquire,
  plus the CPU-sprite (`SwPrimRender`) and texture-cache readback costs, the
  swap-chain image count and the helper thread's time in the acquire. On by default with direct presentation;
  `RRV_PCSX2_GS_WAIT_LOG=0` turns it off (`=1` also headless).
- SW rasterizer: the same static C functions as macOS (no JIT on x86-64
  either). The unsigned colour-step fix applies on every arch; the JIT-exact
  per-pixel mipmap LOD is ARM64-only, so x86-64 uses PCSX2's generic C LOD.
  The x86-64 path has not been checked against PCSX2's x86 JIT.

The ABI has no SDL, Objective-C or PCSX2 types/headers across it; GIF submit,
field transition and guest-visible memory/readback behavior are unchanged in
this milestone. The macOS bridge adapter validates the SDL-provided AppKit
objects but creates no application, window or view.
The [original guide](../../docs/evidence/pre-m0/PCSX2_GS_BRIDGE_README.md) is
retained as dated evidence, including prior commands and local-path observations.

## Windows (Direct3D 11, optional Vulkan)

`scripts/build_pcsx2_gs_bridge.py` builds `rrv-pcsx2-gs-bridge.dll` with clang-cl. It stacks the lock's
`[[pcsx2_bridge_windows_patches]]` after the target patch: the Linux patch (Vulkan direct-present count) and
`tools/patches/pcsx2-gs-bridge-windows-d3d11.patch` (the same count in `GSDevice11::EndPresent`, and the Windows
present pacer). PCSX2's Windows build always compiles its Direct3D renderers, so the one DLL holds both devices.
`RRV_PCSX2_GS_RENDERER_AUTO` and `RRV_PCSX2_GS_RENDERER_VULKAN` open Vulkan; `RRV_PCSX2_GS_RENDERER_D3D11` opens
Direct3D 11 on the same Win32 surface with the same RR5 hardware profile. The product asks for Direct3D 11 unless
rrv.ini says `[rendering] backend = vulkan` (`RRV_PCSX2_GS_BACKEND=vulkan`; `src/product/main_product.cpp`). Direct
presentation fails closed when PCSX2 instantiates a different renderer than the one requested.

Frame pacing on Windows is `RRVPresentPacer.h` in that patch (Direct3D 11 only; its header comment describes it):
the DXGI sync interval on a fixed 120/180/240 Hz display, a presenter thread and an even grid elsewhere, a present
without display sync on a variable-refresh display (`RRV_PCSX2_GS_VRR=1|0`, default: once the display is seen
following the frames). `RRV_PCSX2_GS_PRESENT_PACING=0` (the even-pacing setting off) turns it off.
`RRV_PCSX2_GS_PRESENT_TRACE=<file>` writes one line per present; `tools/win-profile/present_stats.py` reads it.
Vulkan frames are not paced on Windows. Measurements: docs/TESTING.md T-WIN-PACING.
