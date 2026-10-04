// rrv_m2_dma_provenance.h -- bounded producer provenance for M2 causal traces.
//
// This is diagnostic correlation only.  It neither retains guest bytes nor
// participates in DMA, VIF, GIF, GS, timing, or scheduling decisions.
#ifndef RRV_M2_DMA_PROVENANCE_H
#define RRV_M2_DMA_PROVENANCE_H

#include "rrv_m2_causal_trace.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace rrv::m2prov
{
// Values are serialized through m2causal::event() as EventType values 64--86.
// Keep the assigned values stable: trace readers use them to join a PATH2
// packet back to an original VIF1 DMA root.  `a`, `b`, and `c` are always
// fixed-width integers; pair32() is little-endian independent packing.
enum class Event : uint32_t
{
    RootOrigin = 64,          // a=root, b=PC|RA, c=route|pre-selector bit
    RootKick = 65,            // a=root, b=CHCR|MADR, c=0 (ordinary DMA root)
    RootKickChain = 66,       // a=root, b=QWC|TADR, c=ASR0|ASR1
    // space identifies RAM/SPR physical-offset domains 0--5.  `b` contains a
    // physical offset in that domain; `c.high` is kNoFlatOffset for a source
    // that is not part of a flattened chain buffer.
    SourceRange = 67,         // a=root, b=space|physical offset, c=bytes|flat offset
    SourceDigest = 68,        // a=root, b=FNV-1a, c=tag index
    TagWords = 69,            // a=root, b=tag index, c=low 64 bits
    TagDigest = 70,           // a=root, b=FNV-1a, c=tag index
    QueueAccepted = 71,       // a=root, b=queue address|flat bytes, c=packet hint
    QueueDigest = 72,         // a=root, b=FNV-1a(flat bytes), c=flat bytes
    RootActivated = 73,       // a=root, b=pre-selector flag, c=reference count
    RootReleased = 74,        // a=root, b=reference count, c=0
    DecoderBegin = 75,        // a=root, b=input bytes, c=packet
    DecoderCommand = 76,      // a=root, b=command word|input offset, c=command index
    DecoderDigest = 77,       // a=root, b=FNV-1a(commands), c=command count
    DirectBegin = 78,         // a=packet, b=root, c=site|command offset
    DirectPayload = 79,       // a=packet, b=FNV-1a, c=QWC|byte count
    DirectOpeningLink = 80,   // a=packet, b=current root, c=opening root
    DeliveryEnter = 81,       // a=root, b=packet, c=0
    DeliveryExit = 82,        // a=root, b=packet, c=0
    FifoSyntheticRoot = 83,   // a=root, b=PC|RA, c=route
    MissingRoot = 84,         // a=requested root, b=use site, c=0
    ObservedInput = 85,       // a=root, b=guest address|bytes, c=FNV-1a
    TagUpper = 86,            // a=root, b=tag index, c=upper 64 bits
};

constexpr uint64_t kUnknownId = 0u;
constexpr uint32_t kNoTagIndex = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kNoFlatOffset = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kActiveRootCapacity = 128u;
constexpr uint32_t kRetiredRootCapacity = 128u;
constexpr uint32_t kSourceCapacity = 131072u;
constexpr uint32_t kTagCapacity = 32768u;
constexpr uint64_t pair32(uint32_t low, uint32_t high) noexcept
{
    return uint64_t{low} | (uint64_t{high} << 32u);
}

#if defined(RRV_M2_DMA_PROVENANCE)
// Called exactly once before guest threads, after m2causal initialization.
// `configured` is true only for a bounded Mode-C provenance run.  There is no
// getenv call or configuration branch in producer hot paths after this point.
bool initialize(bool configured) noexcept;
bool configured() noexcept;
size_t storageBytes() noexcept;

class OriginScope final
{
public:
    // `eligible=false` is the ordinary Store32 fast path: it leaves the TLS
    // context untouched for non-VIF writes.
    OriginScope(uint32_t pc, uint32_t ra, uint32_t route, bool eligible = true) noexcept;
    ~OriginScope();
    OriginScope(const OriginScope &) = delete;
    OriginScope &operator=(const OriginScope &) = delete;

private:
    uint32_t m_previousPc;
    uint32_t m_previousRa;
    uint32_t m_previousRoute;
    bool m_previousActive;
    bool m_active;
};

class KickScope final
{
public:
    KickScope(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr,
              uint32_t asr0, uint32_t asr1) noexcept;
    ~KickScope();
    KickScope(const KickScope &) = delete;
    KickScope &operator=(const KickScope &) = delete;

    uint64_t id() const noexcept { return m_id; }
    void source(uint32_t space, uint32_t guestAddress, const uint8_t *bytes,
                uint32_t sizeBytes, uint32_t flatOffset = kNoFlatOffset,
                uint32_t tagIndex = kNoTagIndex) noexcept;
    void tag(uint32_t tagIndex, uint64_t low, uint64_t upper) noexcept;
    void queueAccepted(uint32_t queueAddress, uint32_t flatBytes,
                       const uint8_t *flattenedBytes,
                       uint32_t packetHint = 0u) noexcept;

private:
    uint64_t m_id;
    bool m_pending;
};

// Carries a known root through held VIF/GIF work.  It is a thread-local nested
// context; an unknown/evicted ID remains explicitly unknown rather than being
// attributed to a neighbouring root.
class ProvenanceScope final
{
public:
    explicit ProvenanceScope(uint64_t rootId) noexcept;
    ~ProvenanceScope();
    ProvenanceScope(const ProvenanceScope &) = delete;
    ProvenanceScope &operator=(const ProvenanceScope &) = delete;
    uint64_t id() const noexcept { return m_id; }

private:
    uint64_t m_previous;
    uint64_t m_id;
};

class DecoderScope final
{
public:
    DecoderScope(const uint8_t *data, uint32_t sizeBytes) noexcept;
    ~DecoderScope();
    DecoderScope(const DecoderScope &) = delete;
    DecoderScope &operator=(const DecoderScope &) = delete;
    void command(uint32_t word, uint32_t inputOffset) noexcept;

private:
    uint64_t m_id;
    uint64_t m_hash;
    uint32_t m_count;
    uint32_t m_size;
    uint64_t m_packet;
};

// `openingDirectId` intentionally remains distinct from the current producer
// root.  IMAGE continuations can therefore prove both the input root and the
// original DIRECT-opening root without falsely attributing continuation bytes.
class DirectScope final
{
public:
    DirectScope(uint32_t site, uint32_t commandOffset, uint32_t qwc,
                const uint8_t *data, uint64_t openingDirectId = kUnknownId) noexcept;
    ~DirectScope();
    DirectScope(const DirectScope &) = delete;
    DirectScope &operator=(const DirectScope &) = delete;
    uint64_t packet() const noexcept { return m_packet; }
    uint64_t originId() const noexcept { return m_root; }

private:
    uint64_t m_previousPacket;
    uint64_t m_packet;
    uint64_t m_root;
};

class DeliveryScope final
{
public:
    DeliveryScope(uint64_t rootId, uint64_t packet) noexcept;
    ~DeliveryScope();
    DeliveryScope(const DeliveryScope &) = delete;
    DeliveryScope &operator=(const DeliveryScope &) = delete;

private:
    uint64_t m_id;
    uint64_t m_packet;
    uint64_t m_previousRoot;
    uint64_t m_previousPacket;
};

// Direct FIFO has no ordinary DMAC kick.  It receives an explicit synthetic
// root, which is kept distinct from all VIF1 DMA roots.
uint64_t createFifoOrigin(uint32_t pc, uint32_t ra, uint32_t route) noexcept;
// Captures the physical-offset bytes actually handed to the queued decoder.
// It is separate from the source flatten digest so an input mutation is
// observable without retaining a payload. Call before the root's first
// ProvenanceScope where possible.
void observeInput(uint64_t rootId, uint32_t guestAddress, const uint8_t *bytes,
                  uint32_t sizeBytes) noexcept;
void retain(uint64_t rootId) noexcept;
void release(uint64_t rootId) noexcept;
void retainPendingImage(uint64_t rootId) noexcept;
void releasePendingImage(uint64_t rootId) noexcept;
uint64_t currentId() noexcept;
uint64_t currentPacket() noexcept;

// Asset-free only; uses the same fixed storage and no filesystem operation.
bool resetForTest(bool enabled) noexcept;
#else
inline bool initialize(bool) noexcept { return true; }
inline bool configured() noexcept { return false; }
inline size_t storageBytes() noexcept { return 0u; }
class OriginScope final { public: OriginScope(uint32_t, uint32_t, uint32_t, bool = true) noexcept {} };
class KickScope final {
public:
    KickScope(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
    uint64_t id() const noexcept { return kUnknownId; }
    void source(uint32_t, uint32_t, const uint8_t *, uint32_t, uint32_t = kNoFlatOffset,
                uint32_t = kNoTagIndex) noexcept {}
    void tag(uint32_t, uint64_t, uint64_t) noexcept {}
    void queueAccepted(uint32_t, uint32_t, const uint8_t *, uint32_t = 0u) noexcept {}
};
class ProvenanceScope final {
public: explicit ProvenanceScope(uint64_t) noexcept {}
    uint64_t id() const noexcept { return kUnknownId; }
};
class DecoderScope final {
public: DecoderScope(const uint8_t *, uint32_t) noexcept {}
    void command(uint32_t, uint32_t) noexcept {}
};
class DirectScope final {
public: DirectScope(uint32_t, uint32_t, uint32_t, const uint8_t *, uint64_t = kUnknownId) noexcept {}
    uint64_t packet() const noexcept { return kUnknownId; }
    uint64_t originId() const noexcept { return kUnknownId; }
};
class DeliveryScope final { public: DeliveryScope(uint64_t, uint64_t) noexcept {} };
inline uint64_t createFifoOrigin(uint32_t, uint32_t, uint32_t) noexcept { return kUnknownId; }
inline void observeInput(uint64_t, uint32_t, const uint8_t *, uint32_t) noexcept {}
inline void retain(uint64_t) noexcept {}
inline void release(uint64_t) noexcept {}
inline void retainPendingImage(uint64_t) noexcept {}
inline void releasePendingImage(uint64_t) noexcept {}
inline uint64_t currentId() noexcept { return kUnknownId; }
inline uint64_t currentPacket() noexcept { return kUnknownId; }
inline bool resetForTest(bool) noexcept { return true; }
#endif
} // namespace rrv::m2prov

#endif
