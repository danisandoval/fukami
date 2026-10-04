// SPDX-License-Identifier: GPL-3.0-or-later
#include "canonical_receipt.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace rrv::pcsx2::receipt
{
namespace
{
    constexpr uint64_t kPrime1 = 11400714785074694791ull;
    constexpr uint64_t kPrime2 = 14029467366897019727ull;
    constexpr uint64_t kPrime3 = 1609587929392839161ull;
    constexpr uint64_t kPrime4 = 9650029242287828579ull;
    constexpr uint64_t kPrime5 = 2870177450012600261ull;
    constexpr char kEpochDomain[] = "RRV-GS-CONSUMED-EPOCH-v1";

    uint64_t rotateLeft(uint64_t value, uint32_t amount)
    {
        return (value << amount) | (value >> (64u - amount));
    }

    uint32_t loadLe32(const uint8_t *bytes)
    {
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8u) |
               (static_cast<uint32_t>(bytes[2]) << 16u) |
               (static_cast<uint32_t>(bytes[3]) << 24u);
    }

    uint64_t loadLe64(const uint8_t *bytes)
    {
        uint64_t value = 0u;
        for (uint32_t index = 0u; index != 8u; ++index)
            value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
        return value;
    }

    void storeLe16(uint8_t *bytes, uint16_t value)
    {
        bytes[0] = static_cast<uint8_t>(value);
        bytes[1] = static_cast<uint8_t>(value >> 8u);
    }

    void storeLe32(uint8_t *bytes, uint32_t value)
    {
        for (uint32_t index = 0u; index != 4u; ++index)
            bytes[index] = static_cast<uint8_t>(value >> (index * 8u));
    }

    void storeLe64(uint8_t *bytes, uint64_t value)
    {
        for (uint32_t index = 0u; index != 8u; ++index)
            bytes[index] = static_cast<uint8_t>(value >> (index * 8u));
    }

    uint64_t round(uint64_t accumulator, uint64_t input)
    {
        accumulator += input * kPrime2;
        accumulator = rotateLeft(accumulator, 31u);
        return accumulator * kPrime1;
    }

    uint64_t mergeRound(uint64_t accumulator, uint64_t value)
    {
        accumulator ^= round(0u, value);
        return accumulator * kPrime1 + kPrime4;
    }

    uint64_t avalanche(uint64_t hash)
    {
        hash ^= hash >> 33u;
        hash *= kPrime2;
        hash ^= hash >> 29u;
        hash *= kPrime3;
        hash ^= hash >> 32u;
        return hash;
    }

    bool parsePositive(const char *name, uint64_t fallback, uint64_t maximum,
                       uint64_t &result, std::string *error)
    {
        const char *value = std::getenv(name);
        if (!value || value[0] == '\0')
        {
            result = fallback;
            return true;
        }
        if (value[0] < '0' || value[0] > '9')
        {
            if (error) *error = std::string(name) + " must be a positive integer";
            return false;
        }
        char *end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (errno == ERANGE || !end || *end != '\0' || parsed == 0u || parsed > maximum)
        {
            if (error) *error = std::string(name) + " is outside its supported range";
            return false;
        }
        result = static_cast<uint64_t>(parsed);
        return true;
    }

    bool envFlag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && value[0] != '\0' && value[0] != '0';
    }

    bool writeBytes(FILE *file, const void *bytes, size_t size)
    {
        return size == 0u || std::fwrite(bytes, 1u, size, file) == size;
    }

    bool writeU32(FILE *file, uint32_t value)
    {
        std::array<uint8_t, 4u> bytes{};
        storeLe32(bytes.data(), value);
        return writeBytes(file, bytes.data(), bytes.size());
    }

    bool writeU64(FILE *file, uint64_t value)
    {
        std::array<uint8_t, 8u> bytes{};
        storeLe64(bytes.data(), value);
        return writeBytes(file, bytes.data(), bytes.size());
    }
} // namespace

struct HashState
{
    uint64_t totalLength = 0u;
    uint64_t seed = 0u;
    uint64_t v1 = 0u;
    uint64_t v2 = 0u;
    uint64_t v3 = 0u;
    uint64_t v4 = 0u;
    std::array<uint8_t, 32u> tail{};
    size_t tailSize = 0u;

