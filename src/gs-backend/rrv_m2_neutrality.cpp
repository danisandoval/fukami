// SPDX-License-Identifier: MIT
#include "rrv_m2_neutrality.h"
#include "rrv_m2_causal_trace.h"

#include "rrv_gs_backend.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rrv::m2neutral
{
namespace
{
    constexpr size_t kCapacity = 4096u;
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    constexpr char kGeometryDomain[] = "RRV-M2-NEUTRALITY-GEOMETRY-v1";
    constexpr uint32_t kSelector20005 = 0x00020005u;

    struct FieldRecord
    {
        uint64_t guestFieldId = 0u;
        uint64_t gsFieldEpochId = 0u;
        uint64_t pathEventCounts[3]{};
        uint64_t bridgeIngressDigest = 0u;
        uint64_t geometrySignature = 0u;
        uint64_t geometryVertexCount = 0u;
        uint32_t parity = 0u;
        uint32_t phase = 0u;
        uint32_t script = 0u;
        uint32_t step = 0u;
        bool bridgeIngressDigestAvailable = false;
    };

    enum class RunState : uint32_t
    {
        Disabled = 0u,
        WaitingForEdge = 1u,
        WaitingForBoundary = 2u,
        Collecting = 3u,
        Complete = 4u,
        Overflow = 5u,
    };

    struct State
    {
        std::array<FieldRecord, kCapacity> fields{};
        std::atomic<RunState> runState{RunState::Disabled};
        std::atomic<uint32_t> phase{0u};
        std::atomic<uint32_t> script{0u};
        std::atomic<uint32_t> step{0u};
        const char *outputPath = nullptr;
        const char *capturePath = nullptr;
        uint64_t targetFields = 0u;
        uint64_t fieldCount = 0u;
        uint32_t anchorPhase = 0u;
        uint32_t anchorScript = 0u;
        uint32_t anchorStep = 0u;
        uint32_t previousSelector = 0u;
        uint32_t edgePreviousSelector = 0u;
        uint32_t edgeCurrentSelector = 0u;
        uint32_t edgePhase = 0u;
        uint32_t edgeScript = 0u;
        uint32_t edgeStep = 0u;
        uint64_t edgeObservationGuestTick = 0u;
        uint64_t originGuestFieldId = 0u;
        uint64_t originGsFieldEpochId = 0u;
        uint32_t originParity = 0u;
        uint32_t originPhase = 0u;
        uint32_t originScript = 0u;
        uint32_t originStep = 0u;
        uint64_t openGeometryHash = kFnvOffset;
        uint64_t openGeometryVertexCount = 0u;
        bool hasPreviousSelector = false;
        bool sawSelectorEdge = false;
        bool sawOriginBoundary = false;
        bool canonicalReceiptRequested = false;
        ConsumerBoundaryOrdering consumerBoundaryOrdering{};
        bool configured = false;
    };

    State &state()
    {
        static State value;
        return value;
    }

    bool parseU32(const char *begin, const char **end, uint32_t &value)
    {
        if (!begin || begin[0] < '0' || begin[0] > '9')
            return false;
        char *parsedEnd = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(begin, &parsedEnd, 10);
        if (errno == ERANGE || parsed > std::numeric_limits<uint32_t>::max())
            return false;
        value = static_cast<uint32_t>(parsed);
        *end = parsedEnd;
        return true;
    }

    uint64_t loadLe64(const uint8_t *bytes)
    {
        uint64_t value = 0u;
        for (uint32_t index = 0u; index != 8u; ++index)
            value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
        return value;
    }

    void hashBytes(uint64_t &hash, const void *source, size_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(source);
        for (size_t index = 0u; index != size; ++index)
        {
            hash ^= bytes[index];
            hash *= kFnvPrime;
        }
    }

    void resetGeometry(State &s)
    {
        s.openGeometryHash = kFnvOffset;
        hashBytes(s.openGeometryHash, kGeometryDomain, sizeof(kGeometryDomain));
        s.openGeometryVertexCount = 0u;
    }

    void noteVertex(State &s, uint16_t x, uint16_t y, uint32_t z)
    {
        const uint8_t bytes[8] = {
            static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8u),
            static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8u),
            static_cast<uint8_t>(z), static_cast<uint8_t>(z >> 8u),
            static_cast<uint8_t>(z >> 16u), static_cast<uint8_t>(z >> 24u),
        };
        hashBytes(s.openGeometryHash, bytes, sizeof(bytes));
        ++s.openGeometryVertexCount;
    }

    void configureOnce()
    {
        State &s = state();
        if (s.configured)
            return;
        s.configured = true;
        s.outputPath = std::getenv("RRV_M2_NEUTRALITY_MANIFEST");
        s.capturePath = std::getenv("RRV_M2_NEUTRALITY_CAPTURE");
        const char *anchor = std::getenv("RRV_M2_NEUTRALITY_ANCHOR");
        const char *edge = std::getenv("RRV_M2_NEUTRALITY_EDGE");
        const char *fields = std::getenv("RRV_M2_NEUTRALITY_FIELDS");
        const char *receiptOutput = std::getenv("RRV_GS_CANONICAL_RECEIPT");
        const char *receiptArm = std::getenv("RRV_GS_CANONICAL_RECEIPT_ARM");
        const bool hasOutput = s.outputPath && s.outputPath[0] != '\0';
        const bool hasAnchor = anchor && anchor[0] != '\0';
        const bool hasEdge = edge && edge[0] != '\0';
        const bool hasFields = fields && fields[0] != '\0';
        const bool hasCapture = s.capturePath && s.capturePath[0] != '\0';
        if (!hasOutput && !hasAnchor && !hasEdge && !hasFields && !hasCapture)
            return;
        if (!hasOutput || !hasAnchor || !hasEdge || !hasFields)
            throw std::invalid_argument(
                "RRV M2 neutrality control requires manifest, anchor, edge, and field count");
        if (std::strcmp(edge, "selector-20005") != 0)
            throw std::invalid_argument(
                "RRV_M2_NEUTRALITY_EDGE must be selector-20005");
        const bool hasReceiptOutput = receiptOutput && receiptOutput[0] != '\0';
        const bool receiptPreArmed = receiptArm && receiptArm[0] != '\0' &&
            receiptArm[0] != '0';
        if (hasReceiptOutput && !receiptPreArmed)
        {
            throw std::invalid_argument(
                "RRV M2 selector-edge receipt requires RRV_GS_CANONICAL_RECEIPT_ARM=1; "
                "the collector must remain empty until the origin-boundary VSync");
        }

        const char *cursor = anchor;
        if (!parseU32(cursor, &cursor, s.anchorPhase) || *cursor++ != ':' ||
            !parseU32(cursor, &cursor, s.anchorScript) || *cursor++ != ':' ||
            !parseU32(cursor, &cursor, s.anchorStep) || *cursor != '\0')
        {
            throw std::invalid_argument("RRV_M2_NEUTRALITY_ANCHOR must be phase:script:step");
        }
        char *fieldEnd = nullptr;
        errno = 0;
        const unsigned long long parsedFields = std::strtoull(fields, &fieldEnd, 10);
        if (errno == ERANGE || !fieldEnd || fieldEnd == fields || *fieldEnd != '\0' ||
            parsedFields == 0u || parsedFields > kCapacity)
        {
            throw std::invalid_argument(
                "RRV_M2_NEUTRALITY_FIELDS is outside the bounded storage capacity");
        }
        s.targetFields = static_cast<uint64_t>(parsedFields);
        s.canonicalReceiptRequested = hasReceiptOutput;
        resetGeometry(s);
        s.runState.store(RunState::WaitingForEdge, std::memory_order_release);
    }
} // namespace

