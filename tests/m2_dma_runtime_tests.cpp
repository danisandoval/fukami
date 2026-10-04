// Deterministic, asset-free M2 producer provenance harness.
//
// This test intentionally drives PS2Memory through the same public paths as
// the product: guest Store32 to VIF1 DMAC registers, flattened VIF1 chains,
// and the public libdma HLE calls.  Its callback is an ordinary GifArbiter
// consumer, not a synthetic recorder or a GS replacement.

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "rrv_hle.h"
#include "runtime/ps2_gif_arbiter.h"
#include "rrv_m2_causal_trace.h"
#include "rrv_m2_dma_provenance.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace causal = rrv::m2causal;
namespace prov = rrv::m2prov;

namespace
{
constexpr uint32_t kVif1 = 0x10009000u;
constexpr uint32_t kStoreNormal = 0x00000101u; // DIR=1, normal, STR=1.
constexpr uint32_t kStoreChain = 0x00000145u;  // normal bits + chain + TTE.
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
#if defined(RRV_M2_DMA_PROVENANCE)
constexpr bool kProvenanceCompiled = true;
#else
constexpr bool kProvenanceCompiled = false;
#endif

int failures = 0;

void check(bool value, std::string_view message)
{
    if (!value)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void put32(uint8_t *dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8u);
    dst[2] = static_cast<uint8_t>(value >> 16u);
    dst[3] = static_cast<uint8_t>(value >> 24u);
}

void put64(uint8_t *dst, uint64_t value)
{
    for (uint32_t i = 0u; i < 8u; ++i)
        dst[i] = static_cast<uint8_t>(value >> (i * 8u));
}

uint64_t fnvByte(uint64_t hash, uint8_t byte)
{
    return (hash ^ byte) * kFnvPrime;
}

// One VIF DIRECT/DIRECTHL plus one whole GIF QW.  The trailing 12 bytes are
// zero VIF NOPs, deliberately avoiding an accidental second command.
std::array<uint8_t, 32> directInput(uint8_t opcode, uint8_t marker)
{
    std::array<uint8_t, 32> result{};
    put32(result.data(), (uint32_t{opcode} << 24u) | 1u);
    // A valid minimal packed GIFtag. The marker is data, not a host identity.
    put64(result.data() + 4u, uint64_t{1u} | (uint64_t{1u} << 15u) |
                                 (uint64_t{1u} << 60u));
    result[12u] = marker;
    result[13u] = static_cast<uint8_t>(marker ^ 0x5au);
    result[14u] = static_cast<uint8_t>(marker + 3u);
    result[15u] = static_cast<uint8_t>(marker ^ 0xa5u);
    return result;
}

// A DIRECT whose GIFtag declares two IMAGE QWs after itself.  The first QW is
// supplied inline, then imageContinuation() supplies the retained raw QWs via
// the following VIF1 input, exercising the real pending-IMAGE state.
std::array<uint8_t, 32> imageDirectInput(uint8_t marker)
{
    std::array<uint8_t, 32> result{};
    put32(result.data(), (uint32_t{0x50u} << 24u) | 1u);
    put64(result.data() + 4u, uint64_t{2u} | (uint64_t{1u} << 15u) |
                                 (uint64_t{2u} << 58u));
    result[12u] = marker;
    result[13u] = static_cast<uint8_t>(marker ^ 0x33u);
    return result;
}

std::array<uint8_t, 32> imageContinuation(uint8_t marker)
{
    std::array<uint8_t, 32> result{};
    for (uint32_t i = 0u; i < result.size(); ++i)
        result[i] = static_cast<uint8_t>(marker + i * 13u);
    return result;
}

void writeEndTag(uint8_t *dst, uint32_t qwc)
{
    std::memset(dst, 0, 16u);
    // END (id 7) with inline payload immediately after the tag.
    put64(dst, uint64_t{qwc} | (uint64_t{7u} << 28u));
}

void setGpr32(R5900Context &ctx, unsigned reg, uint32_t value)
{
    ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

struct Packet
{
    GifPathId path{};
    uint32_t bytes{};
    uint64_t root{};
    uint64_t packet{};
    uint64_t digest{};
};

struct Sink
{
    std::vector<Packet> packets;
    uint64_t independentDigest = kFnvOffset;

    Sink()
    {
        // The full test interval has exactly eight deliveries.  Reserve before
        // memory/diagnostic initialization so callback recording never grows.
        packets.reserve(8u);
    }

    void accept(GifPathId path, const uint8_t *data, uint32_t bytes)
    {
        uint64_t payload = kFnvOffset;
        independentDigest = fnvByte(independentDigest, static_cast<uint8_t>(path));
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            independentDigest = fnvByte(independentDigest, static_cast<uint8_t>(bytes >> shift));
        for (uint32_t i = 0u; i < bytes; ++i)
        {
            payload = fnvByte(payload, data[i]);
            independentDigest = fnvByte(independentDigest, data[i]);
        }
        packets.push_back(Packet{path, bytes, prov::currentId(), prov::currentPacket(), payload});
    }
};

void guestStore32(PS2Runtime &runtime, R5900Context &ctx, uint32_t address, uint32_t value)
{
    runtime.Store32(runtime.memory().getRDRAM(), &ctx, address, value);
}

void submitStoreNormal(PS2Runtime &runtime, R5900Context &ctx, uint32_t source)
{
    guestStore32(runtime, ctx, kVif1 + 0x20u, 2u);
    guestStore32(runtime, ctx, kVif1 + 0x10u, source);
    guestStore32(runtime, ctx, kVif1, kStoreNormal);
}

void submitStoreChain(PS2Runtime &runtime, R5900Context &ctx, uint32_t tagAddress)
{
    guestStore32(runtime, ctx, kVif1 + 0x20u, 0u);
    guestStore32(runtime, ctx, kVif1 + 0x10u, 0u);
    guestStore32(runtime, ctx, kVif1 + 0x30u, tagAddress);
    guestStore32(runtime, ctx, kVif1, kStoreChain);
}

void submitStubNormal(PS2Runtime &runtime, R5900Context &ctx, uint32_t source)
{
    setGpr32(ctx, 4u, kVif1);
    setGpr32(ctx, 5u, source);
    setGpr32(ctx, 6u, 2u);
    ps2_stubs::sceDmaSendN(runtime.memory().getRDRAM(), &ctx, &runtime);
    check(getRegU32(&ctx, 2) == 0u, "sceDmaSendN returns success");
}

void submitStubChain(PS2Runtime &runtime, R5900Context &ctx, uint32_t tagAddress)
{
    setGpr32(ctx, 4u, kVif1);
    setGpr32(ctx, 5u, tagAddress);
    setGpr32(ctx, 6u, 0u);
    ps2_stubs::sceDmaSend(runtime.memory().getRDRAM(), &ctx, &runtime);
    check(getRegU32(&ctx, 2) == 0u, "sceDmaSend returns success");
}

void submitDevVif1Fifo(PS2Runtime &runtime, R5900Context &ctx, uint32_t source)
{
    setGpr32(ctx, 4u, source);
    setGpr32(ctx, 5u, 2u);
    rrv_hle::devVif1PutFifo_stub(runtime.memory().getRDRAM(), &ctx, &runtime);
    check(getRegU32(&ctx, 2) == 0u, "devVif1PutFifo returns success");
}

std::string jsonReceipt(const Sink &sink, const std::vector<uint32_t> &dmacCauses,
                        bool provenanceRequested, bool provenanceEnabled)
{
    uint32_t path2 = 0u;
    uint32_t path3 = 0u;
    uint32_t path2Bytes = 0u;
    uint32_t path3Bytes = 0u;
    uint32_t nonzeroRoots = 0u;
    uint32_t nonzeroPackets = 0u;
    for (const Packet &packet : sink.packets)
    {
        if (packet.path == GifPathId::Path2)
        {
            ++path2;
            path2Bytes += packet.bytes;
            nonzeroRoots += packet.root != prov::kUnknownId ? 1u : 0u;
            nonzeroPackets += packet.packet != prov::kUnknownId ? 1u : 0u;
        }
        else if (packet.path == GifPathId::Path3)
        {
            ++path3;
            path3Bytes += packet.bytes;
        }
    }
    return "{\n"
           "  \"schema_version\": 1,\n"
           "  \"seed\": \"m2-dma-runtime-fixed-1\",\n"
           "  \"path2_packets\": " + std::to_string(path2) + ",\n"
           "  \"path2_bytes\": " + std::to_string(path2Bytes) + ",\n"
           "  \"path3_packets\": " + std::to_string(path3) + ",\n"
           "  \"path3_bytes\": " + std::to_string(path3Bytes) + ",\n"
           "  \"dmac_vif1_completions\": " + std::to_string(dmacCauses.size()) + ",\n"
           "  \"independent_digest\": \"" + std::to_string(sink.independentDigest) + "\",\n"
           "  \"provenance_compiled\": " + (kProvenanceCompiled ? "true" : "false") + ",\n"
           "  \"provenance_requested\": " + (provenanceRequested ? "true" : "false") + ",\n"
           "  \"provenance_enabled\": " + (provenanceEnabled ? "true" : "false") + ",\n"
           "  \"path2_nonzero_root_count\": " + std::to_string(nonzeroRoots) + ",\n"
           "  \"path2_nonzero_packet_count\": " + std::to_string(nonzeroPackets) + "\n"
           "}\n";
}

bool writeDeferred(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
}

std::filesystem::path causalPathFor(const std::filesystem::path &output)
{
    if (!output.empty())
        return output.string() + ".causal.bin";
    return std::filesystem::temp_directory_path() /
           ("rrv-m2-dma-runtime-" + std::to_string(static_cast<unsigned long>(::getpid())) + ".bin");
}

void run(const std::filesystem::path &output)
{
    const char *requested = std::getenv("RRV_M2_DMA_PROVENANCE");
    const bool requestedProvenance = requested && std::strcmp(requested, "1") == 0;
    const std::filesystem::path causalPath = causalPathFor(output);

    // Configure both diagnostics before PS2Memory allocates or any possible
    // producer path executes. The test later dumps only after field four.
    check(causal::resetForTest(true, causalPath.c_str(), 4u), "causal trace preallocation");
    check(prov::resetForTest(requestedProvenance), "provenance preallocation");
    const bool provenanceEnabled = prov::configured();
    check(provenanceEnabled == (kProvenanceCompiled && requestedProvenance),
          "provenance compiled/requested/configured mode contract");

    Sink sink;
    std::vector<uint32_t> dmacCauses;
    dmacCauses.reserve(7u);

    PS2Runtime runtime;
    if (!runtime.memory().initialize())
    {
        check(false, "memory-only runtime initialization");
        return;
    }
    R5900Context &ctx = runtime.cpu();
    ctx.pc = 0x00220100u;
    setGpr32(ctx, 31u, 0x0022f00du);
    runtime.memory().setDmacCompletionCallback([&dmacCauses](uint32_t cause) {
        dmacCauses.push_back(cause);
    });
    GifArbiter &arbiter = runtime.gifArbiter();
    arbiter.setProcessPacketFn([&sink](GifPathId path, const uint8_t *data, uint32_t bytes,
                                       uint32_t, const GifPacketDiagnostic *) {
        sink.accept(path, data, bytes);
    });
    runtime.memory().setGifArbiter(&arbiter);

    uint8_t *const rdram = runtime.memory().getRDRAM();
    constexpr uint32_t kPreedgeChain = 0x00010000u;
    constexpr uint32_t kStoreNormal = 0x00011000u;
    constexpr uint32_t kImageDirect = 0x00012000u;
    constexpr uint32_t kImageContinuation = 0x00013000u;
    constexpr uint32_t kStubNormal = 0x00014000u;
    constexpr uint32_t kStubChain = 0x00015000u;
    constexpr uint32_t kZeroChain = 0x00016000u;
    constexpr uint32_t kDevVif1Fifo = 0x00017000u;

    const auto preedge = directInput(0x50u, 0x11u);
    const auto normal = directInput(0x50u, 0x22u);
    const auto image = imageDirectInput(0x33u);
    const auto continuation = imageContinuation(0x44u);
    const auto directHl = directInput(0x51u, 0x55u);
    const auto stubChain = directInput(0x50u, 0x66u);
    const auto fifo = directInput(0x50u, 0x88u);
    const auto path3 = directInput(0x50u, 0x77u);

    writeEndTag(rdram + kPreedgeChain, 2u);
    std::memcpy(rdram + kPreedgeChain + 16u, preedge.data(), preedge.size());
    std::memcpy(rdram + kStoreNormal, normal.data(), normal.size());
    std::memcpy(rdram + kImageDirect, image.data(), image.size());
    std::memcpy(rdram + kImageContinuation, continuation.data(), continuation.size());
    std::memcpy(rdram + kStubNormal, directHl.data(), directHl.size());
    writeEndTag(rdram + kStubChain, 2u);
    std::memcpy(rdram + kStubChain + 16u, stubChain.data(), stubChain.size());
    writeEndTag(rdram + kZeroChain, 0u);
    std::memcpy(rdram + kDevVif1Fifo, fifo.data(), fifo.size());

    // This chain enters the real held-VIF1 queue before the selector edge. It
    // must retain its producer association until the post-edge flush.
    submitStoreChain(runtime, ctx, kPreedgeChain);
    check(sink.packets.empty(), "pre-edge VIF1 chain remains held before flush");

    causal::beginSelectorEdge(0x00020000u, 0x00020005u, 700u);
    causal::originBoundary(700u, 900u, 0u);

    runtime.memory().flushHeldVif1AtGuestFrameEnd();
    check(sink.packets.size() == 1u, "pre-edge held chain reaches the arbiter after boundary");
    causal::completeField(701u, 901u, 1u);

    submitStoreNormal(runtime, ctx, kStoreNormal);
    check(sink.packets.size() == 1u, "Store32 normal VIF1 transfer is held");
    runtime.memory().flushHeldVif1AtGuestFrameEnd();
    check(sink.packets.size() == 2u, "Store32 normal VIF1 transfer drains through arbiter");
    causal::completeField(702u, 902u, 0u);

    submitStoreNormal(runtime, ctx, kImageDirect);
    // A new VIF1 transfer causes the existing held DIRECT to run first, which
    // arms the pending IMAGE continuation; the new raw input remains held.
    submitStoreNormal(runtime, ctx, kImageContinuation);
    check(sink.packets.size() == 3u, "IMAGE DIRECT delivered before continuation queue flush");
    runtime.memory().flushHeldVif1AtGuestFrameEnd();
    check(sink.packets.size() == 4u, "IMAGE continuation delivered as raw PATH2 QWs");
    causal::completeField(703u, 903u, 1u);

    submitStubNormal(runtime, ctx, kStubNormal);
    runtime.memory().flushHeldVif1AtGuestFrameEnd();
    submitStubChain(runtime, ctx, kStubChain);
    runtime.memory().flushHeldVif1AtGuestFrameEnd();
    submitDevVif1Fifo(runtime, ctx, kDevVif1Fifo);
    submitStoreChain(runtime, ctx, kZeroChain);
    check(sink.packets.size() == 7u, "zero-QWC chain completes without a PATH2 packet");

    // Reuses GifArbiter pooled packet storage after PATH2 delivery. A PATH3
    // callback must never retain a PATH2 producer root or packet identity.
    runtime.memory().submitGifPacket(GifPathId::Path3, path3.data() + 4u, 16u, true);
    check(sink.packets.size() == 8u, "PATH3 packet reaches the same real arbiter");
    causal::completeField(704u, 904u, 0u);

    check(!causal::overflowed(), "bounded causal trace does not overflow");
    check(causal::dumpDeferred(), "causal trace is exported only after field four");
    check(runtime.memory().dmaStartCount() == 7u, "exact VIF1 DMA start count");
    check(dmacCauses.size() == 7u, "zero-QWC chain retains DMAC completion");
    for (uint32_t cause : dmacCauses)
        check(cause == 1u, "all test completions are VIF1 causes");

    uint32_t path2Count = 0u;
    uint32_t path2Bytes = 0u;
    uint32_t path3Count = 0u;
    uint32_t path3Bytes = 0u;
    std::set<uint64_t> provenancePackets;
    for (const Packet &packet : sink.packets)
    {
        if (packet.path == GifPathId::Path2)
        {
            ++path2Count;
            path2Bytes += packet.bytes;
            if (provenanceEnabled)
            {
                check(packet.root != prov::kUnknownId, "PATH2 delivery has a retained root");
                check(packet.packet != prov::kUnknownId, "PATH2 delivery has a packet identity");
                provenancePackets.insert(packet.packet);
            }
        }
        else if (packet.path == GifPathId::Path3)
        {
            ++path3Count;
            path3Bytes += packet.bytes;
            if (provenanceEnabled)
            {
                check(packet.root == prov::kUnknownId, "PATH3 pooled packet clears PATH2 root identity");
                check(packet.packet == prov::kUnknownId, "PATH3 pooled packet clears PATH2 packet identity");
            }
        }
    }
    check(path2Count == 7u && path2Bytes == 128u, "fixed PATH2 packet and byte totals");
    check(path3Count == 1u && path3Bytes == 16u, "fixed PATH3 packet and byte totals");
    if (provenanceEnabled)
        check(provenancePackets.size() == path2Count, "each PATH2 delivery has a distinct packet identity");

    // The independent digest covers only the delivered packet path, size and
    // bytes. It deliberately excludes provenance IDs and causal records.
    constexpr uint64_t kExpectedIndependentDigest = 0x6c1f3e6ac92b04c1ull;
    check(sink.independentDigest == kExpectedIndependentDigest,
          "fixed independent delivery digest");

    const std::string receipt = jsonReceipt(sink, dmacCauses, requestedProvenance, provenanceEnabled);
    if (!output.empty())
        check(writeDeferred(output, receipt), "deferred JSON receipt export");
    else
        std::cout << receipt;
    if (output.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(causalPath, ignored);
    }
}

void usage(const char *name)
{
    std::cerr << "usage: " << name << " [--output <deferred-json-path>]\n";
}
} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path output;
    if (argc == 3 && std::string_view(argv[1]) == "--output")
        output = argv[2];
    else if (argc != 1)
    {
        usage(argv[0]);
        return 2;
    }
    run(output);
    return failures == 0 ? 0 : 1;
}
