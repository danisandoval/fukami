// Differential test for VU1Interpreter::aotLean() (src/vu-aot/rrv_vu_aot_engine.inc): the lean entry
// must leave exactly what execute() leaves, for both units.
//
// Programs are real instruction sequences: up to three consecutive blocks of the AOT catalogue
// (generated/rr5/vu, compiled into the runtime), placed at pc 0 of an otherwise empty micro memory, so a
// run covers block-to-block continuation, a hand-over to the interpreter where no block matches (the
// zero-filled tail) and, with a small budget, a cut in the middle of a block. Every run starts from
// random state (VF/VI/ACC/Q/P/I/flags, data memory, ITOP/TOP), and the same state goes to
//   A: execute() with the compiled blocks   (isolates the lean entry)
//   I: execute() with RRV_VU_AOT=0          (the plain interpreter, independent of the compiled blocks)
//   L: aotLean()
// After every call the whole VU1State, the data memory and every GIF packet XGKICK produced (bytes, path,
// pc) must be identical. Two calls run back to back on each machine, so state that persists between
// calls is compared as well.
//
// Block analysis (tools/vu-aot/vu_aot_gen.py: flag liveness and operand-clamp elision). A compiled block
// leaves out per-slot steps the generator proved dead inside the block, so EVERY catalogue block is also
// run alone against the interpreter (blockDiff below): same random state, same random flag-visibility ring,
// for exactly the block's slots, at pc 0 and at a random pc. Afterwards VU1State, the four ring entries, the
// ring position, the data memory and the GIF packets must be identical. The state is hostile on purpose:
// Inf, NaN (quiet and signalling), denormals and the largest float are common in every register and in the
// data memory, because an operand clamp that was wrongly left out only shows on an exponent of 255. VF0 is
// random at entry too: a block may not assume it.
//
// `--bench` also times execute() against aotLean() per call on short real programs that end in an E bit
// (the shape of an MSCAL), alternating the two to cancel drift, and `--bench-blocks` times every
// catalogue block bare (ns per slot over the whole catalogue: the number the block analysis changes).
//
// The test reads the instruction words through the catalogue the runtime already carries; it holds no
// game data of its own. Without a catalogue (kVuAotBlockCount == 0) it reports SKIP and passes.
//
// The runtime source is included whole, so the test sees the same anonymous-namespace catalogue and
// private members; three external symbols are stubbed below.

// Every standard header the runtime source includes comes first, so the access hack below touches only
// the project's own classes (the test reads the interpreter's private slot counters).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <queue>
#include <deque>
#include <condition_variable>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <utility>
#include <tuple>
#include <string_view>
#include <cstdint>
#include <cstdarg>
#include <random>
#include <numeric>
#include <list>
#include <iomanip>
#include <cinttypes>
#include <cassert>
#include <cerrno>
#include <ctime>

#define private public
#include "lib/ps2_vu1.cpp"
#undef private

// ---- the three symbols ps2_vu1.cpp needs from the rest of the runtime --------------------------------
namespace
{
std::vector<uint8_t> *g_capture = nullptr;
void capturePacket(GifPathId path, const uint8_t *data, uint32_t size, uint32_t pc)
{
    if (!g_capture)
        return;
    const uint32_t header[3] = {static_cast<uint32_t>(path), size, pc};
    const auto *h = reinterpret_cast<const uint8_t *>(header);
    g_capture->insert(g_capture->end(), h, h + sizeof(header));
    g_capture->insert(g_capture->end(), data, data + size);
}
} // namespace

void GS::processGIFPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, uint32_t vu1Pc,
                          const GifPacketDiagnostic *)
{
    capturePacket(pathId, data, sizeBytes, vu1Pc);
}
void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool, bool,
                                uint32_t vu1Pc, const GifPacketDiagnostic *)
{
    capturePacket(pathId, data, sizeBytes, vu1Pc);
}
uint32_t PS2Memory::read32(uint32_t) { return 0; }

