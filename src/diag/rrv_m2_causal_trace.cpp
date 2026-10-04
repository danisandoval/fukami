#include "rrv_m2_causal_trace.h"
#include "rrv_m2_dma_provenance.h"
#include "rrv_m2_guest_provenance.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace rrv::m2causal
{
namespace
{
constexpr uint32_t kSchemaVersion = 1u;
constexpr uint32_t kHeaderBytes = 80u;
constexpr uint32_t kRecordBytes = 48u;
constexpr uint32_t kDefaultTargetFields = 4u;
constexpr uint64_t kUnknownField = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kClosedBit = uint64_t{1} << 63u;
constexpr uint64_t kSequenceMask = ~kClosedBit;
constexpr size_t kCapacity = 131072u;
constexpr size_t kPathBytes = 1024u;

enum class State : uint32_t { Uninitialized, Disabled, Prearmed, Collecting, Complete, Overflow };
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<State>::is_always_lock_free);

struct Record
{
    uint64_t sequence;
    uint32_t type;
    uint32_t source;
    uint64_t guestField;
    uint64_t a;
    uint64_t b;
    uint64_t c;
};
static_assert(sizeof(Record) == kRecordBytes, "causal trace record layout must stay fixed");

struct Slot
{
    std::atomic<uint64_t> ready{0u};
    Record record{};
};

// This array is intentionally static/preallocated. initialize* touches every
// slot before guest threads begin to avoid first-touch faults in the interval.
// Operating-system paging remains outside the diagnostic's control.
std::array<Slot, kCapacity> g_slots{};
std::atomic<State> g_state{State::Uninitialized};
std::atomic<uint64_t> g_nextSequence{0u};
std::atomic<uint64_t> g_closedCount{0u};
std::atomic<uint64_t> g_overflowSequence{0u};
std::atomic<uint64_t> g_activeField{kUnknownField};
std::atomic<uint32_t> g_completedFields{0u};
std::atomic<uint32_t> g_targetFields{kDefaultTargetFields};
std::array<char, kPathBytes> g_deferredPath{};

void putU32(FILE *file, uint32_t value) noexcept
{
    const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8u),
                              static_cast<uint8_t>(value >> 16u), static_cast<uint8_t>(value >> 24u)};
    (void)std::fwrite(bytes, sizeof(bytes), 1u, file);
}

void putU64(FILE *file, uint64_t value) noexcept
{
    const uint8_t bytes[8] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8u),
        static_cast<uint8_t>(value >> 16u), static_cast<uint8_t>(value >> 24u),
        static_cast<uint8_t>(value >> 32u), static_cast<uint8_t>(value >> 40u),
        static_cast<uint8_t>(value >> 48u), static_cast<uint8_t>(value >> 56u)};
    (void)std::fwrite(bytes, sizeof(bytes), 1u, file);
}

bool writeOk(FILE *file) noexcept
{
    return file && std::ferror(file) == 0;
}

uint32_t parseTargetFields() noexcept
{
    const char *value = std::getenv("RRV_M2_CAUSAL_TRACE_FIELDS");
    if (!value || !value[0])
        return kDefaultTargetFields;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0u || parsed > kDefaultTargetFields)
        return 0u;
    return static_cast<uint32_t>(parsed);
}

bool initialize(bool enableTrace, const char *path, uint32_t targetFields) noexcept
{
    State expected = State::Uninitialized;
    if (!g_state.compare_exchange_strong(expected, State::Disabled, std::memory_order_acq_rel))
        return g_state.load(std::memory_order_acquire) != State::Overflow;

    if (!enableTrace)
        return true;
    if (!path || !path[0] || std::strlen(path) >= g_deferredPath.size() || targetFields == 0u ||
        targetFields > kDefaultTargetFields)
        return false;

    for (Slot &slot : g_slots)
    {
        slot.record = Record{};
        slot.ready.store(0u, std::memory_order_relaxed);
    }
    std::memcpy(g_deferredPath.data(), path, std::strlen(path) + 1u);
    g_targetFields.store(targetFields, std::memory_order_relaxed);
    g_nextSequence.store(0u, std::memory_order_relaxed);
    g_closedCount.store(0u, std::memory_order_relaxed);
    g_overflowSequence.store(0u, std::memory_order_relaxed);
    g_activeField.store(kUnknownField, std::memory_order_relaxed);
    g_completedFields.store(0u, std::memory_order_relaxed);
    // Storage is fully initialized before guest threads exist, but the live
    // trace must not record cold boot.  beginSelectorEdge() later performs the
    // one-shot arm without clearing, allocating, or touching pages.
    g_state.store(State::Prearmed, std::memory_order_release);
    return true;
}

} // namespace

