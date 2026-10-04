// rrv_m2_guest_provenance.h -- bounded guest continuation/write diagnostics.
//
// This is observation only. It does not alter guest dispatch, memory writes,
// DMA, GS, timing, or the canonical receipt.
#ifndef RRV_M2_GUEST_PROVENANCE_H
#define RRV_M2_GUEST_PROVENANCE_H

#include "rrv_m2_causal_trace.h"

#include <cstdint>

namespace rrv::m2guest
{
constexpr uint64_t kUnknownId = 0u;
constexpr uint32_t kSourceRangeStart = 0x00347bb0u;
constexpr uint32_t kSourceRangeBytes = 320u;
constexpr uint32_t kHelperInput20Address = 0x00346fa0u;
constexpr uint32_t kHelperInput50Address = 0x00346fd0u;
constexpr uint32_t kHelperInputWatchBytes = 8u;

// Serialized through m2causal::event() as fixed-width records 96 onward.
// Values 96--111 are accepted trace schema and remain append-only.
enum class Event : uint32_t
{
    DispatchEntry = 96, DispatchExit = 97, DispatchSelection = 98,
    DispatchAborted = 99, EntryRangeSnapshot = 100, ExitRangeSnapshot = 101,
    WriteBegin = 102, WriteBefore = 103, WriteAfter = 104, WriteFailed = 105,
    ControlRegisters = 106, ControlS7 = 107, ControlByte = 108,
    PreemptCheck = 109, PreemptSuppressed = 110, SourceSnapshot = 111,
    // Emitted immediately after a PreemptCheck.  `a` may be zero for the
    // unretained carry-in dispatch scope; zero never implies a fabricated entry.
    PreemptContext = 112,
    // The cooperative dispatcher yielded.  Pending state is explicitly
    // invalid here because the current source exposes none for this decision.
    PreemptYield = 113,
    // Bounded tuple and exact eight-byte inputs observed at FUN_0021e698.
    HelperEntry = 114, HelperLink = 115, HelperInput20 = 116, HelperInput50 = 117,
    // Per-slot exact source-input writer chains.  They do not widen or alter
    // the older 320-byte packet-output write observation.
    InputWriteBegin = 118, InputWriteContext = 119, InputWriteOverlap = 120,
    InputWriteBefore = 121, InputWriteAfter = 122, InputWriteFailed = 123,
};

enum class DispatchReason : uint32_t { Unknown = 0u, ReturnFromFunction = 1u };
enum class WriteFailure : uint32_t { Aborted = 1u, InvalidRange = 2u };
enum class InputWatchSlot : uint32_t { HelperInput20 = 0u, HelperInput50 = 1u };
enum class InputWriteFailure : uint32_t { Aborted = 1u, InvalidRange = 2u };

// Main configures this once, after causal and DMA provenance. There is no
// environment access, allocation, I/O, or blocking synchronization in hot paths.
bool configure(bool enabled) noexcept;
bool configured() noexcept;
uint64_t currentId() noexcept;
using CurrentRegistersFn = bool (*)(const void *context, uint32_t &ra,
                                    uint32_t &sp) noexcept;
// Static slice only: caller supplies no-MMIO-checked inputs for 0x295e38 or
// 0x29c460. For the latter `s7` is zero and validMask documents it is absent.
void controlInputs(uint32_t target, uint32_t s0, uint32_t s1, uint64_t s7,
                   uint32_t address, uint32_t value, bool available) noexcept;
void preemptCheck(uint32_t counterBefore, uint32_t waiterCount, uint32_t interval) noexcept;
void preemptSuppressed(uint32_t depth, uint32_t reason) noexcept;
// Called only on the existing successful cooperative-yield path.  The current
// ctx->pc is read through the DispatchScope TLS pointer; no pending scheduler
// state is serialized because none is source-proven at this hook.
void preemptYield() noexcept;
// Called at the generated helper's first instruction before it reads either
// input.  It records only caller tuple and the two exact LE eight-byte words.
void helperEntry(uint32_t callerRa, uint32_t a0, uint32_t a1, uint32_t sp,
                 const uint8_t *rdram) noexcept;
// DMA calls this at the actual source-byte capture point, not lazy metadata export.
void sourceSnapshot(uint64_t rootId, uint32_t space, uint32_t guestAddress,
                    const uint8_t *bytes, uint32_t sizeBytes) noexcept;

#if defined(RRV_M2_GUEST_PROVENANCE)
class DispatchScope final
{
public:
    DispatchScope(uint32_t target, uint32_t entryPc, uint32_t entryRa,
                  uint32_t entrySp, const uint8_t *rdram,
                  DispatchReason reason = DispatchReason::Unknown,
                  const uint32_t *livePc = nullptr,
                  const void *registerContext = nullptr,
                  CurrentRegistersFn registerReader = nullptr) noexcept;
    ~DispatchScope();
    DispatchScope(const DispatchScope &) = delete;
    DispatchScope &operator=(const DispatchScope &) = delete;

