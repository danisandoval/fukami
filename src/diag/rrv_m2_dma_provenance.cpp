#include "rrv_m2_dma_provenance.h"
#include "rrv_m2_guest_provenance.h"

#if defined(RRV_M2_DMA_PROVENANCE)

#include <array>
#include <atomic>
#include <cstddef>

namespace rrv::m2prov
{
namespace
{
constexpr size_t kActiveRoots = 128u;
constexpr size_t kRetiredRoots = 128u;
constexpr size_t kRoots = kActiveRoots + kRetiredRoots;
constexpr size_t kSourceSlots = 131072u;
constexpr size_t kTagSlots = 32768u;
constexpr uint32_t kNoSlot = 0u;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

enum class RootState : uint32_t { Empty, Initializing, Active, Retiring, Retired };
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<RootState>::is_always_lock_free);
static_assert(kActiveRoots == kActiveRootCapacity);
static_assert(kRetiredRoots == kRetiredRootCapacity);
static_assert(kSourceSlots == kSourceCapacity);
static_assert(kTagSlots == kTagCapacity);

struct SourceInfo
{
    uint32_t next{};
    uint32_t space{};
    uint32_t guestAddress{};
    uint32_t bytes{};
    uint32_t flatOffset{};
    uint32_t tagIndex{kNoTagIndex};
    uint64_t digest{};
};

struct TagInfo
{
    uint32_t next{};
    uint32_t index{};
    uint64_t low{};
    uint64_t upper{};
    uint64_t digest{};
};

template <typename T>
struct ArenaSlot
{
    std::atomic<uint32_t> nextFree{kNoSlot};
    T value{};
};

struct Root
{
    std::atomic<RootState> state{RootState::Empty};
    std::atomic<uint32_t> references{0u};
    std::atomic<uint32_t> emitted{0u};
    std::atomic<uint32_t> sealed{0u};
    std::atomic<uint32_t> busy{0u};
    std::atomic<uint64_t> id{kUnknownId};
    uint32_t pc{};
    uint32_t ra{};
    uint32_t route{};
    uint32_t createdBeforeSelector{};
    uint32_t syntheticFifo{};
    uint32_t chcr{};
    uint32_t madr{};
    uint32_t qwc{};
    uint32_t tadr{};
    uint32_t asr0{};
    uint32_t asr1{};
    uint32_t sourceHead{kNoSlot};
    uint32_t sourceTail{kNoSlot};
    uint32_t sourceCount{};
    uint32_t tagHead{kNoSlot};
    uint32_t tagTail{kNoSlot};
    uint32_t tagCount{};
    uint32_t queueAddress{};
    uint32_t flatBytes{};
    uint32_t packetHint{};
    uint64_t queueDigest{};
    uint32_t queueDigestKnown{};
    uint32_t queueAccepted{};
    uint32_t observedAddress{};
    uint32_t observedBytes{};
    uint64_t observedDigest{};
};

std::array<Root, kRoots> g_roots{};
std::array<ArenaSlot<SourceInfo>, kSourceSlots> g_sources{};
std::array<ArenaSlot<TagInfo>, kTagSlots> g_tags{};
std::atomic<uint32_t> g_sourceFree{kNoSlot};
std::atomic<uint32_t> g_tagFree{kNoSlot};
std::atomic<uint32_t> g_arenaBusy{0u};
std::atomic<uint64_t> g_nextRoot{1u};
std::atomic<uint64_t> g_nextPacket{1u};
std::atomic<uint32_t> g_activeRoots{0u};
std::atomic<uint32_t> g_configured{0u};
struct OriginContext
{
    uint32_t pc{};
    uint32_t ra{};
    uint32_t route{};
    bool active{};
};
thread_local OriginContext g_currentOrigin{};
thread_local uint64_t g_currentRoot = kUnknownId;
thread_local uint64_t g_currentPacket = kUnknownId;

uint64_t fnv1a(const uint8_t *bytes, uint32_t size) noexcept
{
    uint64_t result = kFnvOffset;
    if (!bytes)
        return result;
    for (uint32_t i = 0u; i < size; ++i)
    {
        result ^= bytes[i];
        result *= kFnvPrime;
    }
    return result;
}

bool claimArena() noexcept
{
    uint32_t expected = 0u;
    if (g_arenaBusy.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel))
        return true;
    m2causal::fail();
    return false;
}

