// Conformance and cost check for runtime/rrv_fp_rounding.h (rrv::fp::ScopedRoundTowardZero).
//
// The scope replaces std::fesetround(FE_TOWARDZERO) on the VU hot paths. It must (1) round exactly
// like fesetround does, (2) restore the previous mode, (3) leave every other control-register bit
// alone (x86: exception flags and DAZ/FTZ; the owner thread reads the DE flag after a command),
// (4) nest, and (5) do nothing when disabled. On x86 the entry MXCSR value is written back on exit
// (bit for bit; sticky exception flags raised inside are discarded, see the header).
//
//   rrv-fp-rounding-tests           run the checks
//   rrv-fp-rounding-tests --bench   also print ns per enter/leave: scope vs fegetround+fesetround x2

#include "runtime/rrv_fp_rounding.h"

#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__x86_64__)
#  include <xmmintrin.h>
#endif

namespace
{
int g_failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// 1/3 and 2/3 are not representable: round-to-nearest and toward-zero give different floats.
// volatile keeps the compiler from folding the division at the default rounding mode.
uint32_t divideBits(float a, float b)
{
    volatile float va = a, vb = b;
    volatile float q = va / vb;
    float r = q;
    uint32_t bits;
    std::memcpy(&bits, &r, sizeof bits);
    return bits;
}

uint32_t mulAddBits(float a, float b, float c)
{
    volatile float va = a, vb = b, vc = c;
    volatile float p = va * vb;
    volatile float s = p + vc;
    float r = s;
    uint32_t bits;
    std::memcpy(&bits, &r, sizeof bits);
    return bits;
}

// The rounding mode the arithmetic actually uses. On x86-64 that is MXCSR.RC: glibc's fegetround()
// answers from the x87 control word, which the scope deliberately does not touch.
int observedRound()
{
#if defined(__x86_64__)
    switch (_mm_getcsr() & 0x6000u)
    {
    case 0x0000u: return FE_TONEAREST;
    case 0x2000u: return FE_DOWNWARD;
    case 0x4000u: return FE_UPWARD;
    default: return FE_TOWARDZERO;
    }
#else
    return std::fegetround();
#endif
}

struct Case { float a, b, c; };
constexpr Case kCases[] = {
    {1.0f, 3.0f, 0.1f},    {2.0f, 3.0f, -0.7f},     {-1.0f, 3.0f, 0.25f}, {1.0f, 7.0f, 1e-3f},
    {0.1f, 0.3f, 12345.6f}, {-5.5f, 3.3f, 5.5f},    {1e-20f, 3.0f, 1e-30f}, {16777217.0f, 3.0f, 1.0f},
    {3.14159265f, 2.71828183f, -8.5397f}, {-0.0f, 3.0f, 0.0f},
};

void testRoundsLikeFesetround()
{
    bool differsFromNearest = false;
    for (const Case &c : kCases)
    {
        const uint32_t nearestDiv = divideBits(c.a, c.b);
        const uint32_t nearestFma = mulAddBits(c.a, c.b, c.c);
        uint32_t scopedDiv, scopedFma, libmDiv, libmFma;
        {
            const rrv::fp::ScopedRoundTowardZero scope;
            scopedDiv = divideBits(c.a, c.b);
            scopedFma = mulAddBits(c.a, c.b, c.c);
        }
        {
            const int previous = std::fegetround();
            std::fesetround(FE_TOWARDZERO);
            libmDiv = divideBits(c.a, c.b);
            libmFma = mulAddBits(c.a, c.b, c.c);
            std::fesetround(previous);
        }
        expect(scopedDiv == libmDiv, "divide: scope == fesetround(FE_TOWARDZERO)");
        expect(scopedFma == libmFma, "multiply+add: scope == fesetround(FE_TOWARDZERO)");
        differsFromNearest = differsFromNearest || scopedDiv != nearestDiv || scopedFma != nearestFma;
    }
    expect(differsFromNearest, "the cases are sensitive to rounding (toward-zero differs from nearest somewhere)");
}

void testRestoresPreviousMode()
{
    expect(observedRound() == FE_TONEAREST, "test starts in round-to-nearest");
    {
        const rrv::fp::ScopedRoundTowardZero scope;
        expect(observedRound() == FE_TOWARDZERO, "inside the scope the mode is toward zero");
    }
    expect(observedRound() == FE_TONEAREST, "the scope restores round-to-nearest");

    // Entered while already toward zero: leaves it toward zero, and does not disturb anything.
    std::fesetround(FE_TOWARDZERO);
    {
        const rrv::fp::ScopedRoundTowardZero scope;
        expect(observedRound() == FE_TOWARDZERO, "already toward zero stays toward zero");
    }
    expect(observedRound() == FE_TOWARDZERO, "an outer toward-zero mode survives the scope");
    std::fesetround(FE_UPWARD);
    {
        const rrv::fp::ScopedRoundTowardZero scope;
        expect(observedRound() == FE_TOWARDZERO, "enters toward zero from round-up");
    }
    expect(observedRound() == FE_UPWARD, "restores round-up");
    std::fesetround(FE_TONEAREST);
}

void testDisabledAndNested()
{
    {
        const rrv::fp::ScopedRoundTowardZero off(false);
        expect(observedRound() == FE_TONEAREST, "a disabled scope changes nothing");
    }
    expect(observedRound() == FE_TONEAREST, "a disabled scope restores nothing");
    {
        const rrv::fp::ScopedRoundTowardZero outer;
        {
            const rrv::fp::ScopedRoundTowardZero inner;
            expect(observedRound() == FE_TOWARDZERO, "inner scope is toward zero");
        }
        expect(observedRound() == FE_TOWARDZERO, "leaving the inner scope keeps the outer mode");
    }
    expect(observedRound() == FE_TONEAREST, "leaving the outer scope restores round-to-nearest");
}

#if defined(__x86_64__)
void testOtherMxcsrBitsUntouched()
{
    constexpr unsigned kRC = 0x6000u, kDazFtz = 0x8040u, kFlags = 0x003Fu, kMasks = 0x1F80u;
    const unsigned saved = _mm_getcsr();
    // DAZ+FTZ on, all exceptions masked, flags clear, round-to-nearest.
    _mm_setcsr((saved & ~(kRC | kFlags)) | kDazFtz | kMasks);
    const unsigned before = _mm_getcsr();
    unsigned inside = 0;
    {
        const rrv::fp::ScopedRoundTowardZero scope;
        inside = _mm_getcsr();
        divideBits(1.0f, 3.0f); // raises the inexact flag (0x20) inside the scope
        expect((_mm_getcsr() & 0x20u) != 0u, "the division really raises the inexact flag (test sanity)");
    }
    const unsigned after = _mm_getcsr();
    expect((inside & ~kRC) == (before & ~kRC), "entering changes only MXCSR.RC");
    expect((inside & kRC) == kRC, "RC is 0b11 inside the scope");
    // Exit writes back the entry value: RC, DAZ/FTZ and the masks are exactly as before, and the sticky
    // flags raised inside are discarded (documented in rrv_fp_rounding.h; nothing in the product reads them).
    expect(after == before, "exit restores the MXCSR value from entry, bit for bit");
    expect((after & kDazFtz) == kDazFtz, "DAZ/FTZ survive the scope");
    expect((after & kMasks) == kMasks, "exception masks survive the scope");
    _mm_setcsr(saved);
}

void testNestedScopeWritesNothing()
{
    // An inner scope that finds the mode already toward zero must not write MXCSR at all: a flag raised
    // inside it is still there after it, and so is one raised in the outer scope before it.
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(saved & ~0x603Fu);
    {
        const rrv::fp::ScopedRoundTowardZero outer;
        divideBits(1.0f, 3.0f);
        const unsigned flagsBefore = _mm_getcsr() & 0x3Fu;
        {
            const rrv::fp::ScopedRoundTowardZero inner;
            divideBits(2.0f, 3.0f);
        }
        expect((_mm_getcsr() & 0x6000u) == 0x6000u, "outer mode survives the inner scope");
        expect((_mm_getcsr() & 0x3Fu) == flagsBefore, "the inner scope left the flags alone");
    }
    _mm_setcsr(saved);
}
#endif

template <class F>
double nsPerIteration(F &&body, long iterations)
{
    const auto start = std::chrono::steady_clock::now();
    for (long i = 0; i < iterations; ++i)
        body();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(iterations);
}

void bench()
{
    constexpr long kIterations = 20'000'000;
    volatile uint32_t sink = 0;
    // The old pattern in ps2_vu1.cpp, ScopedVu0Rounding and mulMatVu0: fegetround, fesetround, fesetround.
    const double libm = nsPerIteration(
        [&] {
            const int previous = std::fegetround();
            std::fesetround(FE_TOWARDZERO);
            sink = sink + 1u;
            std::fesetround(previous);
        },
        kIterations);
    const double scoped = nsPerIteration(
        [&] {
            const rrv::fp::ScopedRoundTowardZero scope;
            sink = sink + 1u;
        },
        kIterations);
    // A nested scope (a libvu0 wrapper around a VU0 microprogram): the inner one finds the mode already set.
    const double nested = nsPerIteration(
        [&] {
            const rrv::fp::ScopedRoundTowardZero outer;
            const rrv::fp::ScopedRoundTowardZero inner;
            sink = sink + 1u;
        },
        kIterations);
    std::printf("bench (%ld iterations each, this host, not a Steam Deck):\n", kIterations);
    std::printf("  fegetround + fesetround x2 : %7.2f ns per enter+leave\n", libm);
    std::printf("  rrv::fp scope              : %7.2f ns per enter+leave  (%.1fx faster)\n", scoped, libm / scoped);
    std::printf("  rrv::fp nested pair        : %7.2f ns per outer+inner enter+leave\n", nested);
}
} // namespace

int main(int argc, char **argv)
{
    testRoundsLikeFesetround();
    testRestoresPreviousMode();
    testDisabledAndNested();
#if defined(__x86_64__)
    testOtherMxcsrBitsUntouched();
    testNestedScopeWritesNothing();
#endif
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fp rounding: all checks passed");
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0)
        bench();
    return 0;
}
