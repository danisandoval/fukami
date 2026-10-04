#include "rrv_m2p_game001_fail_closed.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <utility>

namespace rrv::m2pgame001 {
namespace {

constexpr size_t kHistoryCapacity = 16u;
constexpr size_t kOwnerFrameCapacity = 8u;
constexpr uint32_t kTextBegin = 0x00200000u;
constexpr uint32_t kTextEnd = 0x002e1a10u;
constexpr uint32_t kVuTextBegin = 0x002e1b00u;
constexpr uint32_t kVuTextEnd = 0x002e2200u;
// The bounded slice only needs to distinguish the observed BSS target from
// executable text.  Keep this exact range recorded by the static census.
constexpr uint32_t kBssBegin = 0x00335100u;
constexpr uint32_t kBssEnd = 0x01e49c2cu;

enum class TargetClass : uint8_t {
    KnownAotEntry,
    KnownAotReentry,
    ExecutableUnregistered,
    Data,
    Unmapped,
};

struct SavedRaReceipt {
    bool valid = false;
    const R5900Context *ctx = nullptr;
    uint32_t entrySp = 0u;
    uint32_t incomingRa = 0u;
    uint64_t slotAfterSave = 0u;
    uint64_t slotBeforeRestore = 0u;
    uint32_t restoredRa = 0u;
    uint32_t finalOperand = 0u;
};

struct DispatchHistory {
    std::array<uint32_t, kHistoryCapacity> entries{};
    size_t first = 0u;
    size_t size = 0u;