namespace
{
int g_failures = 0;
uint64_t g_checks = 0;

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint64_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

float randomFloat()
{
    switch (rnd() % 6)
    {
    case 0: // an arbitrary bit pattern (denormals, huge, NaN, Inf: the VU clamps them)
    {
        const uint32_t bits = static_cast<uint32_t>(rnd());
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
    case 1: return 0.0f;
    case 2: return static_cast<float>(static_cast<int>(rnd() % 4001) - 2000) / 16.0f;
    default: return static_cast<float>(static_cast<int>(rnd() % 2000001) - 1000000) / 1024.0f;
    }
}

VU1State randomState()
{
    VU1State s;
    std::memset(&s, 0, sizeof s);
    for (auto &row : s.vf)
        for (float &lane : row)
            lane = randomFloat();
    for (int32_t &r : s.vi)
        r = static_cast<int32_t>(rnd() & 0x3FF); // small: they are loop counters and data addresses
    for (float &lane : s.acc)
        lane = randomFloat();
    s.q = randomFloat();
    s.p = randomFloat();
    s.i = randomFloat();
    s.mac = static_cast<uint32_t>(rnd() & 0xFFFF);
    s.clip = static_cast<uint32_t>(rnd() & 0xFFFFFF);
    s.status = static_cast<uint32_t>(rnd() & 0xFFF);
    s.xitop = static_cast<uint32_t>(rnd() & 0x3FF);
    // State a call must reset: a previous program left an E bit or a pending branch behind.
    s.ebit = (rnd() & 1u) != 0u;
    s.branchPending = (rnd() & 1u) != 0u;
    s.branchTarget = static_cast<uint32_t>(rnd() & 0x3FF8);
    return s;
}

struct Machine
{
    VU1Interpreter vu;
    std::vector<uint8_t> code, data;
    std::vector<uint8_t> packets;
    bool vu0;
    Machine(bool isVu0, const std::vector<uint8_t> &c, const std::vector<uint8_t> &d, const VU1State &s)
        : code(c), data(d), vu0(isVu0)
    {
        vu.setIsVu0(isVu0);
        vu.state() = s;
    }
};

alignas(16) unsigned char g_gsStorage[sizeof(GS)]; // never constructed: the stub does not read it
GS &gs() { return *reinterpret_cast<GS *>(g_gsStorage); }

enum class Path { Execute, Lean };

// Returns whether the lean entry took the call (false: it declined and execute() ran).
bool run(Machine &m, Path path, uint32_t startPC, uint32_t itop, uint32_t top, uint32_t maxCycles)
{
    g_capture = &m.packets;
    bool leanTook = false;
    if (path == Path::Lean)
        leanTook = m.vu.aotLean(m.code.data(), static_cast<uint32_t>(m.code.size()), m.data.data(),
                                static_cast<uint32_t>(m.data.size()), gs(), nullptr, startPC, itop,
                                maxCycles, top);
    if (!leanTook)
        m.vu.execute(m.code.data(), static_cast<uint32_t>(m.code.size()), m.data.data(),
                     static_cast<uint32_t>(m.data.size()), gs(), nullptr, startPC, itop, maxCycles, top);
    g_capture = nullptr;
    return leanTook;
}

void expect(bool ok, const char *what, const char *who, unsigned unit, size_t block, uint32_t budget, int call)
{
    ++g_checks;
    if (ok)
        return;
    if (g_failures < 20)
        std::fprintf(stderr, "FAIL: %s (%s, VU%u, catalogue block %zu, budget %u, call %d)\n", what, who, unit,
                     block, budget, call);
    ++g_failures;
}

// Which fields of two states differ (the first few failures only).
void describe(const VU1State &a, const VU1State &b)
{
    if (g_failures >= 20)
        return;
    auto bits = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        return u;
    };
    for (int r = 0; r < 32; ++r)
        if (std::memcmp(a.vf[r], b.vf[r], 16) != 0)
            std::fprintf(stderr, "      vf%02d ref=%08x,%08x,%08x,%08x got=%08x,%08x,%08x,%08x\n", r, bits(a.vf[r][0]),
                         bits(a.vf[r][1]), bits(a.vf[r][2]), bits(a.vf[r][3]), bits(b.vf[r][0]), bits(b.vf[r][1]),
                         bits(b.vf[r][2]), bits(b.vf[r][3]));
    for (int r = 0; r < 16; ++r)
        if (a.vi[r] != b.vi[r])
            std::fprintf(stderr, "      vi%02d ref=%08x got=%08x\n", r, (uint32_t)a.vi[r], (uint32_t)b.vi[r]);
    if (std::memcmp(a.acc, b.acc, 16) != 0)
        std::fprintf(stderr, "      acc ref=%08x,%08x,%08x,%08x got=%08x,%08x,%08x,%08x\n", bits(a.acc[0]), bits(a.acc[1]),
                     bits(a.acc[2]), bits(a.acc[3]), bits(b.acc[0]), bits(b.acc[1]), bits(b.acc[2]), bits(b.acc[3]));
    if (bits(a.q) != bits(b.q) || bits(a.p) != bits(b.p) || bits(a.i) != bits(b.i))
        std::fprintf(stderr, "      q/p/i ref=%08x/%08x/%08x got=%08x/%08x/%08x\n", bits(a.q), bits(a.p), bits(a.i),
                     bits(b.q), bits(b.p), bits(b.i));
    if (a.pc != b.pc || a.mac != b.mac || a.clip != b.clip || a.status != b.status)
        std::fprintf(stderr, "      pc/mac/clip/status ref=%04x/%04x/%06x/%03x got=%04x/%04x/%06x/%03x\n", a.pc, a.mac,
                     a.clip, a.status, b.pc, b.mac, b.clip, b.status);
    if (a.ebit != b.ebit || a.branchPending != b.branchPending || a.branchTarget != b.branchTarget ||
        a.itop != b.itop || a.xitop != b.xitop || a.top != b.top)
        std::fprintf(stderr, "      ebit/branch/target/itop/xitop/top ref=%d/%d/%04x/%x/%x/%x got=%d/%d/%04x/%x/%x/%x\n",
                     a.ebit, a.branchPending, a.branchTarget, a.itop, a.xitop, a.top, b.ebit, b.branchPending,
                     b.branchTarget, b.itop, b.xitop, b.top);
}

void compare(const Machine &ref, const Machine &lean, const char *who, unsigned unit, size_t block,
             uint32_t budget, int call)
{
    if (std::memcmp(&ref.vu.state(), &lean.vu.state(), sizeof(VU1State)) != 0)
        describe(ref.vu.state(), lean.vu.state());
    expect(std::memcmp(&ref.vu.state(), &lean.vu.state(), sizeof(VU1State)) == 0, "VU1State differs", who,
           unit, block, budget, call);
    expect(ref.data == lean.data, "data memory differs", who, unit, block, budget, call);
    expect(ref.packets == lean.packets, "XGKICK packets differ", who, unit, block, budget, call);
}

// Every NaN the block test puts into a register or into the data memory is one of two bit patterns: a
// negative quiet NaN and a negative signalling NaN. When an op that does not clamp its operands (OPMULA,
// the EFU sums) meets two NaNs, the host returns one of them, and which one depends on the operand order,
// which the compiler chooses separately for the interpreter's copy of the op and for each compiled block's
// (observed on x86-64: one block of 63,888 runs differed only in the sign of such a result). With one sign
// and two payloads the stored result is the same for either order.
void canonicalNaNs(void *p, size_t words)
{
    for (size_t w = 0; w < words; ++w)
    {
        uint32_t u;
        std::memcpy(&u, static_cast<uint8_t *>(p) + w * 4, 4);
        if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0u)
        {
            u = (u & 0x00400000u) ? 0xFFC00000u : 0xFFA00000u;
            std::memcpy(static_cast<uint8_t *>(p) + w * 4, &u, 4);
        }
    }
}

