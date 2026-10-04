// Deterministic, asset-free M2 guest continuation/write provenance harness.
//
// The fixture uses the public PS2Runtime dispatchLoop with registered
// synthetic recompiled functions.  It deliberately exercises the exact
// overlay sites used by generated code: DispatchScope around the real loop,
// fast-path WRITE8..128 macros, and PS2Runtime::Store8..128.  It has no game
// assets, guest worker, window, renderer, capture, or timing source.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "Stubs/LibC.h"
#include "Stubs/VU.h"
#include "rrv_m2_causal_trace.h"
#include "rrv_m2_dma_provenance.h"
#include "rrv_m2_guest_provenance.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace causal = rrv::m2causal;
namespace dma = rrv::m2prov;
namespace guest = rrv::m2guest;

namespace
{
constexpr uint32_t kTargetCount = 8u;
constexpr uint32_t kCarryInTarget = 0x0021df9cu;
constexpr std::array<uint32_t, kTargetCount> kWriterTargets = {
    0x00295e38u, 0x0029c460u, 0x00220000u, 0x00220004u,
    0x00220008u, 0x0022000cu, 0x00220010u, 0x00220014u,
};
constexpr std::array<uint32_t, 4u> kBoundaryTargets = {
    0x00221000u, 0x00221004u, 0x00221008u, 0x0022100cu,
};
constexpr uint32_t kTerminalPc = 0x00221ffcu;
constexpr uint32_t kSourceStart = guest::kSourceRangeStart;
constexpr uint32_t kSourceBytes = guest::kSourceRangeBytes;
constexpr uint32_t kSourceEnd = kSourceStart + kSourceBytes;
constexpr uint32_t kVif1Dmac = 0x10009000u;
constexpr uint32_t kSegmentStride = 40u;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint64_t kExpectedWindowHash = 0xf05e74aa1eda9c25ull;
constexpr uint64_t kExpectedOutsideHash = 0xd528f8db6c6e053full;
constexpr uint32_t kCopySource = 0x00018000u;
constexpr uint32_t kVuCopySource = 0x00018100u;
constexpr uint32_t kControlByteAddress = 0x00018266u;
// Match the live helper's real input base.  The two eight-byte source words
// are deliberately outside the older 320-byte VIF-source window and are
// observed by the append-only two-slot input-write provenance seam.
constexpr uint32_t kHelperInput = 0x00346f80u;
constexpr uint32_t kHelperInput20 = kHelperInput + 0x20u;
constexpr uint32_t kHelperInput50 = kHelperInput + 0x50u;
constexpr uint64_t kHelperFirst = 0x0123456789abcdefull;
constexpr uint64_t kHelperSecond = 0xfedcba9876543210ull;
#if defined(RRV_M2_GUEST_PROVENANCE)
constexpr bool kGuestCompiled = true;
#else
constexpr bool kGuestCompiled = false;
#endif

int failures = 0;
std::array<uint32_t, kTargetCount> executedTargets{};
uint32_t executedCount = 0u;
uint64_t helperResult60 = 0u;
uint64_t helperResult100 = 0u;

void check(bool value, std::string_view message)
{
    if (!value)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

uint64_t fnv1a(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = kFnvOffset;
    for (uint32_t index = 0u; index < size; ++index)
        hash = (hash ^ bytes[index]) * kFnvPrime;
    return hash;
}

void storeLe64(uint8_t *destination, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        destination[index] = static_cast<uint8_t>(value >> (index * 8u));
}

void setGpr32(R5900Context &ctx, unsigned reg, uint32_t value)
{
    ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

uint32_t gpr32(const R5900Context &ctx, unsigned reg)
{
    return static_cast<uint32_t>(_mm_extract_epi32(ctx.r[reg], 0));
}

__m128i pair64(uint64_t lo, uint64_t hi)
{
    return _mm_set_epi64x(static_cast<int64_t>(hi), static_cast<int64_t>(lo));
}

// This declaration is intentionally byte-for-byte the generic generated
// helper anchor used by the live function overlay.  The B/C overlay injects
// the caller/input tuple observation immediately after its opening brace;
// Mode A retains this exact uninstrumented helper body.
void sub_0021E698_0x21e698(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
    const uint64_t first = READ64(ADD32(GPR_U32(ctx, 5), 0x20u));
    const uint64_t second = READ64(ADD32(GPR_U32(ctx, 5), 0x50u));
    WRITE64(ADD32(GPR_U32(ctx, 4), 0x60u), first);
    WRITE64(ADD32(GPR_U32(ctx, 4), 0x100u), second);
}

uint32_t targetFor(uint32_t index)
{
    return kWriterTargets[index];
}

uint32_t writerIndex(uint32_t target)
{
    for (uint32_t index = 0u; index < kWriterTargets.size(); ++index)
    {
        if (kWriterTargets[index] == target)
            return index;
    }
    return kTargetCount;
}

uint32_t boundaryIndex(uint32_t target)
{
    for (uint32_t index = 0u; index < kBoundaryTargets.size(); ++index)
    {
        if (kBoundaryTargets[index] == target)
            return index;
    }
    return static_cast<uint32_t>(kBoundaryTargets.size());
}

void prepareEntry(R5900Context &ctx, uint32_t target)
{
    ctx.pc = target;
    setGpr32(ctx, 31u, 0x01000000u + target);
    setGpr32(ctx, 29u, 0x00002000u - (target & 0xf0u));
    if (target == 0x00295e38u)
    {
        setGpr32(ctx, 16u, 0x11223344u);
        setGpr32(ctx, 17u, kControlByteAddress - 0x66u);
        ctx.r[23] = pair64(0x0102030405060708ull, 0u);
    }
    else if (target == 0x0029c460u)
    {
        setGpr32(ctx, 16u, 0x55667788u);
        setGpr32(ctx, 17u, 0x00019000u);
        ctx.r[23] = pair64(0x8877665544332211ull, 0u);
    }
}

// The first real dispatch begins before causal collection.  It arms the
// selector/boundary from inside the already-open runtime DispatchScope, so
// the core can observe the carry-in as dispatch ID 0 without inventing an
// entry record.  The actual preemption policy then selects the first complete
// writer PC and returns through the normal dispatch loop.
void syntheticCarryIn(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    causal::beginSelectorEdge(0x00020000u, 0x00020005u, 700u);
    causal::originBoundary(700u, 900u, 0u);
    prepareEntry(*ctx, targetFor(0u));
    for (uint32_t call = 0u; call < 100u; ++call)
    {
        if (runtime->shouldPreemptGuestExecution())
        {
            check(call == 99u, "carry-in preemption returns only at deterministic threshold");
            return;
        }
    }
    check(false, "carry-in preemption failed to select a dispatcher resume");
}

// This is a registered RecompiledFunction, invoked only through
// PS2Runtime::dispatchLoop.  Its local names deliberately match generated
// code because WRITE8..128 macros require rdram/ctx/runtime at their real
// fast-path hook site.
void syntheticGuestTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t target = ctx->pc;
    const uint32_t index = writerIndex(target);
    check(index < kTargetCount, "dispatch target is a registered writer");
    if (index >= kTargetCount)
    {
        ctx->pc = 0u;
        return;
    }
    check(executedCount < executedTargets.size(), "fixed target ledger does not overflow");
    if (executedCount < executedTargets.size())
        executedTargets[executedCount++] = target;

    const uint32_t address = kSourceStart + index * kSegmentStride;

    if (index == 0u)
    {
        // Exercise source-input writes only through real generated/HLE paths
        // after the carry-in has armed collection.  The first macro overlaps
        // only four bytes of slot 0; the HLE copy spans both slots; the Store
        // in the intervening gap is an explicit non-witness control.  Final
        // macro/Store writes re-establish the exact two helper input words.
        WRITE64(kHelperInput20 - 4u, 0x1122334455667788ull);
        setGpr32(*ctx, 4u, kHelperInput20);
        setGpr32(*ctx, 5u, kCopySource);
        setGpr32(*ctx, 6u, 56u);
        ps2_stubs::memcpy(rdram, ctx, runtime);
        runtime->Store64(rdram, ctx, kHelperInput20 + 8u, 0x8877665544332211ull);
        runtime->Store64(rdram, ctx, kHelperInput20, kHelperFirst);
        WRITE64(kHelperInput50, kHelperSecond);

        // Invoke the same declaration/body that the generated-source overlay
        // instruments in live code.  Its two helper writes are deliberately
        // overwritten by the fixed fixture writes below, so only the bounded
        // caller tuple—not output content—varies by diagnostic mode.
        setGpr32(*ctx, 4u, kSourceStart - 0x60u);
        setGpr32(*ctx, 5u, kHelperInput);
        sub_0021E698_0x21e698(rdram, ctx, runtime);
        helperResult60 = READ64(kSourceStart);
        helperResult100 = READ64(kSourceStart + 0xa0u);
        check(helperResult60 == kHelperFirst, "generated helper preserves first immediate result");
        check(helperResult100 == kHelperSecond, "generated helper preserves second immediate result");
    }

    // The start-edge probe must precede the normal Store8 that defines byte
    // zero, so it verifies overlap without changing the fixed final-window
    // oracle.  The byte-only write is entirely outside and must not trace.
    if (index == 0u)
    {
        WRITE16(kSourceStart - 1u, 0xd0d0u);
        WRITE8(kSourceStart - 1u, 0xe0u);
    }

    if (index == 0u)
    {
        // These invoke the real overlaid LibC and VU copy paths.  Both
        // destinations are overwritten below, leaving the fixed final window
        // oracle independent of stub implementation details.
        setGpr32(*ctx, 4u, address);
        setGpr32(*ctx, 5u, kCopySource);
        setGpr32(*ctx, 6u, 32u);
        ps2_stubs::memcpy(rdram, ctx, runtime);
        setGpr32(*ctx, 4u, address);
        setGpr32(*ctx, 5u, 0x5au);
        setGpr32(*ctx, 6u, 32u);
        ps2_stubs::memset(rdram, ctx, runtime);
        setGpr32(*ctx, 4u, address + 16u);
        setGpr32(*ctx, 5u, kVuCopySource);
        ps2_stubs::sceVu0CopyVector(rdram, ctx, runtime);
    }

    // Real generated fast-path macros: every width writes inside the exact
    // observed source window.  The following runtime Store calls overwrite
    // the same locations via the separate public Store8..128 hook route.
    WRITE8(address, static_cast<uint8_t>(0x10u + index));
    WRITE16(address + 1u, static_cast<uint16_t>(0x2000u + index));
    WRITE32(address + 4u, 0x30000000u + index);
    WRITE64(address + 8u, 0x4000000000000000ull + index);
    WRITE128(address + 16u, pair64(0x5000000000000000ull + index,
                                   0x6000000000000000ull + index));

    runtime->Store8(rdram, ctx, address, static_cast<uint8_t>(0x70u + index));
    runtime->Store16(rdram, ctx, address + 1u, static_cast<uint16_t>(0x8000u + index));
    runtime->Store32(rdram, ctx, address + 4u, 0x90000000u + index);
    runtime->Store64(rdram, ctx, address + 8u, 0xa000000000000000ull + index);
    runtime->Store128(rdram, ctx, address + 16u, pair64(0xb000000000000000ull + index,
                                                         0xc000000000000000ull + index));

    // Exact-window boundary and control cases.  The first and final writes
    // overlap the watched range by only one/eight bytes.  The outside writes
    // test rejection on both macro and public Store routes.
    if (index == kTargetCount - 1u)
    {
        runtime->Store32(rdram, ctx, kSourceEnd - 1u, 0x13572468u);
        runtime->Store128(rdram, ctx, kSourceEnd - 8u,
                          pair64(0x0a0b0c0d0e0f1011ull, 0x1213141516171819ull));
        runtime->Store64(rdram, ctx, kSourceEnd, 0xf0f0f0f0f0f0f0f0ull);

        // Exercise the actual accepted VIF1 normal-DMA source seam.  A full
        // 20-QWC zero/NOP VIF stream occupies the watched 320-byte range; it
        // is then read by a real CHCR start and held-queue flush.  It produces
        // no GIF packet, but its accepted source snapshot is the only proof
        // that Event 111 is attached to the producer seam rather than called
        // directly by this fixture.
        for (uint32_t offset = 0u; offset < kSourceBytes; offset += 16u)
            WRITE128(kSourceStart + offset, _mm_setzero_si128());
        runtime->Store32(rdram, ctx, kVif1Dmac + 0x20u, 20u);
        runtime->Store32(rdram, ctx, kVif1Dmac + 0x10u, kSourceStart);
        runtime->Store32(rdram, ctx, kVif1Dmac, 0x00000101u);
        runtime->memory().flushHeldVif1AtGuestFrameEnd();
    }

    // Call the actual generated back-edge policy only for the statically
    // interesting continuation targets.  Set the real return state first;
    // the 100th predicate is then the genuine generated pattern of selecting
    // a resumable PC and returning to the dispatcher, without a sleep, worker,
    // or guest-timing source.
    if (target == 0x0021df9cu || target == 0x00295e38u)
    {
        setGpr32(*ctx, 31u, 0x0bad0000u + index);
        setGpr32(*ctx, 29u, 0x00001ff0u - index * 0x10u);
        if ((index & 1u) == 0u)
            prepareEntry(*ctx, targetFor(index + 1u));
        else
            prepareEntry(*ctx, kBoundaryTargets[index / 2u]);
        for (uint32_t call = 0u; call < 100u; ++call)
        {
            if (runtime->shouldPreemptGuestExecution())
            {
                check(call == 99u, "fixed 100-call back-edge policy returns only at its threshold");
                return;
            }
        }
        check(false, "fixed back-edge policy failed to return at the deterministic threshold");
        return;
    }

    setGpr32(*ctx, 31u, 0x0bad0000u + index);
    setGpr32(*ctx, 29u, 0x00001ff0u - index * 0x10u);
    if ((index & 1u) == 0u)
        prepareEntry(*ctx, targetFor(index + 1u));
    else
        prepareEntry(*ctx, kBoundaryTargets[index / 2u]);
}

// A boundary target runs after the preceding writer's DispatchScope has
// already captured its exit.  The final boundary uses requestStop with a
// nonzero PC so dispatchLoop exits without its normal PC-zero stderr report.
void syntheticBoundary(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t field = boundaryIndex(ctx->pc);
    check(field < 4u, "dispatch target is a registered boundary");
    if (field >= 4u)
    {
        runtime->requestStop();
        ctx->pc = kTerminalPc;
        return;
    }
    causal::completeField(701u + field, 901u + field, (field + 1u) & 1u);
    if (field + 1u < 4u)
        prepareEntry(*ctx, targetFor((field + 1u) * 2u));
    else
    {
        ctx->pc = kTerminalPc;
        setGpr32(*ctx, 31u, 0x0bad0007u);
        setGpr32(*ctx, 29u, 0x00001f80u);
        runtime->requestStop();
    }
}

std::filesystem::path causalPathFor(const std::filesystem::path &output)
{
    if (!output.empty())
        return output.string() + ".causal.bin";
    return std::filesystem::temp_directory_path() /
           ("rrv-m2-guest-runtime-" + std::to_string(static_cast<unsigned long>(::getpid())) + ".bin");
}

std::string jsonReceipt(uint64_t windowHash, uint64_t outsideHash, uint64_t guestEvents,
                        uint64_t sourceInputEvents, uint64_t vif1DmaStarts,
                        bool requested, bool enabled, const R5900Context &ctx)
{
    std::string targets;
    for (uint32_t index = 0u; index < executedCount; ++index)
    {
        if (index != 0u)
            targets += ", ";
        targets += std::to_string(executedTargets[index]);
    }
    return "{\n"
           "  \"schema_version\": 1,\n"
           "  \"seed\": \"m2-guest-runtime-fixed-1\",\n"
           "  \"executed_target_ids\": [" + targets + "],\n"
           "  \"dispatch_entry_exit_pairs\": " + std::to_string(executedCount) + ",\n"
           "  \"completed_fields\": 4,\n"
           "  \"final_pc\": " + std::to_string(ctx.pc) + ",\n"
           "  \"final_ra\": " + std::to_string(gpr32(ctx, 31u)) + ",\n"
           "  \"final_sp\": " + std::to_string(gpr32(ctx, 29u)) + ",\n"
           "  \"window_fnv1a64\": \"" + std::to_string(windowHash) + "\",\n"
           "  \"outside_control_digest\": \"" + std::to_string(outsideHash) + "\",\n"
           "  \"helper_result_60\": \"" + std::to_string(helperResult60) + "\",\n"
           "  \"helper_result_100\": \"" + std::to_string(helperResult100) + "\",\n"
           "  \"vif1_dma_starts\": " + std::to_string(vif1DmaStarts) + ",\n"
           "  \"guest_provenance_compiled\": " + (kGuestCompiled ? "true" : "false") + ",\n"
           "  \"guest_provenance_requested\": " + (requested ? "true" : "false") + ",\n"
           "  \"guest_provenance_enabled\": " + (enabled ? "true" : "false") + ",\n"
           "  \"guest_provenance_events\": " + std::to_string(guestEvents) + ",\n"
           "  \"source_input_events\": " + std::to_string(sourceInputEvents) + ",\n"
           "  \"guest_event_types\": [96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123],\n"
           "  \"source_input_event_types\": [118, 119, 120, 121, 122, 123]\n"
           "}\n";
}

bool writeDeferred(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
}

uint32_t readLe32(const std::array<uint8_t, 48u> &record, uint32_t offset)
{
    return uint32_t{record[offset]} | (uint32_t{record[offset + 1u]} << 8u) |
           (uint32_t{record[offset + 2u]} << 16u) | (uint32_t{record[offset + 3u]} << 24u);
}

uint64_t countGuestTraceEvents(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return 0u;
    input.seekg(80, std::ios::beg); // fixed causal header; deferred-only parsing.
    std::array<uint8_t, 48u> record{};
    uint64_t count = 0u;
    while (input.read(reinterpret_cast<char *>(record.data()), record.size()))
    {
        const uint32_t type = readLe32(record, 8u);
        count += type >= 96u && type <= 123u ? 1u : 0u;
    }
    return count;
}

uint64_t countSourceInputTraceEvents(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return 0u;
    input.seekg(80, std::ios::beg); // fixed causal header; deferred-only parsing.
    std::array<uint8_t, 48u> record{};
    uint64_t count = 0u;
    while (input.read(reinterpret_cast<char *>(record.data()), record.size()))
    {
        const uint32_t type = readLe32(record, 8u);
        count += type >= 118u && type <= 123u ? 1u : 0u;
    }
    return count;
}

void run(const std::filesystem::path &output)
{
    const char *requestedValue = std::getenv("RRV_M2_GUEST_PROVENANCE");
    const bool requested = requestedValue && std::strcmp(requestedValue, "1") == 0;
    const std::filesystem::path causalPath = causalPathFor(output);

    // All diagnostic state is preallocated/configured before memory creation
    // and before the selector edge.  There is no file output during the four
    // field interval.
    check(causal::resetForTest(true, causalPath.c_str(), 4u), "causal trace preallocation");
    check(dma::resetForTest(true), "existing DMA provenance remains configured");
    check(guest::resetForTest(requested), "guest provenance preallocation");
    const bool guestEnabled = guest::configured();
    check(guestEnabled == (kGuestCompiled && requested), "guest compiled/requested/configured contract");

    PS2Runtime runtime;
    check(runtime.memory().initialize(), "memory-only PS2Runtime initialization");
    if (!runtime.memory().getRDRAM())
        return;
    R5900Context &ctx = runtime.cpu();
    uint8_t *const rdram = runtime.memory().getRDRAM();
    runtime.memory().setGifArbiter(&runtime.gifArbiter());
    std::memset(executedTargets.data(), 0, sizeof(executedTargets));
    executedCount = 0u;
    helperResult60 = 0u;
    helperResult100 = 0u;
    for (uint32_t index = 0u; index < kTargetCount; ++index)
        runtime.registerFunction(targetFor(index), syntheticGuestTarget);
    for (uint32_t index = 0u; index < kBoundaryTargets.size(); ++index)
        runtime.registerFunction(kBoundaryTargets[index], syntheticBoundary);
    runtime.registerFunction(kCarryInTarget, syntheticCarryIn);
    for (uint32_t index = 0u; index < 64u; ++index)
        rdram[kCopySource + index] = static_cast<uint8_t>(0xa0u + index);
    for (uint32_t index = 0u; index < 16u; ++index)
        rdram[kVuCopySource + index] = static_cast<uint8_t>(0xc0u + index);
    rdram[kControlByteAddress] = 0x5du;
    // The generated helper reads two exact little-endian words.  Seed them
    // before the carry-in opens and arms causal collection; the helper output
    // is later overwritten by the fixed VIF source stream.
    storeLe64(rdram + kHelperInput20, kHelperFirst);
    storeLe64(rdram + kHelperInput50, kHelperSecond);

    // Start with an actual runtime dispatch while collection is unarmed.
    // syntheticCarryIn arms the selector/boundary inside that open scope,
    // leaves its provenance ID at zero, and returns by the genuine 100th
    // preemption predicate.  The writer/boundary chain then closes four fields.
    prepareEntry(ctx, kCarryInTarget);
    runtime.dispatchLoop(rdram, &ctx);

    const uint64_t windowHash = fnv1a(rdram + kSourceStart, kSourceBytes);
    std::array<uint8_t, 17u> outside{};
    outside[0] = rdram[kSourceStart - 1u];
    std::memcpy(outside.data() + 1u, rdram + kSourceEnd, 8u);
    const uint64_t outsideHash = fnv1a(outside.data(), static_cast<uint32_t>(outside.size()));
    check(!causal::overflowed(), "bounded causal trace does not overflow");
    check(executedCount == kTargetCount, "all registered targets dispatched through public loop");
    for (uint32_t index = 0u; index < kTargetCount; ++index)
        check(executedTargets[index] == targetFor(index), "executed target IDs stay ordered");
    check(windowHash == kExpectedWindowHash, "exact 320-byte source window final hash");
    check(outsideHash == kExpectedOutsideHash, "outside controls do not become range writes");
    check(runtime.memory().dmaStartCount() == 1u, "one real VIF1 normal DMA reads the watched source range");
    check(ctx.pc == kTerminalPc && gpr32(ctx, 31u) == 0x0bad0007u && gpr32(ctx, 29u) == 0x00001f80u,
          "actual final dispatch exit context");
    // Both causal and JSON exports occur after all four fields have closed.
    check(causal::dumpDeferred(), "causal trace deferred export");
    const uint64_t guestEvents = countGuestTraceEvents(causalPath);
    const uint64_t sourceInputEvents = countSourceInputTraceEvents(causalPath);
    if (guestEnabled)
    {
        check(guestEvents >= 96u, "all ENTRY/EXIT and exact-overlap write events are recorded");
        check(sourceInputEvents >= 25u, "all partial/broad/final source-input writes are recorded");
    }
    else
    {
        check(guestEvents == 0u, "compiled-out/disabled guest diagnostics add no causal records");
        check(sourceInputEvents == 0u, "compiled-out/disabled source-input diagnostics add no causal records");
    }
    if (!output.empty())
        check(writeDeferred(output, jsonReceipt(windowHash, outsideHash, guestEvents, sourceInputEvents,
                                                runtime.memory().dmaStartCount(), requested, guestEnabled, ctx)),
              "JSON receipt deferred export");
}
} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path output;
    if (argc == 3 && std::strcmp(argv[1], "--output") == 0)
        output = argv[2];
    else if (argc != 1)
    {
        std::cerr << "usage: rrv-m2-guest-runtime-tests [--output receipt.json]\n";
        return 2;
    }
    run(output);
    return failures == 0 ? 0 : 1;
}
