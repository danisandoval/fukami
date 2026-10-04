// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RRV_PCSX2_GS_CANONICAL_RECEIPT_H
#define RRV_PCSX2_GS_CANONICAL_RECEIPT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rrv::pcsx2::receipt
{
    inline constexpr uint32_t kSchemaVersion = 1u;
    inline constexpr uint64_t kHashSeed = 0x5252564d32475331ull;
    inline constexpr size_t kDefaultEventCapacity = 1u << 20u;
    inline constexpr size_t kDefaultEpochCapacity = 4096u;

    enum class EventType : uint8_t
    {
        Transfer = 1u,
        Vsync = 2u,
        LocalMemoryRestore = 3u,
    };

    enum EventFlags : uint16_t
    {
        EventFlagNone = 0u,
        EventFlagXgkickDerived = 1u << 0u,
    };

    struct EventRecord
    {
        uint64_t consumerSequence = 0u;
        uint64_t payloadDigest = 0u;
        uint32_t acceptedBytes = 0u;
        uint32_t acceptedQwc = 0u;
        EventType type = EventType::Transfer;
        uint8_t path = 0u;
        uint16_t flags = EventFlagNone;
    };

    struct EpochRecord
    {
        uint64_t guestFieldId = 0u;
        uint64_t gsFieldEpochId = 0u;
        uint64_t firstConsumerSequence = 0u;
        uint64_t lastConsumerSequence = 0u;
        uint64_t firstEventIndex = 0u;
        uint64_t eventCount = 0u;
        uint64_t pathEventCounts[3]{};
        uint64_t xgkickDerivedTransferCount = 0u;
        uint64_t consumerVsyncOrdinal = 0u;
        uint64_t finalEpochDigest = 0u;
        uint32_t fieldParity = 0u;
        uint32_t successfulClosure = 0u;
    };

    static_assert(sizeof(EventRecord) == 32u);
    static_assert(sizeof(EpochRecord) == 104u);

    struct Status
    {
        bool compiled = true;
        bool enabled = false;
        bool armed = false;
        bool collecting = false;
        bool intervalComplete = false;
        bool overflowed = false;
        bool exportFailed = false;
        uint64_t eventCount = 0u;
        uint64_t epochCount = 0u;
        uint64_t targetEpochCount = 0u;
    };

    // XXH64 is serialized as an unsigned 64-bit value and rendered as 16
    // lowercase hexadecimal digits. The algorithm is the portable XXH64
    // specification with the fixed seed above; input byte order is never the
    // host's native struct representation.
    uint64_t xxh64(const void *data, size_t size, uint64_t seed = kHashSeed);

    struct HashState;

    class Collector final
    {
    public:
        Collector();
        ~Collector();

        Collector(const Collector &) = delete;
        Collector &operator=(const Collector &) = delete;

        bool initializeFromEnvironment(std::string *error);
        bool initializeForTest(bool enabled, bool initiallyArmed, uint64_t targetEpochCount,
                               size_t eventCapacity, size_t epochCapacity,
                               const std::string &deferredOutputPath, std::string *error);
        bool arm();
        bool setEpochIdentity(uint64_t guestFieldId, uint64_t gsFieldEpochId);

        // Call only after the corresponding synchronous PCSX2 consumer call
        // returned successfully. These methods allocate, lock, wait and write
        // nothing; false marks the diagnostic interval failed/overflowed.
        bool acknowledgeTransfer(uint32_t path, const uint8_t *bytes, uint32_t sizeBytes);
        bool acknowledgeLocalMemoryRestore(const uint8_t *bytes, uint32_t sizeBytes);
        bool acknowledgeVsync(const uint64_t *regs19, uint32_t pcsx2Field,
                              bool registersWritten, uint32_t fieldParity);

        Status status() const;
        bool dumpDeferred(std::string *error);

        const EventRecord *events() const { return m_events.get(); }
        const EpochRecord *epochs() const { return m_epochs.get(); }

    private:
        bool initialize(bool enabled, bool initiallyArmed, uint64_t targetEpochCount,
                        size_t eventCapacity, size_t epochCapacity,
                        const std::string &deferredOutputPath, std::string *error);
        bool appendEvent(EventType type, uint8_t path, uint16_t flags,
                         uint32_t acceptedBytes, uint32_t acceptedQwc,
                         uint64_t payloadDigest);
        void resetOpenEpochHash();
        void hashOpenEpochEvent(const EventRecord &event);
        void markOverflow();

        std::unique_ptr<EventRecord[]> m_events;
        std::unique_ptr<EpochRecord[]> m_epochs;
        std::unique_ptr<HashState> m_openEpochHash;
        std::string m_outputPath;
        size_t m_eventCapacity = 0u;
        size_t m_epochCapacity = 0u;
        uint64_t m_eventCount = 0u;
        uint64_t m_epochCount = 0u;
        uint64_t m_targetEpochCount = 0u;
        uint64_t m_nextConsumerSequence = 1u;
        uint64_t m_nextConsumerVsyncOrdinal = 1u;
        uint64_t m_openFirstEventIndex = 0u;
        uint64_t m_openPathEventCounts[3]{};
        uint64_t m_openXgkickDerivedTransferCount = 0u;
        uint64_t m_pendingGuestFieldId = 0u;
        uint64_t m_pendingGsFieldEpochId = 0u;
        bool m_enabled = false;
        bool m_armed = false;
        bool m_collecting = false;
        bool m_intervalComplete = false;
        bool m_overflowed = false;
        bool m_exportFailed = false;
        bool m_hasPendingEpochIdentity = false;
    };
} // namespace rrv::pcsx2::receipt

#endif
