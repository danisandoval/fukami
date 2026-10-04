#include "rrv_m2_causal_trace.h"
#include "rrv_m2_dma_provenance.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace causal = rrv::m2causal;
namespace provenance = rrv::m2prov;
namespace
{
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::vector<uint8_t> bytes(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}
uint32_t u32(const std::vector<uint8_t> &data, size_t offset)
{
    return data[offset] | (uint32_t{data[offset + 1u]} << 8u) |
           (uint32_t{data[offset + 2u]} << 16u) | (uint32_t{data[offset + 3u]} << 24u);
}
uint64_t u64(const std::vector<uint8_t> &data, size_t offset)
{
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i)
        value |= uint64_t{data[offset + i]} << (i * 8u);
    return value;
}
bool saw(const std::vector<uint8_t> &data, provenance::Event event)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
    {
        if (u32(data, offset + 8u) == static_cast<uint32_t>(event))
            return true;
    }
    return false;
}
size_t count(const std::vector<uint8_t> &data, provenance::Event event)
{
    size_t result = 0u;
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        result += u32(data, offset + 8u) == static_cast<uint32_t>(event) ? 1u : 0u;
    return result;
}

void makeTrace(const std::filesystem::path &path)
{
    check(causal::resetForTest(true, path.c_str(), 1u), "causal test initialization");
    check(provenance::resetForTest(true), "provenance test initialization");
    const std::array<uint8_t, 32> input = {
        0x50u, 0u, 0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u,
        13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u};
    uint64_t root = provenance::kUnknownId;
    uint64_t packet = provenance::kUnknownId;
    {
        // The root is deliberately created before beginReplay().  Its source
        // metadata must be retained but must not serialize until first use.
        provenance::OriginScope origin(0x220100u, 0x220104u, 2u);
        provenance::KickScope kick(0x100u, 0x00100000u, 2u, 0x00100020u,
                                   0x00100040u, 0x00100060u);
        root = kick.id();
        check(root != provenance::kUnknownId, "pre-arm root allocated");
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            kick.source(3u, 0x00100000u + i * 4u, input.data() + i * 4u, 4u, i * 4u, i);
            kick.tag(i, 0x0102030405060708ull + i, 0x1112131415161718ull + i);
        }
        kick.queueAccepted(0x00100000u, uint32_t(input.size()), input.data(), 9u);
        provenance::observeInput(root, 0x00100000u, input.data(), uint32_t(input.size()));
        check(causal::recordCount() == 0u, "pre-arm provenance is not serialized");

        causal::beginReplay();
        {
            provenance::ProvenanceScope outer(root);
            check(provenance::currentId() == root, "outer provenance context");
            {
                provenance::ProvenanceScope nested(root);
                check(provenance::currentId() == root, "nested provenance context");
            }
            check(provenance::currentId() == root, "nested provenance restores outer context");
            provenance::DecoderScope decoder(input.data(), uint32_t(input.size()));
            decoder.command(0x50000000u, 0u);
            decoder.command(0x51000000u, 4u);
            provenance::DirectScope direct(1u, 4u, 2u, input.data(), root);
            packet = direct.packet();
            check(packet != provenance::kUnknownId, "DIRECT packet ordinal");
            check(provenance::currentPacket() == packet, "DIRECT packet context");
            {
                provenance::DeliveryScope nonPath2(0u, 0u);
                check(provenance::currentId() == 0u && provenance::currentPacket() == 0u,
                      "nested non-PATH2 delivery clears outer provenance");
            }
            check(provenance::currentId() == root && provenance::currentPacket() == packet,
                  "nested non-PATH2 delivery restores outer provenance");
            {
                provenance::retain(root); // post-arbiter packet lease
                provenance::DeliveryScope delivery(root, packet);
                check(provenance::currentId() == root && provenance::currentPacket() == packet,
                      "delivery publishes queued packet context");
            }
            {
                provenance::retain(root); // a second queued packet retains independently
                provenance::DeliveryScope delivery(root, packet);
                check(provenance::currentId() == root && provenance::currentPacket() == packet,
                      "second delivery publishes queued packet context");
            }
            check(provenance::currentId() == root && provenance::currentPacket() == packet,
                  "delivery restores decoder context");
        }
        check(provenance::currentId() == provenance::kUnknownId, "provenance context restored");
        provenance::release(root); // queue-processing lease after both packet deliveries
        causal::completeField(1u, 1u, 0u);
    }
    check(causal::dumpDeferred(), "deferred provenance trace export");
}

