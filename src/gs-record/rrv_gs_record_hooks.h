// rrv_gs_record_hooks.h — thin capture-hook interface between the vendored
// PS2 runtime (tools/PS2Recomp/ps2xRuntime) and the outer GS-stream recorder
// (src/gs-record/). Milestone B4 (docs/MILESTONES.md).
//
// Mirrors the B1 rrv_ir_hooks.h contract exactly: this header is the ENTIRE
// surface the vendored runtime is allowed to see, every hook is a no-op unless
// recording is enabled (single relaxed atomic check, near-zero cost off), and
// the vendored call site guards the call so argument marshalling itself is
// skipped when disabled.
//
// Two funnels only (see format header / report for the boundary justification):
//   1. GifArbiter::submit()      -> hookGifPacket   (the ingress boundary)
//   2. GS::latchHostPresentationFrame() -> hookPresent (privileged-reg + vsync
//      latch, same fields as B1's HookPresent)
#ifndef RRV_GS_RECORD_HOOKS_H
#define RRV_GS_RECORD_HOOKS_H

#include <cstdint>

namespace rrv::gsrecord {

// Optional provider used when the active renderer owns GS local memory outside
// PS2Memory (the in-process PCSX2 backend). It must fill exactly `vramSize`
// bytes and return true. The recorder falls back to the supplied live pointer
// when no provider is installed.
using VramSnapshotProvider = bool (*)(void* context, uint8_t* out, uint32_t vramSize);

// Optional diagnostic action after an armed capture's initial header and VRAM
// image have been written and flushed, but before packet recording is enabled.
// Receives the exact pre-action snapshot bytes. Return false to abort capture.
using PostInitialSnapshotWriteHook = bool (*)(void* context, const uint8_t* snapshot,
                                               uint32_t vramSize);

// Global enable. Set once at startup from the RRV_GS_RECORD env var.
bool rrv_gs_record_enabled();

// Initialise recording from the environment. Idempotent. RRV_GS_RECORD unset
// => disabled; a value that looks like a path => record there; any other
// non-empty value => default /tmp/rrv_gs_record/rec.gsr.
//
// `vram` / `vramSize` / `privRegs` (19 uint64 GSRegisters block, see
// ps2_memory.h) are captured ONCE at init time as the recording's initial-
// state snapshot (see format header rationale — a live capture can start
// with VRAM already populated).
void rrv_gs_record_init_from_env(const uint8_t* vram, uint32_t vramSize,
                                 const uint64_t* privRegs19,
                                 VramSnapshotProvider vramProvider = nullptr,
                                 void* vramProviderContext = nullptr,
                                 PostInitialSnapshotWriteHook postWriteHook = nullptr,
                                 void* postWriteContext = nullptr);

// Flush and close the recording (called at clean shutdown; safe to call more
// than once / when never enabled).
void rrv_gs_record_shutdown();

// --- Bounded ("armed") capture, B-3 girl investigation -----------------------
// Recording from process launch produces multi-GB files because the scene of
// interest is thousands of frames into the attract loop. With
// RRV_GS_RECORD_ARM=1, rrv_gs_record_init_from_env() only *stashes* the output
// path and the live VRAM/privileged-register pointers; nothing is written and
// rrv_gs_record_enabled() stays false until a patch calls the trigger below at
// the exact scene, except with RRV_GS_RECORD_SIGNAL=USR1: in that diagnostic
// mode it requests present hooks while armed, to consume the signal at a field
// boundary. Packet hooks still discard data until recording starts. The
// initial-state snapshot is then taken AT TRIGGER TIME, so
// the recording stays self-contained (same guarantee as a launch-time capture,
// just anchored later). RRV_GS_RECORD_FRAMES=N additionally auto-stops the
// recording after N present latches.
//
// Both are no-ops unless RRV_GS_RECORD is set; default behaviour is unchanged.
bool rrv_gs_record_armed();
void rrv_gs_record_trigger();

// A raw GIF packet has been handed to the arbiter, in arrival order, after
// PATH1/2/3 selection but before any GS decode. `pathId` is the GifPathId
// value (1/2/3).
void hookGifPacket(uint8_t pathId, const uint8_t* data, uint32_t sizeBytes);

// A present/vsync latch (GS::latchHostPresentationFrame). Same fields as B1's
// HookPresent; `vsyncTick` is the recorded tick value replay will inject.
struct RecordPresent {
    uint64_t pmode, smode2;
    uint64_t dispfb1, display1;
    uint64_t dispfb2, display2;
    uint64_t vsyncTick;
};
void hookPresent(const RecordPresent& pr);

} // namespace rrv::gsrecord

#endif // RRV_GS_RECORD_HOOKS_H