// A float for the block test: one in two is a value the VU treats specially.
float nastyFloat()
{
    auto bits = [](uint32_t b) {
        float f;
        std::memcpy(&f, &b, sizeof f);
        return f;
    };
    const uint32_t sign = (rnd() & 1u) ? 0x80000000u : 0u;
    switch (rnd() % 12)
    {
    case 0: return bits(sign | 0x7F800000u);                                             // Inf
    case 1: return bits(sign | 0x7F800000u | (static_cast<uint32_t>(rnd()) & 0x007FFFFFu) | 1u); // NaN
    case 2: return bits(sign | (static_cast<uint32_t>(rnd()) & 0x007FFFFFu));            // denormal or zero
    case 3: return bits(sign | 0x7F7FFFFFu);                                             // largest
    case 4: return bits(sign);                                                           // zero
    case 5: return bits(static_cast<uint32_t>(rnd()));                                   // anything
    default: return randomFloat();
    }
}

VU1State nastyState()
{
    VU1State s = randomState();
    for (auto &row : s.vf)
        for (float &lane : row)
            lane = nastyFloat();
    for (float &lane : s.acc)
        lane = nastyFloat();
    s.q = nastyFloat();
    s.p = nastyFloat();
    s.i = nastyFloat();
    s.ebit = false;
    s.branchPending = false;
    return s;
}

