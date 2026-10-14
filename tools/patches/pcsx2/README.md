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
| `0012-native-size-depth-of-field-blur.patch` | RR5 depth-of-field blur (attract, save screen, fly-by) at internal scale above 1 (`GSRrvSetNativeBlurCopy`; fukami#2, docs/TESTING.md T-DOF-SCALE): FBP 210 forward and return draws both come from a box-downsampled native copy of their source and align to half a native pixel. Off at scale 1; applies before the Linux/Windows patches. |
| `0013-sprite-coverage.patch` | Point-sampled 2D at every output scale (`GSRrvSetSpriteCoverage`; docs/TESTING.md T-SPRITE-COVERAGE). A NEAREST draw of FST sprites or of axis-aligned two-triangle rectangles at one depth that reads guest memory gets its own shader variants: the vertex shader puts its edges on host pixel centres (`VS_RRV_SNAP`) and the pixel shader's 4-tap path weights the two texels under each host pixel by their share of it (`PS_SPRITE_COV`, `SpriteCov` in the pixel constants; Direct3D 11/12, Vulkan and Metal shaders, the Metal vertex shader by a mark in its constants). A LINEAR FST sprite from guest memory above scale 1 has its far texture coordinate pulled in so that its last host pixel reads what its last native pixel does (`RrvLinearSpriteEdges`: no atlas neighbour at the end of the title logos). Sprites keep the texels the RoundSprite fix finds, now at scale 1 too, with exact texel ends; `GSRrvMarkAlignedSprite` lets the bridge supply the U of a sprite it has moved. Off by default in PCSX2; the bridge turns it on in full mode unless `RRV_PCSX2_GS_SPRITE_FILTER=ps2`. Applies before the Linux/Windows patches. |