void releaseArena() noexcept
{
    g_arenaBusy.store(0u, std::memory_order_release);
}

template <typename T, size_t N>
uint32_t popArena(std::array<ArenaSlot<T>, N> &arena, std::atomic<uint32_t> &head) noexcept
{
    uint32_t current = head.load(std::memory_order_acquire);
    if (current == kNoSlot)
        return kNoSlot;
    ArenaSlot<T> &slot = arena[static_cast<size_t>(current - 1u)];
    const uint32_t next = slot.nextFree.load(std::memory_order_relaxed);
    if (!head.compare_exchange_strong(current, next, std::memory_order_acq_rel,
                                      std::memory_order_acquire))
    {
        m2causal::fail();
        return kNoSlot;
    }
    slot.value = T{};
    return current;
}

template <typename T, size_t N>
void pushArena(std::array<ArenaSlot<T>, N> &arena, std::atomic<uint32_t> &head,
               uint32_t index) noexcept
{
    if (index == kNoSlot)
        return;
    ArenaSlot<T> &slot = arena[static_cast<size_t>(index - 1u)];
    uint32_t current = head.load(std::memory_order_acquire);
    slot.nextFree.store(current, std::memory_order_relaxed);
    if (!head.compare_exchange_strong(current, index, std::memory_order_release,
                                      std::memory_order_acquire))
        m2causal::fail();
}

void releaseMetadata(Root &root) noexcept
{
    if (!claimArena())
        return;
    uint32_t source = root.sourceHead;
    while (source != kNoSlot)
    {
        const uint32_t next = g_sources[static_cast<size_t>(source - 1u)].value.next;
        pushArena(g_sources, g_sourceFree, source);
        source = next;
    }
    uint32_t tag = root.tagHead;
    while (tag != kNoSlot)
    {
        const uint32_t next = g_tags[static_cast<size_t>(tag - 1u)].value.next;
        pushArena(g_tags, g_tagFree, tag);
        tag = next;
    }
    root.sourceHead = root.sourceTail = kNoSlot;
    root.sourceCount = 0u;
    root.tagHead = root.tagTail = kNoSlot;
    root.tagCount = 0u;
    releaseArena();
}

void resetArenas() noexcept
{
    for (uint32_t i = 0u; i < kSourceSlots; ++i)
        g_sources[i].nextFree.store(i + 2u <= kSourceSlots ? i + 2u : kNoSlot,
                                    std::memory_order_relaxed);
    for (uint32_t i = 0u; i < kTagSlots; ++i)
        g_tags[i].nextFree.store(i + 2u <= kTagSlots ? i + 2u : kNoSlot,
                                 std::memory_order_relaxed);
    g_sourceFree.store(1u, std::memory_order_release);
    g_tagFree.store(1u, std::memory_order_release);
}

void emit(Event type, m2causal::Source source, uint64_t a = 0u, uint64_t b = 0u,
          uint64_t c = 0u) noexcept
{
    // m2causal::event performs the same collecting gate.  Keeping this guard
    // here prevents ID lookups and source hashing from becoming trace writes
    // in the pre-arm or complete states.
    if (!m2causal::collecting())
        return;
    m2causal::event(static_cast<m2causal::EventType>(static_cast<uint32_t>(type)), source, a, b, c);
}

Root *findRoot(uint64_t id) noexcept
{
    if (id == kUnknownId)
        return nullptr;
    for (Root &root : g_roots)
    {
        if (root.state.load(std::memory_order_acquire) != RootState::Active)
            continue;
        if (root.id.load(std::memory_order_acquire) == id)
            return &root;
    }
    return nullptr;
}

void retireRoot(Root &root) noexcept
{
    if (root.references.load(std::memory_order_acquire) != 0u)
        return;
    RootState expected = RootState::Active;
    if (root.state.compare_exchange_strong(expected, RootState::Retiring, std::memory_order_acq_rel))
    {
        releaseMetadata(root);
        (void)g_activeRoots.fetch_sub(1u, std::memory_order_relaxed);
        // Publish reusable only after the old metadata links are retired.
        root.state.store(RootState::Retired, std::memory_order_release);
    }
}