// Every catalogue block alone, compiled against interpreted, for exactly its slots (see the file comment).
void blockDiff(bool isVu0, uint64_t &blockRuns, uint64_t &blockSlots)
{
    const unsigned unit = isVu0 ? 2u : 1u;
    const uint32_t codeSize = isVu0 ? PS2_VU0_CODE_SIZE : PS2_VU1_CODE_SIZE;
    const uint32_t dataSize = isVu0 ? PS2_VU0_DATA_SIZE : PS2_VU1_DATA_SIZE;
    const std::vector<uint8_t> blankCode(codeSize, 0), blankData(dataSize, 0);
    Machine interp(isVu0, blankCode, blankData, nastyState());
    Machine compiled(isVu0, blankCode, blankData, nastyState());
    compiled.vu.aotEnsure();
    interp.vu.m_aotForceInterp = true; // run() interprets every slot
    constexpr int kTrials = 24;
    for (size_t b = 0; b < kVuAotBlockCount; ++b)
    {
        const auto &blk = kVuAotBlocks[b];
        if (blk.slots * 8u > codeSize)
            continue;
        for (int trial = 0; trial < kTrials; ++trial)
        {
            // Position independent: pc 0, then anywhere the block fits.
            const uint32_t room = (codeSize - blk.slots * 8u) / 8u;
            const uint32_t pc0 = trial == 0 || room == 0u ? 0u : static_cast<uint32_t>(rnd() % (room + 1u)) * 8u;
            std::vector<uint8_t> code(codeSize, 0), data(dataSize);
            std::memcpy(code.data() + pc0, blk.raw, blk.slots * 8u);
            if (trial % 3 == 0)
                for (uint8_t &byte : data)
                    byte = static_cast<uint8_t>(rnd());
            else
                for (uint32_t o = 0; o + 4u <= dataSize; o += 4u)
                {
                    const float f = nastyFloat();
                    std::memcpy(data.data() + o, &f, 4);
                }
            if (trial % 2 == 1)
                for (uint32_t q = 0; q + 32u <= dataSize; q += 32u) // valid GIF tags: XGKICK submits packets
                {
                    const uint64_t lo = 1ull | (1ull << 15) | (1ull << 60), hi = 0xEull;
                    std::memcpy(data.data() + q, &lo, 8);
                    std::memcpy(data.data() + q + 8, &hi, 8);
                }
            VU1State init = nastyState();
            init.pc = pc0;
            canonicalNaNs(init.vf, 128);
            canonicalNaNs(init.acc, 4);
            canonicalNaNs(&init.q, 1);
            canonicalNaNs(&init.p, 1);
            canonicalNaNs(&init.i, 1);
            canonicalNaNs(data.data(), dataSize / 4);
            VU1Interpreter::FlagSnapshot ring[4];
            for (auto &entry : ring)
            {
                entry.mac = static_cast<uint32_t>(rnd() & 0xFFFF);
                entry.clip = static_cast<uint32_t>(rnd() & 0xFFFFFF);
                entry.status = static_cast<uint32_t>(rnd() & 0xFFF);
            }
            const int ringPos = static_cast<int>(rnd() & 3u);
            for (Machine *m : {&interp, &compiled})
            {
                m->code = code;
                m->data = data;
                m->vu.state() = init;
                std::memcpy(m->vu.m_flagRing, ring, sizeof ring);
                m->vu.m_flagRingPos = ringPos;
                m->packets.clear();
                rrvVuCodeChanged(isVu0 ? 0 : 1);
            }
            {
                const ScopedVuRounding rounding(true); // execute() sets it around run()
                g_capture = &interp.packets;
                interp.vu.run(interp.code.data(), codeSize, interp.data.data(), dataSize, gs(), nullptr, blk.slots);
                g_capture = &compiled.packets;
                compiled.vu.m_curData = compiled.data.data();
                compiled.vu.m_curDataSize = dataSize;
                compiled.vu.m_curGs = &gs();
                compiled.vu.m_curMemory = nullptr;
                compiled.vu.m_aotCodeSize = codeSize;
                blk.fn(compiled.vu, pc0);
                g_capture = nullptr;
            }
            ++blockRuns;
            blockSlots += blk.slots;
            // RRV_VU_BLOCK_DEBUG=1: for the first block that differs, what the interpreter changes slot by slot.
            static bool s_traced = false;
            if (!s_traced && std::getenv("RRV_VU_BLOCK_DEBUG") &&
                std::memcmp(&interp.vu.state(), &compiled.vu.state(), sizeof(VU1State)) != 0)
            {
                s_traced = true;
                std::fprintf(stderr, "trace: VU%u block %zu trial %d pc0=%04x, %u slots\n", isVu0 ? 0u : 1u, b, trial, pc0,
                             blk.slots);
                Machine step(isVu0, code, data, init);
                step.vu.m_aotForceInterp = true;
                VU1State prev = init;
                for (uint32_t k = 1; k <= blk.slots; ++k)
                {
                    step.code = code;
                    step.data = data;
                    step.vu.state() = init;
                    std::memcpy(step.vu.m_flagRing, ring, sizeof ring);
                    step.vu.m_flagRingPos = ringPos;
                    const ScopedVuRounding rounding(true);
                    step.vu.run(step.code.data(), codeSize, step.data.data(), dataSize, gs(), nullptr, k);
                    std::fprintf(stderr, "  slot %2u  lower=%08x upper=%08x\n", k - 1, (uint32_t)blk.raw[k - 1],
                                 (uint32_t)(blk.raw[k - 1] >> 32));
                    describe(prev, step.vu.state());
                    prev = step.vu.state();
                }
                std::fprintf(stderr, "  interpreter (ref) against compiled (got):\n");
                describe(interp.vu.state(), compiled.vu.state());
            }
            compare(interp, compiled, "block: interpreter vs compiled", unit, b, blk.slots, trial);
            expect(std::memcmp(interp.vu.m_flagRing, compiled.vu.m_flagRing, sizeof ring) == 0,
                   "flag-visibility ring differs", "block: interpreter vs compiled", unit, b, blk.slots, trial);
            expect(interp.vu.m_flagRingPos == compiled.vu.m_flagRingPos, "ring position differs",
                   "block: interpreter vs compiled", unit, b, blk.slots, trial);
        }
    }
}

} // namespace