bool initializeFromEnvironment() noexcept
{
    const char *path = std::getenv("RRV_M2_CAUSAL_TRACE");
    const bool on = path && path[0] && std::strcmp(path, "0") != 0;
    const bool result = initialize(on, on ? path : nullptr, on ? parseTargetFields() : kDefaultTargetFields);
    const char *provenance = std::getenv("RRV_M2_DMA_PROVENANCE");
    const char *guest = std::getenv("RRV_M2_GUEST_PROVENANCE");
    rrv::m2guest::configure(result && on && guest && std::strcmp(guest, "1") == 0);
    return rrv::m2prov::initialize(result && on && provenance && std::strcmp(provenance, "1") == 0) && result;
}

bool collecting() noexcept
{
    return g_state.load(std::memory_order_acquire) == State::Collecting;
}

bool acceptingDiagnostics() noexcept
{
    const State state = g_state.load(std::memory_order_acquire);
    return state == State::Prearmed || state == State::Collecting;
}

void fail() noexcept
{
    const uint64_t sequence = (g_nextSequence.load(std::memory_order_relaxed) & kSequenceMask) + 1u;
    uint64_t expected = 0u;
    (void)g_overflowSequence.compare_exchange_strong(expected, sequence, std::memory_order_relaxed);
    g_state.store(State::Overflow, std::memory_order_release);
}

bool enabled() noexcept
{
    return g_state.load(std::memory_order_acquire) == State::Collecting ||
           g_state.load(std::memory_order_acquire) == State::Complete;
}

bool overflowed() noexcept
{
    return g_state.load(std::memory_order_acquire) == State::Overflow;
}

uint64_t recordCount() noexcept
{
    const uint64_t cursor = g_nextSequence.load(std::memory_order_acquire);
    const uint64_t count = (cursor & kClosedBit) != 0u
                               ? g_closedCount.load(std::memory_order_acquire)
                               : (cursor & kSequenceMask);
    return count > kCapacity ? kCapacity : count;
}