bool retainRoot(Root &root) noexcept
{
    if (root.state.load(std::memory_order_acquire) != RootState::Active)
        return false;
    uint32_t references = root.references.load(std::memory_order_acquire);
    if (references == 0u || references == UINT32_MAX ||
        !root.references.compare_exchange_strong(references, references + 1u,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire) ||
        root.state.load(std::memory_order_acquire) != RootState::Active)
    {
        m2causal::fail();
        return false;
    }
    return true;
}

void releaseRoot(Root &root) noexcept
{
    uint32_t previous = root.references.load(std::memory_order_acquire);
    if (previous == 0u || !root.references.compare_exchange_strong(previous, previous - 1u,
                                                                    std::memory_order_acq_rel,
                                                                    std::memory_order_acquire))
    {
        m2causal::fail();
        return;
    }
    if (m2causal::collecting())
        emit(Event::RootReleased, m2causal::Source::Vif1,
             root.id.load(std::memory_order_relaxed), previous - 1u, 0u);
    if (previous == 1u)
        retireRoot(root);
}

Root *allocateRoot(uint32_t pc, uint32_t ra, uint32_t route, bool fifoSynthetic,
                   uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr,
                   uint32_t asr0, uint32_t asr1) noexcept
{
    if (g_configured.load(std::memory_order_acquire) == 0u || !m2causal::acceptingDiagnostics())
        return nullptr;

    uint32_t active = g_activeRoots.load(std::memory_order_acquire);
    if (active >= kActiveRoots ||
        !g_activeRoots.compare_exchange_strong(active, active + 1u, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
    {
        m2causal::fail();
        return nullptr;
    }

    Root *selected = nullptr;
    for (Root &root : g_roots)
    {
        RootState expected = RootState::Empty;
        if (root.state.compare_exchange_strong(expected, RootState::Initializing, std::memory_order_acq_rel))
        {
            selected = &root;
            break;
        }
    }
    if (!selected)
    {
        for (Root &root : g_roots)
        {
            RootState expected = RootState::Retired;
            if (root.references.load(std::memory_order_acquire) == 0u &&
                root.state.compare_exchange_strong(expected, RootState::Initializing,
                                                   std::memory_order_acq_rel))
            {
                selected = &root;
                break;
            }
        }
    }
    if (!selected)
    {
        (void)g_activeRoots.fetch_sub(1u, std::memory_order_relaxed);
        m2causal::fail();
        return nullptr;
    }

    selected->references.store(0u, std::memory_order_relaxed);
    selected->emitted.store(0u, std::memory_order_relaxed);
    selected->sealed.store(0u, std::memory_order_relaxed);
    selected->busy.store(0u, std::memory_order_relaxed);
    selected->id.store(kUnknownId, std::memory_order_relaxed);
    selected->pc = 0u;
    selected->ra = 0u;
    selected->route = 0u;
    selected->createdBeforeSelector = 0u;
    selected->syntheticFifo = 0u;
    selected->chcr = 0u;
    selected->madr = 0u;
    selected->qwc = 0u;
    selected->tadr = 0u;
    selected->asr0 = 0u;
    selected->asr1 = 0u;
    selected->sourceHead = selected->sourceTail = kNoSlot;
    selected->sourceCount = 0u;
    selected->tagHead = selected->tagTail = kNoSlot;
    selected->tagCount = 0u;
    selected->queueAddress = 0u;
    selected->flatBytes = 0u;
    selected->packetHint = 0u;
    selected->queueDigest = 0u;
    selected->queueDigestKnown = 0u;
    selected->queueAccepted = 0u;
    selected->observedAddress = 0u;
    selected->observedBytes = 0u;
    selected->observedDigest = 0u;
    selected->id.store(g_nextRoot.fetch_add(1u, std::memory_order_relaxed), std::memory_order_relaxed);
    selected->pc = pc;
    selected->ra = ra;
    selected->route = route;
    selected->createdBeforeSelector = m2causal::collecting() ? 0u : 1u;
    selected->syntheticFifo = fifoSynthetic ? 1u : 0u;
    selected->chcr = chcr;
    selected->madr = madr;
    selected->qwc = qwc;
    selected->tadr = tadr;
    selected->asr0 = asr0;
    selected->asr1 = asr1;
    selected->references.store(1u, std::memory_order_relaxed);
    selected->state.store(RootState::Active, std::memory_order_release);
    return selected;
}

void missing(uint64_t id, uint64_t site) noexcept
{
    emit(Event::MissingRoot, m2causal::Source::Vif1, id, site, 0u);
    m2causal::fail();
}

bool lockMetadata(Root &root) noexcept
{
    uint32_t expected = 0u;
    if (root.busy.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel))
        return true;
    // The provenance collector does not wait for competing producer paths.
    // A concurrent mutation is an unsupported diagnostic condition, not a
    // reason to stall guest execution or race the retained metadata.
    m2causal::fail();
    return false;
}

void unlockMetadata(Root &root) noexcept
{
    root.busy.store(0u, std::memory_order_release);
}

void ensureEmitted(Root &root) noexcept
{
    if (!m2causal::collecting())
        return;
    if (!lockMetadata(root))
        return;
    uint32_t expected = 0u;
    if (!root.emitted.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel))
    {
        unlockMetadata(root);
        return;
    }

    root.sealed.store(1u, std::memory_order_release);
    const uint64_t id = root.id.load(std::memory_order_relaxed);
    emit(Event::RootOrigin, m2causal::Source::Vif1, id,
         pair32(root.pc, root.ra), pair32(root.route, root.createdBeforeSelector));
    if (root.syntheticFifo != 0u)
        emit(Event::FifoSyntheticRoot, m2causal::Source::Vif1, id,
             pair32(root.pc, root.ra), root.route);
    else
    {
        emit(Event::RootKick, m2causal::Source::Vif1, id,
             pair32(root.chcr, root.madr), root.syntheticFifo);
        emit(Event::RootKickChain, m2causal::Source::Vif1, id,
             pair32(root.qwc, root.tadr), pair32(root.asr0, root.asr1));
    }
    for (uint32_t index = root.sourceHead; index != kNoSlot; )
    {
        const SourceInfo &source = g_sources[static_cast<size_t>(index - 1u)].value;
        emit(Event::SourceRange, m2causal::Source::Vif1, id,
             pair32(source.space, source.guestAddress), pair32(source.bytes, source.flatOffset));
        emit(Event::SourceDigest, m2causal::Source::Vif1, id, source.digest, source.tagIndex);
        index = source.next;
    }
    for (uint32_t index = root.tagHead; index != kNoSlot; )
    {
        const TagInfo &tag = g_tags[static_cast<size_t>(index - 1u)].value;
        emit(Event::TagWords, m2causal::Source::Vif1, id, tag.index, tag.low);
        emit(Event::TagUpper, m2causal::Source::Vif1, id, tag.index, tag.upper);
        emit(Event::TagDigest, m2causal::Source::Vif1, id, tag.digest, tag.index);
        index = tag.next;
    }
    if (root.queueAccepted != 0u)
    {
        emit(Event::QueueAccepted, m2causal::Source::Vif1, id,
             pair32(root.queueAddress, root.flatBytes), root.packetHint);
        if (root.queueDigestKnown != 0u)
            emit(Event::QueueDigest, m2causal::Source::Vif1, id, root.queueDigest, root.flatBytes);
    }
    if (root.observedBytes != 0u)
        emit(Event::ObservedInput, m2causal::Source::Vif1, id,
             pair32(root.observedAddress, root.observedBytes), root.observedDigest);
    emit(Event::RootActivated, m2causal::Source::Vif1, id,
         root.createdBeforeSelector, root.references.load(std::memory_order_relaxed));
    unlockMetadata(root);
}