    void reset(uint64_t newSeed)
    {
        totalLength = 0u;
        seed = newSeed;
        v1 = seed + kPrime1 + kPrime2;
        v2 = seed + kPrime2;
        v3 = seed;
        v4 = seed - kPrime1;
        tailSize = 0u;
    }

    void consumeStripe(const uint8_t *bytes)
    {
        v1 = round(v1, loadLe64(bytes));
        v2 = round(v2, loadLe64(bytes + 8u));
        v3 = round(v3, loadLe64(bytes + 16u));
        v4 = round(v4, loadLe64(bytes + 24u));
    }

    void update(const void *source, size_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(source);
        totalLength += size;
        if (tailSize + size < tail.size())
        {
            if (size != 0u)
                std::memcpy(tail.data() + tailSize, bytes, size);
            tailSize += size;
            return;
        }
        if (tailSize != 0u)
        {
            const size_t needed = tail.size() - tailSize;
            std::memcpy(tail.data() + tailSize, bytes, needed);
            consumeStripe(tail.data());
            bytes += needed;
            size -= needed;
            tailSize = 0u;
        }
        while (size >= tail.size())
        {
            consumeStripe(bytes);
            bytes += tail.size();
            size -= tail.size();
        }
        if (size != 0u)
        {
            std::memcpy(tail.data(), bytes, size);
            tailSize = size;
        }
    }

    uint64_t digest() const
    {
        uint64_t hash = 0u;
        if (totalLength >= 32u)
        {
            hash = rotateLeft(v1, 1u) + rotateLeft(v2, 7u) +
                   rotateLeft(v3, 12u) + rotateLeft(v4, 18u);
            hash = mergeRound(hash, v1);
            hash = mergeRound(hash, v2);
            hash = mergeRound(hash, v3);
            hash = mergeRound(hash, v4);
        }
        else
        {
            hash = seed + kPrime5;
        }
        hash += totalLength;
        const uint8_t *bytes = tail.data();
        size_t size = tailSize;
        while (size >= 8u)
        {
            const uint64_t lane = round(0u, loadLe64(bytes));
            hash ^= lane;
            hash = rotateLeft(hash, 27u) * kPrime1 + kPrime4;
            bytes += 8u;
            size -= 8u;
        }
        if (size >= 4u)
        {
            hash ^= static_cast<uint64_t>(loadLe32(bytes)) * kPrime1;
            hash = rotateLeft(hash, 23u) * kPrime2 + kPrime3;
            bytes += 4u;
            size -= 4u;
        }
        while (size != 0u)
        {
            hash ^= static_cast<uint64_t>(*bytes++) * kPrime5;
            hash = rotateLeft(hash, 11u) * kPrime1;
            --size;
        }
        return avalanche(hash);
    }
};

uint64_t xxh64(const void *data, size_t size, uint64_t seed)
{
    HashState state;
    state.reset(seed);
    state.update(data, size);
    return state.digest();
}

Collector::Collector() = default;
Collector::~Collector() = default;

bool Collector::initializeFromEnvironment(std::string *error)
{
    const char *path = std::getenv("RRV_GS_CANONICAL_RECEIPT");
    if (!path || path[0] == '\0')
        return initialize(false, false, 0u, kDefaultEventCapacity,
                          kDefaultEpochCapacity, {}, error);

    uint64_t target = 360u;
    uint64_t eventCapacity = kDefaultEventCapacity;
    uint64_t epochCapacity = kDefaultEpochCapacity;
    if (!parsePositive("RRV_GS_CANONICAL_RECEIPT_FIELDS", 360u,
                       kDefaultEpochCapacity, target, error) ||
        !parsePositive("RRV_GS_CANONICAL_RECEIPT_EVENT_CAPACITY",
                       kDefaultEventCapacity, kDefaultEventCapacity,
                       eventCapacity, error) ||
        !parsePositive("RRV_GS_CANONICAL_RECEIPT_EPOCH_CAPACITY",
                       kDefaultEpochCapacity, kDefaultEpochCapacity,
                       epochCapacity, error))
    {
        return false;
    }
    if (target > epochCapacity)
    {
        if (error) *error = "canonical receipt field target exceeds epoch capacity";
        return false;
    }
    return initialize(true, envFlag("RRV_GS_CANONICAL_RECEIPT_ARM"), target,
                      static_cast<size_t>(eventCapacity),
                      static_cast<size_t>(epochCapacity), path, error);
}