#include <chrono>

// Every catalogue block called bare, from the state the previous one left: ns per slot over the catalogue.
void benchBlocks()
{
    const uint32_t codeSize = PS2_VU1_CODE_SIZE, dataSize = PS2_VU1_DATA_SIZE;
    std::vector<uint8_t> code(codeSize, 0), data(dataSize, 0);
    g_rng = 0x1234567887654321ull; // the same state in every binary that runs this
    Machine m(false, code, data, randomState());
    m.vu.aotEnsure();
    m.vu.m_curData = m.data.data();
    m.vu.m_curDataSize = dataSize;
    m.vu.m_curGs = &gs();
    m.vu.m_curMemory = nullptr;
    m.vu.m_aotCodeSize = codeSize;
    uint64_t slots = 0;
    for (size_t b = 0; b < kVuAotBlockCount; ++b)
        slots += kVuAotBlocks[b].slots;
    const VU1State init = m.vu.state();
    constexpr int kPasses = 40, kRounds = 7;
    double best = 1e30;
    const ScopedVuRounding rounding(true);
    for (int round = 0; round < kRounds; ++round)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int pass = 0; pass < kPasses; ++pass)
        {
            m.vu.state() = init;
            for (size_t b = 0; b < kVuAotBlockCount; ++b)
            {
                m.vu.m_state.ebit = false;
                m.vu.m_state.branchPending = false;
                kVuAotBlocks[b].fn(m.vu, 0);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / (double(kPasses) * double(slots)));
    }
    std::printf("bench blocks: %zu blocks, %llu slots, %.3f ns per slot (best of %d rounds)\n", kVuAotBlockCount,
                (unsigned long long)slots, best, kRounds);
}

