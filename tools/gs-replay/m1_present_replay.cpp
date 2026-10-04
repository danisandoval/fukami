// Test-only M1 presentation control. Replays one immutable field-mode GSR
// through the pinned PCSX2 bridge so legacy CPU snapshots and SDL-backed
// direct Metal presentation consume exactly the same packets, registers and
// recorded guest ticks. This is not linked into rrv-product.
#include "rrv_gs_backend.h"
#include "rrv_gs_record_format.h"
#include "rrv_sdl_presentation.h"
#include "rrv_m2_causal_trace.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <time.h>
#include <vector>

namespace
{
using rrv::gsrecord::EventTag;
using rrv::gsrecord::GsrEventHeader;
using rrv::gsrecord::GsrFileHeader;
using rrv::gsrecord::GsrPresentPayload;

struct Options
{
    std::filesystem::path recording;
    std::filesystem::path outputDirectory;
    rrv::gsbackend::PresentationMode mode = rrv::gsbackend::PresentationMode::LegacyCpuSnapshot;
    std::set<uint64_t> captureIndices;
    uint64_t expectedPresents = 0u;
    bool modeSpecified = false;
    // Opt-in M2 control: a headless, memory-preloaded consumer replay. This
    // deliberately differs from the M1 presentation A/B path below: it never
    // creates an SDL surface or calls copyFrame() per VSync.
    bool m2Neutrality = false;
};

struct Recording
{
    GsrFileHeader header{};
    std::vector<uint8_t> bytes;
    size_t eventOffset = 0u;
};

void usage(const char *program)
{
    std::fprintf(stderr,
                 "usage: %s <stream.gsr> --mode legacy|direct --output-dir <dir> "
                 "--expect-presents N --captures comma-separated-present-indices\n"
                 "       %s <stream.gsr> --m2-neutrality --mode legacy --output-dir <dir> "
                 "--expect-presents N --captures comma-separated-present-indices\n",
                 program,
                 program);
}

bool parseUnsigned(const std::string &text, uint64_t &value)
{
    if (text.empty()) return false;
    uint64_t parsed = 0u;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

bool parseCaptures(const std::string &text, std::set<uint64_t> &captures)
{
    size_t begin = 0u;
    while (begin < text.size())
    {
        const size_t comma = text.find(',', begin);
        const std::string token = text.substr(begin, comma == std::string::npos ? comma : comma - begin);
        uint64_t index = 0u;
        if (!parseUnsigned(token, index) || !captures.insert(index).second) return false;
        if (comma == std::string::npos) return true;
        begin = comma + 1u;
    }
    return false;
}

bool parseOptions(int argc, char **argv, Options &options)
{
    if (argc < 6) return false;
    options.recording = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--mode" && i + 1 < argc)
        {
            if (options.modeSpecified) return false;
            const std::string mode = argv[++i];
            if (mode == "legacy") options.mode = rrv::gsbackend::PresentationMode::LegacyCpuSnapshot;
            else if (mode == "direct") options.mode = rrv::gsbackend::PresentationMode::DirectGpu;
            else return false;
            options.modeSpecified = true;
        }
        else if (argument == "--output-dir" && i + 1 < argc)
        {
            options.outputDirectory = argv[++i];
        }
        else if (argument == "--captures" && i + 1 < argc)
        {
            if (!parseCaptures(argv[++i], options.captureIndices)) return false;
        }
        else if (argument == "--expect-presents" && i + 1 < argc)
        {
            if (options.expectedPresents != 0u ||
                !parseUnsigned(argv[++i], options.expectedPresents) ||
                options.expectedPresents == 0u)
                return false;
        }
        else if (argument == "--m2-neutrality")
        {
            if (options.m2Neutrality) return false;
            options.m2Neutrality = true;
        }
        else return false;
    }
    return !options.recording.empty() && !options.outputDirectory.empty() &&
           options.modeSpecified && options.expectedPresents != 0u &&
           !options.captureIndices.empty();
}

bool loadRecording(const std::filesystem::path &path, Recording &recording)
{
    FILE *file = std::fopen(path.string().c_str(), "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < static_cast<long>(sizeof(GsrFileHeader)))
    {
        std::fclose(file);
        return false;
    }
    recording.bytes.resize(static_cast<size_t>(size));
    const bool read = std::fread(recording.bytes.data(), 1u, recording.bytes.size(), file) ==
                      recording.bytes.size();
    std::fclose(file);
    if (!read) return false;
    std::memcpy(&recording.header, recording.bytes.data(), sizeof(recording.header));
    if (recording.header.magic != rrv::gsrecord::kGsrMagic ||
        recording.header.version != rrv::gsrecord::kGsrVersion ||
        recording.header.privRegsSize != 19u * sizeof(uint64_t) ||
        recording.header.renderMode != 0u)
        return false;
    recording.eventOffset = sizeof(GsrFileHeader) + recording.header.vramSize;
    return recording.eventOffset <= recording.bytes.size();
}

uint64_t fnv1a(const uint8_t *bytes, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool writePpm(const std::filesystem::path &path, const uint8_t *rgba,
              uint32_t width, uint32_t height)
{
    FILE *file = std::fopen(path.string().c_str(), "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "P6\n%u %u\n255\n", width, height) > 0;
    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t index = 0u; index < pixels; ++index)
        ok = std::fwrite(rgba + index * 4u, 1u, 3u, file) == 3u && ok;
    return std::fclose(file) == 0 && ok;
}

// The M2 control deliberately uses only immutable bytes from a GSR v1 file.
// It is not a reinterpretation of the GSR ingress stream as a live canonical
// receipt: its job is to supply a repeatable consumer input to the accepted
// bridge seam. GSR v1 represents local-memory state as the header snapshot;
// the control submits that snapshot through restoreLocalMemory() immediately
// before field zero, so the consumer receipt records the restore in epoch zero.
struct FixtureEvent
{
    EventTag tag = EventTag::GifPacket;
    uint8_t pathId = 0u;
    uint32_t payloadSize = 0u;
    size_t payloadOffset = 0u;
    GsrPresentPayload present{};
};

struct FixtureField
{
    GsrPresentPayload present{};
    uint64_t pathPacketCounts[3]{};
    uint64_t pathByteCounts[3]{};
    uint64_t ingressGeometrySignature = 0u;
    uint64_t ingressGeometryVertexCount = 0u;
};

struct BufferedCapture
{
    uint64_t fieldOrdinal = 0u;
    rrv::gsbackend::CaptureResult result{};
    uint64_t rgbaFnv1a = 0u;
};

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr char kM2GeometryDomain[] = "RRV-M2-NEUTRALITY-GEOMETRY-v1";

uint64_t m2Fnv1a(const uint8_t *bytes, size_t size, uint64_t hash = kFnvOffsetBasis)
{
    for (size_t index = 0u; index != size; ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t loadLe64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index != 8u; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
    return value;
}

void noteFixtureVertex(FixtureField &field, uint16_t x, uint16_t y, uint32_t z)
{
    const uint8_t bytes[8] = {
        static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8u),
        static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8u),
        static_cast<uint8_t>(z), static_cast<uint8_t>(z >> 8u),
        static_cast<uint8_t>(z >> 16u), static_cast<uint8_t>(z >> 24u),
    };
    field.ingressGeometrySignature = m2Fnv1a(bytes, sizeof(bytes),
                                              field.ingressGeometrySignature);
    ++field.ingressGeometryVertexCount;
}

// This is the existing bounded GIF vertex decoding used by the M2 neutrality
// control, applied while validating the immutable fixture rather than while
// the consumer interval runs. It is explicitly an ingress signature, not a
// substitute for the consumer-side receipt.
bool noteFixtureGeometry(FixtureField &field, const uint8_t *bytes, uint32_t sizeBytes)
{
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
        if (loops > SIZE_MAX / registerCount) return false;
        const size_t entries = loops * registerCount;
        if (flag == 0u)
        {
            if (entries > (sizeBytes - offset) / 16u) return false;
            for (size_t index = 0u; index != entries; ++index)
            {
                const uint32_t descriptor = static_cast<uint32_t>(
                    (hi >> (4u * (index % registerCount))) & 0x0fu);
                if (descriptor != 0x04u && descriptor != 0x05u) continue;
                const uint8_t *entry = bytes + offset + index * 16u;
                noteFixtureVertex(field, static_cast<uint16_t>(loadLe64(entry)),
                                  static_cast<uint16_t>(loadLe64(entry) >> 32u),
                                  static_cast<uint32_t>(loadLe64(entry + 8u)));
            }
            offset += entries * 16u;
        }
        else if (flag == 1u)
        {
            const size_t qwords = (entries + 1u) / 2u;
            if (qwords > (sizeBytes - offset) / 16u) return false;
            for (size_t index = 0u; index != entries; ++index)
            {
                const uint32_t descriptor = static_cast<uint32_t>(
                    (hi >> (4u * (index % registerCount))) & 0x0fu);
                if (descriptor != 0x04u && descriptor != 0x05u) continue;
                const uint64_t value = loadLe64(
                    bytes + offset + (index / 2u) * 16u + (index % 2u) * 8u);
                noteFixtureVertex(field, static_cast<uint16_t>(value),
                                  static_cast<uint16_t>(value >> 16u),
                                  static_cast<uint32_t>(value >> 32u));
            }
            offset += qwords * 16u;
        }
        else
        {
            if (loops > (sizeBytes - offset) / 16u) return false;
            offset += loops * 16u;
        }
    }
    return offset == sizeBytes;
}

void beginFixtureField(FixtureField &field)
{
    field = {};
    field.ingressGeometrySignature = m2Fnv1a(
        reinterpret_cast<const uint8_t *>(kM2GeometryDomain), sizeof(kM2GeometryDomain));
}

void finishFixtureGeometry(FixtureField &field)
{
    // Match rrv::m2neutral::noteCompletedField(): vertex content alone is not
    // self-delimiting across fields, so the little-endian vertex count closes
    // the existing independent geometry-signature serialization.
    uint8_t countBytes[8]{};
    for (uint32_t index = 0u; index != 8u; ++index)
        countBytes[index] = static_cast<uint8_t>(field.ingressGeometryVertexCount >> (index * 8u));
    field.ingressGeometrySignature = m2Fnv1a(countBytes, sizeof(countBytes),
                                              field.ingressGeometrySignature);
}

bool preflightM2Fixture(const Recording &recording, uint64_t expectedFields,
                        std::vector<FixtureEvent> &events,
                        std::vector<FixtureField> &fields, std::string *error)
{
    if (recording.header.vramSize != 4u * 1024u * 1024u)
    {
        if (error) *error = "M2 replay requires the complete 4 MiB GSR local-memory snapshot";
        return false;
    }
    const size_t maximumEvents = (recording.bytes.size() - recording.eventOffset) /
                                 sizeof(GsrEventHeader);
    if (recording.header.eventCount > maximumEvents || expectedFields > maximumEvents)
    {
        if (error) *error = "M2 replay fixture count exceeds its byte-bounded event capacity";
        return false;
    }
    events.clear();
    fields.clear();
    events.reserve(static_cast<size_t>(recording.header.eventCount));
    fields.reserve(static_cast<size_t>(expectedFields));
    FixtureField openField{};
    beginFixtureField(openField);
    bool openFieldHasPacket = false;
    uint64_t previousTick = 0u;
    bool hasPreviousTick = false;
    size_t cursor = recording.eventOffset;
    while (cursor + sizeof(GsrEventHeader) <= recording.bytes.size())
    {
        GsrEventHeader event{};
        std::memcpy(&event, recording.bytes.data() + cursor, sizeof(event));
        cursor += sizeof(event);
        if (event.payloadSize > recording.bytes.size() - cursor)
        {
            if (error) *error = "M2 replay fixture has a truncated event payload";
            return false;
        }
        FixtureEvent decoded{};
        decoded.tag = static_cast<EventTag>(event.tag);
        decoded.pathId = event.pathId;
        decoded.payloadSize = event.payloadSize;
        decoded.payloadOffset = cursor;
        const uint8_t *payload = recording.bytes.data() + cursor;
        if (decoded.tag == EventTag::GifPacket)
        {
            if (decoded.pathId < 1u || decoded.pathId > 3u ||
                (decoded.payloadSize & 0x0fu) != 0u)
            {
                if (error) *error = "M2 replay fixture has an invalid GIF path or non-QWC payload";
                return false;
            }
            ++openField.pathPacketCounts[decoded.pathId - 1u];
            openField.pathByteCounts[decoded.pathId - 1u] += decoded.payloadSize;
            if (!noteFixtureGeometry(openField, payload, decoded.payloadSize))
            {
                if (error) *error = "M2 replay fixture contains a truncated GIFtag payload";
                return false;
            }
            openFieldHasPacket = true;
        }
        else if (decoded.tag == EventTag::Present)
        {
            if (decoded.pathId != 0u || decoded.payloadSize != sizeof(GsrPresentPayload))
            {
                if (error) *error = "M2 replay fixture has an invalid VSync payload";
                return false;
            }
            std::memcpy(&decoded.present, payload, sizeof(decoded.present));
            if (hasPreviousTick && decoded.present.vsyncTick != previousTick + 1u)
            {
                if (error) *error = "M2 replay fixture VSync ticks are not contiguous";
                return false;
            }
            previousTick = decoded.present.vsyncTick;
            hasPreviousTick = true;
            openField.present = decoded.present;
            finishFixtureGeometry(openField);
            fields.push_back(openField); // zero-transfer fields are valid.
            beginFixtureField(openField);
            openFieldHasPacket = false;
        }
        else
        {
            if (error) *error = "M2 replay fixture contains an unsupported GSR event tag";
            return false;
        }
        events.push_back(decoded);
        cursor += event.payloadSize;
    }
    if (cursor != recording.bytes.size() || events.size() != recording.header.eventCount)
    {
        if (error) *error = "M2 replay fixture event count or terminal boundary disagrees with its header";
        return false;
    }
    if (openFieldHasPacket || fields.size() != expectedFields)
    {
        if (error) *error = "M2 replay fixture has unclosed packets or the wrong fixed field count";
        return false;
    }
    return true;
}

bool threadCpuNanoseconds(uint64_t *result)
{
    timespec sample{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &sample) != 0 || sample.tv_sec < 0 || sample.tv_nsec < 0)
        return false;
    *result = static_cast<uint64_t>(sample.tv_sec) * 1000000000ull +
              static_cast<uint64_t>(sample.tv_nsec);
    return true;
}

