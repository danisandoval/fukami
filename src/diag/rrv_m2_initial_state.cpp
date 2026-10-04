#include "rrv_m2_initial_state.h"

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace rrv::m2initial
{
namespace
{
constexpr uint32_t kChunkBytes = 1024u * 1024u;
constexpr char kSequenceDomain[] = "rrv-m2-initial-state-v1";

struct Hasher final
{
    uint64_t value = kFnvOffsetBasis;

    void bytes(const uint8_t *data, uint64_t size) noexcept
    {
        if (!data)
            return;
        for (uint64_t index = 0u; index < size; ++index)
            value = (value ^ data[index]) * kFnvPrime;
    }

    void u8(uint8_t valueIn) noexcept { bytes(&valueIn, 1u); }
    void u16(uint16_t valueIn) noexcept
    {
        u8(static_cast<uint8_t>(valueIn));
        u8(static_cast<uint8_t>(valueIn >> 8u));
    }
    void u32(uint32_t valueIn) noexcept
    {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            u8(static_cast<uint8_t>(valueIn >> shift));
    }
    void u64(uint64_t valueIn) noexcept
    {
        for (uint32_t shift = 0u; shift < 64u; shift += 8u)
            u8(static_cast<uint8_t>(valueIn >> shift));
    }
    void text(const char *valueIn) noexcept
    {
        const uint32_t length = valueIn ? static_cast<uint32_t>(std::strlen(valueIn)) : 0u;
        u32(length);
        bytes(reinterpret_cast<const uint8_t *>(valueIn), length);
    }
};

struct Region final
{
    const char *name;
    uint64_t sizeBytes;
    uint64_t digest;
    std::array<uint64_t, PS2_RAM_SIZE / kChunkBytes> chunks{};
    uint32_t chunkCount = 0u;
};

uint64_t hashBytes(const uint8_t *bytes, uint64_t sizeBytes) noexcept
{
    Hasher hash;
    hash.bytes(bytes, sizeBytes);
    return hash.value;
}

uint64_t hashContext(const R5900Context &ctx, uint64_t &serializedBytes) noexcept
{
    Hasher hash;
    auto u8 = [&](uint8_t value) { hash.u8(value); serializedBytes += 1u; };
    auto u16 = [&](uint16_t value) { hash.u16(value); serializedBytes += 2u; };
    auto u32 = [&](uint32_t value) { hash.u32(value); serializedBytes += 4u; };
    auto u64 = [&](uint64_t value) { hash.u64(value); serializedBytes += 8u; };
    auto f32 = [&](float value) { u32(std::bit_cast<uint32_t>(value)); };
    auto lanes = [&](const __m128i value) {
        alignas(16) std::array<uint32_t, 4u> words{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(words.data()), value);
        for (uint32_t word : words)
            u32(word);
    };
    auto vf = [&](const __m128 value) { lanes(_mm_castps_si128(value)); };

    for (const __m128i value : ctx.r) lanes(value);
    u32(ctx.pc); u64(ctx.insn_count); u64(ctx.hi); u64(ctx.lo); u64(ctx.hi1); u64(ctx.lo1); u32(ctx.sa);

    for (const __m128 value : ctx.vu0_vf) vf(value);
    for (uint16_t value : ctx.vi) u16(value);
    f32(ctx.vu0_q); f32(ctx.vu0_p); f32(ctx.vu0_i); vf(ctx.vu0_r); vf(ctx.vu0_acc);
    u16(ctx.vu0_status); u32(ctx.vu0_mac_flags); u32(ctx.vu0_clip_flags); u32(ctx.vu0_clip_flags2);
    u32(ctx.vu0_cmsar0); u32(ctx.vu0_cmsar1); u32(ctx.vu0_cmsar2); u32(ctx.vu0_cmsar3);
    u32(ctx.vu0_vpu_stat); u32(ctx.vu0_vpu_stat2); u32(ctx.vu0_vpu_stat3); u32(ctx.vu0_vpu_stat4);
    u32(ctx.vu0_tpc); u32(ctx.vu0_tpc2); u32(ctx.vu0_fbrst); u32(ctx.vu0_fbrst2);
    u32(ctx.vu0_fbrst3); u32(ctx.vu0_fbrst4); u32(ctx.vu0_itop); u32(ctx.vu0_top);
    u32(ctx.vu0_info); u32(ctx.vu0_xitop); u32(ctx.vu0_pc);
    for (float value : ctx.vu0_cf) f32(value);

    u32(ctx.cop0_index); u32(ctx.cop0_random); u32(ctx.cop0_entrylo0); u32(ctx.cop0_entrylo1);
    u32(ctx.cop0_context); u32(ctx.cop0_pagemask); u32(ctx.cop0_wired); u32(ctx.cop0_badvaddr);
    u32(ctx.cop0_count); u32(ctx.cop0_entryhi); u32(ctx.cop0_compare); u32(ctx.cop0_status);
    u32(ctx.cop0_cause); u32(ctx.cop0_epc); u32(ctx.cop0_prid); u32(ctx.cop0_config);
    u32(ctx.cop0_badpaddr); u32(ctx.cop0_debug); u32(ctx.cop0_perf); u32(ctx.cop0_taglo);
    u32(ctx.cop0_taghi); u32(ctx.cop0_errorepc); u32(ctx.llbit); u32(ctx.lladdr);
    u8(ctx.in_delay_slot ? 1u : 0u); u32(ctx.branch_pc);
    for (uint32_t value : ctx.cop2_ccr) u32(value);
    for (float value : ctx.f) f32(value);
    u32(ctx.fcr31);
    return hash.value;
}

uint64_t hashVu1State(const VU1State &state, uint64_t &serializedBytes) noexcept
{
    Hasher hash;
    auto u8 = [&](uint8_t value) { hash.u8(value); serializedBytes += 1u; };
    auto u32 = [&](uint32_t value) { hash.u32(value); serializedBytes += 4u; };
    auto f32 = [&](float value) { u32(std::bit_cast<uint32_t>(value)); };
    for (const auto &vector : state.vf)
        for (float value : vector) f32(value);
    for (int32_t value : state.vi) u32(static_cast<uint32_t>(value));
    for (float value : state.acc) f32(value);
    f32(state.q); f32(state.p); f32(state.i);
    u32(state.pc); u32(state.mac); u32(state.clip); u32(state.status);
    u8(state.ebit ? 1u : 0u); u32(state.itop); u32(state.xitop); u32(state.top);
    u8(state.branchPending ? 1u : 0u); u32(state.branchTarget);
    return hash.value;
}

Region memoryRegion(const char *name, const uint8_t *bytes, uint64_t sizeBytes,
                    bool includeChunks) noexcept
{
    Region region{name, sizeBytes, hashBytes(bytes, sizeBytes)};
    if (includeChunks && bytes)
    {
        for (uint64_t offset = 0u; offset < sizeBytes; offset += kChunkBytes)
        {
            const uint64_t chunkSize = (sizeBytes - offset) < kChunkBytes ? (sizeBytes - offset) : kChunkBytes;
            region.chunks[region.chunkCount++] = hashBytes(bytes + offset, chunkSize);
        }
    }
    return region;
}

Region scalarRegion(const char *name, uint64_t sizeBytes, uint64_t digest) noexcept
{
    Region region{name, sizeBytes, digest};
    return region;
}

uint64_t hashRegionSequence(const Region *regions, uint32_t count) noexcept
{
    Hasher hash;
    hash.text(kSequenceDomain);
    hash.u32(kSchemaVersion);
    hash.u32(count);
    for (uint32_t index = 0u; index < count; ++index)
    {
        hash.text(regions[index].name);
        hash.u64(regions[index].sizeBytes);
        hash.u64(regions[index].digest);
    }
    return hash.value;
}

void hex64(std::ostream &out, uint64_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    char text[17]{};
    for (uint32_t index = 0u; index < 16u; ++index)
        text[15u - index] = digits[(value >> (index * 4u)) & 0xfu];
    out << text;
}

bool writeJson(const char *path, const Region *regions, uint32_t count, uint64_t overall) noexcept
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << "{\n  \"schema_version\": " << kSchemaVersion
        << ",\n  \"kind\": \"rrv-m2-initial-state-fingerprint\""
        << ",\n  \"coverage\": \"defined architectural subset, not complete runtime state\""
        << ",\n  \"hash_definition\": \"FNV-1a-64; explicit little-endian fields; rrv-m2-initial-state-v1 region order\""
        << ",\n  \"overall_fnv1a64\": \"";
    hex64(out, overall);
    out << "\",\n  \"regions\": [\n";
    for (uint32_t index = 0u; index < count; ++index)
    {
        const Region &region = regions[index];
        out << "    {\"name\": \"" << region.name << "\", \"bytes\": " << region.sizeBytes
            << ", \"fnv1a64\": \"";
        hex64(out, region.digest);
        out << "\"";
        if (region.chunkCount != 0u)
        {
            out << ", \"chunk_bytes\": " << kChunkBytes << ", \"chunk_fnv1a64\": [";
            for (uint32_t chunk = 0u; chunk < region.chunkCount; ++chunk)
            {
                if (chunk != 0u) out << ", ";
                out << "\""; hex64(out, region.chunks[chunk]); out << "\"";
            }
            out << "]";
        }
        out << "}" << (index + 1u == count ? "\n" : ",\n");
    }
    out << "  ],\n  \"excluded\": [\n"
        << "    \"host pointers, allocation addresses, mutexes, threads, and struct padding\",\n"
        << "    \"GS/GIF/VIF/DMAC/INTC/timer and kernel/HLE state: source-audited, outside this bounded fingerprint\",\n"
        << "    \"VU0/VU1 interpreter/JIT/pipeline-private state; public architectural fields are included\"\n"
        << "  ]\n}\n";
    return out.good();
}
} // namespace

