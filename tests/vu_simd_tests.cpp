// vu_simd_tests.cpp — conformance gate for the host-agnostic SIMD layer and
// the VU slot decoder that the optimised VU1 interpreter is built on.
//
// The point of this test is PORTABILITY.  rrv_simd.h has three backends (NEON,
// SSE2, portable scalar) and the VU is bit-exactness-critical, so every
// operation is checked here against an independent scalar reference written in
// plain C++ inside this file — not against another backend of the same header.
// Whichever backend the build selected is the one under test; CMake also builds
// this file a second time with RRV_SIMD_FORCE_SCALAR so the fallback is covered
// on every host.
//
// The corpus is deliberately nasty: signed zeros, denormals, Inf, NaN, the
// largest finite float, and the bit patterns the VU's MAC/CLIP rules turn on.
//
// NOT tested here, on purpose: cvt_f32_to_s32_trunc() outside int32 range.
// That is host-dependent by design (rrv_simd.h rule 4) — AArch64 saturates,
// x86 yields 0x80000000 — and matches what `(int32_t)f` already did on each
// host in the scalar interpreter.

#include "runtime/rrv_simd.h"
#include "runtime/rrv_vu_ir.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

int g_failures = 0;

void fail(const char *what, unsigned lane, uint32_t got, uint32_t want)
{
    if (g_failures < 40)
        std::fprintf(stderr, "FAIL %s lane %u: got %08x want %08x\n", what, lane, got, want);
    ++g_failures;
}

void failScalar(const char *what, uint32_t got, uint32_t want)
{
    if (g_failures < 40)
        std::fprintf(stderr, "FAIL %s: got %08x want %08x\n", what, got, want);
    ++g_failures;
}

uint32_t bits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float fromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

void expectVec(const char *what, rrv::simd::f32x4 got, const float (&want)[4])
{
    float g[4];
    rrv::simd::storeu(g, got);
    for (unsigned i = 0; i < 4; ++i)
        if (bits(g[i]) != bits(want[i]))
            fail(what, i, bits(g[i]), bits(want[i]));
}

void expectVecU(const char *what, rrv::simd::u32x4 got, const uint32_t (&want)[4])
{
    uint32_t g[4];
    rrv::simd::storeu_u(g, got);
    for (unsigned i = 0; i < 4; ++i)
        if (g[i] != want[i])
            fail(what, i, g[i], want[i]);
}

// The corpus.  Every quad below is used as both operands of every binary op,
// against every other quad, so the combinations cover sign/zero/denormal/Inf/
// NaN crossings rather than just a happy path.
const uint32_t kCorpus[][4] = {
    {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u}, // +0, -0, 1, -1
    {0x00000001u, 0x807FFFFFu, 0x007FFFFFu, 0x00800000u}, // denormals + smallest normal
    {0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u}, // +/-max finite, +/-Inf
    {0x7FC00001u, 0xFFC00001u, 0x40490FDBu, 0xC0490FDBu}, // NaNs, +/-pi
    {0x4B7FFFFFu, 0xCB7FFFFFu, 0x477FFF00u, 0x38D1B717u}, // large ints, small
    {0x41200000u, 0x42C80000u, 0x3DCCCCCDu, 0xBDCCCCCDu}, // 10, 100, 0.1, -0.1
};
constexpr size_t kCorpusN = sizeof(kCorpus) / sizeof(kCorpus[0]);

void loadQuad(size_t idx, float (&out)[4])
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = fromBits(kCorpus[idx][i]);
}