    // The current runtime loop observes ctx.pc immediately after the guest
    // call; it is both actual post-call PC and the selected-next-PC here.
    void finish(uint32_t actualPc, uint32_t actualRa, uint32_t actualSp,
                DispatchReason reason = DispatchReason::ReturnFromFunction) noexcept;
    uint64_t id() const noexcept { return m_id; }

private:
    uint64_t m_previous;
    uint64_t m_id;
    uint32_t m_previousTarget;
    uint32_t m_target;
    uint32_t m_entryPc;
    const uint32_t *m_previousLivePc;
    const void *m_previousRegisterContext;
    CurrentRegistersFn m_previousRegisterReader;
    const uint8_t *m_rdram;
    bool m_previousScopeActive;
    bool m_finished;
};

class WriteScope final
{
public:
    WriteScope(const uint8_t *rdram, uint32_t pc, uint32_t guestAddress,
               uint32_t physicalOffset, uint32_t width, uint32_t route) noexcept
        : m_id(kUnknownId), m_dispatch(kUnknownId), m_bytes(nullptr), m_route(route),
          m_clippedBytes(0u), m_outputValid(false), m_finished(false)
    {
        // This physical-range gate precedes every atomic/configuration call on
        // generic Store* paths that cannot touch the exact source range.
        if (overlaps(physicalOffset, width) || overlapsInputWatch(physicalOffset, width))
            begin(rdram, pc, guestAddress, physicalOffset, width);
    }
    ~WriteScope() { if ((m_id != kUnknownId || hasInputWatches()) && !m_finished) abort(); }
    WriteScope(const WriteScope &) = delete;
    WriteScope &operator=(const WriteScope &) = delete;
    void finish() noexcept { if ((m_id != kUnknownId || hasInputWatches()) && !m_finished) complete(); }
    uint64_t id() const noexcept { return m_id; }

private:
    struct InputWatch final
    {
        uint64_t id = kUnknownId;
        uint64_t dispatch = kUnknownId;
        const uint8_t *bytes = nullptr;
        uint32_t physical = 0u;
        uint32_t slot = 0u;
        uint32_t overlapOffset = 0u;
        uint32_t overlapBytes = 0u;
        bool finished = false;
    };
    static constexpr bool overlaps(uint32_t physicalOffset, uint32_t width) noexcept
    {
        const uint64_t begin = physicalOffset;
        const uint64_t end = begin + width;
        return begin < uint64_t{kSourceRangeStart + kSourceRangeBytes} &&
               end > uint64_t{kSourceRangeStart};
    }
    void begin(const uint8_t *rdram, uint32_t pc, uint32_t guestAddress,
               uint32_t physicalOffset, uint32_t width) noexcept;
    void complete() noexcept;
    void abort() noexcept;
    static constexpr bool overlapsInputWatch(uint32_t physicalOffset, uint32_t width) noexcept
    {
        const uint64_t begin = physicalOffset;
        const uint64_t end = begin + width;
        return (begin < uint64_t{kHelperInput20Address + kHelperInputWatchBytes} &&
                end > uint64_t{kHelperInput20Address}) ||
               (begin < uint64_t{kHelperInput50Address + kHelperInputWatchBytes} &&
                end > uint64_t{kHelperInput50Address});
    }
    bool hasInputWatches() const noexcept;
    void beginInputWatch(InputWatchSlot slot, const uint8_t *rdram, uint32_t pc,
                         uint32_t guestAddress, uint32_t physicalOffset,
                         uint32_t width) noexcept;
    void completeInputWatch(InputWatch &watch) noexcept;
    void abortInputWatch(InputWatch &watch) noexcept;
    uint64_t m_id;
    uint64_t m_dispatch;
    const uint8_t *m_bytes;
    uint32_t m_route;
    uint32_t m_clippedBytes;
    bool m_outputValid;
    InputWatch m_inputWatches[2];
    bool m_finished;
};

bool resetForTest(bool enabled) noexcept;
#else
class DispatchScope final
{
public:
    DispatchScope(uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *,
                  DispatchReason = DispatchReason::Unknown,
                  const uint32_t * = nullptr, const void * = nullptr,
                  CurrentRegistersFn = nullptr) noexcept {}
    void finish(uint32_t, uint32_t, uint32_t,
                DispatchReason = DispatchReason::ReturnFromFunction) noexcept {}
    uint64_t id() const noexcept { return kUnknownId; }
};
class WriteScope final
{
public:
    WriteScope(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
    void finish() noexcept {}
    uint64_t id() const noexcept { return kUnknownId; }
};
inline bool resetForTest(bool) noexcept { return true; }
inline void controlInputs(uint32_t, uint32_t, uint32_t, uint64_t,
                          uint32_t, uint32_t, bool) noexcept {}
inline void preemptCheck(uint32_t, uint32_t, uint32_t) noexcept {}
inline void preemptSuppressed(uint32_t, uint32_t) noexcept {}
inline void preemptYield() noexcept {}
inline void helperEntry(uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *) noexcept {}
inline void sourceSnapshot(uint64_t, uint32_t, uint32_t, const uint8_t *, uint32_t) noexcept {}
inline bool configure(bool) noexcept { return true; }
inline bool configured() noexcept { return false; }
inline uint64_t currentId() noexcept { return kUnknownId; }
#endif
} // namespace rrv::m2guest

#endif