// Time execute() and aotLean() per call on a program of one catalogue block that ends the microprogram.
void bench(bool isVu0)
{
    const uint32_t codeSize = isVu0 ? PS2_VU0_CODE_SIZE : PS2_VU1_CODE_SIZE;
    const uint32_t dataSize = isVu0 ? PS2_VU0_DATA_SIZE : PS2_VU1_DATA_SIZE;
    const uint32_t budget = isVu0 ? 4096u : 65536u;
    // Programs of different lengths: the nearest stop blocks to these slot counts (the catalogue's stop
    // blocks are short, so the larger targets resolve to the longest ones; each length is timed once).
    uint32_t last = 0;
    for (const uint32_t wanted : {8u, 24u, 64u, 160u})
    {
        size_t best = kVuAotBlockCount;
        for (size_t i = 0; i < kVuAotBlockCount; ++i)
            if (kVuAotBlocks[i].stop &&
                (best == kVuAotBlockCount ||
                 std::abs(int(kVuAotBlocks[i].slots) - int(wanted)) < std::abs(int(kVuAotBlocks[best].slots) - int(wanted))))
                best = i;
        if (best == kVuAotBlockCount || kVuAotBlocks[best].slots == last)
            continue;
        last = kVuAotBlocks[best].slots;
        const auto &blk = kVuAotBlocks[best];
        std::vector<uint8_t> code(codeSize, 0), data(dataSize, 0);
        std::memcpy(code.data(), blk.raw, blk.slots * 8u);
        const VU1State init = randomState();
        Machine full(isVu0, code, data, init), lean(isVu0, code, data, init);
        rrvVuCodeChanged(isVu0 ? 0 : 1);
        constexpr int kCalls = 200000, kRounds = 5;
        double bestFull = 1e30, bestLean = 1e30;
        for (int round = 0; round < kRounds; ++round)
        {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kCalls; ++i)
                run(full, Path::Execute, 0, 1, 2, budget);
            auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < kCalls; ++i)
                run(lean, Path::Lean, 0, 1, 2, budget);
            auto t2 = std::chrono::steady_clock::now();
            bestFull = std::min(bestFull, std::chrono::duration<double, std::nano>(t1 - t0).count() / kCalls);
            bestLean = std::min(bestLean, std::chrono::duration<double, std::nano>(t2 - t1).count() / kCalls);
        }
        std::printf("bench VU%d, program of %3u slots: execute() %7.1f ns/call, aotLean() %7.1f ns/call  (%.1f ns saved, %.2fx)\n",
                    isVu0 ? 0 : 1, blk.slots, bestFull, bestLean, bestFull - bestLean, bestFull / bestLean);
    }
}

// Per-block dispatch cost: the same stop block called bare (b->fn) and through aotRun() (loop checks, aotLookup,
// budget test, call), alternating. The difference is what each block pays before its first slot runs.
void benchDispatch()
{
    const uint32_t codeSize = PS2_VU1_CODE_SIZE, dataSize = PS2_VU1_DATA_SIZE;
    uint32_t last = 0;
    for (const uint32_t wanted : {8u, 16u, 24u})
    {
        size_t best = kVuAotBlockCount;
        for (size_t i = 0; i < kVuAotBlockCount; ++i)
            if (kVuAotBlocks[i].stop &&
                (best == kVuAotBlockCount ||
                 std::abs(int(kVuAotBlocks[i].slots) - int(wanted)) < std::abs(int(kVuAotBlocks[best].slots) - int(wanted))))
                best = i;
        if (best == kVuAotBlockCount || kVuAotBlocks[best].slots == last)
            continue;
        last = kVuAotBlocks[best].slots;
        const auto &blk = kVuAotBlocks[best];
        std::vector<uint8_t> code(codeSize, 0), data(dataSize, 0);
        std::memcpy(code.data(), blk.raw, blk.slots * 8u);
        Machine m(false, code, data, randomState());
        rrvVuCodeChanged(1);
        m.vu.aotEnsure(); // resolves the switches and prints once
        m.vu.m_curData = m.data.data();
        m.vu.m_curDataSize = dataSize;
        m.vu.m_curGs = &gs();
        m.vu.m_curMemory = nullptr;
        m.vu.m_aotCodeSize = codeSize;
        constexpr int kCalls = 400000, kRounds = 7;
        double bare = 1e30, viaRun = 1e30;
        for (int round = 0; round < kRounds; ++round)
        {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kCalls; ++i)
            {
                m.vu.m_state.pc = 0;
                m.vu.m_state.ebit = false;
                m.vu.m_state.branchPending = false;
                blk.fn(m.vu, 0);
            }
            auto t1 = std::chrono::steady_clock::now();
            uint32_t ran = 0;
            for (int i = 0; i < kCalls; ++i)
            {
                m.vu.m_state.pc = 0;
                m.vu.m_state.ebit = false;
                m.vu.m_state.branchPending = false;
                m.vu.aotRun(m.code.data(), codeSize, 65536, ran);
            }
            auto t2 = std::chrono::steady_clock::now();
            bare = std::min(bare, std::chrono::duration<double, std::nano>(t1 - t0).count() / kCalls);
            viaRun = std::min(viaRun, std::chrono::duration<double, std::nano>(t2 - t1).count() / kCalls);
        }
        std::printf("dispatch, block of %3u slots: bare call %6.1f ns, through aotRun() %6.1f ns  -> %.1f ns per block for lookup + loop\n",
                    blk.slots, bare, viaRun, viaRun - bare);
    }
}

