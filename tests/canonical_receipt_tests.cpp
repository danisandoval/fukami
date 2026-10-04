// SPDX-License-Identifier: MIT
#include "canonical_receipt.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace receipt = rrv::pcsx2::receipt;

namespace
{
int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::array<uint64_t, 19u> vsyncRegisters(uint64_t marker)
{
    std::array<uint64_t, 19u> result{};
    for (size_t index = 0u; index != result.size(); ++index)
        result[index] = marker + index;
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    expect(receipt::xxh64(nullptr, 0u, 0u) == 0xef46db3751d8e999ull,
           "XXH64 empty-vector mismatch");
    expect(receipt::xxh64("a", 1u, 0u) == 0xd24ec4f1a98c6e5bull,
           "XXH64 one-byte vector mismatch");
    expect(receipt::xxh64("abc", 3u, 0u) == 0x44bc2cf5ad770999ull,
           "XXH64 three-byte vector mismatch");

    const bool preserveFixture = argc == 2;
    const std::filesystem::path output = preserveFixture ? std::filesystem::path(argv[1]) :
        std::filesystem::current_path() / "canonical-receipt-test.bin";
    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    {
        receipt::Collector collector;
        std::string error;
        expect(collector.initializeForTest(true, true, 2u, 16u, 2u,
                                           output.string(), &error),
               "collector initialization failed");
        std::array<uint8_t, 16u> ignoredBeforeArm{};
        expect(collector.acknowledgeTransfer(1u, ignoredBeforeArm.data(),
                                             ignoredBeforeArm.size()),
               "waiting collector rejected ignored transfer");
        expect(!std::filesystem::exists(output), "collector performed hot-path file I/O");
        expect(collector.arm(), "collector arm failed");

        const auto firstRegs = vsyncRegisters(0x1000u);
        expect(collector.setEpochIdentity(40u, 1u), "zero-transfer identity failed");
        expect(collector.acknowledgeVsync(firstRegs.data(), 1u, true, 0u),
               "zero-transfer field did not close");

        std::array<uint8_t, 32u> path1{};
        std::array<uint8_t, 16u> path2{};
        std::array<uint8_t, 16u> restored{};
        for (size_t index = 0u; index != path1.size(); ++index)
            path1[index] = static_cast<uint8_t>(index);
        path2.fill(0x5au);
        restored.fill(0xc3u);
        expect(collector.acknowledgeTransfer(1u, path1.data(), path1.size()),
               "PATH1 acknowledgement failed");
        expect(collector.acknowledgeTransfer(2u, path2.data(), path2.size()),
               "PATH2 acknowledgement failed");
        expect(collector.acknowledgeLocalMemoryRestore(restored.data(), restored.size()),
               "local-memory restore acknowledgement failed");
        expect(collector.setEpochIdentity(41u, 2u), "second identity failed");
        const auto secondRegs = vsyncRegisters(0x2000u);
        expect(collector.acknowledgeVsync(secondRegs.data(), 0u, false, 1u),
               "second field did not close");

        const receipt::Status status = collector.status();
        expect(status.intervalComplete && !status.overflowed,
               "bounded receipt did not complete cleanly");
        expect(status.epochCount == 2u && status.eventCount == 5u,
               "unexpected retained counts");
        const receipt::EpochRecord *epochs = collector.epochs();
        expect(epochs[0].eventCount == 1u && epochs[0].pathEventCounts[0] == 0u,
               "zero-transfer field was not represented by VSync alone");
        expect(epochs[1].eventCount == 4u && epochs[1].pathEventCounts[0] == 1u &&
                   epochs[1].pathEventCounts[1] == 1u &&
                   epochs[1].xgkickDerivedTransferCount == 1u,
               "multi-event field counts are wrong");
        expect(epochs[0].finalEpochDigest != epochs[1].finalEpochDigest,
               "different epochs unexpectedly share a digest");
        expect(!std::filesystem::exists(output), "receipt was written before deferred dump");
        expect(collector.dumpDeferred(&error), "deferred dump failed");
        expect(std::filesystem::file_size(output) == 80u + 5u * 32u + 2u * 104u,
               "deferred binary size violates schema");
    }

    {
        receipt::Collector collector;
        std::string error;
        const std::filesystem::path overflowOutput =
            std::filesystem::current_path() / "canonical-receipt-overflow-test.bin";
        std::filesystem::remove(overflowOutput, ignored);
        expect(collector.initializeForTest(true, false, 1u, 1u, 1u,
                                           overflowOutput.string(), &error),
               "overflow collector initialization failed");
        std::array<uint8_t, 16u> bytes{};
        expect(collector.acknowledgeTransfer(3u, bytes.data(), bytes.size()),
               "first bounded event failed");
        expect(collector.setEpochIdentity(1u, 1u), "overflow identity failed");
        const auto regs = vsyncRegisters(0u);
        expect(!collector.acknowledgeVsync(regs.data(), 0u, true, 0u),
               "event-capacity overflow was silently accepted");
        expect(collector.status().overflowed, "overflow status was not retained");
        expect(!collector.dumpDeferred(&error), "failed interval exported as successful");
        std::filesystem::remove(overflowOutput, ignored);
    }

    if (!preserveFixture)
        std::filesystem::remove(output, ignored);
    return failures == 0 ? 0 : 1;
}