Root *contextRoot(uint64_t id, uint64_t site) noexcept
{
    if (id == kUnknownId)
        return nullptr;
    Root *root = findRoot(id);
    if (!root)
        missing(id, site);
    return root;
}

bool canMutate(Root &root) noexcept
{
    if (!lockMetadata(root))
        return false;
    if (root.sealed.load(std::memory_order_acquire) == 0u)
        return true;
    // Late mutation would make lazy history depend on producer interleaving.
    m2causal::fail();
    unlockMetadata(root);
    return false;
}
} // namespace

bool initialize(bool configured) noexcept
{
    g_configured.store(configured ? 1u : 0u, std::memory_order_release);
    if (configured)
        resetArenas();
    return true;
}

bool configured() noexcept
{
    return g_configured.load(std::memory_order_acquire) != 0u;
}

size_t storageBytes() noexcept
{
    return sizeof(g_roots) + sizeof(g_sources) + sizeof(g_tags);
}

OriginScope::OriginScope(uint32_t pc, uint32_t ra, uint32_t route, bool eligible) noexcept
    : m_previousPc(g_currentOrigin.pc), m_previousRa(g_currentOrigin.ra),
      m_previousRoute(g_currentOrigin.route), m_previousActive(g_currentOrigin.active),
      m_active(eligible)
{
    if (m_active)
        g_currentOrigin = OriginContext{pc, ra, route, true};
}

