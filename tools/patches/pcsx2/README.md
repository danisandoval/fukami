# PCSX2 bridge patch series

The changes RRV makes to PCSX2 (fd9d310, 2.8.2) so its GS runs as the in-process bridge target, as an ordered
series. Applied in the order of `series`, one `git apply` per file, they give the same tree as the single
patch the lock pins; concatenated byte for byte they **are** that patch
(`tools/patches/pcsx2-gs-bridge-target.patch`, sha256 in `config/dependencies.lock.toml`).
`scripts/pcsx2_patch_series.py check [--pcsx2-source DIR]` proves both; `scripts/build_pcsx2_gs_bridge.sh`
assembles the series, checks the lock's SHA-256 and applies exactly those bytes.

Editing: change the relevant file here (patches are plain `git diff` output; later files may depend on earlier
ones), regenerate the artifact with
`python3 scripts/pcsx2_patch_series.py assemble --output tools/patches/pcsx2-gs-bridge-target.patch`,
update the lock's `sha256` and `bridge_patched_tree`, and rebuild the Metal resource directory named by the
first 12 hex of the new SHA (`scripts/build_pcsx2_metal_resources.py`). PCSX2 code is GPL-3.0+; these patches
carry its licence.

| Patch | Purpose |
|---|---|
| `0001-bridge-target-and-local-memory-entry-points.patch` | CMake hook `RRV_PCSX2_GS_BRIDGE_SOURCE_DIR`; bridge entry points that read and write GS local memory (`GS.cpp`, `GS.h`, `GSState.*`, `GSLocalMemory.h`); the RR5 Metal profile switches (`Config.h`, `GSRendererHW`: RewriteLargeST). |
| `0002-metal-lifetime-diagnostic.patch` | Metal object lifetime diagnostic (`RRVMetalLifetimeDiag.h`, `GSDeviceMTL.mm`, `GSMTLDeviceInfo.mm`). |
| `0003-software-renderer-without-vm-jit-map.patch` | The SW rasterizer without PCSX2's VM-wide JIT code map (`RRV_PCSX2_GS_C_SCANLINE`), the ARM64 PSMZ24 per-pixel depth path, `RrvSyncRasterizer`. |
| `0004-gif-packed-partial-exhaustion.patch` | `GSState::Transfer`: a split path-3 PACKED payload that finishes the current loop with residual QWC must not enter the PACKED fallback. |
| `0005-texture-cache-scratch-capacity.patch` | `GSTextureCache::Target::Update`: check scratch capacity before aligning, multiplying or narrowing; keep dirty rectangles for a retry. |
| `0006-full-frame-canonical-field-ofy.patch` | Gate-6 full-frame: canonical field OFY (`GSRrvSetFieldOfyCanonical`). |
| `0007-full-frame-round-sprite-u-only.patch` | Gate-6 full-frame: HW RoundSprite rounds only U (`GSRrvSetRoundSpriteUOnly`). |
| `0008-drop-large-st-triangles.patch` | Gate-6 diagnostic: collapse triangles whose ST had to be rewritten (`GSRrvSetDropLargeSTTriangles`). |
| `0009-full-frame-round-sprite-minified-v.patch` | Gate-6 full-frame: vertically minified sprites round V on the progressive grid. |
| `0010-software-renderer-static-keys-and-arm64-c-path.patch` | SW rasterizer without runtime JIT: ahead-of-time specialised C functions per selector (`GSDrawScanlineStaticKeys.inc`), generic C functions as fallback, two ARM64 C-path fixes (unsigned colour-step pack in `CSetupPrim`, JIT-exact per-pixel mipmap LOD), `RRV_PCSX2_GS_BRIDGE_SW_JIT=ON` restores the JIT for the differential check. |
| `0011-metal-present-pacing.patch` | Direct-present frame pacing in `GSDeviceMTL::EndPresent` (`presentDrawable:atTime:` on an even 1001/60000 s grid; `RRV_PCSX2_GS_PRESENT_PACING=0` off, `RRV_PCSX2_GS_PRESENT_TRACE=<path>` logs on-screen times). |