void testLazyPrearmAndNestedScopes(const std::filesystem::path &directory)
{
    const auto output = directory / "provenance.bin";
    makeTrace(output);
    const auto data = bytes(output);
    check(data.size() >= 80u && u32(data, 8u) == 1u && u32(data, 16u) == 48u,
          "causal fixed-width export remains intact");
    check(saw(data, provenance::Event::RootOrigin), "lazy root origin emitted");
    check(saw(data, provenance::Event::RootKick), "DMA kick metadata emitted");
    check(saw(data, provenance::Event::SourceRange), "original source range emitted");
    check(saw(data, provenance::Event::SourceDigest), "original source digest emitted");
    check(saw(data, provenance::Event::TagWords), "DMA tag words emitted");
    check(saw(data, provenance::Event::TagUpper), "full DMA tag upper 64 bits emitted");
    check(count(data, provenance::Event::SourceRange) == 8u,
          "shared source arena retains more than the former four chunks");
    check(count(data, provenance::Event::TagWords) == 8u,
          "shared tag arena retains more than the former four tags");
    check(saw(data, provenance::Event::QueueAccepted), "flattened queue acceptance emitted");
    check(saw(data, provenance::Event::ObservedInput), "queued decoder input digest emitted");
    check(saw(data, provenance::Event::DecoderDigest), "decoder command digest emitted");
    check(saw(data, provenance::Event::DirectOpeningLink), "DIRECT opening root remains explicit");
    check(saw(data, provenance::Event::DeliveryExit), "queued root is released at delivery completion");

    bool prearm = false;
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
    {
        if (u32(data, offset + 8u) == static_cast<uint32_t>(provenance::Event::RootOrigin))
            prearm = (u64(data, offset + 40u) >> 32u) == 1u;
    }
    check(prearm, "retained root declares created-before-selector");
}

void testDisabled(const std::filesystem::path &directory)
{
    const auto output = directory / "disabled.bin";
    check(causal::resetForTest(false, output.c_str(), 1u), "disabled causal initialization");
    check(provenance::resetForTest(false), "disabled provenance initialization");
    provenance::OriginScope origin(1u, 2u, 3u);
    provenance::KickScope kick(1u, 2u, 3u, 4u, 5u, 6u);
    kick.source(0u, 0u, nullptr, 0u);
    check(kick.id() == provenance::kUnknownId, "disabled provenance allocates no root ID");
    check(!causal::enabled() && causal::dumpDeferred(), "disabled trace stays inert");
    check(!std::filesystem::exists(output), "disabled provenance exports no file");
}

void testOverflow(const std::filesystem::path &directory)
{
    const auto output = directory / "overflow.bin";
    check(causal::resetForTest(true, output.c_str(), 1u), "overflow causal initialization");
    check(provenance::resetForTest(true), "overflow provenance initialization");
    const std::array<uint8_t, 4> data = {0u, 1u, 2u, 3u};
    provenance::OriginScope origin(0x220100u, 0u, 2u);
    provenance::KickScope kick(1u, 2u, 3u, 4u, 5u, 6u);
    kick.queueAccepted(0u, uint32_t(data.size()), data.data());
    causal::beginReplay();
    {
        provenance::ProvenanceScope scope(kick.id());
        provenance::DecoderScope decoder(data.data(), uint32_t(data.size()));
        for (uint32_t i = 0u; i < 131100u; ++i)
            decoder.command(i, i * 4u);
    }
    causal::completeField(1u, 1u, 0u);
    check(causal::overflowed(), "causal recorder reports provenance overflow");
    check(!causal::dumpDeferred(), "overflow cannot export a pass artifact");
    check(!std::filesystem::exists(output), "overflow produces no trace file");
}
} // namespace

int main()
{
    std::string pattern = (std::filesystem::temp_directory_path() / "rrv-m2-dma-provenance-XXXXXX").string();
    char *created = mkdtemp(pattern.data());
    if (!created)
        return 1;
    const std::filesystem::path directory(created);
    std::error_code error;
    testLazyPrearmAndNestedScopes(directory);
    testDisabled(directory);
    testOverflow(directory);
    std::filesystem::remove_all(directory, error);
    return failures;
}