void testArithmetic()
{
    using namespace rrv::simd;
    for (size_t a = 0; a < kCorpusN; ++a)
    {
        for (size_t b = 0; b < kCorpusN; ++b)
        {
            float x[4], y[4], want[4];
            loadQuad(a, x);
            loadQuad(b, y);
            const f32x4 vx = loadu(x), vy = loadu(y);

            for (unsigned i = 0; i < 4; ++i) want[i] = x[i] + y[i];
            expectVec("add", add(vx, vy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = x[i] - y[i];
            expectVec("sub", sub(vx, vy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = x[i] * y[i];
            expectVec("mul", mul(vx, vy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = x[i] / y[i];
            expectVec("div", div(vx, vy), want);

            // max_c/min_c are C's ternary, NOT the host max/min instruction:
            // this is the case that separates them (rrv_simd.h rule 2).
            for (unsigned i = 0; i < 4; ++i) want[i] = (x[i] > y[i]) ? x[i] : y[i];
            expectVec("max_c", max_c(vx, vy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = (x[i] < y[i]) ? x[i] : y[i];
            expectVec("min_c", min_c(vx, vy), want);

            for (unsigned i = 0; i < 4; ++i) want[i] = std::fabs(x[i]);
            expectVec("abs_f", abs_f(vx), want);

            // A multiply-add must round the product first — the VU's FMAC is
            // not an FMA.  If a backend ever contracts this, the corpus's
            // large/small mixes will show it.
            for (unsigned i = 0; i < 4; ++i) want[i] = y[i] + x[i] * y[i];
            expectVec("mul+add (unfused)", add(vy, mul(vx, vy)), want);
        }
    }
}

void testCompareAndSelect()
{
    using namespace rrv::simd;
    for (size_t a = 0; a < kCorpusN; ++a)
    {
        for (size_t b = 0; b < kCorpusN; ++b)
        {
            float x[4], y[4];
            uint32_t want[4];
            loadQuad(a, x);
            loadQuad(b, y);
            const f32x4 vx = loadu(x), vy = loadu(y);

            for (unsigned i = 0; i < 4; ++i) want[i] = (x[i] > y[i]) ? 0xFFFFFFFFu : 0u;
            expectVecU("cmpgt", cmpgt(vx, vy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = (x[i] < y[i]) ? 0xFFFFFFFFu : 0u;
            expectVecU("cmplt", cmplt(vx, vy), want);

            const u32x4 ux = as_u(vx), uy = as_u(vy);
            for (unsigned i = 0; i < 4; ++i) want[i] = (kCorpus[a][i] == kCorpus[b][i]) ? 0xFFFFFFFFu : 0u;
            expectVecU("cmpeq_u", cmpeq_u(ux, uy), want);
            for (unsigned i = 0; i < 4; ++i)
                want[i] = (static_cast<int32_t>(kCorpus[a][i]) > static_cast<int32_t>(kCorpus[b][i]))
                              ? 0xFFFFFFFFu : 0u;
            expectVecU("cmpgt_s", cmpgt_s(ux, uy), want);

            for (unsigned i = 0; i < 4; ++i) want[i] = kCorpus[a][i] & kCorpus[b][i];
            expectVecU("and_u", and_u(ux, uy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = kCorpus[a][i] | kCorpus[b][i];
            expectVecU("or_u", or_u(ux, uy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = kCorpus[a][i] ^ kCorpus[b][i];
            expectVecU("xor_u", xor_u(ux, uy), want);
            for (unsigned i = 0; i < 4; ++i) want[i] = (~kCorpus[a][i]) & kCorpus[b][i];
            expectVecU("andnot_u", andnot_u(ux, uy), want);

            // select over an arbitrary lane mask
            const u32x4 mask = cmpgt(vx, vy);
            uint32_t m[4];
            storeu_u(m, mask);
            float wantf[4];
            for (unsigned i = 0; i < 4; ++i) wantf[i] = m[i] ? x[i] : y[i];
            expectVec("select", select(mask, vx, vy), wantf);
        }
    }
}

void testShiftsAndMasks()
{
    using namespace rrv::simd;
    for (size_t a = 0; a < kCorpusN; ++a)
    {
        uint32_t want[4];
        const u32x4 u = loadu_u(kCorpus[a]);
        for (unsigned i = 0; i < 4; ++i) want[i] = kCorpus[a][i] >> 23;
        expectVecU("shr_u<23>", shr_u<23>(u), want);
        for (unsigned i = 0; i < 4; ++i) want[i] = kCorpus[a][i] << 1;
        expectVecU("shl_u<1>", shl_u<1>(u), want);
    }

    // mask_bits / mask_bits_rev over all 16 lane patterns.
    for (unsigned pattern = 0; pattern < 16u; ++pattern)
    {
        uint32_t lanes[4];
        uint32_t fwd = 0, rev = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            const bool on = (pattern >> i) & 1u;
            lanes[i] = on ? 0xFFFFFFFFu : 0u;
            if (on)
            {
                fwd |= 1u << i;
                rev |= 1u << (3 - i);
            }
        }
        const u32x4 m = loadu_u(lanes);
        if (mask_bits(m) != fwd)
            failScalar("mask_bits", mask_bits(m), fwd);
        if (mask_bits_rev(m) != rev)
            failScalar("mask_bits_rev", mask_bits_rev(m), rev);
    }

    // reduce_add_u32: plain lane sum, including the wrap the VU never relies on
    // but a reader might. It is used in place of four mask_bits_rev() calls, so
    // also check the exact identity the MAC update depends on: with DISJOINT
    // per-mask weights, one reduce_add equals the four weighted reductions ORed
    // together.
    {
        const uint32_t sums[][5] = {
            {0u, 0u, 0u, 0u, 0u},
            {1u, 2u, 3u, 4u, 10u},
            {0x80000000u, 0x80000000u, 0u, 0u, 0u},        // wraps to 0
            {0xFFFFFFFFu, 1u, 0u, 0u, 0u},                 // wraps to 0
            {0x0008u, 0x0080u, 0x0800u, 0x8000u, 0x8888u}, // the MAC weights
        };
        for (const auto &row : sums)
        {
            uint32_t lanes[4] = {row[0], row[1], row[2], row[3]};
            if (reduce_add_u32(loadu_u(lanes)) != row[4])
                failScalar("reduce_add_u32", reduce_add_u32(loadu_u(lanes)), row[4]);
        }

        static const uint32_t kWz[4] = {0x0008u, 0x0004u, 0x0002u, 0x0001u};
        static const uint32_t kWs[4] = {0x0080u, 0x0040u, 0x0020u, 0x0010u};
        static const uint32_t kWu[4] = {0x0800u, 0x0400u, 0x0200u, 0x0100u};
        static const uint32_t kWo[4] = {0x8000u, 0x4000u, 0x2000u, 0x1000u};
        for (unsigned z = 0; z < 16u; ++z)
            for (unsigned sgn = 0; sgn < 16u; ++sgn)
            {
                uint32_t zl[4], sl[4];
                for (unsigned i = 0; i < 4; ++i)
                {
                    zl[i] = ((z >> i) & 1u) ? 0xFFFFFFFFu : 0u;
                    sl[i] = ((sgn >> i) & 1u) ? 0xFFFFFFFFu : 0u;
                }
                const u32x4 zm = loadu_u(zl), sm = loadu_u(sl);
                const uint32_t want = mask_bits_rev(zm) | (mask_bits_rev(sm) << 4) |
                                      (mask_bits_rev(zm) << 8) | (mask_bits_rev(sm) << 12);
                const uint32_t got = reduce_add_u32(
                    or_u(or_u(and_u(zm, loadu_u(kWz)), and_u(sm, loadu_u(kWs))),
                         or_u(and_u(zm, loadu_u(kWu)), and_u(sm, loadu_u(kWo)))));
                if (got != want)
                    failScalar("weighted single-reduction == four mask_bits_rev", got, want);
            }
    }

    // dest_mask must agree with the VU's x=8/y=4/z=2/w=1 order, and must be
    // the exact inverse of mask_bits_rev.
    for (unsigned dest = 0; dest < 16u; ++dest)
    {
        uint32_t want[4];
        for (unsigned i = 0; i < 4; ++i)
            want[i] = (dest & (0x8u >> i)) ? 0xFFFFFFFFu : 0u;
        expectVecU("dest_mask", dest_mask(static_cast<uint8_t>(dest)), want);
        if (mask_bits_rev(dest_mask(static_cast<uint8_t>(dest))) != dest)
            failScalar("dest_mask/mask_bits_rev round trip",
                       mask_bits_rev(dest_mask(static_cast<uint8_t>(dest))), dest);
    }
}

void testConversions()
{
    using namespace rrv::simd;
    const int32_t ints[][4] = {
        {0, 1, -1, 2},
        {32767, -32768, 65536, -65536},
        {1 << 23, -(1 << 23), 12345678, -12345678},
        {2147483647, -2147483647 - 1, 1000000, -1000000},
    };
    for (const auto &q : ints)
    {
        uint32_t raw[4];
        float want[4];
        for (unsigned i = 0; i < 4; ++i)
        {
            raw[i] = static_cast<uint32_t>(q[i]);
            want[i] = static_cast<float>(q[i]);
        }
        expectVec("cvt_s32_to_f32", cvt_s32_to_f32(loadu_u(raw)), want);
    }

    // Float -> int, in-range only (see the header note at the top of the file).
    const float floats[][4] = {
        {0.0f, -0.0f, 1.9f, -1.9f},
        {16.0f * 1.5f, -16.0f * 1.5f, 4095.75f, -4095.75f},
        {8388607.0f, -8388607.0f, 0.49999997f, -0.49999997f},
    };
    for (const auto &q : floats)
    {
        uint32_t want[4];
        for (unsigned i = 0; i < 4; ++i)
            want[i] = static_cast<uint32_t>(static_cast<int32_t>(q[i]));
        expectVecU("cvt_f32_to_s32_trunc", cvt_f32_to_s32_trunc(loadu(q)), want);
    }
}

void testDupLane()
{
    using namespace rrv::simd;
    const float q[4] = {1.5f, -2.5f, 3.5f, -4.5f};
    for (unsigned lane = 0; lane < 4u; ++lane)
    {
        const float want[4] = {q[lane], q[lane], q[lane], q[lane]};
        expectVec("dup_lane", dup_lane(q, lane), want);
    }
}

// ---------------------------------------------------------------- decoder

struct DecodeCase
{
    const char *name;
    uint32_t lower;
    uint32_t upper;
    uint8_t uop;
    uint8_t udst;
    uint8_t dest;
    uint8_t fs, ft, fd, bc;
    uint8_t lclass;
    bool mac;
};

// Build an upper word the way the ISA lays it out.
constexpr uint32_t upperWord(uint32_t funct, uint32_t dest, uint32_t ft, uint32_t fs,
                             uint32_t fd, uint32_t bits31_27 = 0)
{
    return (bits31_27 << 27) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}

// Upper-special: the secondary opcode is (upper & 3) | ((upper >> 4) & 0x7C),
// so an op2 is encoded as funct = 0x3C | (op2 & 3) and bits 10:6 of the FD
// field carry (op2 >> 2).  Rebuild that here so the test encodes the same way
// the hardware does rather than trusting the decoder's own arithmetic.
constexpr uint32_t upperSpecialWord(uint32_t op2, uint32_t dest, uint32_t ft, uint32_t fs)
{
    const uint32_t funct = 0x3Cu | (op2 & 3u);
    const uint32_t fdField = (op2 >> 2) & 0x1Fu;
    return (dest << 21) | (ft << 16) | (fs << 11) | (fdField << 6) | funct;
}

void testDecoder()
{
    using namespace rrv::vu;
    const uint32_t kNopLower = 0x8000033Cu;

    const DecodeCase cases[] = {
        {"ADDbc(y)", kNopLower, upperWord(0x01u, 0xF, 5, 3, 7), U_ADDbc, UD_FD, 0xF, 3, 5, 7, 1, LC_NOP, true},
        {"MADDbc(w)", 0x12345678u, upperWord(0x0Bu, 0x8, 2, 1, 4), U_MADDbc, UD_FD, 0x8, 1, 2, 4, 3, LC_GENERIC, true},
        {"MAXbc(x)", 0x00000000u, upperWord(0x10u, 0x4, 9, 8, 10), U_MAXbc, UD_FD, 0x4, 8, 9, 10, 0, LC_NOP, false},
        {"MINI", kNopLower, upperWord(0x2Fu, 0xE, 6, 5, 4), U_MINI, UD_FD, 0xE, 5, 6, 4, 3, LC_NOP, false},
        {"MUL", kNopLower, upperWord(0x2Au, 0xF, 6, 5, 4), U_MUL, UD_FD, 0xF, 5, 6, 4, 2, LC_NOP, true},
        {"OPMSUB", kNopLower, upperWord(0x2Eu, 0xE, 3, 2, 1), U_OPMSUB, UD_FD, 0xE, 2, 3, 1, 2, LC_NOP, true},
        {"MULAbc(z)", kNopLower, upperSpecialWord(0x1Au, 0xF, 4, 3), U_MULAbc, UD_ACC, 0xF, 3, 4, 0x06, 2, LC_NOP, true},
        {"MADDAbc(x)", kNopLower, upperSpecialWord(0x08u, 0x8, 2, 1), U_MADDAbc, UD_ACC, 0x8, 1, 2, 0x02, 0, LC_NOP, true},
        {"ITOF12", kNopLower, upperSpecialWord(0x12u, 0xF, 7, 6), U_ITOF12, UD_FT, 0xF, 6, 7, 0x04, 2, LC_NOP, false},
        {"FTOI15", kNopLower, upperSpecialWord(0x17u, 0x1, 7, 6), U_FTOI15, UD_FT, 0x1, 6, 7, 0x05, 3, LC_NOP, false},
        {"ABS", kNopLower, upperSpecialWord(0x1Du, 0xF, 9, 8), U_ABS, UD_FT, 0xF, 8, 9, 0x07, 1, LC_NOP, false},
        {"CLIP", kNopLower, upperSpecialWord(0x1Fu, 0xF, 2, 1), U_CLIP, UD_NONE, 0xF, 1, 2, 0x07, 3, LC_NOP, false},
        {"OPMULA", kNopLower, upperSpecialWord(0x2Eu, 0xE, 3, 2), U_OPMULA, UD_ACC, 0xE, 2, 3, 0x0B, 2, LC_NOP, true},
        {"NOP upper", kNopLower, upperWord(0x33u, 0x0, 0, 0, 0), U_NOP, UD_NONE, 0x0, 0, 0, 0, 3, LC_NOP, false},
    };

    for (const auto &c : cases)
    {
        SlotIR ir{};
        decodeSlot(c.lower, c.upper, ir);
        auto check = [&](const char *field, uint32_t got, uint32_t want) {
            if (got != want)
            {
                if (g_failures < 40)
                    std::fprintf(stderr, "FAIL decode %s.%s: got %u want %u\n", c.name, field, got, want);
                ++g_failures;
            }
        };
        check("uop", ir.uop, c.uop);
        check("udst", ir.udst, c.udst);
        check("dest", ir.dest, c.dest);
        check("fs", ir.fs, c.fs);
        check("ft", ir.ft, c.ft);
        check("fd", ir.fd, c.fd);
        check("bc", ir.bc, c.bc);
        check("lclass", ir.lclass, c.lclass);
        check("UF_MAC", (ir.uflags & UF_MAC) ? 1u : 0u, c.mac ? 1u : 0u);
        check("raw lower", ir.lower, c.lower);
        check("raw upper", ir.upper, c.upper);
    }

    // The I bit makes the lower word an immediate, whatever it looks like.
    {
        SlotIR ir{};
        decodeSlot(0x3F800000u, upperWord(0x2Au, 0xF, 1, 2, 3, 0x10u), ir);
        if (ir.lclass != LC_LOI)
            failScalar("decode LOI lclass", ir.lclass, LC_LOI);
        if (!(ir.ubits & UB_I))
            failScalar("decode LOI ubits", ir.ubits, UB_I);
    }
    // The E bit must survive decode: run() ends the microprogram on it.
    {
        SlotIR ir{};
        decodeSlot(0u, upperWord(0x2Au, 0xF, 1, 2, 3, 0x08u), ir);
        if (!(ir.ubits & UB_E))
            failScalar("decode E bit", ir.ubits, UB_E);
    }
    // An all-zero instruction pair is LEGAL: funct 0 is ADDbc, and with an
    // empty dest mask it writes no lane but still refreshes the MAC flags.
    // A decode cache that starts zeroed must therefore not mistake "never
    // decoded" for this pair — hence SlotIR::valid.
    {
        SlotIR ir{};
        if (ir.valid != 0)
            failScalar("zeroed SlotIR must not look decoded", ir.valid, 0u);
        decodeSlot(0u, 0u, ir);
        if (ir.uop != U_ADDbc)
            failScalar("decode all-zero slot uop", ir.uop, U_ADDbc);
        if (!(ir.uflags & UF_MAC))
            failScalar("decode all-zero slot UF_MAC", ir.uflags, UF_MAC);
        if (ir.dest != 0)
            failScalar("decode all-zero slot dest", ir.dest, 0u);
        if (ir.valid != 1)
            failScalar("decodeSlot must mark the entry valid", ir.valid, 1u);
    }

    // A decoded slot must carry the bits it came from, so a consumer can
    // re-validate a cached decode with one compare.
    {
        SlotIR ir{};
        const uint32_t lo = 0xDEADBEEFu, up = upperWord(0x28u, 0x5, 4, 3, 2);
        decodeSlot(lo, up, ir);
        if (ir.lower != lo || ir.upper != up)
            failScalar("decode validity tag", ir.lower, lo);
    }
}

} // namespace

int main()
{
    std::printf("rrv::simd backend: %s\n", RRV_SIMD_BACKEND_NAME);
    testArithmetic();
    testCompareAndSelect();
    testShiftsAndMasks();
    testConversions();
    testDupLane();
    testDecoder();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "vu_simd_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vu_simd_tests: all checks passed\n");
    return 0;
}