bool writeM2Manifest(const std::filesystem::path &path, const Recording &recording,
                     const std::vector<FixtureField> &fixture,
                     const std::vector<FixtureField> &executedGeometry,
                     const rrv::gsbackend::CompletedFieldEpochHistory &history,
                     const std::vector<BufferedCapture> &captures,
                     const rrv::gsbackend::PresentationStats &stats,
                     uint64_t intervalCpuNanoseconds, uint64_t intervalWallNanoseconds,
                     uint64_t shutdownCpuNanoseconds, uint64_t shutdownWallNanoseconds)
{
    FILE *file = std::fopen(path.string().c_str(), "wb");
    if (!file) return false;
    const uint64_t inputHash = m2Fnv1a(recording.bytes.data(), recording.bytes.size());
    bool ok = std::fprintf(file,
        "{\n  \"schema\": 1,\n  \"fixture\": {\"gsr_schema\": %u, \"input_fnv1a64\": \"%016llx\", "
        "\"event_count\": %llu, \"field_count\": %zu, \"initial_restore_bytes\": %u},\n"
        "  \"interval\": {\"thread_cpu_ns\": %llu, \"wall_ns\": %llu},\n"
        "  \"shutdown_and_receipt_export\": {\"thread_cpu_ns\": %llu, \"wall_ns\": %llu},\n"
        "  \"presentation_stats\": {\"presented_frames\": %llu, \"direct_gpu_presents\": %llu, "
        "\"requested_captures\": %llu, \"completed_captures\": %llu, \"synchronous_cpu_readbacks\": %llu, "
        "\"unexpected_readbacks\": %llu, \"cpu_waits\": %llu},\n  \"fields\": [\n",
        recording.header.version, static_cast<unsigned long long>(inputHash),
        static_cast<unsigned long long>(recording.header.eventCount), fixture.size(),
        recording.header.vramSize, static_cast<unsigned long long>(intervalCpuNanoseconds),
        static_cast<unsigned long long>(intervalWallNanoseconds),
        static_cast<unsigned long long>(shutdownCpuNanoseconds),
        static_cast<unsigned long long>(shutdownWallNanoseconds),
        static_cast<unsigned long long>(stats.presentedFrames),
        static_cast<unsigned long long>(stats.directGpuPresents),
        static_cast<unsigned long long>(stats.requestedCaptures),
        static_cast<unsigned long long>(stats.completedCaptures),
        static_cast<unsigned long long>(stats.synchronousCpuReadbacks),
        static_cast<unsigned long long>(stats.unexpectedReadbacks),
        static_cast<unsigned long long>(stats.cpuWaits)) > 0;
    for (size_t index = 0u; ok && index != fixture.size(); ++index)
    {
        const FixtureField &expected = fixture[index];
        const FixtureField &executed = executedGeometry[index];
        const rrv::gsbackend::CompletedFieldEpoch &actual = history.epochs[index];
        ok = std::fprintf(file,
            "    {\"ordinal\": %zu, \"guest_field_id\": %llu, \"gs_field_epoch_id\": %llu, \"parity\": %u, "
            "\"vsync_tick\": %llu, \"path_packet_counts\": [%llu, %llu, %llu], "
            "\"path_byte_counts\": [%llu, %llu, %llu], "
            "\"fixture_ingress_geometry_vertex_count\": %llu, "
            "\"fixture_ingress_geometry_signature_fnv1a64\": \"%016llx\", "
            "\"executed_ingress_geometry_vertex_count\": %llu, "
            "\"executed_ingress_geometry_signature_fnv1a64\": \"%016llx\", "
            "\"existing_epoch_workload_hash_available\": %s, \"existing_epoch_workload_hash_fnv1a64\": \"%016llx\"}%s\n",
            index, static_cast<unsigned long long>(actual.guestFieldId),
            static_cast<unsigned long long>(actual.gsFieldEpochId), actual.fieldParity,
            static_cast<unsigned long long>(expected.present.vsyncTick),
            static_cast<unsigned long long>(actual.pathEventCounts[0]),
            static_cast<unsigned long long>(actual.pathEventCounts[1]),
            static_cast<unsigned long long>(actual.pathEventCounts[2]),
            static_cast<unsigned long long>(actual.pathByteCounts[0]),
            static_cast<unsigned long long>(actual.pathByteCounts[1]),
            static_cast<unsigned long long>(actual.pathByteCounts[2]),
            static_cast<unsigned long long>(expected.ingressGeometryVertexCount),
            static_cast<unsigned long long>(expected.ingressGeometrySignature),
            static_cast<unsigned long long>(executed.ingressGeometryVertexCount),
            static_cast<unsigned long long>(executed.ingressGeometrySignature),
            actual.canonicalWorkloadHashAvailable ? "true" : "false",
            static_cast<unsigned long long>(actual.canonicalWorkloadHash),
            index + 1u == fixture.size() ? "" : ",") > 0;
    }
    ok = ok && std::fprintf(file, "  ],\n  \"captures\": [\n") > 0;
    for (size_t index = 0u; ok && index != captures.size(); ++index)
    {
        const BufferedCapture &capture = captures[index];
        ok = std::fprintf(file,
            "    {\"ordinal\": %llu, \"capture_id\": %llu, \"guest_field_id\": %llu, "
            "\"gs_field_epoch_id\": %llu, \"width\": %u, \"height\": %u, \"rgba_fnv1a64\": \"%016llx\"}%s\n",
            static_cast<unsigned long long>(capture.fieldOrdinal),
            static_cast<unsigned long long>(capture.result.captureId),
            static_cast<unsigned long long>(capture.result.tag.fieldIndex),
            static_cast<unsigned long long>(capture.result.sourceGsFieldEpochId),
            capture.result.width, capture.result.height,
            static_cast<unsigned long long>(capture.rgbaFnv1a),
            index + 1u == captures.size() ? "" : ",") > 0;
    }
    ok = ok && std::fprintf(file, "  ]\n}\n") > 0;
    return std::fclose(file) == 0 && ok;
}