bool Collector::initializeForTest(bool enabled, bool initiallyArmed,
                                  uint64_t targetEpochCount, size_t eventCapacity,
                                  size_t epochCapacity,
                                  const std::string &deferredOutputPath,
                                  std::string *error)
{
    return initialize(enabled, initiallyArmed, targetEpochCount, eventCapacity,
                      epochCapacity, deferredOutputPath, error);
}

bool Collector::initialize(bool enabled, bool initiallyArmed,
                           uint64_t targetEpochCount, size_t eventCapacity,
                           size_t epochCapacity,
                           const std::string &deferredOutputPath,
                           std::string *error)
{
    m_enabled = enabled;
    m_armed = enabled && initiallyArmed;
    m_collecting = enabled && !initiallyArmed;
    m_targetEpochCount = targetEpochCount;
    m_eventCapacity = eventCapacity;
    m_epochCapacity = epochCapacity;
    m_outputPath = deferredOutputPath;
    if (!enabled)
        return true;
    if (targetEpochCount == 0u || eventCapacity == 0u || epochCapacity == 0u ||
        targetEpochCount > epochCapacity || deferredOutputPath.empty())
    {
        if (error) *error = "invalid canonical receipt storage configuration";
        return false;
    }
    m_events.reset(new (std::nothrow) EventRecord[eventCapacity]{});
    m_epochs.reset(new (std::nothrow) EpochRecord[epochCapacity]{});
    m_openEpochHash.reset(new (std::nothrow) HashState{});
    if (!m_events || !m_epochs || !m_openEpochHash)
    {
        if (error) *error = "canonical receipt preallocation failed";
        return false;
    }
    resetOpenEpochHash();
    return true;
}

bool Collector::arm()
{
    if (!m_enabled || m_intervalComplete || m_overflowed)
        return !m_enabled;
    if (m_armed)
    {
        m_armed = false;
        m_collecting = true;
    }
    return true;
}

bool Collector::setEpochIdentity(uint64_t guestFieldId, uint64_t gsFieldEpochId)
{
    if (!m_collecting || m_intervalComplete)
        return true;
    if (m_overflowed || m_hasPendingEpochIdentity || gsFieldEpochId == 0u)
    {
        markOverflow();
        return false;
    }
    m_pendingGuestFieldId = guestFieldId;
    m_pendingGsFieldEpochId = gsFieldEpochId;
    m_hasPendingEpochIdentity = true;
    return true;
}

bool Collector::appendEvent(EventType type, uint8_t path, uint16_t flags,
                            uint32_t acceptedBytes, uint32_t acceptedQwc,
                            uint64_t payloadDigest)
{
    if (!m_collecting || m_intervalComplete)
        return true;
    if (m_overflowed || m_eventCount >= m_eventCapacity ||
        m_nextConsumerSequence == 0u)
    {
        markOverflow();
        return false;
    }
    EventRecord &event = m_events[m_eventCount++];
    event.consumerSequence = m_nextConsumerSequence++;
    event.payloadDigest = payloadDigest;
    event.acceptedBytes = acceptedBytes;
    event.acceptedQwc = acceptedQwc;
    event.type = type;
    event.path = path;
    event.flags = flags;
    hashOpenEpochEvent(event);
    return true;
}

bool Collector::acknowledgeTransfer(uint32_t path, const uint8_t *bytes,
                                    uint32_t sizeBytes)
{
    if (!m_collecting || m_intervalComplete)
        return true;
    if (path < 1u || path > 3u || (sizeBytes != 0u && !bytes) ||
        (sizeBytes & 0x0fu) != 0u)
    {
        markOverflow();
        return false;
    }
    const uint16_t flags = path == 1u ? EventFlagXgkickDerived : EventFlagNone;
    if (!appendEvent(EventType::Transfer, static_cast<uint8_t>(path), flags,
                     sizeBytes, sizeBytes / 16u, xxh64(bytes, sizeBytes)))
    {
        return false;
    }
    ++m_openPathEventCounts[path - 1u];
    if (path == 1u)
        ++m_openXgkickDerivedTransferCount;
    return true;
}

