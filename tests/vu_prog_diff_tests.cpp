// Differential test for the whole-program native VU microprograms (RRV_VU_PROG,
// src/vu-aot/rrv_vu_prog_engine.inc, generated/rr5/vu/rrv_vu_aot_programs.inc): a program must leave
// exactly what the interpreter leaves.
//
// Every program of the catalogue is run from its entry on two machines that start from the same state:
//   I: execute() with the compiled code off (the plain interpreter),
//   P: aotLean() with programs on (the program, then the compiled blocks for whatever it hands over).
// Micro memory holds the program's image at address 0 or, to check that programs are position independent,
// at a random address. The state is hostile on purpose (the block test's: Inf, NaN, denormals and the
// largest float in every register and in the data memory), because an operand clamp that the generator's
// analysis wrongly left out only shows on an exponent of 255. The data memory is one of four kinds, so
// that the loops (their counters come from the data memory) end early, late or not at all: random bytes,
// hostile floats, rows of small integers mixed with hostile floats, and valid GIF tags. The slot budget is
// the product's (65536) or a small random one, which makes the program hand over in the middle. Two calls
// run back to back on each machine. After every call the whole VU1State, the data memory and every GIF
// packet XGKICK produced (bytes, path, pc) must be identical.
//
// The test also counts which segments (labels) of each program ran, and fails if a program never ran or
// the overall coverage is below the floor: a test that only ever takes the first exit proves nothing. A
// program with many segments gets proportionally more trials (a wrong clamp on one path of the clipping
// programs shows about once in 100,000 checks).
//
// Synthetic slots (no game data). The catalogue only uses the ops and the flag timings the game's
// microcode uses, so the engine is also run slot by slot against the interpreter on instruction words
// built here: every upper opcode (primary and special, mapped or not) under several dest masks, with and
// without the operand-clean flags; every lower op the engine implements, alone and after an upper op that
// writes its source; the ops it hands to execLowerT; every branch form; and sequences in which the MAC,
// clip and status registers change in consecutive slots while flag readers look back at them, which is
// what fixes the depth of the flag-visibility delay line.
//
// `--bench DIR` replays invocations captured from the game (RRV_VU_PROG_CAPTURE=DIR, files under local/:
// they hold game data) through the compiled blocks and through the programs and prints ns per slot.
//
// The test holds no game data: it reads the images through the catalogue the runtime carries. Without a
// catalogue (kVuProgCount == 0) it reports SKIP and passes.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Segment coverage: the generated programs call this at every label.
namespace
{
std::set<std::pair<uint32_t, uint32_t>> g_segments;
bool g_countSegments = true;
inline void progSegment(uint32_t program, uint32_t label)
{
    if (g_countSegments)
        g_segments.insert({program, label});
}
} // namespace
// -DRRV_VU_PROG_TEST_NO_COVERAGE builds the programs as the product does (no call at the labels): the
// build to use for --bench, where the hook would cost more than the slots it counts.
#if !defined(RRV_VU_PROG_TEST_NO_COVERAGE)
#define RRV_VU_PROG_SEG(program, label) progSegment(program, label)
#endif

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

