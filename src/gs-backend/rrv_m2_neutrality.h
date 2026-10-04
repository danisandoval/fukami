// SPDX-License-Identifier: MIT
#ifndef RRV_M2_NEUTRALITY_H
#define RRV_M2_NEUTRALITY_H

#include <cstdint>
#include <string>

namespace rrv::m2neutral
{
    // Non-canonical, fixed-size evidence for the optional live consumer
    // collector arm. Mode A/B intentionally leave this unavailable: their
    // independent boundary proof is the successful synchronous backend vsync
    // return followed by the discarded local measurement epoch.
    struct ConsumerBoundaryOrdering
    {
        bool available = false;
        bool armScheduled = false;
        bool armedAfterSuccessfulVsync = false;
        uint32_t firstPostArmConsumerEventType = 0u;
        uint64_t mixedVsyncConsumerAckOrdinal = 0u;
        uint64_t armConsumerAckOrdinal = 0u;
        uint64_t bridgeVsyncReturnConsumerAckOrdinal = 0u;
        uint64_t firstPostArmConsumerAckOrdinal = 0u;
        uint64_t firstPostArmTransferAckOrdinal = 0u;
    };

#if defined(RRV_M2_NEUTRALITY_CONTROL)
    // Independent bounded control for M2 receipt-neutrality runs. It contains
    // no consumer-receipt data and is compiled identically into modes A/B/C.
    // All storage is static; the measured interval performs no file I/O.
    // The configured phase/script/step values are recorded as context only.
    // A run arms exclusively on the selector transition configured by
    // RRV_M2_NEUTRALITY_EDGE, never on a long-lived level condition.
    void observeSemanticState(uint32_t phase, uint32_t script, uint32_t step,
                              uint32_t selector, uint64_t observationGuestTick);
    void noteSubmittedPacket(uint32_t path, const uint8_t *bytes, uint32_t sizeBytes);
    void noteCompletedField(uint64_t guestFieldId, uint64_t gsFieldEpochId,
                            uint32_t parity, const uint64_t pathEventCounts[3],
                            bool bridgeIngressDigestAvailable,
                            uint64_t bridgeIngressDigest,
                            const ConsumerBoundaryOrdering *consumerBoundaryOrdering);
    bool intervalComplete();
    bool enabled();
    uint64_t lastGuestFieldId();
    bool dumpDeferred(std::string *error);
#else
    inline void observeSemanticState(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
    inline void noteSubmittedPacket(uint32_t, const uint8_t *, uint32_t) {}
    inline void noteCompletedField(uint64_t, uint64_t, uint32_t, const uint64_t[3],
                                   bool, uint64_t, const ConsumerBoundaryOrdering *) {}
    inline bool intervalComplete() { return false; }
    inline bool enabled() { return false; }
    inline uint64_t lastGuestFieldId() { return 0u; }
    inline bool dumpDeferred(std::string *) { return true; }
#endif
} // namespace rrv::m2neutral

#endif