bool Collector::acknowledgeLocalMemoryRestore(const uint8_t *bytes,
                                               uint32_t sizeBytes)
{
    if (!m_collecting || m_intervalComplete)
        return true;
    if ((sizeBytes != 0u && !bytes) || (sizeBytes & 0x0fu) != 0u)
    {
        markOverflow();
        return false;
    }
    return appendEvent(EventType::LocalMemoryRestore, 0u, EventFlagNone,
                       sizeBytes, sizeBytes / 16u, xxh64(bytes, sizeBytes));
}

bool Collector::acknowledgeVsync(const uint64_t *regs19, uint32_t pcsx2Field,
                                 bool registersWritten, uint32_t fieldParity)
{
    if (!m_collecting || m_intervalComplete)
        return true;
    if (!regs19 || pcsx2Field > 1u || fieldParity > 1u ||
        !m_hasPendingEpochIdentity || m_epochCount >= m_epochCapacity)
    {
        markOverflow();
        return false;
    }

    std::array<uint8_t, 19u * 8u + 5u> vsyncPayload{};
    for (size_t index = 0u; index != 19u; ++index)
        storeLe64(vsyncPayload.data() + index * 8u, regs19[index]);
    storeLe32(vsyncPayload.data() + 19u * 8u, pcsx2Field);
    vsyncPayload.back() = registersWritten ? 1u : 0u;
    if (!appendEvent(EventType::Vsync, 0u, EventFlagNone,
                     static_cast<uint32_t>(vsyncPayload.size()), 0u,
                     xxh64(vsyncPayload.data(), vsyncPayload.size())))
    {
        return false;
    }

    EpochRecord &epoch = m_epochs[m_epochCount];
    epoch.guestFieldId = m_pendingGuestFieldId;
    epoch.gsFieldEpochId = m_pendingGsFieldEpochId;
    epoch.firstEventIndex = m_openFirstEventIndex;
    epoch.eventCount = m_eventCount - m_openFirstEventIndex;
    epoch.firstConsumerSequence = m_events[m_openFirstEventIndex].consumerSequence;
    epoch.lastConsumerSequence = m_events[m_eventCount - 1u].consumerSequence;
    for (size_t index = 0u; index != 3u; ++index)
        epoch.pathEventCounts[index] = m_openPathEventCounts[index];
    epoch.xgkickDerivedTransferCount = m_openXgkickDerivedTransferCount;
    epoch.consumerVsyncOrdinal = m_nextConsumerVsyncOrdinal++;
    epoch.fieldParity = fieldParity;
    epoch.successfulClosure = 1u;

    std::array<uint8_t, 88u> trailer{};
    size_t offset = 0u;
    const auto append64 = [&](uint64_t value) {
        storeLe64(trailer.data() + offset, value);
        offset += 8u;
    };
    append64(epoch.guestFieldId);
    append64(epoch.gsFieldEpochId);
    append64(epoch.firstConsumerSequence);
    append64(epoch.lastConsumerSequence);
    append64(epoch.eventCount);
    for (uint64_t count : epoch.pathEventCounts)
        append64(count);
    append64(epoch.xgkickDerivedTransferCount);
    append64(epoch.consumerVsyncOrdinal);
    storeLe32(trailer.data() + offset, epoch.fieldParity);
    offset += 4u;
    storeLe32(trailer.data() + offset, epoch.successfulClosure);
    offset += 4u;
    m_openEpochHash->update(trailer.data(), offset);
    epoch.finalEpochDigest = m_openEpochHash->digest();

    ++m_epochCount;
    m_hasPendingEpochIdentity = false;
    m_openFirstEventIndex = m_eventCount;
    std::memset(m_openPathEventCounts, 0, sizeof(m_openPathEventCounts));
    m_openXgkickDerivedTransferCount = 0u;
    if (m_epochCount == m_targetEpochCount)
    {
        m_intervalComplete = true;
        m_collecting = false;
        return true;
    }

    resetOpenEpochHash();
    return true;
}

void Collector::resetOpenEpochHash()
{
    m_openEpochHash->reset(kHashSeed);
    // sizeof includes exactly one canonical NUL terminator.
    m_openEpochHash->update(kEpochDomain, sizeof(kEpochDomain));
}

