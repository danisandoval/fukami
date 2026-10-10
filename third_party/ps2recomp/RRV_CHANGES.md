# RRV changes to the frozen PS2Recomp tree

`third_party/ps2recomp` started as the byte-exact import of the producer tree `88f6378c0502a22ba55ab4d01dfcce4af7cb0d36` (commit `bdbdede9…`, "RRV d52-compatible PS2Recomp producer v2", itself upstream `ran-j/PS2Recomp` `cf9d22b9…` plus RRV compatibility edits). The commit that follows the import applies the Gate-3 product stage `build-gate3-temporal-stage-g8-2c3f04f` (runtime v141) on top of it. From then on this is ordinary source: edit it directly.

Upstream licence and GPL-3.0 notices are retained: `LICENSE` is unchanged, and the in-file headers of the upstream sources were not touched except where an edit lies inside them. PCSX2-derived behaviour carries its provenance (revision `d5f75c9e4`, file, function) in the overlay docstrings summarised below and in the code comments.

The Python overlays that produced these edits are history only: `scripts/historical/` (templates in `scripts/historical/templates/`; see `tools/patches/INDEX.md`). The files below are the truth.

## Overlays folded in (chain order of the product stage)

| Overlay | What it did |
|---|---|
| `scripts/historical/gate3_temporal_memory.py` | Gate-3 memory-bus MMIO boundary (ps2_memory.h/.cpp). |
| `scripts/historical/gate3_guest_admission.py` | Gate-3 input, CD and named-HLE admission into the guest-time scheduler. |
| `scripts/historical/gate4_vu_vif_overlay.py` | Gate-4 G4-8 VU1/VIF host-overhead trims (VIF1 UNPACK fast path etc.); same guest-visible result. |
| `scripts/historical/gate4_owner_timeline_overlay.py` | Gate-4 S0 owner-work timeline instrument (off by default). Header: rrv_gate4_owner_timeline.h. |
| `scripts/historical/gate4_owner_stream_overlay.py` | Gate-4 VU1+GS owner stream (RRV_VU1GS_EXECUTION; default inline). |
| `scripts/historical/gate4_lazy_checkpoint_overlay.py` | Gate-4 lazy instruction checkpoint (RRV_GATE4_LAZY_CHECKPOINT=0 disables). |
| `scripts/historical/gate4_store_path_overlay.py` | Gate-4 guest load/store path trims (RRV_GATE4_SPR_FAST=0 disables). |
| `scripts/historical/gate8_audio_overlay.py` | Gate-8 audio hook points for the PCSX2 SPU2 + host IOP sound driver; null unless linked. |
| `scripts/historical/vu_opclamp_overlay.py` | VU FMAC operand clamp, PCSX2 vuDouble rules (RRV_VU_OPCLAMP=0 disables). |
| `scripts/historical/ee_fpclamp_overlay.py` | EE COP2-macro/COP1 float clamps (RRV_EE_FPCLAMP=0 disables). |
| `scripts/historical/vu_eatan_overlay.py` | VU EFU arctangent as microVU computes it (RRV_VU_EATAN_MVU=0 disables). |
| `scripts/historical/pad_rumble_overlay.py` | DualShock 2 vibration: libpad actuator calls drive the host pad (RRV_PAD_RUMBLE=0 disables). |
| `scripts/historical/wait_idle_overlay.py` | RR5-specific: job-table wait 0x298EB0 skips idle guest time (src/product/patches.cpp). |
| `scripts/historical/vu_aot_overlay.py` | Statically recompiled VU microcode (RRV_VU_AOT=0 interprets). |

The six base stages and the Gate-3 scheduler/temporal composition are not listed separately above; edits not attributed to a listed overlay come from the base product stages (`gate3_product_sources.compose`: spr-pending-chain, pad-pressure, product-host, callback-stack-main-reservation, iop-heap, rpc-memory-safety) and the Gate-3 scheduler/temporal/accounting composition (`gate3_scheduler_overlay.py`, `gate3_temporal_overlay.py`, `gate3_temporal_accounting_overlay.py`, templates under `tools/patches/gate3_*`).

## Per-file changes

Paths are relative to `third_party/ps2recomp/`. "Overlays" are those whose transform changed the stage file; an empty cell means the edit comes from the base stages / Gate-3 composition above.