OriginScope::~OriginScope()
{
    if (m_active)
        g_currentOrigin = OriginContext{m_previousPc, m_previousRa, m_previousRoute, m_previousActive};
}

KickScope::KickScope(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr,
                     uint32_t asr0, uint32_t asr1) noexcept
    : m_id(kUnknownId), m_pending(false)
{
    const uint32_t pc = g_currentOrigin.active ? g_currentOrigin.pc : 0u;
    const uint32_t ra = g_currentOrigin.active ? g_currentOrigin.ra : 0u;
    const uint32_t route = g_currentOrigin.active ? g_currentOrigin.route : 0u;
    Root *root = allocateRoot(pc, ra, route, false, chcr, madr, qwc, tadr, asr0, asr1);
    if (!root)
        return;
    m_id = root->id.load(std::memory_order_relaxed);
}

KickScope::~KickScope()
{
    if (Root *root = findRoot(m_id))
        releaseRoot(*root);
}

void KickScope::source(uint32_t space, uint32_t guestAddress, const uint8_t *bytes,
                       uint32_t sizeBytes, uint32_t flatOffset, uint32_t tagIndex) noexcept
{
    Root *root = contextRoot(m_id, 1u);
    if (!root || !canMutate(*root))
        return;
    if (!claimArena())
    {
        unlockMetadata(*root);
        return;
    }
    const uint32_t index = popArena(g_sources, g_sourceFree);
    if (index == kNoSlot)
    {
        m2causal::fail();
        releaseArena();
        unlockMetadata(*root);
        return;
    }
    SourceInfo &source = g_sources[static_cast<size_t>(index - 1u)].value;
    source = SourceInfo{kNoSlot, space, guestAddress, sizeBytes, flatOffset, tagIndex,
                        fnv1a(bytes, sizeBytes)};
    // Original-source capture marker, distinct from this root's later lazy
    // history export. No GS receipt or producer byte is changed.
    m2guest::sourceSnapshot(m_id, space, guestAddress, bytes, sizeBytes);
    if (root->sourceTail != kNoSlot)
        g_sources[static_cast<size_t>(root->sourceTail - 1u)].value.next = index;
    else
        root->sourceHead = index;
    root->sourceTail = index;
    ++root->sourceCount;
    releaseArena();
    unlockMetadata(*root);
}

void KickScope::tag(uint32_t tagIndex, uint64_t low, uint64_t upper) noexcept
{
    Root *root = contextRoot(m_id, 2u);
    if (!root || !canMutate(*root))
        return;
    if (!claimArena())
    {
        unlockMetadata(*root);
        return;
    }
    const uint32_t index = popArena(g_tags, g_tagFree);
    if (index == kNoSlot)
    {
        m2causal::fail();
        releaseArena();
        unlockMetadata(*root);
        return;
    }
    const uint8_t words[16] = {
        static_cast<uint8_t>(low), static_cast<uint8_t>(low >> 8u),
        static_cast<uint8_t>(low >> 16u), static_cast<uint8_t>(low >> 24u),
        static_cast<uint8_t>(low >> 32u), static_cast<uint8_t>(low >> 40u),
        static_cast<uint8_t>(low >> 48u), static_cast<uint8_t>(low >> 56u),
        static_cast<uint8_t>(upper), static_cast<uint8_t>(upper >> 8u),
        static_cast<uint8_t>(upper >> 16u), static_cast<uint8_t>(upper >> 24u),
        static_cast<uint8_t>(upper >> 32u), static_cast<uint8_t>(upper >> 40u),
        static_cast<uint8_t>(upper >> 48u), static_cast<uint8_t>(upper >> 56u)};
    TagInfo &tag = g_tags[static_cast<size_t>(index - 1u)].value;
    tag = TagInfo{kNoSlot, tagIndex, low, upper, fnv1a(words, sizeof(words))};
    if (root->tagTail != kNoSlot)
        g_tags[static_cast<size_t>(root->tagTail - 1u)].value.next = index;
    else
        root->tagHead = index;
    root->tagTail = index;
    ++root->tagCount;
    releaseArena();
    unlockMetadata(*root);
}