void noteSubmittedPacket(uint32_t, const uint8_t *bytes, uint32_t sizeBytes)
{
    State &s = state();
    if (s.runState.load(std::memory_order_acquire) == RunState::Disabled ||
        (sizeBytes != 0u && !bytes))
    {
        return;
    }
    size_t offset = 0u;
    while (offset + 16u <= sizeBytes)
    {
        const uint64_t lo = loadLe64(bytes + offset);
        const uint64_t hi = loadLe64(bytes + offset + 8u);
        offset += 16u;
        const size_t loops = static_cast<size_t>(lo & 0x7fffu);
        const uint32_t flag = static_cast<uint32_t>((lo >> 58u) & 3u);
        const size_t encodedRegisters = static_cast<size_t>((lo >> 60u) & 0x0fu);
        const size_t registerCount = encodedRegisters == 0u ? 16u : encodedRegisters;
        const size_t entries = loops * registerCount;
        if (flag == 0u)
        {
            if (entries > (sizeBytes - offset) / 16u)
                return;
            for (size_t index = 0u; index != entries; ++index)
            {
                const uint32_t descriptor = static_cast<uint32_t>(
                    (hi >> (4u * (index % registerCount))) & 0x0fu);
                if (descriptor != 0x04u && descriptor != 0x05u)
                    continue;
                const uint8_t *entry = bytes + offset + index * 16u;
                const uint64_t low = loadLe64(entry);
                const uint64_t high = loadLe64(entry + 8u);
                noteVertex(s, static_cast<uint16_t>(low),
                           static_cast<uint16_t>(low >> 32u),
                           static_cast<uint32_t>(high));
            }
            offset += entries * 16u;
        }
        else if (flag == 1u)
        {
            const size_t qwords = (entries + 1u) / 2u;
            if (qwords > (sizeBytes - offset) / 16u)
                return;
            for (size_t index = 0u; index != entries; ++index)
            {
                const uint32_t descriptor = static_cast<uint32_t>(
                    (hi >> (4u * (index % registerCount))) & 0x0fu);
                if (descriptor != 0x04u && descriptor != 0x05u)
                    continue;
                const uint64_t value = loadLe64(
                    bytes + offset + (index / 2u) * 16u + (index % 2u) * 8u);
                noteVertex(s, static_cast<uint16_t>(value),
                           static_cast<uint16_t>(value >> 16u),
                           static_cast<uint32_t>(value >> 32u));
            }
            offset += qwords * 16u;
        }
        else
        {
            if (loops > (sizeBytes - offset) / 16u)
                return;
            offset += loops * 16u;
        }
    }
}

