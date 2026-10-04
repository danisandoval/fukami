#include "rrv_m2_guest_provenance.h"

#if defined(RRV_M2_GUEST_PROVENANCE)

#include <atomic>
#include <cstdint>
#include <limits>

namespace rrv::m2guest
{
namespace
{
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint32_t kRangeEnd = kSourceRangeStart + kSourceRangeBytes;
constexpr uint32_t kEntryRetained = 1u;
constexpr uint32_t kUnknownEntry = 2u;
constexpr uint32_t kNormalFinish = 4u;
constexpr uint32_t kAborted = 8u;
constexpr uint32_t kHelperTarget = 0x0021e698u;
constexpr uint32_t kHelperInputBytes = 8u;
constexpr uint32_t kRdramBytes = 0x02000000u;
constexpr uint32_t kHelperInput20 = 0x20u;
constexpr uint32_t kHelperInput50 = 0x50u;
constexpr uint32_t kInputWatchAddresses[] = {
    kHelperInput20Address, kHelperInput50Address,
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
std::atomic<uint64_t> g_nextDispatch{1u};
std::atomic<uint64_t> g_nextWrite{1u};
std::atomic<uint64_t> g_nextHelper{1u};
std::atomic<uint64_t> g_nextInputWrite{1u};
std::atomic<uint32_t> g_configured{0u};
thread_local uint64_t g_currentDispatch = kUnknownId;
thread_local uint32_t g_currentTarget = 0u;
thread_local const uint32_t *g_currentLivePc = nullptr;
thread_local const void *g_currentRegisterContext = nullptr;
thread_local CurrentRegistersFn g_currentRegisterReader = nullptr;
thread_local bool g_dispatchScopeActive = false;

constexpr uint64_t pair32(uint32_t low, uint32_t high) noexcept
{
    return uint64_t{low} | (uint64_t{high} << 32u);
}

bool active() noexcept
{
    // Before the semantic edge, B and C take the same single state check.
    // Do not read the enabled flag for an interval which cannot emit records.
    return m2causal::collecting() && g_configured.load(std::memory_order_acquire) != 0u;
}

bool selectedTarget() noexcept
{
    return g_currentTarget == 0x0021df9cu || g_currentTarget == 0x00295e38u ||
           g_currentTarget == 0x0029c460u;
}

uint32_t livePc() noexcept
{
    return g_currentLivePc ? *g_currentLivePc : 0u;
}

uint64_t fnv1a(const uint8_t *bytes, uint32_t size) noexcept
{
    uint64_t hash = kFnvOffset;
    if (!bytes)
        return hash;
    for (uint32_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

void emit(Event event, uint64_t a, uint64_t b, uint64_t c) noexcept
{
    if (active())
        m2causal::event(static_cast<m2causal::EventType>(static_cast<uint32_t>(event)),
                         m2causal::Source::GuestExecution, a, b, c);
}

void snapshot(Event event, uint64_t id, const uint8_t *rdram, uint32_t flags) noexcept
{
    if (rdram && selectedTarget())
        emit(event, id, fnv1a(rdram + kSourceRangeStart, kSourceRangeBytes),
             pair32(flags, kSourceRangeBytes));
}

bool currentRegisters(uint32_t &ra, uint32_t &sp) noexcept
{
    return g_currentRegisterReader && g_currentRegisterContext &&
           g_currentRegisterReader(g_currentRegisterContext, ra, sp);
}

bool preemptEligible() noexcept
{
    // A retained dispatch keeps the static target filter.  An id-zero scope
    // is explicitly a carry-in observation: no entry was retained, so its
    // target must not hide an already-running dispatcher decision.
    return g_currentDispatch != kUnknownId ? selectedTarget() : g_dispatchScopeActive;
}

uint64_t readLe64(const uint8_t *bytes) noexcept
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < kHelperInputBytes; ++index)
        value |= uint64_t{bytes[index]} << (index * 8u);
    return value;
}

bool helperPhysical(uint32_t guestAddress, uint32_t &physical) noexcept
{
    // Match only physical/KSEG0/KSEG1 aliases.  A generic mask would make
    // unrelated high virtual segments look like RDRAM and could dereference
    // an invalid host offset in this diagnostic.
    const uint32_t segment = guestAddress & 0xe0000000u;
    if (segment != 0u && segment != 0x80000000u && segment != 0xa0000000u)
        return false;
    physical = guestAddress & 0x1fffffffu;
    return physical <= kRdramBytes - kHelperInputBytes;
}

void emitPreemptContext() noexcept
{
    uint32_t ra = 0u;
    uint32_t sp = 0u;
    const uint32_t valid = currentRegisters(ra, sp) ? 1u : 0u;
    emit(Event::PreemptContext, g_currentDispatch, pair32(ra, sp),
         pair32(g_currentTarget, valid));
}
} // namespace

bool configure(bool enabled) noexcept
{
    g_configured.store(enabled ? 1u : 0u, std::memory_order_release);
    g_nextDispatch.store(1u, std::memory_order_release);
    g_nextWrite.store(1u, std::memory_order_release);
    g_nextHelper.store(1u, std::memory_order_release);
    g_nextInputWrite.store(1u, std::memory_order_release);
    g_currentDispatch = kUnknownId;
    g_currentTarget = 0u;
    g_currentLivePc = nullptr;
    g_currentRegisterContext = nullptr;
    g_currentRegisterReader = nullptr;
    g_dispatchScopeActive = false;
    return true;
}

bool configured() noexcept
{
    return g_configured.load(std::memory_order_acquire) != 0u;
}

uint64_t currentId() noexcept
{
    return g_currentDispatch;
}

DispatchScope::DispatchScope(uint32_t target, uint32_t entryPc, uint32_t entryRa,
                             uint32_t entrySp, const uint8_t *rdram,
                             DispatchReason reason, const uint32_t *livePc,
                             const void *registerContext,
                             CurrentRegistersFn registerReader) noexcept
    : m_previous(g_currentDispatch), m_id(kUnknownId), m_previousTarget(g_currentTarget),
      m_target(target), m_entryPc(entryPc), m_previousLivePc(g_currentLivePc),
      m_previousRegisterContext(g_currentRegisterContext),
      m_previousRegisterReader(g_currentRegisterReader), m_rdram(rdram),
      m_previousScopeActive(g_dispatchScopeActive), m_finished(false)
{
    g_currentDispatch = kUnknownId;
    g_currentTarget = target;
    g_currentLivePc = livePc;
    g_currentRegisterContext = registerContext;
    g_currentRegisterReader = registerReader;
    g_dispatchScopeActive = true;
    if (!active())
        return;
    m_id = g_nextDispatch.fetch_add(1u, std::memory_order_relaxed);
    g_currentDispatch = m_id;
    emit(Event::DispatchEntry, m_id, pair32(target, entryPc), pair32(entryRa, entrySp));
    emit(Event::DispatchSelection, m_id, pair32(static_cast<uint32_t>(reason), kEntryRetained), 0u);
    snapshot(Event::EntryRangeSnapshot, m_id, m_rdram, kEntryRetained);
}

DispatchScope::~DispatchScope()
{
    if (!m_finished && active())
    {
        const uint32_t flags = (m_id == kUnknownId ? kUnknownEntry : kEntryRetained) | kAborted;
        emit(Event::DispatchAborted, m_id, pair32(m_target, m_entryPc), flags);
    }
    g_currentDispatch = m_previous;
    g_currentTarget = m_previousTarget;
    g_currentLivePc = m_previousLivePc;
    g_currentRegisterContext = m_previousRegisterContext;
    g_currentRegisterReader = m_previousRegisterReader;
    g_dispatchScopeActive = m_previousScopeActive;
}

void DispatchScope::finish(uint32_t actualPc, uint32_t actualRa, uint32_t actualSp,
                           DispatchReason reason) noexcept
{
    if (m_finished)
        return;
    m_finished = true;
    if (!active())
        return;
    const uint32_t flags = m_id == kUnknownId ? kUnknownEntry : kEntryRetained;
    emit(Event::DispatchExit, m_id, pair32(actualPc, actualRa), pair32(actualSp, actualPc));
    emit(Event::DispatchSelection, m_id, pair32(static_cast<uint32_t>(reason), flags), 0u);
    snapshot(Event::ExitRangeSnapshot, m_id, m_rdram, flags | kNormalFinish);
}

void WriteScope::begin(const uint8_t *rdram, uint32_t pc, uint32_t guestAddress,
                       uint32_t physicalOffset, uint32_t width) noexcept
{
    if (!active())
        return;
    const uint64_t begin = physicalOffset;
    const uint64_t end = begin + width;
    const uint64_t clippedBegin = begin > kSourceRangeStart ? begin : kSourceRangeStart;
    const uint64_t clippedEnd = end < kRangeEnd ? end : kRangeEnd;
    if (clippedBegin < clippedEnd)
    {
        m_id = g_nextWrite.fetch_add(1u, std::memory_order_relaxed);
        m_dispatch = g_currentDispatch;
        const uint64_t physical = clippedBegin;
        if (!rdram || physical > std::numeric_limits<uint32_t>::max())
        {
            emit(Event::WriteFailed, m_id, m_dispatch,
                 pair32(m_route, static_cast<uint32_t>(WriteFailure::InvalidRange)));
        }
        else
        {
            m_bytes = rdram + physical;
            m_clippedBytes = static_cast<uint32_t>(clippedEnd - clippedBegin);
            m_outputValid = true;
            emit(Event::WriteBegin, m_id, pair32(pc, guestAddress), pair32(physicalOffset, width));
            emit(Event::WriteBefore, m_id, m_dispatch, fnv1a(m_bytes, m_clippedBytes));
        }
    }
    beginInputWatch(InputWatchSlot::HelperInput20, rdram, pc, guestAddress, physicalOffset, width);
    beginInputWatch(InputWatchSlot::HelperInput50, rdram, pc, guestAddress, physicalOffset, width);
}

void WriteScope::abort() noexcept
{
    if (active() && m_id != kUnknownId && m_outputValid)
        emit(Event::WriteFailed, m_id, m_dispatch,
             pair32(m_route, static_cast<uint32_t>(WriteFailure::Aborted)));
    for (InputWatch &watch : m_inputWatches)
        abortInputWatch(watch);
    m_finished = true;
}

void WriteScope::complete() noexcept
{
    m_finished = true;
    if (active() && m_id != kUnknownId && m_outputValid)
        emit(Event::WriteAfter, m_id, fnv1a(m_bytes, m_clippedBytes),
             pair32(m_route, m_clippedBytes));
    for (InputWatch &watch : m_inputWatches)
        completeInputWatch(watch);
}

bool WriteScope::hasInputWatches() const noexcept
{
    return m_inputWatches[0].id != kUnknownId || m_inputWatches[1].id != kUnknownId;
}

void WriteScope::beginInputWatch(InputWatchSlot slot, const uint8_t *rdram, uint32_t pc,
                                 uint32_t guestAddress, uint32_t physicalOffset,
                                 uint32_t width) noexcept
{
    const uint32_t slotIndex = static_cast<uint32_t>(slot);
    const uint64_t writeBegin = physicalOffset;
    const uint64_t writeEnd = writeBegin + width;
    const uint64_t watchBegin = kInputWatchAddresses[slotIndex];
    const uint64_t watchEnd = watchBegin + kHelperInputWatchBytes;
    const uint64_t overlapBegin = writeBegin > watchBegin ? writeBegin : watchBegin;
    const uint64_t overlapEnd = writeEnd < watchEnd ? writeEnd : watchEnd;
    if (overlapBegin >= overlapEnd)
        return;

    InputWatch &watch = m_inputWatches[slotIndex];
    watch.id = g_nextInputWrite.fetch_add(1u, std::memory_order_relaxed);
    watch.dispatch = g_currentDispatch;
    watch.physical = static_cast<uint32_t>(watchBegin);
    watch.slot = slotIndex;
    watch.overlapOffset = static_cast<uint32_t>(overlapBegin - watchBegin);
    watch.overlapBytes = static_cast<uint32_t>(overlapEnd - overlapBegin);
    emit(Event::InputWriteBegin, watch.id, pair32(pc, guestAddress), pair32(physicalOffset, width));
    emit(Event::InputWriteContext, watch.id, watch.dispatch, pair32(watch.slot, m_route));
    emit(Event::InputWriteOverlap, watch.id, pair32(watch.physical, watch.overlapOffset),
         watch.overlapBytes);
    if (!rdram)
    {
        emit(Event::InputWriteFailed, watch.id, watch.dispatch,
             pair32(static_cast<uint32_t>(InputWriteFailure::InvalidRange), watch.slot));
        watch.finished = true;
        return;
    }
    watch.bytes = rdram + watch.physical;
    emit(Event::InputWriteBefore, watch.id, watch.physical, readLe64(watch.bytes));
}

void WriteScope::completeInputWatch(InputWatch &watch) noexcept
{
    if (watch.id == kUnknownId || watch.finished)
        return;
    watch.finished = true;
    if (active())
        emit(Event::InputWriteAfter, watch.id, watch.physical, readLe64(watch.bytes));
}

void WriteScope::abortInputWatch(InputWatch &watch) noexcept
{
    if (watch.id == kUnknownId || watch.finished)
        return;
    watch.finished = true;
    if (active())
        emit(Event::InputWriteFailed, watch.id, watch.dispatch,
             pair32(static_cast<uint32_t>(InputWriteFailure::Aborted), watch.slot));
}

void controlInputs(uint32_t target, uint32_t s0, uint32_t s1, uint64_t s7,
                   uint32_t address, uint32_t value, bool available) noexcept
{
    const uint64_t id = g_currentDispatch;
    if (id == kUnknownId || !active() || target != g_currentTarget ||
        (target != 0x00295e38u && target != 0x0029c460u))
        return;
    const uint32_t validMask = target == 0x00295e38u ? 1u : 0u;
    emit(Event::ControlRegisters, id, pair32(target, s0), pair32(s1, validMask));
    emit(Event::ControlS7, id, s7, validMask);
    emit(Event::ControlByte, id, pair32(address, value), available ? 1u : 0u);
}

void preemptCheck(uint32_t counterBefore, uint32_t waiterCount, uint32_t interval) noexcept
{
    if (!preemptEligible() || !active())
        return;
    emit(Event::PreemptCheck, g_currentDispatch, pair32(counterBefore, waiterCount),
         pair32(interval, livePc()));
    emitPreemptContext();
}

void preemptSuppressed(uint32_t depth, uint32_t reason) noexcept
{
    if (!preemptEligible() || !active())
        return;
    emit(Event::PreemptSuppressed, g_currentDispatch, pair32(depth, reason),
         pair32(g_currentTarget, livePc()));
    emitPreemptContext();
}

void preemptYield() noexcept
{
    if (!preemptEligible() || !active())
        return;
    // There is no source-proven pending scheduler state at this hook.  The
    // low/high halves of c are therefore (pending_state=0, valid=false).
    emit(Event::PreemptYield, g_currentDispatch, pair32(livePc(), g_currentTarget), 0u);
}

void helperEntry(uint32_t callerRa, uint32_t a0, uint32_t a1, uint32_t sp,
                 const uint8_t *rdram) noexcept
{
    if (!active())
        return;
    const uint64_t helper = g_nextHelper.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t input20Address = a1 + kHelperInput20;
    const uint32_t input50Address = a1 + kHelperInput50;
    uint32_t input20Physical = 0u;
    uint32_t input50Physical = 0u;
    const bool valid20 = rdram && helperPhysical(input20Address, input20Physical);
    const bool valid50 = rdram && helperPhysical(input50Address, input50Physical);
    const uint32_t validMask = (valid20 ? 1u : 0u) | (valid50 ? 2u : 0u);

    emit(Event::HelperEntry, helper, pair32(callerRa, a0), pair32(a1, sp));
    emit(Event::HelperLink, helper, g_currentDispatch, pair32(validMask, kHelperTarget));
    if (valid20)
        emit(Event::HelperInput20, helper, pair32(input20Physical, kHelperInputBytes),
             readLe64(rdram + input20Physical));
    if (valid50)
        emit(Event::HelperInput50, helper, pair32(input50Physical, kHelperInputBytes),
             readLe64(rdram + input50Physical));
}

void sourceSnapshot(uint64_t rootId, uint32_t space, uint32_t guestAddress,
                    const uint8_t *bytes, uint32_t sizeBytes) noexcept
{
    if (rootId == kUnknownId || !active() || (space != 0u && space != 2u && space != 4u))
        return;
    const uint64_t begin = guestAddress;
    const uint64_t end = begin + sizeBytes;
    const uint64_t clippedBegin = begin > kSourceRangeStart ? begin : kSourceRangeStart;
    const uint64_t clippedEnd = end < kRangeEnd ? end : kRangeEnd;
    if (!bytes || clippedBegin >= clippedEnd)
        return;
    const uint32_t clippedBytes = static_cast<uint32_t>(clippedEnd - clippedBegin);
    emit(Event::SourceSnapshot, rootId, pair32(static_cast<uint32_t>(clippedBegin), clippedBytes),
         fnv1a(bytes + (clippedBegin - begin), clippedBytes));
}

bool resetForTest(bool enabled) noexcept
{
    return configure(enabled);
}
} // namespace rrv::m2guest

#endif
