// rrv_gs_record_format.h — on-disk format for the GS-stream record/replay
// harness (milestone B4, docs/MILESTONES.md).
//
// Recording boundary (see report for full justification): GifArbiter::submit()
// — the raw GIF packet (path id + bytes) at the moment it is handed to
// PS2Memory::submitGifPacket(), i.e. AFTER PATH1/2/3 DMA arbitration has
// selected it but BEFORE any GS decode has touched it. This is the narrowest
// point that still sees 100% of what GS::processGIFPacket consumes (register
// writes via A+D/PACKED/REGLIST, vertex kicks, and IMAGE payload bytes all
// arrive as bytes inside these packets), so replaying the recorded packets
// through the real, unmodified GS decoder re-executes the maximum amount of
// real code — only DMA-channel arbitration/timing is not re-derived (and the
// arbiter's sort in drain() is itself deterministic given packet arrival
// order, which the recording preserves).
//
// A SECOND, independent data source is captured explicitly: the privileged GS
// registers (PMODE/SMODE2/DISPFB/DISPLAY/...) are ordinary EE MMIO stores, not
// part of the GIF stream, and only change at mode boundaries. Rather than
// tracing every individual MMIO write, we snapshot the full GSRegisters block
// once per present latch (GS::latchHostPresentationFrame() call) as an
// explicit Present stream event — this is the same set of fields B1's
// HookPresent already captures, and "only changes at scene/mode boundaries"
// matches the measured B1 fact that these fields are stable for long runs.
//
// The recording is fully self-contained (replay needs no ELF/game file): the
// header carries one snapshot of initial GS local memory (VRAM) and the
// initial GSRegisters block, because a live capture can start mid-run with
// VRAM already populated from before RRV_GS_RECORD was set (e.g. static
// textures uploaded once at boot, long before the attract loop). Recording
// this ~4MB snapshot once is far cheaper than replaying-from-empty and
// guessing; it is the only "extra" state beyond the packet stream itself.
//
// Hygiene: recordings are game-derived data — never committed, /tmp or the
// session scratchpad only (CLAUDE.md hard constraint). See .gitignore additions
// for *.gsr / rrv_gs_record/.
#ifndef RRV_GS_RECORD_FORMAT_H
#define RRV_GS_RECORD_FORMAT_H

#include <cstdint>
#include <cstddef>

namespace rrv::gsrecord {

static constexpr uint32_t kGsrMagic   = 0x52524752; // "RGRR" (RRv Gs Record)
static constexpr uint32_t kGsrVersion = 1;

#pragma pack(push, 1)

// File header. Followed immediately by `vramSize` bytes of initial GS local
// memory, then the event stream (see EventTag).
struct GsrFileHeader {
    uint32_t magic;         // kGsrMagic
    uint32_t version;       // kGsrVersion
    uint32_t vramSize;      // bytes of initial-VRAM snapshot that follow
    uint32_t privRegsSize;  // sizeof(GSRegisters) at record time (layout guard)
    uint64_t initialPrivRegs[19]; // GSRegisters at record start (pmode..siglblid)
    uint64_t eventCount;    // number of stream events that follow (informational)
    // Reuses reserved v1 bytes. Zero is Field, preserving old recordings.
    uint32_t renderMode;
    uint32_t reserved;
};

static_assert(sizeof(GsrFileHeader) == 184u, "GSR header size/version contract changed");
static_assert(offsetof(GsrFileHeader, renderMode) == 176u,
              "GSR render-mode metadata must remain in v1 reserved space");

enum class EventTag : uint8_t {
    GifPacket = 1,  // a raw GIF packet, post-arbitration, arrival order
    Present   = 2,  // a present/vsync latch (GS::latchHostPresentationFrame)
};

// Preceding every event: a small fixed record so a reader can seek without
// fully decoding payloads it doesn't need.
struct GsrEventHeader {
    uint8_t  tag;        // EventTag
    uint8_t  pathId;     // GifPathId (1/2/3); 0 for non-GifPacket events
    uint16_t reserved;
    uint32_t payloadSize; // bytes of payload immediately following this header
};

// Payload for EventTag::Present (fixed size, payloadSize == sizeof(this)).
// Mirrors rrv::ir::HookPresent (B1) — same fields, same provenance rationale.
struct GsrPresentPayload {
    uint64_t pmode, smode2;
    uint64_t dispfb1, display1;
    uint64_t dispfb2, display2;
    uint64_t vsyncTick;      // recorded tick value; replay sets this exactly
                             // (see report: the live tick source is a wall-clock
                             // interrupt-worker thread, which is NOT re-created
                             // during headless replay — the recorded tick is
                             // authoritative and injected via a small vendored
                             // setter instead).
};

// GifPacket payload is exactly `payloadSize` raw bytes (the packet as handed
// to GifArbiter::submit) with no further framing.

#pragma pack(pop)

} // namespace rrv::gsrecord

#endif // RRV_GS_RECORD_FORMAT_H