void KickScope::queueAccepted(uint32_t queueAddress, uint32_t flatBytes,
                              const uint8_t *flattenedBytes, uint32_t packetHint) noexcept
{
    Root *root = contextRoot(m_id, 3u);
    if (!root || !canMutate(*root))
        return;
    root->queueAddress = queueAddress;
    root->flatBytes = flatBytes;
    root->packetHint = packetHint;
    root->queueDigest = flattenedBytes ? fnv1a(flattenedBytes, flatBytes) : 0u;
    root->queueDigestKnown = flattenedBytes ? 1u : 0u;
    root->queueAccepted = 1u;
    if (!m_pending && retainRoot(*root))
        m_pending = true;
    unlockMetadata(*root);
}

ProvenanceScope::ProvenanceScope(uint64_t rootId) noexcept
    : m_previous(g_currentRoot), m_id(rootId)
{
    g_currentRoot = kUnknownId;
    Root *root = contextRoot(rootId, 4u);
    if (!root || !retainRoot(*root))
    {
        m_id = kUnknownId;
        return;
    }
    g_currentRoot = rootId;
    ensureEmitted(*root);
}

ProvenanceScope::~ProvenanceScope()
{
    if (Root *root = findRoot(m_id))
        releaseRoot(*root);
    g_currentRoot = m_previous;
}

DecoderScope::DecoderScope(const uint8_t * /*data*/, uint32_t sizeBytes) noexcept
    : m_id(g_currentRoot), m_hash(kFnvOffset), m_count(0u), m_size(sizeBytes), m_packet(g_currentPacket)
{
    Root *root = contextRoot(m_id, 5u);
    if (root)
    {
        ensureEmitted(*root);
        emit(Event::DecoderBegin, m2causal::Source::Vif1, m_id, m_size, m_packet);
    }
}

DecoderScope::~DecoderScope()
{
    if (m_id != kUnknownId)
        emit(Event::DecoderDigest, m2causal::Source::Vif1, m_id, m_hash, m_count);
}

void DecoderScope::command(uint32_t word, uint32_t inputOffset) noexcept
{
    if (m_id == kUnknownId || !m2causal::collecting())
        return;
    const uint8_t bytes[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8u),
                              static_cast<uint8_t>(word >> 16u), static_cast<uint8_t>(word >> 24u)};
    for (const uint8_t byte : bytes)
    {
        m_hash ^= byte;
        m_hash *= kFnvPrime;
    }
    emit(Event::DecoderCommand, m2causal::Source::Vif1, m_id, pair32(word, inputOffset), m_count++);
}

DirectScope::DirectScope(uint32_t site, uint32_t commandOffset, uint32_t qwc,
                         const uint8_t *data, uint64_t openingDirectId) noexcept
    : m_previousPacket(g_currentPacket), m_packet(kUnknownId), m_root(g_currentRoot)
{
    g_currentPacket = kUnknownId;
    if (!m2causal::collecting())
        return;
    if (configured() && m_root == kUnknownId)
    {
        missing(m_root, 6u);
        return;
    }
    Root *root = contextRoot(m_root, 6u);
    if (!root)
        return;
    ensureEmitted(*root);
    m_packet = g_nextPacket.fetch_add(1u, std::memory_order_relaxed);
    g_currentPacket = m_packet;
    const uint32_t bytes = qwc * 16u;
    emit(Event::DirectBegin, m2causal::Source::Vif1, m_packet, m_root, pair32(site, commandOffset));
    emit(Event::DirectPayload, m2causal::Source::Vif1, m_packet, fnv1a(data, bytes), pair32(qwc, bytes));
    emit(Event::DirectOpeningLink, m2causal::Source::Vif1, m_packet, m_root, openingDirectId);
}

DirectScope::~DirectScope()
{
    g_currentPacket = m_previousPacket;
}

