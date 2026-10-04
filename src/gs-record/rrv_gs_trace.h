// rrv_gs_trace.h — authoritative GS CONSUMPTION-boundary trace.
//
// Why this exists (KNOWN_ISSUES #22)
// ----------------------------------
// The older `.gsr` recorder hooks `GifArbiter::submit()`, i.e. packet ARRIVAL.
// The GS is not fed there: it is fed from `GifArbiter::drain()`, which
// `stable_sort`s the queue by path priority first, and it is also fed by HLE
// A+D sequences and by privileged/lifecycle calls that never pass through the
// arbiter at all. A `.gsr` is therefore not an oracle for what PCSX2 actually
// consumed, and every offline `.gs` comparison built on one validates a
// sequence the live GS may never have seen.
//
// This trace is taken at the real boundary: immediately around the bridge calls
// in `rrv::gsbackend::Backend`, under the same mutex that serialises them. It
// records the ORDERED CALL SEQUENCE, not concatenated bytes, so a replay can
// reissue exactly the same calls with exactly the same arguments and chunking:
//
//   Submit        one GSgifTransfer: path, size, exact payload (chunk boundary
//                 is the record boundary -- two adjacent submits are NOT merged)
//   Vsync         one GSvsync: field index, field parity, and the full 19-entry
//                 privileged register block delivered with it
//   Readback      CSR/SIGLBLID poll (observable GS state transition)
//   ReadLocal     local->host transfer request (bitbltbuf/trxpos/trxreg)
//   RestoreLocal  a wholesale local-memory write (lifecycle/state transition)
//   SnapshotLocal a wholesale local-memory read
//   CopyFrame     host presentation snapshot
//
// The header carries the initial 4 MiB local memory and the initial privileged
// register block, so all three replay paths (live, direct-call harness, MTGS
// runner) can be seeded identically.
//
// Hygiene: traces are game-derived data. Scratchpad or /tmp only, never
// committed (CLAUDE.md hard constraint). See .gitignore for *.gstrace.
#ifndef RRV_GS_TRACE_H
#define RRV_GS_TRACE_H

#include <cstdint>
#include <cstddef>

namespace rrv::gstrace {

static constexpr uint32_t kTraceMagic = 0x54524752;   // "RGRT"
static constexpr uint32_t kTraceVersion = 1;

enum class EventTag : uint8_t {
    Submit = 1,
    Vsync = 2,
    Readback = 3,
    ReadLocal = 4,
    RestoreLocal = 5,
    SnapshotLocal = 6,
    CopyFrame = 7,
    // Guest scene coordinates, stamped by src/patches.cpp when the attract
    // cursor moves. Carries no GS state and is skipped by every consumer, but
    // it is what makes ONE long capture sweepable: without it a per-checkpoint
    // comparison needs a fresh live cold boot per checkpoint.
    SceneMark = 8,
};

#pragma pack(push, 1)

// File header, followed by `vramSize` bytes of initial local memory, then the
// event stream. Each event is EventHeader followed by `payloadSize` bytes.
struct TraceFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vramSize;            // bytes of initial local memory that follow
    uint32_t privRegsCount;       // 19
    uint64_t initialPrivRegs[19];
    uint64_t eventCount;
    // Reuses reserved v1 bytes. Zero is Field, preserving old traces.
    uint32_t renderMode;
    uint32_t reserved0;
    uint64_t reserved1;
};

static_assert(sizeof(TraceFileHeader) == 192u, "gstrace header size/version contract changed");
static_assert(offsetof(TraceFileHeader, renderMode) == 176u,
              "gstrace render-mode metadata must remain in v1 reserved space");

struct EventHeader {
    uint8_t tag;                  // EventTag
    uint8_t pathId;               // Submit only (1..3); 0 otherwise
    uint16_t reserved;
    uint32_t payloadSize;         // bytes following this header
    uint64_t sequence;            // monotonic call index, for divergence reports
};

// Vsync payload: the exact GSvsync arguments.
struct VsyncPayload {
    uint64_t regs19[19];
    uint64_t fieldIndex;
    uint32_t fieldParity;         // the `registers_written`/field argument
    uint32_t reserved;
};

// ReadLocal payload: the guest's own transfer descriptors.
struct ReadLocalPayload {
    uint64_t bitbltbuf;
    uint64_t trxpos;
    uint64_t trxreg;
    uint32_t byteCount;
    uint32_t reserved;
};

// Readback payload: what the GS returned, so a replay can be checked against
// the live values rather than merely reissuing the call.
struct ReadbackPayload {
    uint64_t csr;
    uint64_t siglblid;
};

// SceneMark payload: the attract cursor as src/patches.cpp reads it.
// `vsyncIndex` is the number of Vsync events already recorded, so a consumer
// can map a checkpoint onto a frame range without decoding any GS state.
struct SceneMarkPayload {
    uint32_t phase;
    uint32_t script;
    uint32_t step;
    uint32_t reserved;
    uint64_t vsyncIndex;
};

#pragma pack(pop)

}  // namespace rrv::gstrace

#endif  // RRV_GS_TRACE_H