void observeSemanticState(uint32_t phase, uint32_t script, uint32_t step,
                          uint32_t selector, uint64_t observationGuestTick)
{
    configureOnce();
    State &s = state();
    s.phase.store(phase, std::memory_order_relaxed);
    s.script.store(script, std::memory_order_relaxed);
    s.step.store(step, std::memory_order_relaxed);
    const uint32_t previousSelector = s.previousSelector;
    const bool crossedSelectorEdge = s.hasPreviousSelector &&
        previousSelector != kSelector20005 && selector == kSelector20005;
    s.previousSelector = selector;
    s.hasPreviousSelector = true;

    RunState expected = RunState::WaitingForEdge;
    if (crossedSelectorEdge &&
        s.runState.load(std::memory_order_acquire) == RunState::WaitingForEdge)
    {
        rrv::m2causal::beginSelectorEdge(previousSelector, selector, observationGuestTick);
        // This is the one-shot guest-semantic edge. Geometry is reset at each
        // earlier successful VSync while waiting, so the following closure
        // retains all transfers from its complete open field, including any
        // that preceded this edge.
        s.edgePreviousSelector = previousSelector;
        s.edgeCurrentSelector = selector;
        s.edgeObservationGuestTick = observationGuestTick;
        s.edgePhase = phase;
        s.edgeScript = script;
        s.edgeStep = step;
        s.sawSelectorEdge = true;
        if (!s.runState.compare_exchange_strong(expected, RunState::WaitingForBoundary,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
        {
            // The state changed before the edge could arm. This cannot be a
            // successful run, and must not publish a stale edge observation.
            s.sawSelectorEdge = false;
        }
        else if (s.canonicalReceiptRequested)
        {
            // This schedules, but does not immediately arm, the preallocated
            // collector. Backend will pass that request into the next bridge
            // vsync, where the bridge arms only after the mixed field closes.
            rrv::gsbackend::requestCanonicalConsumerReceipt();
        }
    }
}

void noteCompletedField(uint64_t guestFieldId, uint64_t gsFieldEpochId,
                        uint32_t parity, const uint64_t pathEventCounts[3],
                        bool bridgeIngressDigestAvailable,
                        uint64_t bridgeIngressDigest,
                        const ConsumerBoundaryOrdering *consumerBoundaryOrdering)
{
    State &s = state();
    const RunState current = s.runState.load(std::memory_order_acquire);
    // Freeze the deferred diagnostic snapshot at the exact 360th closure.
    // A later host-side VSync must not mutate sideband evidence while export
    // reads the completed interval.
    if (current == RunState::Complete || current == RunState::Overflow ||
        current == RunState::Disabled)
    {
        return;
    }
    if (consumerBoundaryOrdering && consumerBoundaryOrdering->available)
        s.consumerBoundaryOrdering = *consumerBoundaryOrdering;
    if (current == RunState::WaitingForEdge)
    {
        // Do not carry geometry from an earlier closed field into the eventual
        // selector-edge origin. The next field remains open for observation.
        resetGeometry(s);
        return;
    }
    if (current == RunState::WaitingForBoundary)
    {
        s.originGuestFieldId = guestFieldId;
        s.originGsFieldEpochId = gsFieldEpochId;
        s.originParity = parity;
        s.originPhase = s.phase.load(std::memory_order_relaxed);
        s.originScript = s.script.load(std::memory_order_relaxed);
        s.originStep = s.step.load(std::memory_order_relaxed);
        s.sawOriginBoundary = true;
        if (s.canonicalReceiptRequested &&
            (!consumerBoundaryOrdering || !consumerBoundaryOrdering->available ||
             !consumerBoundaryOrdering->armScheduled ||
             !consumerBoundaryOrdering->armedAfterSuccessfulVsync ||
             consumerBoundaryOrdering->mixedVsyncConsumerAckOrdinal == 0u ||
             consumerBoundaryOrdering->mixedVsyncConsumerAckOrdinal !=
                 consumerBoundaryOrdering->armConsumerAckOrdinal ||
             consumerBoundaryOrdering->armConsumerAckOrdinal !=
                 consumerBoundaryOrdering->bridgeVsyncReturnConsumerAckOrdinal))
        {
            // A canonical live result must fail closed if it cannot prove the
            // exact after-close/before-return arm boundary. Mode A/B have no
            // receipt and deliberately take the independent path below.
            s.runState.store(RunState::Overflow, std::memory_order_release);
            return;
        }
        // The VSync above closes the epoch that was already open when the
        // selector changed. It is a diagnostic origin boundary only. Its
        // geometry and all preceding ingress are discarded; the following
        // VSync, not this one, closes relative field 0.
        resetGeometry(s);
        s.runState.store(RunState::Collecting, std::memory_order_release);
        rrv::m2causal::originBoundary(guestFieldId, gsFieldEpochId, parity);
        return;
    }
    if (current != RunState::Collecting)
        return;
    rrv::m2causal::completeField(guestFieldId, gsFieldEpochId, parity);
    if (s.fieldCount >= s.targetFields || s.fieldCount >= s.fields.size())
    {
        s.runState.store(RunState::Overflow, std::memory_order_release);
        return;
    }
    FieldRecord &record = s.fields[s.fieldCount++];
    record.guestFieldId = guestFieldId;
    record.gsFieldEpochId = gsFieldEpochId;
    record.parity = parity;
    record.phase = s.phase.load(std::memory_order_relaxed);
    record.script = s.script.load(std::memory_order_relaxed);
    record.step = s.step.load(std::memory_order_relaxed);
    for (size_t index = 0u; index != 3u; ++index)
        record.pathEventCounts[index] = pathEventCounts[index];
    record.bridgeIngressDigestAvailable = bridgeIngressDigestAvailable;
    record.bridgeIngressDigest = bridgeIngressDigest;
    record.geometryVertexCount = s.openGeometryVertexCount;
    uint8_t countBytes[8]{};
    for (uint32_t index = 0u; index != 8u; ++index)
        countBytes[index] = static_cast<uint8_t>(record.geometryVertexCount >> (index * 8u));
    hashBytes(s.openGeometryHash, countBytes, sizeof(countBytes));
    record.geometrySignature = s.openGeometryHash;
    resetGeometry(s);
    if (s.fieldCount == s.targetFields)
        s.runState.store(RunState::Complete, std::memory_order_release);
}

bool intervalComplete()
{
    const RunState value = state().runState.load(std::memory_order_acquire);
    return value == RunState::Complete || value == RunState::Overflow;
}

bool enabled()
{
    configureOnce();
    return state().runState.load(std::memory_order_acquire) != RunState::Disabled;
}

uint64_t lastGuestFieldId()
{
    const State &s = state();
    return s.fieldCount == 0u ? 0u : s.fields[s.fieldCount - 1u].guestFieldId;
}

bool dumpDeferred(std::string *error)
{
    State &s = state();
    if (!enabled())
        return true;
    const RunState finalState = s.runState.load(std::memory_order_acquire);
    // A bounded run exports only after its last whole field closed. This keeps
    // the interval free of both manifest and deferred-capture I/O.
    if (finalState != RunState::Complete)
    {
        if (error) *error = "M2 neutrality interval did not close successfully";
        return false;
    }
    rrv::gsbackend::CaptureResult capture{};
    const bool captureRequested = s.capturePath && s.capturePath[0] != '\0';
    bool captureSucceeded = !captureRequested;
    if (captureRequested)
    {
        std::string captureError;
        captureSucceeded = rrv::gsbackend::captureActiveCompletedField(
            lastGuestFieldId(), capture, &captureError);
        if (captureSucceeded)
        {
            FILE *captureFile = std::fopen(s.capturePath, "wb");
            if (!captureFile)
            {
                captureSucceeded = false;
            }
            else
            {
                bool captureOk = std::fprintf(captureFile, "P6\n%u %u\n255\n",
                                              capture.width, capture.height) > 0;
                for (uint32_t y = 0u; captureOk && y != capture.height; ++y)
                {
                    const uint8_t *row = capture.rgba.data() +
                        static_cast<size_t>(y) * capture.width * 4u;
                    for (uint32_t x = 0u; captureOk && x != capture.width; ++x)
                        captureOk = std::fwrite(row + static_cast<size_t>(x) * 4u,
                                                1u, 3u, captureFile) == 3u;
                }
                if (std::fclose(captureFile) != 0)
                    captureOk = false;
                captureSucceeded = captureOk;
            }
        }
    }
    FILE *file = std::fopen(s.outputPath, "wb");
    if (!file)
    {
        if (error) *error = "cannot open M2 neutrality manifest";
        return false;
    }
    bool ok = std::fprintf(file,
        "{\n  \"schema\": 3,\n"
        "  \"expected_semantic_metadata\": {\"phase\": %u, \"script\": %u, \"step\": %u},\n"
        "  \"selector_edge\": {\"kind\": \"selector-20005\", \"observed\": %s, "
        "\"previous_selector\": \"%08x\", \"current_selector\": \"%08x\", "
        "\"observation_guest_tick_id\": %llu, \"phase\": %u, \"script\": %u, \"step\": %u},\n"
        "  \"origin_boundary_vsync\": {\"observed\": %s, \"mixed_epoch_discarded\": true, \"guest_field_id\": %llu, "
        "\"guest_tick_id\": %llu, \"gs_field_epoch_id\": %llu, \"parity\": %u, "
        "\"phase\": %u, \"script\": %u, \"step\": %u, "
        "\"first_consumer_sequence_ordinal\": null},\n"
        "  \"consumer_boundary_ordering\": {\"available\": %s, \"arm_scheduled\": %s, "
        "\"armed_after_successful_vsync\": %s, \"mixed_vsync_consumer_ack_ordinal\": %llu, "
        "\"arm_consumer_ack_ordinal\": %llu, \"bridge_vsync_return_consumer_ack_ordinal\": %llu, "
        "\"first_post_arm_consumer_ack_ordinal\": %llu, \"first_post_arm_consumer_event_type\": %u, "
        "\"first_post_arm_transfer_ack_ordinal\": %llu},\n"
        "  \"target_fields\": %llu,\n  \"field_count\": %llu,\n"
        "  \"complete\": %s,\n  \"overflowed\": %s,\n"
        "  \"deferred_capture\": {\"requested\": %s, \"succeeded\": %s, \"capture_id\": %llu, "
        "\"guest_field_id\": %llu, \"gs_field_epoch_id\": %llu, "
        "\"width\": %u, \"height\": %u},\n  \"fields\": [\n",
        s.anchorPhase, s.anchorScript, s.anchorStep,
        s.sawSelectorEdge ? "true" : "false",
        s.edgePreviousSelector, s.edgeCurrentSelector,
        static_cast<unsigned long long>(s.edgeObservationGuestTick),
        s.edgePhase, s.edgeScript, s.edgeStep,
        s.sawOriginBoundary ? "true" : "false",
        static_cast<unsigned long long>(s.originGuestFieldId),
        static_cast<unsigned long long>(s.originGuestFieldId),
        static_cast<unsigned long long>(s.originGsFieldEpochId), s.originParity,
        s.originPhase, s.originScript, s.originStep,
        s.consumerBoundaryOrdering.available ? "true" : "false",
        s.consumerBoundaryOrdering.armScheduled ? "true" : "false",
        s.consumerBoundaryOrdering.armedAfterSuccessfulVsync ? "true" : "false",
        static_cast<unsigned long long>(s.consumerBoundaryOrdering.mixedVsyncConsumerAckOrdinal),
        static_cast<unsigned long long>(s.consumerBoundaryOrdering.armConsumerAckOrdinal),
        static_cast<unsigned long long>(s.consumerBoundaryOrdering.bridgeVsyncReturnConsumerAckOrdinal),
        static_cast<unsigned long long>(s.consumerBoundaryOrdering.firstPostArmConsumerAckOrdinal),
        s.consumerBoundaryOrdering.firstPostArmConsumerEventType,
        static_cast<unsigned long long>(s.consumerBoundaryOrdering.firstPostArmTransferAckOrdinal),
        static_cast<unsigned long long>(s.targetFields),
        static_cast<unsigned long long>(s.fieldCount),
        finalState == RunState::Complete ? "true" : "false",
        finalState == RunState::Overflow ? "true" : "false",
        captureRequested ? "true" : "false",
        captureSucceeded ? "true" : "false",
        static_cast<unsigned long long>(capture.captureId),
        static_cast<unsigned long long>(capture.tag.fieldIndex),
        static_cast<unsigned long long>(capture.sourceGsFieldEpochId),
        capture.width, capture.height) > 0;
    for (uint64_t index = 0u; ok && index != s.fieldCount; ++index)
    {
        const FieldRecord &record = s.fields[index];
        ok = std::fprintf(file,
            "    {\"ordinal\": %llu, \"relative_guest_field_id\": %llu, "
            "\"guest_field_id\": %llu, "
            "\"guest_tick_id\": %llu, \"gs_field_epoch_id\": %llu, \"parity\": %u, "
            "\"phase\": %u, \"script\": %u, \"step\": %u, "
            "\"path_event_counts\": [%llu, %llu, %llu], "
            "\"bridge_ingress_digest_available\": %s, "
            "\"bridge_ingress_digest\": \"%016llx\", "
            "\"geometry_vertex_count\": %llu, "
            "\"geometry_signature_fnv1a64\": \"%016llx\"}%s\n",
            static_cast<unsigned long long>(index),
            static_cast<unsigned long long>(index),
            static_cast<unsigned long long>(record.guestFieldId),
            static_cast<unsigned long long>(record.guestFieldId),
            static_cast<unsigned long long>(record.gsFieldEpochId), record.parity,
            record.phase, record.script, record.step,
            static_cast<unsigned long long>(record.pathEventCounts[0]),
            static_cast<unsigned long long>(record.pathEventCounts[1]),
            static_cast<unsigned long long>(record.pathEventCounts[2]),
            record.bridgeIngressDigestAvailable ? "true" : "false",
            static_cast<unsigned long long>(record.bridgeIngressDigest),
            static_cast<unsigned long long>(record.geometryVertexCount),
            static_cast<unsigned long long>(record.geometrySignature),
            index + 1u == s.fieldCount ? "" : ",") > 0;
    }
    ok = ok && std::fprintf(file, "  ]\n}\n") > 0;
    if (std::fclose(file) != 0)
        ok = false;
    if (!ok && error)
        *error = "M2 neutrality manifest export is incomplete";
    return ok;
}
} // namespace rrv::m2neutral