uint64_t fnv1a64ForTesting(const uint8_t *bytes, uint64_t sizeBytes) noexcept
{
    return hashBytes(bytes, sizeBytes);
}

uint64_t fingerprintRegionSequenceForTesting(const ByteRegion *regions, uint32_t count) noexcept
{
    if (!regions && count != 0u)
        return 0u;
    Hasher hash;
    hash.text(kSequenceDomain);
    hash.u32(kSchemaVersion);
    hash.u32(count);
    for (uint32_t index = 0u; index < count; ++index)
    {
        hash.text(regions[index].name);
        hash.u64(regions[index].sizeBytes);
        hash.u64(hashBytes(regions[index].bytes, regions[index].sizeBytes));
    }
    return hash.value;
}

bool observe(PS2Runtime *runtime, R5900Context *context) noexcept
{
    const char *const outputPath = std::getenv("RRV_M2_INITIAL_STATE_FINGERPRINT");
    if (!outputPath || outputPath[0] == '\0')
        return true;
    static std::atomic_flag observed = ATOMIC_FLAG_INIT;
    if (observed.test_and_set(std::memory_order_acq_rel))
        return true;
    if (!runtime || !context)
        return false;

    PS2Memory &memory = runtime->memory();
    const uint8_t *const rdram = memory.getRDRAM();
    const uint8_t *const scratch = memory.getScratchpad();
    const uint8_t *const iopRam = memory.getIOPRAM();
    const uint8_t *const vu0Code = memory.getVU0Code();
    const uint8_t *const vu0Data = memory.getVU0Data();
    const uint8_t *const vu1Code = memory.getVU1Code();
    const uint8_t *const vu1Data = memory.getVU1Data();
    if (!rdram || !scratch || !iopRam || !vu0Code || !vu0Data || !vu1Code || !vu1Data)
    {
        std::fputs("[m2-initial-state] required initialized guest-memory region is unavailable\n", stderr);
        return false;
    }

    uint64_t contextBytes = 0u;
    uint64_t vu1StateBytes = 0u;
    std::array<Region, 9u> regions = {
        memoryRegion("ee_rdram", rdram, PS2_RAM_SIZE, true),
        memoryRegion("ee_scratchpad", scratch, PS2_SCRATCHPAD_SIZE, false),
        memoryRegion("iop_ram", iopRam, 2u * 1024u * 1024u, true),
        memoryRegion("vu0_micro", vu0Code, PS2_VU0_CODE_SIZE, false),
        memoryRegion("vu0_data", vu0Data, PS2_VU0_DATA_SIZE, false),
        memoryRegion("vu1_micro", vu1Code, PS2_VU1_CODE_SIZE, false),
        memoryRegion("vu1_data", vu1Data, PS2_VU1_DATA_SIZE, false),
        scalarRegion("r5900_context_explicit_le", 0u, hashContext(*context, contextBytes)),
        scalarRegion("vu1_state_explicit_le", 0u, hashVu1State(runtime->vu1().state(), vu1StateBytes)),
    };
    regions[7].sizeBytes = contextBytes;
    regions[8].sizeBytes = vu1StateBytes;
    const bool written = writeJson(outputPath, regions.data(), static_cast<uint32_t>(regions.size()),
                                   hashRegionSequence(regions.data(), static_cast<uint32_t>(regions.size())));
    if (!written)
        std::fputs("[m2-initial-state] unable to write requested fingerprint\n", stderr);
    return written;
}
} // namespace rrv::m2initial