int main(int argc, char **argv)
{
    if (kVuAotBlockCount == 0)
    {
        std::puts("SKIP: this build has no AOT catalogue");
        return 0;
    }
    const bool wantBench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--bench-blocks") == 0)
    {
        benchBlocks();
        return 0;
    }
    uint64_t leanDeclined = 0, runs = 0, packetBytes = 0, cut = 0;
    uint64_t compiledSlots = 0, interpretedSlots = 0;

    uint64_t blockRuns = 0, blockSlots = 0;
    blockDiff(false, blockRuns, blockSlots);
    blockDiff(true, blockRuns, blockSlots);
    std::printf("vu block diff: %llu block runs (%zu blocks, both units), %llu slots, %llu comparisons; %d failures\n",
                (unsigned long long)blockRuns, kVuAotBlockCount, (unsigned long long)blockSlots,
                (unsigned long long)g_checks, g_failures);

    // Every `stride`-th catalogue block starts a program (the two units start at different offsets).
    const size_t stride = 5;
    for (unsigned unit = 1; unit <= 2; ++unit) // 1 = VU1, 2 = VU0
    {
        const bool isVu0 = unit == 2;
        const uint32_t codeSize = isVu0 ? PS2_VU0_CODE_SIZE : PS2_VU1_CODE_SIZE;
        const uint32_t dataSize = isVu0 ? PS2_VU0_DATA_SIZE : PS2_VU1_DATA_SIZE;
        const uint32_t fullBudget = isVu0 ? 4096u : 65536u;
        const std::vector<uint8_t> blankCode(codeSize, 0), blankData(dataSize, 0);
        const VU1State blankState = randomState();

        // I is the plain interpreter: RRV_VU_AOT is read once, at an instance's first call, so make that call
        // (a zero-slot run) with it off and restore it before A and L make theirs.
        setenv("RRV_VU_AOT", "0", 1);
        Machine interp(isVu0, blankCode, blankData, blankState);
        run(interp, Path::Execute, 0, 0, 0, 0);
        unsetenv("RRV_VU_AOT");
        Machine aot(isVu0, blankCode, blankData, blankState);
        Machine lean(isVu0, blankCode, blankData, blankState);

        // Catalogue blocks that leave a branch pending (their last slot is the branch, its delay slot is
        // not in the block): the lean entry must hand over to run() WITHOUT clearing it (m_aotLeanResume).
        // Found by calling each block on random state; every one of them also starts a program below.
        std::vector<size_t> pendingBranch;
        {
            std::vector<uint8_t> noCode(codeSize, 0), noData(dataSize, 0);
            Machine probe(isVu0, noCode, noData, randomState());
            probe.vu.aotEnsure();
            probe.vu.m_curData = probe.data.data();
            probe.vu.m_curDataSize = dataSize;
            probe.vu.m_curGs = &gs();
            probe.vu.m_curMemory = nullptr;
            probe.vu.m_aotCodeSize = codeSize;
            for (size_t i = 0; i < kVuAotBlockCount; ++i)
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    probe.vu.state() = randomState();
                    probe.vu.m_state.ebit = false;
                    probe.vu.m_state.branchPending = false;
                    kVuAotBlocks[i].fn(probe.vu, 0);
                    if (probe.vu.m_state.branchPending)
                    {
                        pendingBranch.push_back(i);
                        break;
                    }
                }
        }
        std::printf("VU%u: %zu of %zu catalogue blocks can leave a branch pending\n", isVu0 ? 0u : 1u,
                    pendingBranch.size(), kVuAotBlockCount);
        std::vector<size_t> starts;
        for (size_t b = unit - 1; b < kVuAotBlockCount; b += stride)
            starts.push_back(b);
        starts.insert(starts.end(), pendingBranch.begin(), pendingBranch.end());
        for (const size_t b : starts)
        {
            const uint64_t slots0 = lean.vu.m_aotSlots, interp0 = lean.vu.m_interpSlots;
            struct Tally { Machine &m; uint64_t &c, &i; uint64_t s0, i0; ~Tally() { c += m.vu.m_aotSlots - s0; i += m.vu.m_interpSlots - i0; } }
                tally{lean, compiledSlots, interpretedSlots, slots0, interp0};
            // Up to three consecutive catalogue blocks from b, at pc 0.
            std::vector<uint8_t> code(codeSize, 0);
            uint32_t at = 0;
            for (size_t k = 0; k < 3 && b + k < kVuAotBlockCount; ++k)
            {
                const auto &blk = kVuAotBlocks[b + k];
                if (at + blk.slots * 8u > codeSize)
                    break;
                std::memcpy(code.data() + at, blk.raw, blk.slots * 8u);
                at += blk.slots * 8u;
            }
            if (at == 0)
                continue;
            for (const uint32_t budget : {fullBudget, 7u, 33u, 250u})
            {
                std::vector<uint8_t> data(dataSize);
                for (uint8_t &byte : data)
                    byte = static_cast<uint8_t>(rnd());
                if ((b / stride) % 2 == 0)
                {
                    // Every other program: valid one-register PACKED GIF tags (NLOOP 1, EOP, A+D) at even
                    // quadwords, each followed by a random register write, so XGKICK at a random VI address
                    // often submits a real packet instead of being dropped as malformed.
                    for (uint32_t q = 0; q + 32u <= dataSize; q += 32u)
                    {
                        const uint64_t lo = 1ull | (1ull << 15) | (1ull << 60), hi = 0xEull;
                        std::memcpy(data.data() + q, &lo, 8);
                        std::memcpy(data.data() + q + 8, &hi, 8);
                    }
                }
                const VU1State init = randomState();
                for (Machine *m : {&interp, &aot, &lean})
                {
                    m->code = code;
                    m->data = data;
                    m->vu.state() = init;
                    m->packets.clear();
                    rrvVuCodeChanged(isVu0 ? 0 : 1); // what every real micro-memory writer does
                }
                for (int call = 0; call < 2; ++call)
                {
                    const uint32_t itop = static_cast<uint32_t>(rnd() & 0x3FF);
                    const uint32_t top = static_cast<uint32_t>(rnd() & 0x3FF);
                    run(interp, Path::Execute, 0, itop, top, budget);
                    run(aot, Path::Execute, 0, itop, top, budget);
                    if (!run(lean, Path::Lean, 0, itop, top, budget))
                        ++leanDeclined;
                    ++runs;
                    if (budget < 250u)
                        ++cut;
                    compare(aot, lean, "execute() vs aotLean()", unit, b, budget, call);
                    compare(interp, lean, "interpreter vs aotLean()", unit, b, budget, call);
                }
                packetBytes += lean.packets.size();
            }
        }
    }
    std::printf("vu lean diff: %llu runs (%llu with a tiny budget), %llu comparisons, %llu GIF packet bytes compared, "
                "lean entry declined %llu times; lean machine: %llu slots in compiled blocks, %llu interpreted "
                "after hand-over; %d failures\n",
                (unsigned long long)runs, (unsigned long long)cut, (unsigned long long)g_checks,
                (unsigned long long)packetBytes, (unsigned long long)leanDeclined,
                (unsigned long long)compiledSlots, (unsigned long long)interpretedSlots, g_failures);
    if (leanDeclined != 0)
    {
        std::fprintf(stderr, "FAIL: the lean entry declined %llu calls; the comparison would be vacuous\n",
                     (unsigned long long)leanDeclined);
        ++g_failures;
    }
    if (wantBench && g_failures == 0)
    {
        bench(false);
        bench(true);
        benchDispatch();
    }
    return g_failures == 0 ? 0 : 1;
}