void event(EventType type, Source source, uint64_t a, uint64_t b, uint64_t c) noexcept
{
    if (g_state.load(std::memory_order_acquire) != State::Collecting)
        return;

    const uint64_t previous = g_nextSequence.fetch_add(1u, std::memory_order_acq_rel);
    if ((previous & kClosedBit) != 0u)
        return;
    const uint64_t sequence = previous + 1u;
    if (sequence > kCapacity)
    {
        uint64_t expected = 0u;
        (void)g_overflowSequence.compare_exchange_strong(expected, sequence,
                                                          std::memory_order_relaxed);
        State collecting = State::Collecting;
        (void)g_state.compare_exchange_strong(collecting, State::Overflow,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
        return;
    }

    Slot &slot = g_slots[static_cast<size_t>(sequence - 1u)];
    slot.record = Record{sequence, static_cast<uint32_t>(type), static_cast<uint32_t>(source),
                         g_activeField.load(std::memory_order_relaxed), a, b, c};
    slot.ready.store(sequence, std::memory_order_release);
}

void packetFingerprint(Source source, const uint8_t *bytes, uint32_t sizeBytes,
                       uint64_t site) noexcept
{
    if (g_state.load(std::memory_order_relaxed) != State::Collecting || !bytes)
        return;
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t index = 0u; index < sizeBytes; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    event(EventType::Path2PayloadFingerprint, source, hash, sizeBytes, site);
}

void beginSelectorEdge(uint32_t previousSelector, uint32_t currentSelector, uint64_t guestTick) noexcept
{
    State expected = State::Prearmed;
    if (!g_state.compare_exchange_strong(expected, State::Disabled, std::memory_order_acq_rel))
        return;
    // No other writer can pass the Collecting gate until this first record is
    // complete.  The selector edge therefore has observation sequence 1.
    Slot &slot = g_slots[0];
    slot.record = Record{1u, static_cast<uint32_t>(EventType::SelectorEdge),
                         static_cast<uint32_t>(Source::GuestExecution), kUnknownField,
                         previousSelector, currentSelector, guestTick};
    slot.ready.store(1u, std::memory_order_release);
    g_nextSequence.store(1u, std::memory_order_release);
    g_state.store(State::Collecting, std::memory_order_release);
}

void originBoundary(uint64_t guestFieldId, uint64_t gsFieldEpochId, uint32_t parity) noexcept
{
    // The mixed epoch closes before this call.  Keep its identity only as a
    // boundary event; subsequent serialized consumer events own field+1.
    event(EventType::OriginBoundary, Source::Backend, guestFieldId, gsFieldEpochId, parity);
    g_activeField.store(guestFieldId + 1u, std::memory_order_release);
}

void beginCompleteField(uint64_t guestFieldId, uint64_t gsFieldEpochId, uint32_t parity) noexcept
{
    if (g_state.load(std::memory_order_acquire) != State::Collecting)
        return;
    g_activeField.store(guestFieldId, std::memory_order_release);
    event(EventType::CompleteFieldBegin, Source::Replay, guestFieldId, gsFieldEpochId, parity);
}

void completeField(uint64_t guestFieldId, uint64_t gsFieldEpochId, uint32_t parity) noexcept
{
    if (g_state.load(std::memory_order_acquire) != State::Collecting)
        return;
    event(EventType::CompleteField, Source::Backend, guestFieldId, gsFieldEpochId, parity);
    const uint32_t complete = g_completedFields.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (complete >= g_targetFields.load(std::memory_order_relaxed))
    {
        // Close freezes the accepted reservation count.  A writer that had
        // already reserved a slot must publish it before deferred export; an
        // unfinished slot fails the diagnostic rather than waiting or silently
        // omitting it.  Writers arriving after this operation are ignored.
        const uint64_t previous = g_nextSequence.fetch_or(kClosedBit, std::memory_order_acq_rel);
        g_closedCount.store(previous & kSequenceMask, std::memory_order_release);
        if (g_overflowSequence.load(std::memory_order_acquire) != 0u)
        {
            g_state.store(State::Overflow, std::memory_order_release);
            return;
        }
        State collecting = State::Collecting;
        (void)g_state.compare_exchange_strong(collecting, State::Complete,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
    }
    else
    {
        g_activeField.store(guestFieldId + 1u, std::memory_order_release);
    }
}

void beginReplay() noexcept
{
    State expected = State::Prearmed;
    if (!g_state.compare_exchange_strong(expected, State::Disabled, std::memory_order_acq_rel))
        return;
    Slot &slot = g_slots[0];
    slot.record = Record{1u, static_cast<uint32_t>(EventType::ReplayBegin),
                         static_cast<uint32_t>(Source::Replay), kUnknownField, 0u, 0u, 0u};
    slot.ready.store(1u, std::memory_order_release);
    g_nextSequence.store(1u, std::memory_order_release);
    g_state.store(State::Collecting, std::memory_order_release);
}

void endReplay() noexcept
{
    event(EventType::ReplayEnd, Source::Replay,
          g_completedFields.load(std::memory_order_relaxed), 0u, 0u);
}

bool dumpDeferred(const char * /*error*/) noexcept
{
    if (g_state.load(std::memory_order_acquire) == State::Disabled)
        return true;
    if (g_state.load(std::memory_order_acquire) != State::Complete ||
        g_overflowSequence.load(std::memory_order_acquire) != 0u || overflowed() ||
        g_completedFields.load(std::memory_order_acquire) != g_targetFields.load(std::memory_order_relaxed) ||
        !g_deferredPath[0])
        return false;

    const uint64_t count = recordCount();
    if (count == 0u || count > kCapacity)
        return false;
    for (uint64_t i = 0u; i < count; ++i)
    {
        if (g_slots[static_cast<size_t>(i)].ready.load(std::memory_order_acquire) != i + 1u)
            return false;
    }

    FILE *file = std::fopen(g_deferredPath.data(), "wb");
    if (!file)
        return false;
    constexpr uint8_t magic[8] = {'R', 'R', 'V', 'M', 'C', 'A', 'S', '1'};
    (void)std::fwrite(magic, sizeof(magic), 1u, file);
    putU32(file, kSchemaVersion);
    putU32(file, kHeaderBytes);
    putU32(file, kRecordBytes);
    putU32(file, 0x2u); // complete; overflow is a failed export, never serialized as pass.
    putU64(file, kCapacity);
    putU64(file, count);
    putU64(file, 0u);
    putU64(file, 1u);
    putU64(file, count);
    putU32(file, g_completedFields.load(std::memory_order_relaxed));
    putU32(file, g_targetFields.load(std::memory_order_relaxed));
    putU32(file, 0u);
    putU32(file, 0u);
    for (uint64_t i = 0u; i < count; ++i)
    {
        const Record &record = g_slots[static_cast<size_t>(i)].record;
        putU64(file, record.sequence);
        putU32(file, record.type);
        putU32(file, record.source);
        putU64(file, record.guestField);
        putU64(file, record.a);
        putU64(file, record.b);
        putU64(file, record.c);
    }
    const bool writesOk = writeOk(file);
    const int closeResult = std::fclose(file);
    return writesOk && closeResult == 0;
}

Scope::Scope(EventType enterType, EventType exitType, Source source, uint64_t a, uint64_t b,
             uint64_t c) noexcept
    : m_exit(exitType), m_source(source), m_a(a), m_b(b), m_c(c)
{
    event(enterType, source, a, b, c);
}

Scope::~Scope()
{
    // A scope may start before the selector edge and finish after it. Keep
    // that post-arm exit as an explicitly partial scope, never silently omit it.
    event(m_exit, m_source, m_a, m_b, m_c);
}

bool resetForTest(bool enableTrace, const char *path, uint32_t targetFields) noexcept
{
    g_state.store(State::Uninitialized, std::memory_order_release);
    g_deferredPath.fill('\0');
    return initialize(enableTrace, path ? path : "rrv-m2-causal-test.bin", targetFields);
}
} // namespace rrv::m2causal