    void append(uint32_t pc) noexcept {
        if (size < entries.size()) {
            entries[(first + size++) % entries.size()] = pc;
            return;
        }
        entries[first] = pc;
        first = (first + 1u) % entries.size();
    }
};

struct ReentryEvent {
    bool valid = false;
    const R5900Context *ctx = nullptr;
    uint32_t pc = 0u;
    uint32_t sp = 0u;
    uint32_t ra = 0u;
    uint64_t frameGeneration = 0u;
    bool activeFrame = false;
    uint32_t arrivalSource = 0u;
};

struct ReentryReceipt {
    bool frameOverflow = false;
};

// Latest observations at each of the three bounded handoff sites. These have
// no inferred frame identity: a mismatched SP must remain visible even when
// the frame correlator cannot attribute it. Sequence orders the independent
// records; it does not establish that they belong to one invocation.
struct RawReentryEvent {
    const R5900Context *ctx = nullptr;
    uint32_t pc = 0u;
    uint32_t sp = 0u;
    uint32_t ra = 0u;
    uint64_t sequence = 0u;
    uint32_t arrivalSource = 0u;
};

struct OwnerFrame {
    bool valid = false;
    bool active = false;
    const R5900Context *ctx = nullptr;
    uint32_t postPrologueSp = 0u;
    uint64_t generation = 0u;
    uint64_t activity = 0u;
    ReentryEvent primary;
    ReentryEvent epilogue;
    ReentryEvent preempt268b38;
    ReentryEvent dispatcher268910;
    ReentryEvent label268910;
};

thread_local DispatchHistory g_history;
thread_local SavedRaReceipt g_savedRa;
thread_local ReentryReceipt g_reentry;
thread_local RawReentryEvent g_rawPreempt268b38;
thread_local RawReentryEvent g_rawDispatcher268910;
thread_local RawReentryEvent g_rawLabel268910;
thread_local uint64_t g_nextRawSequence = 0u;
thread_local std::array<OwnerFrame, kOwnerFrameCapacity> g_ownerFrames;
thread_local uint64_t g_nextOwnerFrameGeneration = 0u;
thread_local uint64_t g_nextOwnerActivity = 0u;
thread_local uint64_t g_latestRelevantFrameGeneration = 0u;
std::atomic<bool> g_failureRetained{false};

// Detailed lifecycle storage is local to the observing host thread and keyed by
// explicit guest context. Cross-thread overlap uses only atomic publications.
constexpr size_t kChildCapacity = 8u;
constexpr size_t kChildEventCapacity = 32u;
constexpr size_t kPublishedChildCapacity = 16u;
static_assert(std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<uintptr_t>::is_always_lock_free &&
              std::atomic<uint32_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free,
              "GAME-001 publication must not introduce hidden locks");
struct ChildEvent {
    const char *kind = nullptr;
    uint64_t sequence = 0u;
    const R5900Context *ctx = nullptr;
    const R5900Context *ownerCtx = nullptr;
    uint64_t generation = 0u;
    uint32_t pc = 0u, nextPc = 0u, sp = 0u, ra = 0u, slot = 0u;
    uint64_t expected = 0u, observed = 0u;
    bool active = false, spMatches = false, slotRead = false;
};
struct ChildFrame {
    const R5900Context *ctx = nullptr;
    uint64_t generation = 0u;
    uint32_t frameSp = 0u, slot = 0u;
    uint64_t expected = 0u;
    bool active = false, saved = false;
    size_t publication = kPublishedChildCapacity;
    ChildEvent allocation, save, returned;
};
struct PublishedChild {
    std::atomic<uint64_t> version{0u}, generation{0u}, expected{0u};
    // Completion cannot depend on winning the best-effort version CAS. A
    // generation-specific retirement stays authoritative if that update fails.
    std::atomic<uint64_t> retiredGeneration{0u};
    std::atomic<uint64_t> allocationSequence{0u}, saveSequence{0u};
    std::atomic<uintptr_t> ctx{0u};
    std::atomic<uint32_t> slot{0u}, frameSp{0u};
    std::atomic<bool> active{false}, saved{false};
};
struct ChildSnapshot {
    const R5900Context *ctx = nullptr;
    uint64_t generation = 0u, expected = 0u, allocationSequence = 0u, saveSequence = 0u;
    uint32_t slot = 0u, frameSp = 0u;
    bool active = false, saved = false;
};
struct FrozenChildEvent {
    // Only the CAS winner writes the non-atomic payload. It never changes after
    // the release publication; readers inspect it only after an acquire of 2.
    std::atomic<unsigned> state{0u};
    ChildEvent event;
    uint64_t allocationSequence = 0u, saveSequence = 0u;
};
std::atomic<uint64_t> g_observationSequence{0u};
std::array<PublishedChild, kPublishedChildCapacity> g_publishedChildren;
FrozenChildEvent g_firstChildMismatch, g_firstChildOverlap;
std::atomic<bool> g_childPublicationIncomplete{false};
thread_local std::array<ChildFrame, kChildCapacity> g_childFrames;
thread_local std::array<ChildEvent, kChildEventCapacity> g_childEvents;
thread_local size_t g_childEventCount = 0u;
thread_local bool g_childFrameOverflow = false;

uint64_t nextObservationSequence() noexcept {
    return g_observationSequence.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

bool childSnapshot(const PublishedChild &publication, ChildSnapshot &out) noexcept {
    const uint64_t before = publication.version.load(std::memory_order_acquire);
    if (before & 1u) return false;
    ChildSnapshot snapshot;
    snapshot.ctx = reinterpret_cast<const R5900Context *>(publication.ctx.load(std::memory_order_relaxed));
    snapshot.generation = publication.generation.load(std::memory_order_relaxed);
    snapshot.expected = publication.expected.load(std::memory_order_relaxed);
    snapshot.allocationSequence = publication.allocationSequence.load(std::memory_order_relaxed);
    snapshot.saveSequence = publication.saveSequence.load(std::memory_order_relaxed);
    snapshot.slot = publication.slot.load(std::memory_order_relaxed);
    snapshot.frameSp = publication.frameSp.load(std::memory_order_relaxed);
    snapshot.active = publication.active.load(std::memory_order_relaxed);
    snapshot.saved = publication.saved.load(std::memory_order_relaxed);
    // Validate the version after retirement too: otherwise slot reuse followed
    // by retirement of a newer generation could make an old snapshot look live.
    const uint64_t retiredGeneration = publication.retiredGeneration.load(std::memory_order_acquire);
    // All payload fields are atomic: even an inconsistent concurrent snapshot
    // has no C++ data race. Never use an in-progress or changed publication.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before != publication.version.load(std::memory_order_acquire)) return false;
    if (retiredGeneration == snapshot.generation)
        snapshot.active = false;
    out = snapshot;
    return true;
}

bool publishChild(ChildFrame &frame, size_t index, bool allocate) noexcept {
    PublishedChild &publication = g_publishedChildren[index];
    uint64_t version = publication.version.load(std::memory_order_acquire);
    if ((version & 1u) || !publication.version.compare_exchange_strong(
            version, version + 1u, std::memory_order_acq_rel)) return false;
    const uint64_t publishedGeneration = publication.generation.load(std::memory_order_relaxed);
    const bool publishedActive = publication.active.load(std::memory_order_relaxed) &&
        publication.retiredGeneration.load(std::memory_order_acquire) != publishedGeneration;
    if ((allocate && publishedActive) ||
        (!allocate && publication.generation.load(std::memory_order_relaxed) != frame.generation)) {
        publication.version.store(version + 2u, std::memory_order_release);
        return false;
    }
    // If a reader sees any of these payload stores, its trailing acquire fence
    // synchronizes with this release fence. The odd version then happens before
    // its final version load, preventing acceptance of a mixed old/new snapshot.
    std::atomic_thread_fence(std::memory_order_release);
    publication.ctx.store(reinterpret_cast<uintptr_t>(frame.ctx), std::memory_order_relaxed);
    publication.generation.store(frame.generation, std::memory_order_relaxed);
    publication.expected.store(frame.expected, std::memory_order_relaxed);
    publication.allocationSequence.store(frame.allocation.sequence, std::memory_order_relaxed);
    publication.saveSequence.store(frame.save.sequence, std::memory_order_relaxed);
    publication.slot.store(frame.slot, std::memory_order_relaxed);
    publication.frameSp.store(frame.frameSp, std::memory_order_relaxed);
    publication.active.store(frame.active, std::memory_order_relaxed);
    publication.saved.store(frame.saved, std::memory_order_relaxed);
    publication.version.store(version + 2u, std::memory_order_release);
    return true;
}

void updateChildPublication(ChildFrame &frame) noexcept {
    if (frame.publication == kPublishedChildCapacity || !publishChild(frame, frame.publication, false))
        g_childPublicationIncomplete.store(true, std::memory_order_relaxed);
}

void freezeChildEvent(FrozenChildEvent &frozen, const ChildEvent &event,
                      uint64_t allocation, uint64_t save) noexcept {
    unsigned empty = 0u;
    if (!frozen.state.compare_exchange_strong(empty, 1u, std::memory_order_acq_rel)) return;
    frozen.event = event;
    frozen.allocationSequence = allocation;
    frozen.saveSequence = save;
    frozen.state.store(2u, std::memory_order_release);
}

bool readChildSlot(const uint8_t *rdram, uint32_t slot, uint64_t &value) noexcept {
    const uint32_t offset = slot & PS2_RAM_MASK;
    if (!rdram || offset > PS2_RAM_SIZE - sizeof(uint64_t)) return false;
    value = 0u;
    for (unsigned byte = 0u; byte < 8u; ++byte)
        value |= static_cast<uint64_t>(rdram[offset + byte]) << (8u * byte);
    return true;
}

ChildEvent childEvent(const char *kind, const R5900Context *ctx, uint32_t pc,
                       uint32_t nextPc, uint32_t sp, uint32_t ra,
                       const ChildFrame *frame = nullptr) noexcept {
    ChildEvent event;
    event.kind = kind;
    event.sequence = nextObservationSequence();
    event.ctx = ctx;
    event.pc = pc; event.nextPc = nextPc; event.sp = sp; event.ra = ra;
    if (frame) {
        event.ownerCtx = frame->ctx; event.generation = frame->generation;
        event.slot = frame->slot; event.expected = frame->expected;
        event.active = frame->active; event.spMatches = sp == frame->frameSp;
    }
    return event;
}

void appendChildEvent(const ChildEvent &event) noexcept {
    g_childEvents[g_childEventCount++ % kChildEventCapacity] = event;
}

void sampleChildSlot(ChildEvent &event, const ChildFrame &frame, const uint8_t *rdram) noexcept {
    if (!frame.active || !frame.saved) return;
    event.slotRead = readChildSlot(rdram, frame.slot, event.observed);
    if (event.slotRead && event.observed != frame.expected)
        freezeChildEvent(g_firstChildMismatch, event, frame.allocation.sequence, frame.save.sequence);
}

void recordChildBoundary(const char *kind, const uint8_t *rdram, const R5900Context *ctx,
                          uint32_t pc, uint32_t nextPc, uint32_t sp, uint32_t ra) noexcept {
    bool found = false;
    for (const ChildFrame &frame : g_childFrames) {
        if (!frame.generation || frame.ctx != ctx) continue;
        // These are candidate-frame observations, not inferred ownership of PC.
        // Retain completed records too so a stale reentry stays distinguishable.
        ChildEvent event = childEvent(kind, ctx, pc, nextPc, sp, ra, &frame);
        sampleChildSlot(event, frame, rdram);
        appendChildEvent(event);
        found = true;
    }
    if (!found) appendChildEvent(childEvent(kind, ctx, pc, nextPc, sp, ra));
}

ChildFrame *latestChild(const R5900Context *ctx, bool unsaved = false) noexcept {
    ChildFrame *latest = nullptr;
    for (ChildFrame &frame : g_childFrames) {
        if (!frame.active || frame.ctx != ctx || (unsaved && frame.saved)) continue;
        if (!latest || frame.generation > latest->generation) latest = &frame;
    }
    return latest;
}

void printChildEvent(const char *name, const ChildEvent &event) noexcept {
    if (!event.kind) return;
    std::fprintf(stderr, " %s={kind=%s sequence=%llu ctx=%p owner-ctx=%p generation=%llu "
                 "pc=%08x next=%08x sp=%08x ra=%08x slot=%08x expected=%016llx "
                 "observed=%016llx active=%u sp-match=%u slot-read=%u}", name, event.kind,
                 static_cast<unsigned long long>(event.sequence), static_cast<const void *>(event.ctx),
                 static_cast<const void *>(event.ownerCtx), static_cast<unsigned long long>(event.generation),
                 event.pc, event.nextPc, event.sp, event.ra, event.slot,
                 static_cast<unsigned long long>(event.expected), static_cast<unsigned long long>(event.observed),
                 event.active ? 1u : 0u, event.spMatches ? 1u : 0u, event.slotRead ? 1u : 0u);
}

void printChildReceipt() noexcept {
    std::fputs(" child-coverage=thread-local-lifecycle/global-atomic-overlap child-boundaries=candidates-not-pc-owners", stderr);
    for (const ChildFrame &frame : g_childFrames) {
        if (!frame.generation) continue;
        std::fprintf(stderr, " child-frame={ctx=%p generation=%llu frame-sp=%08x slot=%08x active=%u}",
                     static_cast<const void *>(frame.ctx), static_cast<unsigned long long>(frame.generation),
                     frame.frameSp, frame.slot, frame.active ? 1u : 0u);
        printChildEvent("child-allocation", frame.allocation);
        printChildEvent("child-save", frame.save);
        printChildEvent("child-return", frame.returned);
    }
    const size_t first = g_childEventCount > kChildEventCapacity ? g_childEventCount - kChildEventCapacity : 0u;
    for (size_t index = first; index < g_childEventCount; ++index)
        printChildEvent("child-event", g_childEvents[index % kChildEventCapacity]);
    if (g_childEventCount > kChildEventCapacity) std::fputs(" child-events-truncated=1", stderr);
    if (g_childFrameOverflow) std::fputs(" child-frame-overflow=1", stderr);
    if (g_childPublicationIncomplete.load(std::memory_order_relaxed))
        std::fputs(" child-publication-incomplete=1", stderr);
    for (const auto &[name, frozen] : std::array<std::pair<const char *, const FrozenChildEvent *>, 2>{{
            {"child-first-mismatch", &g_firstChildMismatch}, {"child-first-overlap", &g_firstChildOverlap}}}) {
        if (frozen->state.load(std::memory_order_acquire) != 2u) continue;
        printChildEvent(name, frozen->event);
        std::fprintf(stderr, " %s-anchors={allocation-sequence=%llu save-sequence=%llu}", name,
                     static_cast<unsigned long long>(frozen->allocationSequence),
                     static_cast<unsigned long long>(frozen->saveSequence));
    }
}

bool hasAnyActiveOwnerFrame() noexcept {
    for (const OwnerFrame &frame : g_ownerFrames) {
        if (frame.valid && frame.active) return true;
    }
    return false;
}

OwnerFrame *findOwnerFrame(const R5900Context *ctx, uint32_t postPrologueSp,
                           bool activeOnly = false) noexcept {
    OwnerFrame *latestActive = nullptr;
    OwnerFrame *latestInactive = nullptr;
    for (OwnerFrame &frame : g_ownerFrames) {
        if (!frame.valid || frame.ctx != ctx || frame.postPrologueSp != postPrologueSp) continue;
        if (frame.active) {
            if (!latestActive || frame.generation > latestActive->generation)
                latestActive = &frame;
        } else if (!activeOnly &&
                   (!latestInactive || frame.generation > latestInactive->generation)) {
            latestInactive = &frame;
        }
    }
    return latestActive ? latestActive : latestInactive;
}

OwnerFrame *allocateOwnerFrame(const R5900Context *ctx, uint32_t postPrologueSp,
                               uint64_t generation) noexcept {
    OwnerFrame *oldestInactive = nullptr;
    OwnerFrame *oldestAny = nullptr;
    for (OwnerFrame &frame : g_ownerFrames) {
        if (!frame.valid) {
            frame = {};
            frame.valid = true;
            frame.active = true;
            frame.ctx = ctx;
            frame.postPrologueSp = postPrologueSp;
            frame.generation = generation;
            return &frame;
        }
        if (!oldestAny || frame.generation < oldestAny->generation) oldestAny = &frame;
        if (!frame.active && (!oldestInactive || frame.generation < oldestInactive->generation))
            oldestInactive = &frame;
    }
    OwnerFrame *slot = oldestInactive ? oldestInactive : oldestAny;
    if (slot && slot->active) g_reentry.frameOverflow = true;
    if (slot) {
        *slot = {};
        slot->valid = true;
        slot->active = true;
        slot->ctx = ctx;
        slot->postPrologueSp = postPrologueSp;
        slot->generation = generation;
    }
    return slot;
}

ReentryEvent makeReentryEvent(const R5900Context *ctx, uint32_t pc,
                              uint32_t sp, uint32_t ra,
                              const OwnerFrame *frame, uint32_t arrivalSource = 0u) noexcept {
    return {true, ctx, pc, sp, ra, frame ? frame->generation : 0u,
            frame && frame->active && frame->ctx == ctx && frame->postPrologueSp == sp,
            arrivalSource};
}

uint32_t read32(const uint8_t *rdram, uint32_t address) noexcept {
    if (!rdram) return 0u;
    const uint32_t offset = address & PS2_RAM_MASK;
    if (offset > PS2_RAM_SIZE - sizeof(uint32_t)) return 0u;
    return static_cast<uint32_t>(rdram[offset]) |
        (static_cast<uint32_t>(rdram[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(rdram[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(rdram[offset + 3u]) << 24u);
}

TargetClass classify(uint32_t target, bool registered) noexcept {
    // The generated registry covers both function entries and re-entry labels.
    if (registered) {
        if (target == 0x0021fa00u || target == 0x0021fd40u || target == 0x00258980u)
            return TargetClass::KnownAotReentry;
        return TargetClass::KnownAotEntry;
    }
    if ((target >= kTextBegin && target < kTextEnd) ||
        (target >= kVuTextBegin && target < kVuTextEnd))
        return TargetClass::ExecutableUnregistered;
    if (target >= kBssBegin && target < kBssEnd)
        return TargetClass::Data;
    // Other physical EE RAM is ordinary data for the purposes of an attempted
    // control transfer; zero and non-RAM addresses remain unmapped.
    if (target < PS2_RAM_SIZE) return TargetClass::Data;
    return TargetClass::Unmapped;
}

const char *className(TargetClass classification) noexcept {
    switch (classification) {
    case TargetClass::KnownAotEntry: return "known-aot-entry";
    case TargetClass::KnownAotReentry: return "known-aot-reentry";
    case TargetClass::ExecutableUnregistered: return "executable-unregistered";
    case TargetClass::Data: return "data";
    case TargetClass::Unmapped: return "unmapped";
    }
    return "unmapped";
}

void printHistory() noexcept {
    std::fputs(" history=", stderr);
    for (size_t index = 0u; index < g_history.size; ++index) {
        std::fprintf(stderr, "%s%08x", index == 0u ? "" : "->",
                     g_history.entries[(g_history.first + index) % g_history.entries.size()]);
    }
}

void printReentryEvent(const char *name, const ReentryEvent &event) noexcept {
    if (!event.valid) return;
    std::fprintf(stderr,
                 " %s={ctx=%p pc=%08x sp=%08x ra=%08x source=%08x frame=%llu active=%u}",
                 name, static_cast<const void *>(event.ctx), event.pc, event.sp, event.ra,
                 event.arrivalSource,
                 static_cast<unsigned long long>(event.frameGeneration),
                 event.activeFrame ? 1u : 0u);
}

void printRawReentryEvent(const char *name, const RawReentryEvent &event) noexcept {
    if (event.sequence == 0u) return;
    std::fprintf(stderr,
                 " %s={ctx=%p pc=%08x sp=%08x ra=%08x source=%08x sequence=%llu association=none}",
                 name, static_cast<const void *>(event.ctx), event.pc, event.sp, event.ra,
                 event.arrivalSource,
                 static_cast<unsigned long long>(event.sequence));
}

const OwnerFrame *findOwnerFrameGeneration(uint64_t generation) noexcept {
    if (generation == 0u) return nullptr;
    for (const OwnerFrame &frame : g_ownerFrames) {
        if (frame.valid && frame.generation == generation) return &frame;
    }
    return nullptr;
}

const OwnerFrame *latestOwnerFrame() noexcept {
    const OwnerFrame *latest = nullptr;
    for (const OwnerFrame &frame : g_ownerFrames) {
        if (frame.valid && (!latest || frame.generation > latest->generation)) latest = &frame;
    }
    return latest;
}

void markRelevant(OwnerFrame *frame) noexcept {
    if (!frame) return;
    frame->activity = ++g_nextOwnerActivity;
    g_latestRelevantFrameGeneration = frame->generation;
}

OwnerFrame *solePendingYieldFrame() noexcept {
    OwnerFrame *only = nullptr;
    for (OwnerFrame &frame : g_ownerFrames) {
        if (!frame.valid || !frame.preempt268b38.valid || frame.label268910.valid) continue;
        if (only) return nullptr;
        only = &frame;
    }
    return only;
}

OwnerFrame *pendingFrameForDispatcherDestination(const R5900Context *ctx,
                                                 uint32_t sp) noexcept {
    OwnerFrame *latest = nullptr;
    for (OwnerFrame &frame : g_ownerFrames) {
        if (!frame.valid || !frame.preempt268b38.valid || frame.label268910.valid ||
            !frame.dispatcher268910.valid || frame.dispatcher268910.ctx != ctx ||
            frame.dispatcher268910.sp != sp) continue;
        if (!latest || frame.activity > latest->activity) latest = &frame;
    }
    return latest;
}

void printReentryReceipt() noexcept {
    const OwnerFrame *receiptFrame = findOwnerFrameGeneration(g_latestRelevantFrameGeneration);
    if (!receiptFrame) receiptFrame = latestOwnerFrame();
    if (!receiptFrame && !g_reentry.frameOverflow && g_nextRawSequence == 0u) return;
    std::fputs(" reentry-receipt", stderr);
    // Deliberately survive frame resets. These are latest-per-site raw facts,
    // not a claimed yield/dispatch/label relation or first-label observation.
    if (g_nextRawSequence != 0u) {
        std::fputs(" raw-events=latest-per-site-unpaired", stderr);
        printRawReentryEvent("raw-preempt268b38", g_rawPreempt268b38);
        printRawReentryEvent("raw-dispatch268910", g_rawDispatcher268910);
        printRawReentryEvent("raw-label268910", g_rawLabel268910);
    }
    if (receiptFrame) {
        printReentryEvent("primary", receiptFrame->primary);
        printReentryEvent("epilogue", receiptFrame->epilogue);
        printReentryEvent("preempt-268b38", receiptFrame->preempt268b38);
        printReentryEvent("dispatch-268910-main", receiptFrame->dispatcher268910);
        printReentryEvent("label-268910", receiptFrame->label268910);
    }
    for (const OwnerFrame &frame : g_ownerFrames) {
        if (!frame.valid) continue;
        std::fprintf(stderr, " frame={ctx=%p post-sp=%08x generation=%llu activity=%llu active=%u}",
                     static_cast<const void *>(frame.ctx), frame.postPrologueSp,
                     static_cast<unsigned long long>(frame.generation),
                     static_cast<unsigned long long>(frame.activity), frame.active ? 1u : 0u);
    }
    if (g_reentry.frameOverflow) std::fputs(" frame-overflow=1", stderr);
}

} // namespace

void recordChildAllocation(const R5900Context *ctx, uint32_t frameSp, uint32_t ra) noexcept {
    ChildFrame *slot = nullptr;
    for (ChildFrame &frame : g_childFrames) {
        if (!frame.generation) { slot = &frame; break; }
        if (!frame.active && (!slot || frame.generation < slot->generation)) slot = &frame;
    }
    if (!slot) {
        g_childFrameOverflow = true;
        appendChildEvent(childEvent("allocation-untracked", ctx, 0x00258640u, 0u, frameSp, ra));
        return;
    }
    *slot = {};
    slot->ctx = ctx; slot->generation = nextObservationSequence();
    slot->frameSp = frameSp; slot->slot = frameSp + 0x100u; slot->active = true;
    slot->allocation = childEvent("allocation", ctx, 0x00258640u, 0u, frameSp, ra, slot);
    for (size_t index = 0u; index < kPublishedChildCapacity; ++index) {
        if (publishChild(*slot, index, true)) { slot->publication = index; break; }
    }
    if (slot->publication == kPublishedChildCapacity)
        g_childPublicationIncomplete.store(true, std::memory_order_relaxed);
    appendChildEvent(slot->allocation);
}

void recordChildYield(const uint8_t *rdram, const R5900Context *ctx, uint32_t source,
                      uint32_t sp, uint32_t ra, uint32_t nextPc) noexcept {
    recordChildBoundary("yield", rdram, ctx, source, nextPc, sp, ra);
}

void recordChildDispatch(const uint8_t *, const R5900Context *ctx, uint32_t pc,
                         uint32_t sp, uint32_t ra) noexcept {
    // Dispatcher selection is outside GuestExecutionScope. Reading guest RAM
    // here could race a callback; retain architectural values without slot reads.
    recordChildBoundary("dispatch", nullptr, ctx, pc, pc, sp, ra);
}

void recordChildReturn(const uint8_t *rdram, const R5900Context *ctx, uint32_t postSp,
                       uint32_t ra, uint32_t target) noexcept {
    ChildFrame *match = nullptr;
    for (ChildFrame &frame : g_childFrames) {
        if (!frame.active || frame.ctx != ctx || frame.frameSp + 0x110u != postSp) continue;
        if (!match || frame.generation > match->generation) match = &frame;
    }
    if (!match) {
        // Do not claim a generation completed when the observed post-pop SP
        // cannot identify its allocation. The raw return exposes that boundary.
        recordChildBoundary("return-unmatched", rdram, ctx, 0x002589b4u, target, postSp, ra);
        return;
    }
    ChildEvent event = childEvent("return", ctx, 0x002589b4u, target, postSp, ra, match);
    sampleChildSlot(event, *match, rdram);
    match->active = false;
    event.active = false;
    event.spMatches = true; // Return compares post-pop SP to the original entry SP.
    match->returned = event;
    if (match->publication != kPublishedChildCapacity)
        g_publishedChildren[match->publication].retiredGeneration.store(match->generation, std::memory_order_release);
    updateChildPublication(*match);
    appendChildEvent(event);
}

void recordChildSlotOverlap(const uint8_t *rdram, const R5900Context *ctx,
                            uint32_t sp, uint32_t ra, uint64_t writtenValue) noexcept {
    const uint32_t writeSlot = (sp + 0x20u) & PS2_RAM_MASK;
    for (const PublishedChild &publication : g_publishedChildren) {
        ChildSnapshot frame;
        if (!childSnapshot(publication, frame)) {
            g_childPublicationIncomplete.store(true, std::memory_order_relaxed);
            continue;
        }
        const uint32_t slot = frame.slot & PS2_RAM_MASK;
        if (!frame.active || !frame.saved || writeSlot >= slot + 8u || slot >= writeSlot + 8u) continue;
        ChildEvent event = childEvent("store-2d2c44", ctx, 0x002d2c44u, writeSlot, sp, ra);
        event.ownerCtx = frame.ctx; event.generation = frame.generation;
        event.slot = frame.slot; event.expected = frame.expected; event.active = true;
        event.spMatches = sp == frame.frameSp;
        event.slotRead = readChildSlot(rdram, frame.slot, event.observed);
        // The caller supplies the actual SD operand; exact-slot overlap can be
        // observed without RAM, which also keeps asset-free controls meaningful.
        if (writeSlot == slot && !event.slotRead) event.observed = writtenValue;
        freezeChildEvent(g_firstChildOverlap, event, frame.allocationSequence, frame.saveSequence);
        if (event.slotRead && event.observed != frame.expected)
            freezeChildEvent(g_firstChildMismatch, event, frame.allocationSequence, frame.saveSequence);
        appendChildEvent(event);
    }
}

void recordSavedRaStore(const R5900Context *ctx, uint32_t frameSp, uint32_t incomingRa,
                        uint64_t savedValue) noexcept {
    g_savedRa = {true, ctx, frameSp + 0x110u, incomingRa, savedValue, 0u, 0u, 0u};
    ChildFrame *frame = latestChild(ctx, true);
    if (!frame) {
        appendChildEvent(childEvent("save-untracked", ctx, 0x00258670u, 0u, frameSp, incomingRa));
        return;
    }
    frame->slot = frameSp + 0x100u;
    frame->expected = savedValue;
    frame->saved = true;
    frame->save = childEvent("save", ctx, 0x00258670u, 0u, frameSp, incomingRa, frame);
    frame->save.observed = savedValue;
    frame->save.slotRead = true;
    updateChildPublication(*frame);
    appendChildEvent(frame->save);
}

void recordSavedRaRestore(const R5900Context *ctx, uint32_t frameSp, uint64_t savedValue,
                          uint32_t restoredRa) noexcept {
    for (const ChildFrame &frame : g_childFrames) {
        if (!frame.generation || frame.ctx != ctx) continue;
        ChildEvent event = childEvent("restore", ctx, 0x00258988u, 0u, frameSp, restoredRa, &frame);
        // This operand belongs to the actual restore address, not every candidate slot.
        event.slotRead = frame.slot == frameSp + 0x100u;
        if (event.slotRead) {
            event.observed = savedValue;
            if (frame.active && frame.saved && savedValue != frame.expected)
                freezeChildEvent(g_firstChildMismatch, event, frame.allocation.sequence, frame.save.sequence);
        }
        appendChildEvent(event);
    }
    if (!g_savedRa.valid || g_savedRa.ctx != ctx) return;
    if (frameSp + 0x110u != g_savedRa.entrySp) return;
    g_savedRa.slotBeforeRestore = savedValue;
    g_savedRa.restoredRa = restoredRa;
}

void recordFinalOperand(const R5900Context *ctx, uint32_t frameSp, uint32_t finalOperand) noexcept {
    if (!g_savedRa.valid || g_savedRa.ctx != ctx || frameSp + 0x110u != g_savedRa.entrySp) return;
    g_savedRa.finalOperand = finalOperand;
}

void recordOwnerPrimaryEntry(const R5900Context *ctx, uint32_t preSwitchPc,
                             uint32_t sp, uint32_t ra) noexcept {
    // A primary entry after every known owner frame has unwound starts a fresh
    // bounded relation. Nested activations retain the suspended frame records
    // and their earlier yield relation until that frame is resumed or cleared.
    if (!hasAnyActiveOwnerFrame()) {
        g_reentry = {};
        g_ownerFrames = {};
        g_latestRelevantFrameGeneration = 0u;
    }
    const uint64_t generation = ++g_nextOwnerFrameGeneration;
    OwnerFrame *frame = allocateOwnerFrame(ctx, sp, generation);
    if (frame) {
        frame->primary = makeReentryEvent(ctx, preSwitchPc, sp, ra, frame);
        markRelevant(frame);
    }
}

void recordOwnerPreempt268b38(const R5900Context *ctx, uint32_t sp,
                              uint32_t ra) noexcept {
    g_rawPreempt268b38 = {ctx, 0x00268910u, sp, ra, (g_nextRawSequence = nextObservationSequence()), 0x00268b38u};
    OwnerFrame *frame = findOwnerFrame(ctx, sp);
    if (!frame) return;
    // A new taken yield resets only this invocation's handoff relation.
    frame->preempt268b38 = makeReentryEvent(ctx, 0x00268910u, sp, ra, frame);
    frame->dispatcher268910 = {};
    frame->label268910 = {};
    markRelevant(frame);
}

void recordDispatcherSelection268910(const R5900Context *ctx, uint32_t sp,
                                     uint32_t ra) noexcept {
    g_rawDispatcher268910 = {ctx, 0x00268910u, sp, ra, (g_nextRawSequence = nextObservationSequence()), 0x00268910u};
    OwnerFrame *frame = findOwnerFrame(ctx, sp);
    if (frame && (!frame->preempt268b38.valid || frame->label268910.valid)) frame = nullptr;
    // A foreign selected context cannot be paired to its own frame. Attribute
    // it only when exactly one yield remains pending; otherwise no owner token
    // is defensible and the terminal receipt remains explicitly incomplete.
    if (!frame) frame = solePendingYieldFrame();
    if (!frame) return;
    frame->dispatcher268910 = makeReentryEvent(ctx, 0x00268910u, sp, ra, frame);
    markRelevant(frame);
}

void recordOwnerLabel268910(const R5900Context *ctx, uint32_t preSwitchPc,
                            uint32_t sp, uint32_t ra, uint32_t arrivalSource,
                            const uint8_t *rdram) noexcept {
    g_rawLabel268910 = {ctx, preSwitchPc, sp, ra, (g_nextRawSequence = nextObservationSequence()), arrivalSource};
    recordChildBoundary("parent-label", rdram, ctx, 0x00268910u, arrivalSource, sp, ra);
    OwnerFrame *frame = nullptr;
    if (arrivalSource == 0x00268910u) {
        frame = pendingFrameForDispatcherDestination(ctx, sp);
        if (!frame) {
            frame = findOwnerFrame(ctx, sp);
            if (frame && (!frame->preempt268b38.valid || frame->label268910.valid)) frame = nullptr;
        }
        if (!frame) frame = solePendingYieldFrame();
    } else {
        frame = findOwnerFrame(ctx, sp);
    }
    if (!frame) return;
    if (arrivalSource != 0x00268910u) {
        // Ordinary fall-through or a nonpreempted back-edge does not establish
        // a dispatcher handoff, even if this host invocation entered at 268910.
        frame->preempt268b38 = {};
        frame->dispatcher268910 = {};
    }
    frame->label268910 = makeReentryEvent(ctx, preSwitchPc, sp, ra, frame, arrivalSource);
    markRelevant(frame);
}

void clearOwnerActiveFrame(const R5900Context *ctx, uint32_t postPrologueSp) noexcept {
    OwnerFrame *frame = findOwnerFrame(ctx, postPrologueSp, true);
    if (!frame) return;
    frame->epilogue = makeReentryEvent(ctx, 0x00268bd4u, postPrologueSp, 0u, frame);
    frame->active = false;
    markRelevant(frame);
}

bool beforeDispatch(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx,
                    uint32_t target) noexcept {
    const bool registered = runtime && runtime->hasFunction(target);
    if (registered) {
        g_history.append(target);
        switch (target) {
        case 0x00258640u: case 0x00258688u: case 0x002586a8u: case 0x002586bcu:
        case 0x002586d8u: case 0x002586f0u: case 0x002586f8u: case 0x00258720u:
        case 0x002587b0u: case 0x00258930u: case 0x00258944u: case 0x00258980u:
        case 0x00258988u: case 0x00268af4u:
            recordChildDispatch(rdram, ctx, target,
                ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u,
                ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u);
            break;
        default: break;
        }
        if (target == 0x00268910u) {
            const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
            const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
            recordDispatcherSelection268910(ctx, sp, ra);
        }
        return true;
    }

    // Retain just the first terminal observation globally.  The stop below
    // prevents normal execution from emitting a second guest failure receipt.
    if (!g_failureRetained.exchange(true, std::memory_order_acq_rel)) {
        const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
        const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
        const uint32_t phase = read32(rdram, 0x334e94u);
        const uint32_t cursor = read32(rdram, 0x3348acu);
        const bool validCursor = cursor != 0u && (cursor & 3u) == 0u &&
            cursor <= 0x01fffff8u;
        const uint32_t script = validCursor ? read32(rdram, cursor) : 0xffffffffu;
        const uint32_t step = validCursor ? read32(rdram, cursor + 4u) : 0xffffffffu;
        const uint32_t guestField = read32(rdram, 0x346e84u);
        const TargetClass classification = classify(target, false);
        std::fprintf(stderr,
                     "[m2p-game001:fail-closed] target=%08x current=%08x ra=%08x sp=%08x "
                     "classification=%s phase=%u script=%u step=%u "
                     "guest-field=%u gs-epoch=unavailable",
                     target, target, ra, sp, className(classification), phase, script, step,
                     guestField);
        printHistory();
        printReentryReceipt();
        printChildReceipt();
        if (g_savedRa.valid && g_savedRa.ctx == ctx && g_savedRa.finalOperand == target) {
            std::fprintf(stderr,
                         " source=restored-ra entry-sp=%08x incoming-ra=%08x saved-ra-slot=%08x "
                         "slot-after-save=%016llx "
                         "slot-before-restore=%016llx restored-ra=%08x final-operand=%08x",
                         g_savedRa.entrySp, g_savedRa.incomingRa, g_savedRa.entrySp - 0x10u,
                         static_cast<unsigned long long>(g_savedRa.slotAfterSave),
                         static_cast<unsigned long long>(g_savedRa.slotBeforeRestore),
                         g_savedRa.restoredRa, g_savedRa.finalOperand);
        }
        std::fputs(" action=request-stop-before-lookup-default-fallback\n", stderr);
        std::fflush(stderr);
    }
    if (runtime) runtime->requestStop();
    return false;
}

} // namespace rrv::m2pgame001