float bitsToFloat(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

float plainFloat()
{
    switch (rnd() % 6)
    {
    case 0: return bitsToFloat(static_cast<uint32_t>(rnd()));
    case 1: return 0.0f;
    case 2: return static_cast<float>(static_cast<int>(rnd() % 4001) - 2000) / 16.0f;
    default: return static_cast<float>(static_cast<int>(rnd() % 2000001) - 1000000) / 1024.0f;
    }
}

// One in two is a value the VU treats specially.
float nastyFloat()
{
    const uint32_t sign = (rnd() & 1u) ? 0x80000000u : 0u;
    switch (rnd() % 12)
    {
    case 0: return bitsToFloat(sign | 0x7F800000u);                                             // Inf
    case 1: return bitsToFloat(sign | 0x7F800000u | (static_cast<uint32_t>(rnd()) & 0x007FFFFFu) | 1u); // NaN
    case 2: return bitsToFloat(sign | (static_cast<uint32_t>(rnd()) & 0x007FFFFFu));            // denormal or zero
    case 3: return bitsToFloat(sign | 0x7F7FFFFFu);                                             // largest
    case 4: return bitsToFloat(sign);                                                           // zero
    case 5: return bitsToFloat(static_cast<uint32_t>(rnd()));                                   // anything
    default: return plainFloat();
    }
}

// See tests/vu_lean_diff_tests.cpp: two NaN payloads, one sign, so that an op which clamps nothing gives
// the same stored result whichever operand order the compiler chose.
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

VU1State nastyState(bool tame)
{
    VU1State s;
    std::memset(&s, 0, sizeof s);
    for (auto &row : s.vf)
        for (float &lane : row)
            lane = tame ? plainFloat() : nastyFloat();
    for (int32_t &r : s.vi)
        r = static_cast<int32_t>(rnd() & 0x3FF);
    for (float &lane : s.acc)
        lane = tame ? plainFloat() : nastyFloat();
    s.q = tame ? plainFloat() : nastyFloat();
    s.p = tame ? plainFloat() : nastyFloat();
    s.i = tame ? plainFloat() : nastyFloat();
    s.mac = static_cast<uint32_t>(rnd() & 0xFFFF);
    s.clip = static_cast<uint32_t>(rnd() & 0xFFFFFF);
    s.status = static_cast<uint32_t>(rnd() & 0xFFF);
    s.xitop = static_cast<uint32_t>(rnd() & 0x3FF);
    s.ebit = (rnd() & 1u) != 0u;
    s.branchPending = (rnd() & 1u) != 0u;
    s.branchTarget = static_cast<uint32_t>(rnd() & 0x3FF8);
    canonicalNaNs(s.vf, 128);
    canonicalNaNs(s.acc, 4);
    canonicalNaNs(&s.q, 1);
    canonicalNaNs(&s.p, 1);
    canonicalNaNs(&s.i, 1);
    return s;
}

std::vector<uint8_t> makeData(uint32_t dataSize, unsigned kind)
{
    std::vector<uint8_t> data(dataSize);
    switch (kind & 3u)
    {
    case 0:
        for (uint8_t &byte : data)
            byte = static_cast<uint8_t>(rnd());
        break;
    case 1:
        for (uint32_t o = 0; o + 4u <= dataSize; o += 4u)
        {
            const float f = nastyFloat();
            std::memcpy(data.data() + o, &f, 4);
        }
        break;
    case 2: // rows of small integers (loop counters, strip lengths) between rows of floats
    case 3:
        for (uint32_t o = 0; o + 16u <= dataSize; o += 16u)
        {
            const unsigned what = static_cast<unsigned>(rnd() % 8u);
            for (uint32_t w = 0; w < 4u; ++w)
            {
                if (what < 3u)
                {
                    const uint32_t v = static_cast<uint32_t>(rnd() % ((kind & 3u) == 2u ? 6u : 40u));
                    std::memcpy(data.data() + o + w * 4u, &v, 4);
                }
                else if (what < 5u)
                {
                    const int32_t v = static_cast<int32_t>(rnd() % 8192u) - 4096; // fixed-point vertices
                    std::memcpy(data.data() + o + w * 4u, &v, 4);
                }
                else
                {
                    const float f = what == 7u ? nastyFloat() : plainFloat();
                    std::memcpy(data.data() + o + w * 4u, &f, 4);
                }
            }
        }
        break;
    }
    if ((kind & 4u) != 0u)
        for (uint32_t q = 0; q + 64u <= dataSize; q += 64u) // valid GIF tags: XGKICK submits packets
        {
            const uint64_t lo = 1ull | (1ull << 15) | (1ull << 60), hi = 0xEull;
            std::memcpy(data.data() + q, &lo, 8);
            std::memcpy(data.data() + q + 8, &hi, 8);
        }
    canonicalNaNs(data.data(), dataSize / 4);
    return data;
}

struct Machine
{
    VU1Interpreter vu;
    std::vector<uint8_t> code, data;
    std::vector<uint8_t> packets;
};

alignas(16) unsigned char g_gsStorage[sizeof(GS)]; // never constructed: the stub does not read it
GS &gs() { return *reinterpret_cast<GS *>(g_gsStorage); }

void describe(const VU1State &a, const VU1State &b)
{
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

bool check(bool ok, const char *what, size_t program, int trial, uint32_t base, uint32_t budget, int call)
{
    ++g_checks;
    if (ok)
        return true;
    if (g_failures < 20)
        std::fprintf(stderr, "FAIL: %s (program %zu, trial %d, base %04x, budget %u, call %d)\n", what, program,
                     trial, base, budget, call);
    ++g_failures;
    return false;
}

void enablePrograms(VU1Interpreter &vu)
{
    vu.aotEnsure();
    vu.m_progInit = true;
    vu.m_progEnabled = true;
    vu.m_progVerify = false;
}

int diffAll(int trials, bool verbose)
{
    const uint32_t codeSize = PS2_VU1_CODE_SIZE;
    const uint32_t dataSize = PS2_VU1_DATA_SIZE;
    uint64_t runs = 0, slotsNative = 0, handOvers = 0;
    std::vector<uint64_t> perProgram(kVuProgCount, 0);
    for (size_t k = 0; k < kVuProgCount; ++k)
    {
        const auto &prog = kVuProgs[k];
        const uint32_t imageBytes = prog.imageSlots * 8u;
        if (imageBytes > codeSize)
            continue;
        Machine ref, got;
        ref.vu.aotEnsure();
        ref.vu.m_aotForceInterp = true; // run() interprets every slot
        enablePrograms(got.vu);
        const int programTrials = trials * static_cast<int>(std::clamp(prog.segments / 4u, 1u, 16u));
        for (int trial = 0; trial < programTrials; ++trial)
        {
            const uint32_t room = (codeSize - imageBytes) / 8u;
            const uint32_t base = (trial % 4 != 3 || room == 0u) ? 0u : static_cast<uint32_t>(rnd() % (room + 1u)) * 8u;
            std::vector<uint8_t> code(codeSize, 0);
            std::memcpy(code.data() + base, prog.raw, imageBytes);
            const std::vector<uint8_t> data = makeData(dataSize, static_cast<unsigned>(trial));
            VU1State init = nastyState(trial % 5 == 4);
            const uint32_t budget = (trial % 6 == 5) ? 1u + static_cast<uint32_t>(rnd() % 600u) : 65536u;
            const uint32_t itop = static_cast<uint32_t>(rnd() & 0x3FF), top = static_cast<uint32_t>(rnd() & 0x3FF);
            for (Machine *m : {&ref, &got})
            {
                m->code = code;
                m->data = data;
                m->vu.state() = init;
                m->packets.clear();
            }
            rrvVuCodeChanged(1);
            const uint64_t callsBefore = got.vu.m_progCalls, handBefore = got.vu.m_progHandOvers;
            const uint64_t slotsBefore = got.vu.m_progSlots;
            for (int call = 0; call < 2; ++call)
            {
                g_capture = &ref.packets;
                ref.vu.execute(ref.code.data(), codeSize, ref.data.data(), dataSize, gs(), nullptr, base + prog.entry,
                               itop, budget, top);
                g_capture = &got.packets;
                const bool took = got.vu.aotLean(got.code.data(), codeSize, got.data.data(), dataSize, gs(), nullptr,
                                                 base + prog.entry, itop, budget, top);
                g_capture = nullptr;
                check(took, "the lean entry declined the call", k, trial, base, budget, call);
                const bool sameState = std::memcmp(&ref.vu.state(), &got.vu.state(), sizeof(VU1State)) == 0;
                if (!sameState && g_failures < 20)
                    describe(ref.vu.state(), got.vu.state());
                check(sameState, "VU1State differs", k, trial, base, budget, call);
                check(ref.data == got.data, "data memory differs", k, trial, base, budget, call);
                check(ref.packets == got.packets, "XGKICK packets differ", k, trial, base, budget, call);
            }
            check(got.vu.m_progCalls == callsBefore + 2u, "no program ran", k, trial, base, budget, 0);
            runs += 2;
            handOvers += got.vu.m_progHandOvers - handBefore;
            slotsNative += got.vu.m_progSlots - slotsBefore;
            perProgram[k] += got.vu.m_progSlots - slotsBefore;
        }
    }
    // Coverage: segments entered, of the segments the catalogue has.
    uint64_t total = 0, hit = 0;
    for (size_t k = 0; k < kVuProgCount; ++k)
    {
        total += kVuProgs[k].segments;
        uint32_t mine = 0;
        for (const auto &s : g_segments)
            mine += s.first == k ? 1u : 0u;
        hit += mine;
        if (verbose)
            std::printf("  program %3zu entry %04x: %3u of %3u segments, %llu slots\n", k, kVuProgs[k].entry, mine,
                        kVuProgs[k].segments, (unsigned long long)perProgram[k]);
        check(mine != 0u, "a program never ran", k, 0, 0, 0, 0);
    }
    const double coverage = total ? 100.0 * static_cast<double>(hit) / static_cast<double>(total) : 100.0;
    std::printf("programs: %zu, runs %llu, %llu slots in programs, %llu hand-overs, segments entered %llu of %llu (%.1f%%)\n",
                kVuProgCount, (unsigned long long)runs, (unsigned long long)slotsNative, (unsigned long long)handOvers,
                (unsigned long long)hit, (unsigned long long)total, coverage);
    check(coverage >= 85.0, "segment coverage below 85%", 0, 0, 0, 0, 0);
    return g_failures;
}

// ---- synthetic slots: the engine against the interpreter on instruction words built here ---------------
using P = VU1Interpreter::Prog;
using ScopedRounding = rrv::fp::ScopedRoundTowardZero;

constexpr uint32_t kNop = 0x8000033Cu;  // lower NOP
constexpr uint32_t kUNop = 0x000002FFu; // upper NOP
constexpr uint32_t kAllVf = 0xFFFFFFFFu;
constexpr uint32_t kAllVi = 0xFFFFu;

// Upper word `index`: 0..63 the primary opcodes, 64..191 the special ones, with fs = 1, ft = 2, fd = 3.
constexpr uint32_t upperWord(unsigned index, uint32_t dest)
{
    const uint32_t fields = (dest << 21) | (2u << 16) | (1u << 11);
    if (index < 64u)
        return fields | (3u << 6) | index;
    const unsigned op2 = index - 64u; // (upper & 3) | ((upper >> 4) & 0x7C)
    return fields | ((op2 >> 2) << 6) | 0x3Cu | (op2 & 3u);
}
constexpr uint32_t up(uint32_t op, uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft)
{
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
}
constexpr uint32_t upSpecial(uint32_t op2, uint32_t dest, uint32_t fs, uint32_t ft)
{
    return (dest << 21) | (ft << 16) | (fs << 11) | ((op2 >> 2) << 6) | 0x3Cu | (op2 & 3u);
}
constexpr uint32_t lo(uint32_t op, uint32_t dest, uint32_t it, uint32_t is, uint32_t imm11)
{
    return (op << 25) | (dest << 21) | (it << 16) | (is << 11) | (imm11 & 0x7FFu);
}
constexpr uint32_t loSpecial(uint32_t funct, uint32_t dest, uint32_t it, uint32_t is, uint32_t id)
{
    return (0x40u << 25) | (dest << 21) | (it << 16) | (is << 11) | (id << 6) | funct;
}
constexpr uint32_t loSpecial2(uint32_t f2, uint32_t dest, uint32_t it, uint32_t is)
{
    return (0x40u << 25) | (dest << 21) | (it << 16) | (is << 11) | ((f2 >> 2) << 6) | 0x3Cu | (f2 & 3u);
}
constexpr uint32_t flagOp(uint32_t op, uint32_t imm24) { return (op << 25) | (imm24 & 0xFFFFFFu); }

// No exponent of 255 anywhere: the state the operand-clean flags are valid for.
float cleanFloat()
{
    float f = nastyFloat();
    uint32_t u;
    std::memcpy(&u, &f, 4);
    if ((u & 0x7F800000u) == 0x7F800000u)
        u &= 0xFF7FFFFFu; // the largest float, same sign
    std::memcpy(&f, &u, 4);
    return f;
}

VU1State syntheticState(bool clean)
{
    VU1State s = nastyState(false);
    if (clean)
    {
        for (auto &row : s.vf)
            for (float &lane : row)
                lane = cleanFloat();
        for (float &lane : s.acc)
            lane = cleanFloat();
    }
    s.ebit = false;
    s.branchPending = false;
    s.pc = 0;
    s.itop = static_cast<uint32_t>(rnd() & 0x3FF);
    s.top = static_cast<uint32_t>(rnd() & 0x3FF);
    // Integer registers: small addresses, sign-extended 16-bit values and an all-ones mask (flag readers).
    for (int32_t &r : s.vi)
        r = (rnd() & 3u) == 0u ? static_cast<int32_t>(static_cast<int16_t>(rnd())) : static_cast<int32_t>(rnd() & 0x3FF);
    s.vi[0] = 0;
    s.vi[15] = -1;
    return s;
}

struct Synthetic
{
    Machine ref, got;
    uint64_t cases = 0;
    Synthetic()
    {
        ref.vu.aotEnsure();
        ref.vu.m_aotForceInterp = true;
        got.vu.aotEnsure();
        ref.code.assign(PS2_VU1_CODE_SIZE, 0);
        got.code.assign(PS2_VU1_CODE_SIZE, 0);
    }
    // Both machines from one state, the flag ring as aotLean() seeds it.
    void arm(const VU1State &init, const std::vector<uint8_t> &data, const uint64_t *words, size_t count)
    {
        for (Machine *m : {&ref, &got})
        {
            std::fill(m->code.begin(), m->code.end(), uint8_t{0});
            std::memcpy(m->code.data(), words, count * 8u);
            m->data = data;
            m->vu.state() = init;
            const VU1Interpreter::FlagSnapshot seed{init.mac, init.clip, init.status};
            for (auto &entry : m->vu.m_flagRing)
                entry = seed;
            m->vu.m_flagRingPos = 0;
            m->vu.m_flagVisible = seed;
            m->packets.clear();
            m->vu.m_curData = m->data.data();
            m->vu.m_curDataSize = PS2_VU1_DATA_SIZE;
            m->vu.m_curGs = &gs();
            m->vu.m_curMemory = nullptr;
            m->vu.m_aotCodeSize = PS2_VU1_CODE_SIZE;
        }
        rrvVuCodeChanged(1);
    }
    void interpret(uint32_t slots)
    {
        const ScopedRounding rounding(true);
        g_capture = &ref.packets;
        ref.vu.run(ref.code.data(), PS2_VU1_CODE_SIZE, ref.data.data(), PS2_VU1_DATA_SIZE, gs(), nullptr, slots);
        g_capture = nullptr;
    }
    void compare(const char *what, uint64_t word, int trial)
    {
        ++cases;
        ++g_checks;
        const bool sameState = std::memcmp(&ref.vu.state(), &got.vu.state(), sizeof(VU1State)) == 0;
        const bool same = sameState && ref.data == got.data && ref.packets == got.packets;
        if (same)
            return;
        if (g_failures < 20)
        {
            std::fprintf(stderr, "FAIL: synthetic %s lower=%08x upper=%08x trial %d (state=%s data=%s packets=%s)\n", what,
                         static_cast<uint32_t>(word), static_cast<uint32_t>(word >> 32), trial, sameState ? "ok" : "DIFF",
                         ref.data == got.data ? "ok" : "DIFF", ref.packets == got.packets ? "ok" : "DIFF");
            describe(ref.vu.state(), got.vu.state());
        }
        ++g_failures;
    }
};
Synthetic &synthetic()
{
    static Synthetic s;
    return s;
}

constexpr int kSlotTrials = 24;

// One slot through P::slot (a native lower op, or none).
template <uint32_t LOWER, uint32_t UPPER, uint32_t FLAGS>
void slotCase(const char *what)
{
    Synthetic &t = synthetic();
    const uint64_t word = (static_cast<uint64_t>(UPPER) << 32) | LOWER;
    for (int trial = 0; trial < kSlotTrials; ++trial)
    {
        const VU1State init = syntheticState(FLAGS != 0u);
        t.arm(init, makeData(PS2_VU1_DATA_SIZE, static_cast<unsigned>(trial)), &word, 1);
        t.interpret(1);
        {
            const ScopedRounding rounding(true);
            P::Regs r;
            P::enter<kAllVf, kAllVi>(r, t.got.vu);
            P::slot<LOWER, UPPER, FLAGS>(r, t.got.data.data(), PS2_VU1_DATA_SIZE - 1u);
            P::leave<kAllVf, kAllVi>(t.got.vu, r, 8u, false);
        }
        t.compare(what, word, trial);
    }
}

// One slot through P::slotGeneric (a lower op the engine hands to execLowerT).
template <uint32_t LOWER, uint32_t UPPER>
void genericCase(const char *what)
{
    Synthetic &t = synthetic();
    const uint64_t word = (static_cast<uint64_t>(UPPER) << 32) | LOWER;
    for (int trial = 0; trial < kSlotTrials; ++trial)
    {
        const VU1State init = syntheticState(false);
        t.arm(init, makeData(PS2_VU1_DATA_SIZE, static_cast<unsigned>(trial) | 4u), &word, 1);
        t.interpret(1);
        {
            const ScopedRounding rounding(true);
            g_capture = &t.got.packets;
            P::Regs r;
            P::enter<kAllVf, kAllVi>(r, t.got.vu);
            P::slotGeneric<LOWER, UPPER, 0u, kAllVf, kAllVi>(t.got.vu, r, 0u);
            P::leave<kAllVf, kAllVi>(t.got.vu, r, 8u, false);
            g_capture = nullptr;
        }
        t.compare(what, word, trial);
    }
}

// A branch and its delay slot (a NOP): the decision, the link register, the target and where control is.
template <uint32_t LOWER, uint32_t UPPER>
void branchCase(const char *what)
{
    Synthetic &t = synthetic();
    const uint64_t words[2] = {(static_cast<uint64_t>(UPPER) << 32) | LOWER, (static_cast<uint64_t>(kUNop) << 32) | kNop};
    constexpr bool kJump = P::op(LOWER) == 0x24u || P::op(LOWER) == 0x25u;
    for (int trial = 0; trial < kSlotTrials; ++trial)
    {
        VU1State init = syntheticState(false);
        if (trial % 3 == 0) // equal operands, and zero, so that every condition goes both ways
            init.vi[(LOWER >> 16) & 0xFu] = init.vi[(LOWER >> 11) & 0xFu];
        if (trial % 4 == 1)
            init.vi[(LOWER >> 11) & 0xFu] = 0;
        init.vi[0] = 0;
        t.arm(init, makeData(PS2_VU1_DATA_SIZE, static_cast<unsigned>(trial)), words, 2);
        t.interpret(2);
        {
            const ScopedRounding rounding(true);
            P::Regs r;
            P::enter<kAllVf, kAllVi>(r, t.got.vu);
            uint32_t pc;
            if constexpr (kJump)
            {
                pc = P::slotJump<LOWER, UPPER, 0u>(r, 0u);
            }
            else
            {
                const bool taken = P::slotBranch<LOWER, UPPER, 0u>(r, 0u);
                pc = taken ? ((8u + static_cast<uint32_t>(static_cast<int32_t>(P::imm11(LOWER)) * 8)) & 0x3FFFu) : 16u;
            }
            P::slot<kNop, kUNop, 0u>(r, t.got.data.data(), PS2_VU1_DATA_SIZE - 1u);
            P::leave<kAllVf, kAllVi>(t.got.vu, r, pc, false);
        }
        t.compare(what, words[0], trial);
    }
}

// A straight line of slots (native lower ops only): what a flag reader sees depends on how many slots
// back it looks, and the registers it reads change in every slot.
template <uint64_t... WORDS>
void sequenceCase(const char *what)
{
    Synthetic &t = synthetic();
    const uint64_t words[] = {WORDS...};
    for (int trial = 0; trial < 4 * kSlotTrials; ++trial)
    {
        const VU1State init = syntheticState(false);
        t.arm(init, makeData(PS2_VU1_DATA_SIZE, static_cast<unsigned>(trial)), words, sizeof...(WORDS));
        t.interpret(static_cast<uint32_t>(sizeof...(WORDS)));
        {
            const ScopedRounding rounding(true);
            P::Regs r;
            P::enter<kAllVf, kAllVi>(r, t.got.vu);
            (P::slot<static_cast<uint32_t>(WORDS), static_cast<uint32_t>(WORDS >> 32), 0u>(
                 r, t.got.data.data(), PS2_VU1_DATA_SIZE - 1u),
             ...);
            P::leave<kAllVf, kAllVi>(t.got.vu, r, static_cast<uint32_t>(sizeof...(WORDS)) * 8u, false);
        }
        t.compare(what, words[0], trial);
    }
}
constexpr uint64_t w(uint32_t lower, uint32_t upper) { return (static_cast<uint64_t>(upper) << 32) | lower; }

template <uint32_t DEST, uint32_t FLAGS, size_t... I>
void upperCases(std::index_sequence<I...>)
{
    (slotCase<kNop, upperWord(static_cast<unsigned>(I), DEST), FLAGS>("upper op"), ...);
}

// Upper ops paired with a lower op that reads what the upper op wrote (the upper pipe runs first).
constexpr uint32_t kAdd123 = up(0x28u, 0xFu, 1u, 2u, 3u);    // ADD vf1 = vf2 + vf3
constexpr uint32_t kMulA = upSpecial(0x2Au, 0xFu, 2u, 3u);   // MULA ACC = vf2 * vf3
constexpr uint32_t kClip45 = upSpecial(0x1Fu, 0xEu, 4u, 5u); // CLIP vf4, vf5
constexpr uint32_t kSub678 = up(0x2Cu, 0xFu, 6u, 7u, 8u);    // SUB vf6 = vf7 - vf8
constexpr uint32_t kMul912 = up(0x2Au, 0xBu, 9u, 1u, 2u);    // MUL.xzw vf9 = vf1 * vf2
constexpr uint32_t kFtoi = upSpecial(0x15u, 0xFu, 2u, 1u);   // FTOI4 vf1 = vf2
constexpr uint32_t kLoi = 0x80000000u | kUNop;               // I bit: the lower word is a float

int syntheticAll()
{
    const int before = g_failures;
    // Every upper opcode. FLAGS = 7 (no operand clamp) only from a state without an exponent of 255.
    upperCases<0xFu, 0u>(std::make_index_sequence<192>{});
    upperCases<0x6u, 0u>(std::make_index_sequence<192>{});
    upperCases<0x9u, 0u>(std::make_index_sequence<192>{});
    upperCases<0x0u, 0u>(std::make_index_sequence<192>{});
    upperCases<0xFu, 7u>(std::make_index_sequence<192>{});
    upperCases<0x5u, 7u>(std::make_index_sequence<192>{});
    // An upper op that writes VF0 (the slot ends with VF0 = (0, 0, 0, 1)), and the I bit.
    slotCase<kNop, up(0x28u, 0xFu, 0u, 2u, 3u), 0u>("upper op into vf0");
    slotCase<0x437F0000u, kLoi, 0u>("LOI");
    slotCase<0xC2F00000u, 0x80000000u | up(0x1Eu, 0xFu, 1u, 2u, 0u), 0u>("LOI with MULi");

#define RRV_LOWER(L) slotCase<L, kUNop, 0u>(#L)
#define RRV_AFTER(L, U) slotCase<L, U, 0u>(#L " after " #U)
    // Quadword and integer loads and stores: every dest mask shape, negative offsets, register 0.
    RRV_LOWER(lo(0x00u, 0xFu, 5u, 3u, 7u));
    RRV_LOWER(lo(0x00u, 0x6u, 5u, 3u, 0x7F0u));
    RRV_LOWER(lo(0x00u, 0x1u, 0u, 0u, 940u));
    RRV_LOWER(lo(0x00u, 0x0u, 5u, 3u, 1u));
    RRV_LOWER(lo(0x01u, 0xFu, 3u, 5u, 9u));
    RRV_LOWER(lo(0x01u, 0xEu, 3u, 5u, 0x7FBu));
    RRV_LOWER(lo(0x01u, 0x9u, 0u, 5u, 12u));
    RRV_AFTER(lo(0x01u, 0xFu, 3u, 1u, 9u), kAdd123);
    RRV_LOWER(lo(0x04u, 0x8u, 7u, 3u, 5u));
    RRV_LOWER(lo(0x04u, 0x4u, 7u, 3u, 0x7FEu));
    RRV_LOWER(lo(0x04u, 0x2u, 7u, 0u, 940u));
    RRV_LOWER(lo(0x04u, 0x1u, 0u, 3u, 5u));
    RRV_LOWER(lo(0x04u, 0x0u, 7u, 3u, 5u));
    RRV_LOWER(lo(0x05u, 0x8u, 7u, 3u, 5u));
    RRV_LOWER(lo(0x05u, 0x1u, 7u, 3u, 0x7FCu));
    RRV_LOWER(lo(0x05u, 0xFu, 15u, 3u, 2u));
    RRV_LOWER(lo(0x05u, 0x6u, 0u, 3u, 2u));
    // Integer arithmetic.
    RRV_LOWER(0x10010390u);                       // IADDIU vi01, vi00, 912
    RRV_LOWER(0x11EC07FFu);                       // IADDIU vi12, vi00, 32767
    RRV_LOWER((0x08u << 25) | (5u << 16) | (6u << 11) | 0x7FFu | (0xFu << 21));
    RRV_LOWER((0x08u << 25) | (0u << 16) | (6u << 11) | 5u);
    RRV_LOWER((0x09u << 25) | (5u << 16) | (6u << 11) | 3u);
    RRV_LOWER((0x09u << 25) | (5u << 16) | (5u << 11) | 0x7FFu | (0xFu << 21));
    RRV_LOWER(loSpecial(0x30u, 0u, 4u, 5u, 6u));
    RRV_LOWER(loSpecial(0x30u, 0u, 4u, 5u, 0u));
    RRV_LOWER(loSpecial(0x31u, 0u, 4u, 5u, 6u));
    RRV_LOWER(loSpecial(0x32u, 0u, 4u, 5u, 0x1Fu));
    RRV_LOWER(loSpecial(0x32u, 0u, 4u, 5u, 0x0Fu));
    RRV_LOWER(loSpecial(0x34u, 0u, 4u, 15u, 6u));
    RRV_LOWER(loSpecial(0x35u, 0u, 4u, 5u, 6u));
    // Register moves.
    RRV_LOWER(loSpecial2(0x30u, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x30u, 0x5u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x30u, 0xFu, 0u, 5u));
    RRV_AFTER(loSpecial2(0x30u, 0xFu, 4u, 1u), kAdd123);
    RRV_LOWER(loSpecial2(0x31u, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x31u, 0xAu, 4u, 4u));
    RRV_LOWER(loSpecial2(0x3Du, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Du, 0x3u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Cu, 0x0u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Cu, 0x1u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Cu, 0x2u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Cu, 0x3u, 0u, 5u));
    RRV_AFTER(loSpecial2(0x3Cu, 0x3u, 4u, 1u), kFtoi);
    RRV_LOWER(loSpecial2(0x3Eu, 0x8u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Eu, 0x1u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Fu, 0x4u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Fu, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x34u, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x34u, 0x7u, 4u, 0u));
    RRV_LOWER(loSpecial2(0x35u, 0xFu, 5u, 4u));
    RRV_LOWER(loSpecial2(0x35u, 0xCu, 0u, 4u));
    RRV_LOWER(loSpecial2(0x36u, 0xFu, 4u, 5u));
    RRV_LOWER(loSpecial2(0x36u, 0x2u, 4u, 0u));
    RRV_LOWER(loSpecial2(0x37u, 0xFu, 5u, 4u));
    RRV_LOWER(loSpecial2(0x37u, 0x8u, 0u, 4u));
    // The Q register: every source lane, zero and negative divisors come from the hostile state.
    RRV_LOWER(loSpecial2(0x38u, 0x0u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x38u, 0x7u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x38u, 0xCu, 4u, 0u));
    RRV_LOWER(loSpecial2(0x38u, 0xFu, 0u, 5u));
    RRV_AFTER(loSpecial2(0x38u, 0xCu, 1u, 0u), kAdd123);
    RRV_LOWER(loSpecial2(0x39u, 0x4u, 4u, 0u));
    RRV_LOWER(loSpecial2(0x39u, 0xCu, 4u, 0u));
    RRV_LOWER(loSpecial2(0x3Au, 0x9u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Au, 0x6u, 4u, 5u));
    RRV_LOWER(loSpecial2(0x3Bu, 0x0u, 0u, 0u));
    RRV_LOWER(loSpecial2(0x68u, 0x0u, 4u, 0u));
    RRV_LOWER(loSpecial2(0x68u, 0x0u, 0u, 0u));
    RRV_LOWER(loSpecial2(0x69u, 0x0u, 4u, 0u));
    // Flag writers, and readers in the slot of an upper op that changes the flag (they see the old one).
    RRV_LOWER(flagOp(0x11u, 0xABCDEFu));
    RRV_LOWER(flagOp(0x15u, 0x03FFC0u));
    RRV_AFTER(flagOp(0x10u, 0x000000u), kClip45);
    RRV_AFTER(flagOp(0x12u, 0x02FBEFu), kClip45);
    RRV_AFTER(flagOp(0x13u, 0xFFF000u), kClip45);
    RRV_LOWER(flagOp(0x14u, 0x000FFFu));
    RRV_LOWER(flagOp(0x16u, 0x000FC0u));
    RRV_LOWER(flagOp(0x17u, 0x000F3Fu));
    RRV_AFTER(lo(0x18u, 0u, 4u, 5u, 0u), kAdd123);
    RRV_AFTER(lo(0x1Au, 0u, 4u, 15u, 0u), kAdd123);
    RRV_AFTER(lo(0x1Bu, 0u, 4u, 5u, 0u), kAdd123);
    RRV_AFTER(lo(0x1Cu, 0u, 4u, 0u, 0u), kClip45);
    RRV_LOWER(lo(0x1Au, 0u, 0u, 15u, 0u));
#undef RRV_AFTER
#undef RRV_LOWER

    // Lower ops the engine runs in execLowerT: the EFU, MFP, the random-number ops, an unknown opcode,
    // and XGKICK (valid GIF tags in the data memory, so packets are submitted and compared).
    genericCase<loSpecial2(0x64u, 0xFu, 4u, 0u), kUNop>("MFP");
    genericCase<loSpecial2(0x64u, 0x3u, 0u, 0u), kAdd123>("MFP into vf0 after ADD");
    genericCase<loSpecial2(0x70u, 0x0u, 0u, 5u), kUNop>("ESADD");
    genericCase<loSpecial2(0x71u, 0x0u, 0u, 5u), kUNop>("ERSADD");
    genericCase<loSpecial2(0x72u, 0x0u, 0u, 5u), kUNop>("ELENG");
    genericCase<loSpecial2(0x73u, 0x0u, 0u, 5u), kUNop>("ERLENG");
    genericCase<loSpecial2(0x74u, 0x0u, 0u, 5u), kUNop>("EATANxy");
    genericCase<loSpecial2(0x75u, 0x0u, 0u, 5u), kUNop>("EATANxz");
    genericCase<loSpecial2(0x76u, 0x0u, 0u, 5u), kUNop>("ESUM");
    genericCase<loSpecial2(0x78u, 0x1u, 0u, 5u), kUNop>("ESQRT");
    genericCase<loSpecial2(0x79u, 0x2u, 0u, 5u), kUNop>("ERSQRT");
    genericCase<loSpecial2(0x7Au, 0x3u, 0u, 5u), kUNop>("ERCPR");
    genericCase<loSpecial2(0x7Cu, 0x0u, 0u, 5u), kUNop>("ESIN");
    genericCase<loSpecial2(0x7Du, 0x1u, 0u, 5u), kUNop>("EATAN");
    genericCase<loSpecial2(0x7Eu, 0x2u, 0u, 5u), kUNop>("EEXP");
    genericCase<loSpecial2(0x7Bu, 0x0u, 0u, 0u), kUNop>("WAITP");
    genericCase<loSpecial2(0x40u, 0x0u, 4u, 5u), kUNop>("RNEXT");
    genericCase<0x7E000000u, kUNop>("an unknown lower opcode");
    genericCase<loSpecial2(0x6Cu, 0x0u, 0u, 4u), kUNop>("XGKICK");
    genericCase<loSpecial2(0x6Cu, 0x0u, 0u, 0u), kAdd123>("XGKICK vi0 after ADD");

    // Branches: unconditional, call, every condition (taken and not), indirect, call indirect.
    branchCase<(0x20u << 25) | 0x010u, kUNop>("B forward");
    branchCase<(0x20u << 25) | 0x7F0u, kAdd123>("B backward after ADD");
    branchCase<(0x21u << 25) | (9u << 16) | 0x020u, kUNop>("BAL");
    branchCase<(0x21u << 25) | (0u << 16) | 0x020u, kUNop>("BAL vi0");
    branchCase<(0x28u << 25) | (4u << 16) | (5u << 11) | 0x008u, kUNop>("IBEQ");
    branchCase<(0x29u << 25) | (4u << 16) | (5u << 11) | 0x7F8u, kUNop>("IBNE");
    branchCase<(0x2Cu << 25) | (5u << 11) | 0x004u, kUNop>("IBLTZ");
    branchCase<(0x2Du << 25) | (5u << 11) | 0x004u, kUNop>("IBGTZ");
    branchCase<(0x2Eu << 25) | (5u << 11) | 0x004u, kUNop>("IBLEZ");
    branchCase<(0x2Fu << 25) | (5u << 11) | 0x004u, kUNop>("IBGEZ");
    branchCase<(0x24u << 25) | (5u << 11), kUNop>("JR");
    branchCase<(0x25u << 25) | (9u << 16) | (5u << 11), kUNop>("JALR");
    branchCase<(0x25u << 25) | (5u << 16) | (5u << 11), kUNop>("JALR through its own link register");

    // Flag visibility: the MAC register changes in slots 0, 1, 3 and 4, the clip register in slots 2 and 6,
    // and FMAND (mask VI15 = all ones) reads in every slot from 1 on; then clip readers likewise.
    sequenceCase<w(kNop, kAdd123), w(lo(0x1Au, 0u, 1u, 15u, 0u), kSub678), w(lo(0x1Au, 0u, 2u, 15u, 0u), kClip45),
                 w(lo(0x1Au, 0u, 3u, 15u, 0u), kMul912), w(lo(0x1Au, 0u, 4u, 15u, 0u), kMulA),
                 w(lo(0x1Au, 0u, 5u, 15u, 0u), kUNop), w(lo(0x1Au, 0u, 6u, 15u, 0u), kClip45),
                 w(lo(0x1Au, 0u, 7u, 15u, 0u), kUNop), w(lo(0x1Au, 0u, 8u, 15u, 0u), kUNop),
                 w(lo(0x1Au, 0u, 9u, 15u, 0u), kUNop)>("MAC visibility");
    sequenceCase<w(kNop, kClip45), w(lo(0x1Cu, 0u, 1u, 0u, 0u), upSpecial(0x1Fu, 0xEu, 6u, 7u)),
                 w(lo(0x1Cu, 0u, 2u, 0u, 0u), upSpecial(0x1Fu, 0xEu, 8u, 9u)), w(lo(0x1Cu, 0u, 3u, 0u, 0u), kClip45),
                 w(lo(0x1Cu, 0u, 4u, 0u, 0u), kUNop), w(lo(0x1Cu, 0u, 5u, 0u, 0u), kUNop),
                 w(lo(0x1Cu, 0u, 6u, 0u, 0u), kUNop), w(lo(0x1Cu, 0u, 7u, 0u, 0u), kUNop),
                 w(flagOp(0x12u, 0x02FBEFu), kUNop)>("clip visibility");
    // Writers through the lower pipe are instant, but a reader still looks four slots back.
    sequenceCase<w(flagOp(0x11u, 0x123456u), kUNop), w(lo(0x1Cu, 0u, 1u, 0u, 0u), kUNop),
                 w(flagOp(0x15u, 0x03FFC0u), kClip45), w(lo(0x1Cu, 0u, 2u, 0u, 0u), kUNop),
                 w(flagOp(0x16u, 0x000FFFu), kUNop), w(lo(0x1Cu, 0u, 3u, 0u, 0u), kUNop),
                 w(lo(0x1Cu, 0u, 4u, 0u, 0u), kUNop), w(flagOp(0x16u, 0x000FFFu), kUNop),
                 w(lo(0x1Cu, 0u, 5u, 0u, 0u), kUNop)>("flag writers and readers");
    // The strip loops' idiom: transform, divide, multiply by Q, convert, store, with the registers
    // feeding one another across slots.
    sequenceCase<w(lo(0x00u, 0xEu, 20u, 8u, 8u), kUNop), w(lo(0x00u, 0x1u, 20u, 0u, 940u), upSpecial(0x11u, 0xEu, 20u, 20u)),
                 w(kNop, upSpecial(0x18u, 0xFu, 28u, 20u)), w(kNop, upSpecial(0x09u, 0xFu, 29u, 20u)),
                 w(kNop, upSpecial(0x0Au, 0xFu, 30u, 20u)), w(kNop, up(0x0Bu, 0xFu, 17u, 31u, 20u)),
                 w(loSpecial2(0x38u, 0xCu, 17u, 0u), kUNop), w(kNop, up(0x1Cu, 0xEu, 14u, 17u, 0u)),
                 w(kNop, upSpecial(0x15u, 0xFu, 14u, 11u)), w(lo(0x01u, 0xEu, 11u, 10u, 2u), kUNop),
                 w(loSpecial2(0x3Cu, 0x3u, 6u, 11u), kUNop), w(lo(0x05u, 0x1u, 6u, 10u, 2u), kUNop)>("transform idiom");
    std::printf("synthetic slots: %llu cases, %d failures\n", (unsigned long long)synthetic().cases, g_failures - before);
    return g_failures - before;
}

// ---- --bench DIR: captured invocations, blocks against programs ----------------------------------------
struct Capture
{
    uint32_t startPC = 0, itop = 0, top = 0;
    VU1State state;
    std::vector<uint8_t> code, data;
};

std::vector<Capture> loadCaptures(const std::string &dir)
{
    std::vector<Capture> out;
    std::vector<std::filesystem::path> files;
    for (const auto &e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".vucap")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto &path : files)
    {
        std::ifstream in(path, std::ios::binary);
        Capture c;
        uint32_t header[4] = {};
        in.read(reinterpret_cast<char *>(header), sizeof header);
        if (header[0] != 0x50564343u) // "CCVP"
            continue;
        c.startPC = header[1];
        c.itop = header[2];
        c.top = header[3];
        in.read(reinterpret_cast<char *>(&c.state), sizeof c.state);
        c.code.resize(PS2_VU1_CODE_SIZE);
        c.data.resize(PS2_VU1_DATA_SIZE);
        in.read(reinterpret_cast<char *>(c.code.data()), c.code.size());
        in.read(reinterpret_cast<char *>(c.data.data()), c.data.size());
        if (in)
            out.push_back(std::move(c));
    }
    return out;
}

int bench(const std::string &dir)
{
    const std::vector<Capture> caps = loadCaptures(dir);
    if (caps.empty())
    {
        std::printf("no captures in %s\n", dir.c_str());
        return 1;
    }
    g_countSegments = false;
    struct Row
    {
        uint64_t slots = 0, calls = 0;
        double nsBlocks = 0.0, nsProgs = 0.0;
    };
    std::map<uint32_t, Row> rows;
    Machine blocks, progs;
    blocks.vu.aotEnsure();
    blocks.vu.m_progInit = true;
    blocks.vu.m_progEnabled = false;
    enablePrograms(progs.vu);
    int mismatches = 0;
    constexpr int kReps = 40;
    for (int pass = 0; pass < 3; ++pass) // pass 0 warms up and checks equality
    {
        for (const Capture &c : caps)
        {
            Row &row = rows[c.startPC | (static_cast<uint32_t>(c.code[c.startPC]) << 16)];
            double t[2] = {0.0, 0.0};
            uint64_t slots = 0;
            for (int which = 0; which < 2; ++which)
            {
                Machine &m = which == 0 ? blocks : progs;
                m.code = c.code;
                rrvVuCodeChanged(1);
                for (int rep = 0; rep < kReps; ++rep)
                {
                    m.data = c.data;
                    m.vu.state() = c.state;
                    const uint64_t before = m.vu.m_aotSlots;
                    const auto t0 = std::chrono::steady_clock::now();
                    m.vu.aotLean(m.code.data(), PS2_VU1_CODE_SIZE, m.data.data(), PS2_VU1_DATA_SIZE, gs(), nullptr,
                                 c.startPC, c.itop, 65536u, c.top);
                    t[which] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
                    slots = m.vu.m_aotSlots - before;
                }
            }
            if (pass == 0)
            {
                if (std::memcmp(&blocks.vu.state(), &progs.vu.state(), sizeof(VU1State)) != 0 || blocks.data != progs.data)
                {
                    if (mismatches < 5)
                    {
                        std::fprintf(stderr, "MISMATCH on a captured invocation, entry %04x\n", c.startPC);
                        describe(blocks.vu.state(), progs.vu.state());
                    }
                    ++mismatches;
                }
                continue;
            }
            row.slots += slots * kReps;
            row.calls += kReps;
            row.nsBlocks += t[0];
            row.nsProgs += t[1];
        }
    }
    double totalB = 0.0, totalP = 0.0;
    uint64_t totalSlots = 0;
    for (const auto &[key, row] : rows)
    {
        std::printf("entry %04x: %7.1f slots/call  blocks %6.2f ns/slot  programs %6.2f ns/slot  (x%.2f)\n",
                    key & 0xFFFFu, row.calls ? static_cast<double>(row.slots) / static_cast<double>(row.calls) : 0.0,
                    row.slots ? row.nsBlocks / static_cast<double>(row.slots) : 0.0,
                    row.slots ? row.nsProgs / static_cast<double>(row.slots) : 0.0,
                    row.nsProgs > 0.0 ? row.nsBlocks / row.nsProgs : 0.0);
        totalB += row.nsBlocks;
        totalP += row.nsProgs;
        totalSlots += row.slots;
    }
    std::printf("all: %zu captures, blocks %.2f ns/slot, programs %.2f ns/slot (x%.2f), %d mismatches\n", caps.size(),
                totalSlots ? totalB / static_cast<double>(totalSlots) : 0.0,
                totalSlots ? totalP / static_cast<double>(totalSlots) : 0.0, totalP > 0.0 ? totalB / totalP : 0.0,
                mismatches);
    return mismatches ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
    int trials = 240;
    bool verbose = false;
    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        if (arg == "--bench" && a + 1 < argc)
            return bench(argv[a + 1]);
        if (arg == "--trials" && a + 1 < argc)
            trials = std::atoi(argv[++a]);
        if (arg == "--verbose")
            verbose = true;
    }
    if (kVuProgCount == 0u)
    {
        // The engine itself needs no catalogue.
        syntheticAll();
        std::printf("SKIP of the catalogue part: no program catalogue in this build; %llu checks, %d failures\n",
                    (unsigned long long)g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    syntheticAll();
    diffAll(trials, verbose);
    std::printf("%llu checks, %d failures\n", (unsigned long long)g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
