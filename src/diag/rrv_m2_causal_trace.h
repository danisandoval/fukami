// rrv_m2_causal_trace.h -- bounded, deferred M2 scheduling-causality trace.
//
// This is deliberately separate from the canonical GS-consumption receipt.
// Its sequence is the atomic observation-linearization order of diagnostic
// sites, not a claim that independently executing sources have a guest causal
// order.  `Source` records the known logical context for every observation.
#ifndef RRV_M2_CAUSAL_TRACE_H
#define RRV_M2_CAUSAL_TRACE_H

#include <cstdint>

namespace rrv::m2causal
{
enum class EventType : uint32_t
{
    SelectorEdge = 1,
    OriginBoundary = 2,
    CompleteField = 3,
    TickPublished = 4,
    TickDelivered = 5,
    BackendFieldDelivery = 6,
    VblankStartRequest = 7,
    VblankStartHandlerEnter = 8,
    VblankStartHandlerExit = 9,
    VblankEndRequest = 10,
    VblankEndHandlerEnter = 11,
    VblankEndHandlerExit = 12,
    IntcDispatchRequest = 13,
    DmacDispatchRequest = 14,
    DmacHandlerEnter = 15,
    DmacHandlerExit = 16,
    WaitEnter = 17,
    WaitReturn = 18,
    GuestLockRelease = 19,
    GuestLockReacquire = 20,
    GuestDispatchEnter = 21,
    GuestDispatchExit = 22,
    CooperativeYield = 23,
    Vif0DmaStart = 24,
    Vif0DmaComplete = 25,
    Vif1DmaStart = 26,
    Vif1DmaComplete = 27,
    GifDmaStart = 28,
    GifDmaComplete = 29,
    Vif1Direct = 30,
    PathSubmission = 31,
    TransferAcknowledged = 32,
    LocalMemoryRestoreAcknowledged = 33,
    GsVsyncAcknowledged = 34,
    CdCallbackDispatch = 35,
    SifCallbackDispatch = 36,
    ReplayBegin = 37,
    ReplayEnd = 38,
    AlarmCallbackDispatch = 39,
    CompleteFieldBegin = 40,
    Path2PayloadFingerprint = 41,
    Vif1FifoEnter = 42,
    Vif1FifoExit = 43,
};

enum class Source : uint32_t
{
    Unknown = 0,
    VblankWorker = 1,
    GuestExecution = 2,
    InterruptDispatcher = 3,
    DmacDispatcher = 4,
    Vif0 = 5,
    Vif1 = 6,
    Gif = 7,
    Backend = 8,
    Replay = 9,
    CdOrSif = 10,
};

#if defined(RRV_M2_CAUSAL_TRACE)
// No event has an implicit source.  `a`, `b`, and `c` are type-specific,
// fixed-width semantic fields documented by the emitted overlay manifest.
// The active complete guest field is attached by the collector, or is
// UINT64_MAX when the observation predates the post-boundary interval.
void event(EventType type, Source source, uint64_t a = 0u, uint64_t b = 0u,
           uint64_t c = 0u) noexcept;
// FNV-1a is calculated only while collecting.  It correlates existing packet
// bytes across producer/arbiter/backend observations; it is not a uniqueness
// proof and never contributes to the canonical GS receipt.
void packetFingerprint(Source source, const uint8_t *bytes, uint32_t sizeBytes,
                       uint64_t site) noexcept;

// Called once before guest threads exist.  Mode B is compiled in with
// RRV_M2_CAUSAL_TRACE unset or set to 0.  Mode C sets it to the deferred output
// file path.  No file is opened here or while the interval is measured.
bool initializeFromEnvironment() noexcept;
bool enabled() noexcept;
bool collecting() noexcept;
bool acceptingDiagnostics() noexcept;
void fail() noexcept;
bool overflowed() noexcept;
uint64_t recordCount() noexcept;

// The live state machine: selector edge, discarded mixed closure, then the
// first four complete post-boundary fields.  OriginBoundary deliberately does
// not create a retained complete field.
void beginSelectorEdge(uint32_t previousSelector, uint32_t currentSelector,
                       uint64_t guestTick) noexcept;
void originBoundary(uint64_t guestFieldId, uint64_t gsFieldEpochId,
                    uint32_t parity) noexcept;
// Replay invokes this immediately before it submits a known complete field.
// Live derives the same identity from originBoundary()/completeField().
void beginCompleteField(uint64_t guestFieldId, uint64_t gsFieldEpochId,
                        uint32_t parity) noexcept;
void completeField(uint64_t guestFieldId, uint64_t gsFieldEpochId,
                   uint32_t parity) noexcept;

// Immutable replay has no interrupt worker.  It uses the same bounded storage
// and complete-field rule, but records an explicit replay source envelope.
void beginReplay() noexcept;
void endReplay() noexcept;

// Deferred only: call after the measured interval and after all trace writers
// are quiescent.  `error` is metadata for the caller, never recorded in the
// binary identity stream. Disabled is a successful no-op. Incomplete,
// overflowed, unpublished records or failed I/O fail the diagnostic.
bool dumpDeferred(const char *error = nullptr) noexcept;

// Exception-safe handler/dispatch boundaries for the producer overlay.
class Scope final
{
public:
    Scope(EventType enterType, EventType exitType, Source source, uint64_t a = 0u,
          uint64_t b = 0u, uint64_t c = 0u) noexcept;
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    EventType m_exit;
    Source m_source;
    uint64_t m_a;
    uint64_t m_b;
    uint64_t m_c;
};

// Asset-free tests only.  It avoids process-global environment cache concerns
// while preserving the production initialization and hot-path implementation.
bool resetForTest(bool enable, const char *deferredPath = nullptr,
                  uint32_t targetFields = 4u) noexcept;
#else
// Mode A intentionally has no state, storage, or external symbol references.
// Keep call sites identical to B/C so the producer overlay is mechanically
// auditable without changing guest control flow.
inline void event(EventType, Source, uint64_t = 0u, uint64_t = 0u, uint64_t = 0u) noexcept {}
inline void packetFingerprint(Source, const uint8_t *, uint32_t, uint64_t) noexcept {}
inline bool initializeFromEnvironment() noexcept { return true; }
inline bool enabled() noexcept { return false; }
inline bool collecting() noexcept { return false; }
inline bool acceptingDiagnostics() noexcept { return false; }
inline void fail() noexcept {}
inline bool overflowed() noexcept { return false; }
inline uint64_t recordCount() noexcept { return 0u; }
inline void beginSelectorEdge(uint32_t, uint32_t, uint64_t) noexcept {}
inline void originBoundary(uint64_t, uint64_t, uint32_t) noexcept {}
inline void beginCompleteField(uint64_t, uint64_t, uint32_t) noexcept {}
inline void completeField(uint64_t, uint64_t, uint32_t) noexcept {}
inline void beginReplay() noexcept {}
inline void endReplay() noexcept {}
inline bool dumpDeferred(const char * = nullptr) noexcept { return true; }
class Scope final
{
public:
    Scope(EventType, EventType, Source, uint64_t = 0u, uint64_t = 0u, uint64_t = 0u) noexcept {}
};
inline bool resetForTest(bool, const char * = nullptr, uint32_t = 4u) noexcept { return true; }
#endif
} // namespace rrv::m2causal

#endif