bool runM2NeutralityReplay(const Options &options, const Recording &recording,
                           std::string *error)
{
    if (!rrv::m2causal::initializeFromEnvironment())
    {
        if (error) *error = "M2 causal trace initialization failed";
        return false;
    }
    if (options.mode != rrv::gsbackend::PresentationMode::LegacyCpuSnapshot)
    {
        if (error) *error = "--m2-neutrality requires --mode legacy: it deliberately has no drawable";
        return false;
    }
    for (uint64_t capture : options.captureIndices)
    {
        if (capture >= options.expectedPresents)
        {
            if (error) *error = "M2 replay capture index is outside the fixed field interval";
            return false;
        }
    }

    std::vector<FixtureEvent> events;
    std::vector<FixtureField> fixture;
    if (!preflightM2Fixture(recording, options.expectedPresents, events, fixture, error))
        return false;

    rrv::gsbackend::InitializeOptions initialize{};
    initialize.snapshotWidth = 640u;
    initialize.snapshotHeight = 448u;
    initialize.rendererKind = rrv::gsbackend::RendererKind::Metal;
    // No SDL surface: this control must not observe drawable availability,
    // display refresh or host event pumping.
    initialize.presentationMode = rrv::gsbackend::PresentationMode::LegacyCpuSnapshot;
    rrv::gsbackend::Backend backend;
    if (!backend.initialize(initialize, error)) return false;
    if (rrv::gsbackend::activeRendererKind() != rrv::gsbackend::RendererKind::Metal)
    {
        if (error) *error = "M2 replay bridge did not instantiate Metal";
        return false;
    }

    std::vector<BufferedCapture> captures;
    captures.reserve(options.captureIndices.size());
    // Separate execution-time geometry state. It is deliberately not copied
    // from the fixture: only packets accepted by Backend::submit() contribute.
    std::vector<FixtureField> executedGeometry(fixture.size());
    for (FixtureField &field : executedGeometry)
        beginFixtureField(field);

    rrv::m2causal::beginReplay();
    rrv::m2causal::beginCompleteField(fixture.front().present.vsyncTick, 1u,
        static_cast<uint32_t>(fixture.front().present.vsyncTick & 1u));
    uint64_t startCpu = 0u;
    if (!threadCpuNanoseconds(&startCpu))
    {
        if (error) *error = "CLOCK_THREAD_CPUTIME_ID is unavailable for the M2 replay control";
        return false;
    }
    const auto startWall = std::chrono::steady_clock::now();

    // The first receipt epoch starts here: the preloaded header VRAM is a
    // real consumer restore, before field zero's transfers and successful VSync.
    if (!backend.restoreLocalMemory(recording.bytes.data() + sizeof(GsrFileHeader),
                                    recording.header.vramSize, error))
        return false;

    std::array<uint64_t, 19u> registers{};
    std::memcpy(registers.data(), recording.header.initialPrivRegs, sizeof(registers));
    uint64_t fieldOrdinal = 0u;
    for (const FixtureEvent &event : events)
    {
        const uint8_t *payload = recording.bytes.data() + event.payloadOffset;
        if (event.tag == EventTag::GifPacket)
        {
            if (!backend.submit(event.pathId, payload, event.payloadSize, error)) return false;
            if (!noteFixtureGeometry(executedGeometry[fieldOrdinal], payload, event.payloadSize))
            {
                if (error) *error = "M2 replay executed packet has an invalid GIFtag payload";
                return false;
            }
            continue;
        }
        registers[0] = event.present.pmode;
        registers[2] = event.present.smode2;
        registers[7] = event.present.dispfb1;
        registers[8] = event.present.display1;
        registers[9] = event.present.dispfb2;
        registers[10] = event.present.display2;
        constexpr uint64_t fieldBit = 1ull << 13u;
        registers[15] = (registers[15] & ~fieldBit) |
                        ((event.present.vsyncTick & 1u) ? fieldBit : 0u);
        if (!backend.vsync(registers.data(), event.present.vsyncTick,
                           static_cast<uint32_t>(event.present.vsyncTick & 1u), error))
            return false;
        rrv::m2causal::completeField(event.present.vsyncTick, fieldOrdinal + 1u,
                                   static_cast<uint32_t>(event.present.vsyncTick & 1u));
        finishFixtureGeometry(executedGeometry[fieldOrdinal]);
        if (executedGeometry[fieldOrdinal].ingressGeometryVertexCount !=
                fixture[fieldOrdinal].ingressGeometryVertexCount ||
            executedGeometry[fieldOrdinal].ingressGeometrySignature !=
                fixture[fieldOrdinal].ingressGeometrySignature)
        {
            if (error) *error = "M2 replay executed geometry diverged from immutable fixture";
            return false;
        }
        if (options.captureIndices.contains(fieldOrdinal))
        {
            // Explicit diagnostic capture is permitted, retained in memory,
            // and exported only after the full immutable interval closes.
            BufferedCapture capture{};
            capture.fieldOrdinal = fieldOrdinal;
            const rrv::gsbackend::CaptureTag tag{event.present.vsyncTick,
                                                 event.present.vsyncTick,
                                                 fieldOrdinal + 1u};
            if (!backend.capture(tag, capture.result, error)) return false;
            capture.rgbaFnv1a = fnv1a(capture.result.rgba.data(), capture.result.rgba.size());
            captures.push_back(std::move(capture));
        }
        ++fieldOrdinal;
    }
    rrv::m2causal::endReplay();
    uint64_t endCpu = 0u;
    if (!threadCpuNanoseconds(&endCpu))
    {
        if (error) *error = "CLOCK_THREAD_CPUTIME_ID failed after M2 replay interval";
        return false;
    }
    const uint64_t intervalWall = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startWall).count());
    if (fieldOrdinal != fixture.size())
    {
        if (error) *error = "M2 replay did not execute its fixed prevalidated field count";
        return false;
    }

    const rrv::gsbackend::CompletedFieldEpochHistory history = backend.completedFieldEpochHistory();
    if (history.droppedCompletedEpochs != 0u || history.totalCompletedEpochs != fixture.size() ||
        history.epochs.size() != fixture.size())
    {
        if (error) *error = "M2 replay completed-epoch history is incomplete";
        return false;
    }
    for (size_t index = 0u; index != fixture.size(); ++index)
    {
        const auto &actual = history.epochs[index];
        const auto &expected = fixture[index];
        if (actual.guestFieldId != expected.present.vsyncTick ||
            actual.gsFieldEpochId != index + 1u ||
            actual.fieldParity != (expected.present.vsyncTick & 1u) ||
            actual.pathEventCounts[0] != expected.pathPacketCounts[0] ||
            actual.pathEventCounts[1] != expected.pathPacketCounts[1] ||
            actual.pathEventCounts[2] != expected.pathPacketCounts[2] ||
            actual.pathByteCounts[0] != expected.pathByteCounts[0] ||
            actual.pathByteCounts[1] != expected.pathByteCounts[1] ||
            actual.pathByteCounts[2] != expected.pathByteCounts[2])
        {
            if (error) *error = "M2 replay independent completed-epoch counters diverged from fixture";
            return false;
        }
    }
    if (captures.size() != options.captureIndices.size())
    {
        if (error) *error = "M2 replay did not materialize every requested deferred capture";
        return false;
    }
    rrv::gsbackend::PresentationStats stats{};
    if (!backend.presentationStats(stats, error)) return false;
    if (stats.directGpuPresents != 0u || stats.unexpectedReadbacks != 0u ||
        stats.requestedCaptures != captures.size() || stats.completedCaptures != captures.size())
    {
        if (error) *error = "M2 replay presentation/capture statistics violate its headless control contract";
        return false;
    }

    uint64_t shutdownStartCpu = 0u, shutdownEndCpu = 0u;
    if (!threadCpuNanoseconds(&shutdownStartCpu))
    {
        if (error) *error = "CLOCK_THREAD_CPUTIME_ID failed before M2 receipt export";
        return false;
    }
    const auto shutdownStartWall = std::chrono::steady_clock::now();
    backend.shutdown(); // Mode C receipt export happens here, after the interval.
    if (!rrv::m2causal::dumpDeferred())
    {
        if (error) *error = "M2 causal trace incomplete, overflowed, or export failed";
        return false;
    }
    const uint64_t shutdownWall = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - shutdownStartWall).count());
    if (!threadCpuNanoseconds(&shutdownEndCpu))
    {
        if (error) *error = "CLOCK_THREAD_CPUTIME_ID failed after M2 receipt export";
        return false;
    }

    for (const BufferedCapture &capture : captures)
    {
        if (!writePpm(options.outputDirectory /
                ("m2-neutrality-" + std::to_string(capture.fieldOrdinal) + ".ppm"),
                capture.result.rgba.data(), capture.result.width, capture.result.height))
        {
            if (error) *error = "M2 replay deferred capture export failed";
            return false;
        }
    }
    if (!writeM2Manifest(options.outputDirectory / "m2-neutrality.json", recording, fixture,
                         executedGeometry,
                         history, captures, stats, endCpu - startCpu, intervalWall,
                         shutdownEndCpu - shutdownStartCpu, shutdownWall))
    {
        if (error) *error = "M2 replay deferred manifest export failed";
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseOptions(argc, argv, options))
    {
        usage(argv[0]);
        return 2;
    }
    options.recording = std::filesystem::absolute(options.recording);
    options.outputDirectory = std::filesystem::absolute(options.outputDirectory);
    const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
    std::filesystem::current_path(executable.parent_path());
    Recording recording;
    if (!loadRecording(options.recording, recording))
    {
        std::fprintf(stderr, "[m1-replay] invalid field-mode GSR: %s\n", options.recording.string().c_str());
        return 1;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(options.outputDirectory, filesystemError);
    if (filesystemError)
    {
        std::fprintf(stderr, "[m1-replay] cannot create output directory: %s\n",
                     filesystemError.message().c_str());
        return 1;
    }
    std::string error;
    if (options.m2Neutrality)
    {
        if (!runM2NeutralityReplay(options, recording, &error))
        {
            std::fprintf(stderr, "[m1-replay:m2-neutrality] %s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    const char *modeName = options.mode == rrv::gsbackend::PresentationMode::DirectGpu ?
                           "direct" : "legacy";
    const std::filesystem::path manifestPath = options.outputDirectory /
                                               (std::string(modeName) + "-records.txt");
    std::unique_ptr<FILE, decltype(&std::fclose)> manifest(
        std::fopen(manifestPath.string().c_str(), "wb"), &std::fclose);
    if (!manifest)
    {
        std::fprintf(stderr, "[m1-replay] cannot create comparison manifest\n");
        return 1;
    }

    rrv::host::SdlPresentation host;
    rrv::gsbackend::InitializeOptions initialize{};
    initialize.snapshotWidth = 640u;
    initialize.snapshotHeight = 448u;
    initialize.rendererKind = rrv::gsbackend::RendererKind::Metal;
    initialize.presentationMode = options.mode;
    if (options.mode == rrv::gsbackend::PresentationMode::DirectGpu)
    {
        if (!host.create("RRV M1 deterministic presentation replay", &error) ||
            !host.currentSurface(&initialize.surface))
        {
            std::fprintf(stderr, "[m1-replay] SDL Metal surface failed: %s\n", error.c_str());
            return 1;
        }
    }

    rrv::gsbackend::Backend backend;
    if (!backend.initialize(initialize, &error))
    {
        std::fprintf(stderr, "[m1-replay] bridge initialization failed: %s\n", error.c_str());
        return 1;
    }
    if (rrv::gsbackend::activeRendererKind() != rrv::gsbackend::RendererKind::Metal)
    {
        std::fprintf(stderr, "[m1-replay] bridge did not instantiate Metal\n");
        return 1;
    }
    if (recording.header.vramSize != 4u * 1024u * 1024u ||
        !backend.restoreLocalMemory(recording.bytes.data() + sizeof(GsrFileHeader),
                                    recording.header.vramSize, &error))
    {
        std::fprintf(stderr, "[m1-replay] initial GS local-memory restore failed: %s\n", error.c_str());
        return 1;
    }

    std::array<uint64_t, 19u> registers{};
    std::memcpy(registers.data(), recording.header.initialPrivRegs, sizeof(registers));
    size_t cursor = recording.eventOffset;
    uint64_t presentIndex = 0u;
    uint64_t decodedEvents = 0u;
    std::set<uint64_t> materializedCaptures;
    uint64_t readbackNanoseconds = 0u;
    uint64_t captureNanoseconds = 0u;
    const auto replayStart = std::chrono::steady_clock::now();
    while (cursor + sizeof(GsrEventHeader) <= recording.bytes.size())
    {
        GsrEventHeader event{};
        std::memcpy(&event, recording.bytes.data() + cursor, sizeof(event));
        cursor += sizeof(event);
        if (event.payloadSize > recording.bytes.size() - cursor)
        {
            std::fprintf(stderr, "[m1-replay] truncated event stream\n");
            return 1;
        }
        const uint8_t *payload = recording.bytes.data() + cursor;
        if (event.tag == static_cast<uint8_t>(EventTag::GifPacket))
        {
            if (!backend.submit(event.pathId, payload, event.payloadSize, &error))
            {
                std::fprintf(stderr, "[m1-replay] packet submit failed: %s\n", error.c_str());
                return 1;
            }
        }
        else if (event.tag == static_cast<uint8_t>(EventTag::Present))
        {
            if (event.payloadSize != sizeof(GsrPresentPayload))
            {
                std::fprintf(stderr, "[m1-replay] invalid present payload\n");
                return 1;
            }
            GsrPresentPayload present{};
            std::memcpy(&present, payload, sizeof(present));
            registers[0] = present.pmode;
            registers[2] = present.smode2;
            registers[7] = present.dispfb1;
            registers[8] = present.display1;
            registers[9] = present.dispfb2;
            registers[10] = present.display2;
            constexpr uint64_t fieldBit = 1ull << 13u;
            registers[15] = (registers[15] & ~fieldBit) |
                            ((present.vsyncTick & 1u) ? fieldBit : 0u);
            if (!backend.vsync(registers.data(), present.vsyncTick,
                               static_cast<uint32_t>(present.vsyncTick & 1u), &error))
            {
                std::fprintf(stderr, "[m1-replay] field transition failed: %s\n", error.c_str());
                return 1;
            }

            const bool selected = options.captureIndices.contains(presentIndex);
            if (options.mode == rrv::gsbackend::PresentationMode::LegacyCpuSnapshot)
            {
                std::vector<uint8_t> rgba;
                uint32_t width = 0u, height = 0u;
                const auto before = std::chrono::steady_clock::now();
                if (!backend.copyFrame(rgba, width, height, &error))
                {
                    std::fprintf(stderr, "[m1-replay] legacy snapshot failed: %s\n", error.c_str());
                    return 1;
                }
                readbackNanoseconds += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - before).count());
                if (selected)
                {
                    const uint64_t hash = fnv1a(rgba.data(), rgba.size());
                    std::printf("%llu %llu %016llx %ux%u\n",
                                static_cast<unsigned long long>(presentIndex),
                                static_cast<unsigned long long>(present.vsyncTick),
                                static_cast<unsigned long long>(hash), width, height);
                    std::fprintf(manifest.get(), "%llu %llu %016llx %ux%u\n",
                                 static_cast<unsigned long long>(presentIndex),
                                 static_cast<unsigned long long>(present.vsyncTick),
                                 static_cast<unsigned long long>(hash), width, height);
                    if (!writePpm(options.outputDirectory /
                            ("legacy-" + std::to_string(presentIndex) + ".ppm"),
                            rgba.data(), width, height))
                        return 1;
                    materializedCaptures.insert(presentIndex);
                }
            }
            else
            {
                host.pumpEvents();
                if (host.shouldClose()) return 1;
                if (selected)
                {
                    rrv::gsbackend::PresentationStats stats{};
                    rrv::gsbackend::CaptureResult capture{};
                    if (!backend.presentationStats(stats, &error)) return 1;
                    const rrv::gsbackend::CaptureTag tag{present.vsyncTick, present.vsyncTick,
                                                         stats.directGpuPresents};
                    const auto before = std::chrono::steady_clock::now();
                    if (!backend.capture(tag, capture, &error))
                    {
                        std::fprintf(stderr, "[m1-replay] explicit capture failed: %s\n", error.c_str());
                        return 1;
                    }
                    captureNanoseconds += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - before).count());
                    const uint64_t hash = fnv1a(capture.rgba.data(), capture.rgba.size());
                    std::printf("%llu %llu %016llx %ux%u\n",
                                static_cast<unsigned long long>(presentIndex),
                                static_cast<unsigned long long>(present.vsyncTick),
                                static_cast<unsigned long long>(hash),
                                capture.width, capture.height);
                    std::fprintf(manifest.get(), "%llu %llu %016llx %ux%u\n",
                                 static_cast<unsigned long long>(presentIndex),
                                 static_cast<unsigned long long>(present.vsyncTick),
                                 static_cast<unsigned long long>(hash),
                                 capture.width, capture.height);
                    if (!writePpm(options.outputDirectory /
                            ("direct-" + std::to_string(presentIndex) + ".ppm"),
                            capture.rgba.data(), capture.width, capture.height))
                        return 1;
                    materializedCaptures.insert(presentIndex);
                }
            }
            ++presentIndex;
        }
        else
        {
            std::fprintf(stderr, "[m1-replay] unknown event tag %u\n", event.tag);
            return 1;
        }
        cursor += event.payloadSize;
        ++decodedEvents;
    }

    if (cursor != recording.bytes.size() || decodedEvents != recording.header.eventCount)
    {
        std::fprintf(stderr,
                     "[m1-replay] event-stream integrity failure decoded=%llu header=%llu cursor=%zu size=%zu\n",
                     static_cast<unsigned long long>(decodedEvents),
                     static_cast<unsigned long long>(recording.header.eventCount), cursor,
                     recording.bytes.size());
        return 1;
    }

    rrv::gsbackend::PresentationStats stats{};
    if (!backend.presentationStats(stats, &error)) return 1;
    const auto wallNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - replayStart).count());
    std::fprintf(stderr,
                 "[m1-replay] mode=%s presents=%llu direct=%llu requested=%llu completed=%llu "
                 "sync-readbacks=%llu unexpected=%llu cpu-waits=%llu wall-ms=%.3f "
                 "legacy-readback-ms=%.3f capture-ms=%.3f\n",
                 options.mode == rrv::gsbackend::PresentationMode::DirectGpu ? "direct" : "legacy",
                 static_cast<unsigned long long>(presentIndex),
                 static_cast<unsigned long long>(stats.directGpuPresents),
                 static_cast<unsigned long long>(stats.requestedCaptures),
                 static_cast<unsigned long long>(stats.completedCaptures),
                 static_cast<unsigned long long>(stats.synchronousCpuReadbacks),
                 static_cast<unsigned long long>(stats.unexpectedReadbacks),
                 static_cast<unsigned long long>(stats.cpuWaits), wallNanoseconds / 1.0e6,
                 readbackNanoseconds / 1.0e6, captureNanoseconds / 1.0e6);
    if (presentIndex != options.expectedPresents || stats.unexpectedReadbacks != 0u ||
        materializedCaptures != options.captureIndices)
        return 1;
    if (options.mode == rrv::gsbackend::PresentationMode::DirectGpu &&
        (stats.directGpuPresents != presentIndex ||
         stats.requestedCaptures != options.captureIndices.size() ||
         stats.completedCaptures != options.captureIndices.size() ||
         stats.synchronousCpuReadbacks != options.captureIndices.size() ||
         stats.cpuWaits != options.captureIndices.size()))
        return 1;
    return 0;
}