| File | Status | Overlays that changed it |
|---|---|---|
| `RRV_STAGE_PROVENANCE.md` | new (RRV-authored) | base / Gate-3 composition |
| `ps2xRecomp/src/lib/code_generator.cpp` | edited | `ee_fpclamp_overlay.py` |
| `ps2xRecomp/src/lib/ps2_recompiler.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/include/ps2_host_backend.h` | edited | base / Gate-3 composition |
| `ps2xRuntime/include/ps2_runtime.h` | edited | `gate3_guest_admission.py`, `gate4_owner_stream_overlay.py`, `gate4_lazy_checkpoint_overlay.py`, `gate8_audio_overlay.py` |
| `ps2xRuntime/include/ps2_runtime_macros.h` | edited | `ee_fpclamp_overlay.py` |
| `ps2xRuntime/include/rrv_gate4_owner_timeline.h` | new (RRV-authored) | `gate4_owner_timeline_overlay.py` |
| `ps2xRuntime/include/rrv_guest_scheduler.h` | new (RRV-authored) | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/include/rrv_intc.h` | new (RRV-authored) | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/include/rrv_temporal_owner.h` | new (RRV-authored) | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/include/runtime/ps2_memory.h` | edited | `gate3_temporal_memory.py`, `gate4_owner_stream_overlay.py` |
| `ps2xRuntime/include/runtime/ps2_pad.h` | edited | `pad_rumble_overlay.py` |
| `ps2xRuntime/include/runtime/ps2_vu1.h` | edited | `vu_aot_overlay.py` |
| `ps2xRuntime/include/runtime/rrv_vu_ir.h` | edited | `vu_aot_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Stubs/CD.cpp` | edited | `gate3_guest_admission.py` |
| `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp` | edited | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Stubs/Helpers/Support.h` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Stubs/Pad.cpp` | edited | `gate3_guest_admission.py`, `pad_rumble_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp` | edited | `gate8_audio_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Syscalls/Helpers/Runtime.h` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Syscalls/Helpers/State.h` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Syscalls/Interrupt.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp` | edited | `gate8_audio_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Syscalls/Sync.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp` | edited | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/gate3_guest_admission_loader.inc` | new (RRV-authored) | `gate3_guest_admission.py` |
| `ps2xRuntime/src/lib/gate3_guest_admission_runtime.inc` | new (RRV-authored) | `gate3_guest_admission.py`, `gate4_lazy_checkpoint_overlay.py`, `gate8_audio_overlay.py` |
| `ps2xRuntime/src/lib/gate3_rrv_intc.inc` | new (RRV-authored) | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/gate3_rrv_temporal_owner.inc` | new (RRV-authored) | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/gate3_temporal_runtime.inc` | new (RRV-authored) | `gate4_owner_timeline_overlay.py`, `gate4_owner_stream_overlay.py`, `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/ps2_audio.cpp` | edited | base / Gate-3 composition |
| `ps2xRuntime/src/lib/ps2_gif_arbiter.cpp` | edited | `gate4_owner_timeline_overlay.py` |
| `ps2xRuntime/src/lib/ps2_gs_gpu.cpp` | edited | `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/ps2_memory.cpp` | edited | `gate3_temporal_memory.py`, `gate4_owner_timeline_overlay.py`, `gate4_owner_stream_overlay.py`, `gate4_lazy_checkpoint_overlay.py` |
| `ps2xRuntime/src/lib/ps2_pad.cpp` | edited | `pad_rumble_overlay.py` |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | edited | `gate3_guest_admission.py`, `gate4_owner_timeline_overlay.py`, `gate4_owner_stream_overlay.py`, `gate4_lazy_checkpoint_overlay.py`, `gate4_store_path_overlay.py`, `gate8_audio_overlay.py`, `vu_aot_overlay.py` |
| `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp` | edited | `gate4_vu_vif_overlay.py`, `gate4_owner_timeline_overlay.py` |
| `ps2xRuntime/src/lib/ps2_vu1.cpp` | edited | `gate4_vu_vif_overlay.py`, `vu_opclamp_overlay.py`, `vu_eatan_overlay.py`, `vu_aot_overlay.py` |

`RRV_STAGE_PROVENANCE.md` is the stage's own provenance note, kept verbatim because the stage file bytes are part of the identity check. `rrv-stage-map.json` maps every stage file to its frozen source.

## Edits after the freeze

- `ps2xRecomp/CMakeLists.txt`, `ps2xAnalyzer/CMakeLists.txt`: `${CMAKE_SOURCE_DIR}/ps2x...` →
  `${CMAKE_CURRENT_SOURCE_DIR}/../ps2x...` so the analyzer and recompiler can be built from the RRV
  repository (`-DRRV_BUILD_RECOMPILER=ON`) instead of only as a top-level project.
- `ps2xRuntime/src/lib/ps2_runtime.cpp`: the three stage-era includes `gate3_guest_rtc.inc`,
  `gate3_rrv_guest_time.inc`, `gate3_rrv_ee_timers.inc` now name the live sources they always were
  (`rrv_guest_rtc.cpp`, `rrv_guest_time.cpp`, `rrv_ee_timers.cpp`, found in `src/guest-time`): no build-local copies.
- `ps2xRuntime/include/ps2_runtime_macros.h`, `src/lib/Kernel/Syscalls/System.cpp`, `src/lib/ps2_memory.cpp`,
  `src/lib/ps2_vu1.cpp`: SPDX / PCSX2 provenance header lines added (they adapt PCSX2 logic); no code change.
- `CMakeLists.txt`: `ps2xStudio` (upstream's GUI) and `ps2xTest` (its tests) are added only if their directories exist. The product never builds
  it, and the public Fukami export leaves it out (its bundled fonts carry no licence file).
- `ps2xRuntime/include/ps2_runtime.h`, `src/lib/gate3_guest_admission_runtime.inc`: `gate3ChargeNamedHleV1` keeps the
  cost entry it found for a name's address (64 slots, the text compared on every use) instead of building a
  `std::string` and walking the map on every call. Same cost, same order of service; host time only.