DeliveryScope::DeliveryScope(uint64_t rootId, uint64_t packet) noexcept
    : m_id(rootId), m_packet(packet), m_previousRoot(g_currentRoot), m_previousPacket(g_currentPacket)
{
    // An unowned/non-PATH2 callback must not inherit its caller's VIF1 root.
    g_currentRoot = kUnknownId;
    g_currentPacket = kUnknownId;
    Root *root = contextRoot(rootId, 7u);
    if (!root)
    {
        m_id = kUnknownId;
        return;
    }
    ensureEmitted(*root);
    g_currentRoot = m_id;
    g_currentPacket = m_packet;
    emit(Event::DeliveryEnter, m2causal::Source::Gif, m_id, m_packet, 0u);
}

DeliveryScope::~DeliveryScope()
{
    if (m_id != kUnknownId)
    {
        emit(Event::DeliveryExit, m2causal::Source::Gif, m_id, m_packet, 0u);
        if (Root *root = findRoot(m_id))
            releaseRoot(*root);
    }
    g_currentRoot = m_previousRoot;
    g_currentPacket = m_previousPacket;
}

uint64_t createFifoOrigin(uint32_t pc, uint32_t ra, uint32_t route) noexcept
{
    Root *root = allocateRoot(pc, ra, route, true, 0u, 0u, 0u, 0u, 0u, 0u);
    return root ? root->id.load(std::memory_order_relaxed) : kUnknownId;
}

void observeInput(uint64_t rootId, uint32_t guestAddress, const uint8_t *bytes,
                  uint32_t sizeBytes) noexcept
{
    Root *root = contextRoot(rootId, 12u);
    if (!root)
        return;
    if (!lockMetadata(*root))
        return;
    if (root->sealed.load(std::memory_order_acquire) != 0u)
    {
        // Queue processing intentionally observes bytes after its provenance
        // scope has started.  Emit this independent decoder-input fact rather
        // than mutating the already serialized retained history.
        emit(Event::ObservedInput, m2causal::Source::Vif1, rootId,
             pair32(guestAddress, sizeBytes), fnv1a(bytes, sizeBytes));
        unlockMetadata(*root);
        return;
    }
    root->observedAddress = guestAddress;
    root->observedBytes = sizeBytes;
    root->observedDigest = fnv1a(bytes, sizeBytes);
    unlockMetadata(*root);
}

void retainPendingImage(uint64_t rootId) noexcept
{
    if (Root *root = contextRoot(rootId, 8u))
        (void)retainRoot(*root);
}

void releasePendingImage(uint64_t rootId) noexcept
{
    if (Root *root = contextRoot(rootId, 9u))
        releaseRoot(*root);
}

void retain(uint64_t rootId) noexcept
{
    if (Root *root = contextRoot(rootId, 10u))
        (void)retainRoot(*root);
}

void release(uint64_t rootId) noexcept
{
    if (Root *root = contextRoot(rootId, 11u))
        releaseRoot(*root);
}

uint64_t currentId() noexcept { return g_currentRoot; }
uint64_t currentPacket() noexcept { return g_currentPacket; }

bool resetForTest(bool enabled) noexcept
{
    g_configured.store(enabled ? 1u : 0u, std::memory_order_release);
    g_nextRoot.store(1u, std::memory_order_release);
    g_nextPacket.store(1u, std::memory_order_release);
    g_activeRoots.store(0u, std::memory_order_release);
    g_currentOrigin = OriginContext{};
    g_currentRoot = kUnknownId;
    g_currentPacket = kUnknownId;
    resetArenas();
    for (Root &root : g_roots)
    {
        root.references.store(0u, std::memory_order_relaxed);
        root.emitted.store(0u, std::memory_order_relaxed);
        root.sealed.store(0u, std::memory_order_relaxed);
        root.busy.store(0u, std::memory_order_relaxed);
        root.id.store(kUnknownId, std::memory_order_relaxed);
        root.sourceHead = root.sourceTail = kNoSlot;
        root.sourceCount = 0u;
        root.tagHead = root.tagTail = kNoSlot;
        root.tagCount = 0u;
        root.observedAddress = 0u;
        root.observedBytes = 0u;
        root.observedDigest = 0u;
        root.queueAccepted = 0u;
        root.state.store(RootState::Empty, std::memory_order_relaxed);
    }
    return true;
}
} // namespace rrv::m2prov

#endif