void Collector::hashOpenEpochEvent(const EventRecord &event)
{
    std::array<uint8_t, 32u> bytes{};
    storeLe64(bytes.data(), event.consumerSequence);
    storeLe64(bytes.data() + 8u, event.payloadDigest);
    storeLe32(bytes.data() + 16u, event.acceptedBytes);
    storeLe32(bytes.data() + 20u, event.acceptedQwc);
    bytes[24] = static_cast<uint8_t>(event.type);
    bytes[25] = event.path;
    storeLe16(bytes.data() + 26u, event.flags);
    m_openEpochHash->update(bytes.data(), bytes.size());
}

void Collector::markOverflow()
{
    m_overflowed = true;
    m_collecting = false;
}

Status Collector::status() const
{
    Status result{};
    result.enabled = m_enabled;
    result.armed = m_armed;
    result.collecting = m_collecting;
    result.intervalComplete = m_intervalComplete;
    result.overflowed = m_overflowed;
    result.exportFailed = m_exportFailed;
    result.eventCount = m_eventCount;
    result.epochCount = m_epochCount;
    result.targetEpochCount = m_targetEpochCount;
    return result;
}

bool Collector::dumpDeferred(std::string *error)
{
    if (!m_enabled)
        return true;
    FILE *file = std::fopen(m_outputPath.c_str(), "wb");
    if (!file)
    {
        m_exportFailed = true;
        if (error) *error = "cannot open deferred canonical receipt output";
        return false;
    }
    const std::array<uint8_t, 8u> magic = {'R','R','V','G','S','C','R','1'};
    uint32_t flags = 0u;
    if (m_intervalComplete) flags |= 1u << 0u;
    if (m_overflowed) flags |= 1u << 1u;
    if (m_hasPendingEpochIdentity || m_openFirstEventIndex != m_eventCount)
        flags |= 1u << 2u;
    bool ok = writeBytes(file, magic.data(), magic.size()) &&
              writeU32(file, kSchemaVersion) && writeU32(file, 80u) &&
              writeU32(file, 32u) && writeU32(file, 104u) &&
              writeU32(file, flags) && writeU32(file, 0u) &&
              writeU64(file, m_eventCapacity) && writeU64(file, m_epochCapacity) &&
              writeU64(file, m_eventCount) && writeU64(file, m_epochCount) &&
              writeU64(file, m_targetEpochCount) &&
              writeU64(file, 0u);
    for (uint64_t index = 0u; ok && index != m_eventCount; ++index)
    {
        const EventRecord &event = m_events[index];
        ok = writeU64(file, event.consumerSequence) &&
             writeU64(file, event.payloadDigest) &&
             writeU32(file, event.acceptedBytes) &&
             writeU32(file, event.acceptedQwc) &&
             std::fputc(static_cast<uint8_t>(event.type), file) != EOF &&
             std::fputc(event.path, file) != EOF;
        std::array<uint8_t, 2u> flagBytes{};
        storeLe16(flagBytes.data(), event.flags);
        ok = ok && writeBytes(file, flagBytes.data(), flagBytes.size()) &&
             writeU32(file, 0u);
    }
    for (uint64_t index = 0u; ok && index != m_epochCount; ++index)
    {
        const EpochRecord &epoch = m_epochs[index];
        ok = writeU64(file, epoch.guestFieldId) &&
             writeU64(file, epoch.gsFieldEpochId) &&
             writeU64(file, epoch.firstConsumerSequence) &&
             writeU64(file, epoch.lastConsumerSequence) &&
             writeU64(file, epoch.firstEventIndex) &&
             writeU64(file, epoch.eventCount);
        for (uint64_t count : epoch.pathEventCounts)
            ok = ok && writeU64(file, count);
        ok = ok && writeU64(file, epoch.xgkickDerivedTransferCount) &&
             writeU64(file, epoch.consumerVsyncOrdinal) &&
             writeU64(file, epoch.finalEpochDigest) &&
             writeU32(file, epoch.fieldParity) &&
             writeU32(file, epoch.successfulClosure);
    }
    if (std::fclose(file) != 0)
        ok = false;
    if (!m_intervalComplete || m_overflowed)
    {
        if (error) *error = "canonical receipt interval did not close successfully";
        return false;
    }
    if (!ok)
    {
        m_exportFailed = true;
        if (error) *error = "deferred canonical receipt output is incomplete";
    }
    return ok;
}
} // namespace rrv::pcsx2::receipt
