// rrv_hle::vu_exact (src/hle/rrv_vu_exact.h) against the formulation it replaces: comparisons on the
// float values and std::nextafterf for the step toward zero. Results are compared bit for bit.
// Asset-free: no game data. `--bench` also prints the cost of one sceVu0MulMatrix body, old and new.
//
// One case is the compiler's, not the source's: when two operands of one multiply or add are NaN, the
// result is the instruction's first operand, and the compiler chooses the order. A result that is a NaN on
// both sides but not the same NaN is counted and reported, not failed, on every compiler: it agreed on the
// author's Mac with Apple clang (ARM64) and in a Linux container (x86-64), but not with the clang of
// GitHub's macOS ARM64 runner (24,049 of 371,565,132 checks, the printed ones all NaN against another NaN,
// 2026-10-05) nor with g++ 13 on x86-64 (57,112 checks at -O2). Every other difference still fails.
#include "rrv_vu_exact.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ref {
constexpr float kSmallestNormalF32 = 1.17549435082228750797e-38f;
float flushF32(float v) {
    return (v != 0.0f && std::fabs(v) < kSmallestNormalF32) ? std::copysignf(0.0f, v) : v;
}
float truncToF32(double v) {
    const float nearest = static_cast<float>(v);
    if (!std::isfinite(nearest))
        return nearest;
    if (static_cast<double>(nearest) == v)
        return flushF32(nearest);
    const bool overshot = v >= 0.0 ? (static_cast<double>(nearest) > v) : (static_cast<double>(nearest) < v);
    return flushF32(overshot ? std::nextafterf(nearest, 0.0f) : nearest);
}
float vuMul(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) * static_cast<double>(flushF32(b))); }
float vuAdd(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) + static_cast<double>(flushF32(b))); }
float vuSub(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) - static_cast<double>(flushF32(b))); }
float vuSqrt(float a) { return truncToF32(std::sqrt(std::fabs(static_cast<double>(flushF32(a))))); }
float vuDiv(float a, float b) {
    a = flushF32(a);
    b = flushF32(b);
    if (b == 0.0f)
        return std::copysignf(3.4028234663852886e+38f, a * b);
    return truncToF32(static_cast<double>(a) / static_cast<double>(b));
}
void mulMatVu0Exact(const float (&columns)[16], const float (&rows)[16], float (&out)[16]) {
    for (int i = 0; i < 4; ++i) {
        const float* v = rows + 4 * i;
        for (int j = 0; j < 4; ++j) {
            float acc = vuMul(columns[j], v[0]);
            acc = vuAdd(acc, vuMul(columns[4 + j], v[1]));
            acc = vuAdd(acc, vuMul(columns[8 + j], v[2]));
            acc = vuAdd(acc, vuMul(columns[12 + j], v[3]));
            out[4 * i + j] = acc;
        }
    }
}
} // namespace ref

namespace {
using rrv_hle::vu_exact::bitsOf;
using rrv_hle::vu_exact::floatOf;

uint64_t g_checks = 0, g_failures = 0, g_nanChoice = 0;
constexpr bool kNanChoiceIsChecked = false;
bool isNan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
uint64_t g_seed = 0x9E3779B97F4A7C15ull;
uint64_t next64() {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}
double doubleOf(uint64_t u) { double d; std::memcpy(&d, &u, sizeof(d)); return d; }
uint64_t bitsOf64(double d) { uint64_t u; std::memcpy(&u, &d, sizeof(u)); return u; }

void expect(const char* what, uint32_t got, uint32_t want, uint64_t a, uint64_t b) {
    ++g_checks;
    if (got == want) return;
    if (!kNanChoiceIsChecked && isNan(got) && isNan(want)) { ++g_nanChoice; return; }
    if (++g_failures <= 20)
        std::fprintf(stderr, "FAIL %s: got %08x want %08x (inputs %016llx %016llx)\n", what, got, want,
                     (unsigned long long)a, (unsigned long long)b);
}

// Zeros, the denormal range and its edges, the float32 range's edges, infinities, NaNs, ordinary values.
const uint32_t kMagnitudes[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00400000u, 0x007FFFFFu, 0x00800000u, 0x00800001u, 0x00FFFFFFu,
    0x01000000u, 0x33800000u, 0x34000000u, 0x3F000000u, 0x3F7FFFFFu, 0x3F800000u, 0x3F800001u, 0x40490FDBu,
    0x4B800000u, 0x4B7FFFFFu, 0x5F000000u, 0x7E800000u, 0x7F000000u, 0x7F7FFFFEu, 0x7F7FFFFFu, 0x7F800000u,
    0x7F800001u, 0x7FC00000u, 0x7FFFFFFFu};
std::vector<uint32_t> specials() {
    std::vector<uint32_t> out;
    for (uint32_t m : kMagnitudes) { out.push_back(m); out.push_back(m | 0x80000000u); }
    return out;
}

// A random float32: any bit pattern, or one whose exponent is close to `near`'s (sums that cancel,
// products that land in the denormal range).
uint32_t randomBits(uint32_t near, unsigned mode) {
    const uint64_t r = next64();
    if (mode == 0) return static_cast<uint32_t>(r);
    const int32_t exponent = static_cast<int32_t>((near >> 23) & 0xFFu) + static_cast<int32_t>((r >> 40) % 61u) - 30;
    const uint32_t e = static_cast<uint32_t>(exponent < 0 ? 0 : exponent > 255 ? 255 : exponent);
    return (static_cast<uint32_t>(r) & 0x807FFFFFu) | (e << 23);
}

void checkPair(uint32_t ua, uint32_t ub) {
    using namespace rrv_hle::vu_exact;
    const float a = floatOf(ua), b = floatOf(ub);
    expect("vuMul", bitsOf(vuMul(a, b)), bitsOf(ref::vuMul(a, b)), ua, ub);
    expect("vuAdd", bitsOf(vuAdd(a, b)), bitsOf(ref::vuAdd(a, b)), ua, ub);
    expect("vuSub", bitsOf(vuSub(a, b)), bitsOf(ref::vuSub(a, b)), ua, ub);
    expect("vuDiv", bitsOf(vuDiv(a, b)), bitsOf(ref::vuDiv(a, b)), ua, ub);
}
void checkOne(uint32_t u) {
    using namespace rrv_hle::vu_exact;
    const float v = floatOf(u);
    expect("flushF32", bitsOf(flushF32(v)), bitsOf(ref::flushF32(v)), u, 0);
    expect("vuSqrt", bitsOf(vuSqrt(v)), bitsOf(ref::vuSqrt(v)), u, 0);
}
void checkDouble(uint64_t u) {
    const double d = doubleOf(u);
    expect("truncToF32", bitsOf(rrv_hle::vu_exact::truncToF32(d)), bitsOf(ref::truncToF32(d)), u, 0);
}

void fillMatrix(float (&m)[16], const std::vector<uint32_t>& sp, unsigned mode) {
    uint32_t prev = 0x3F800000u;
    for (float& f : m) {
        const uint64_t r = next64();
        // Mostly ordinary matrix values (rotations, translations), some anything, some special.
        uint32_t u;
        if (mode == 0) u = (r & 7u) == 0 ? sp[(r >> 8) % sp.size()] : randomBits(prev, static_cast<unsigned>(r >> 3) & 1u);
        else u = bitsOf(static_cast<float>(static_cast<int32_t>(r >> 16) % 4096) / 64.0f);
        f = floatOf(u);
        prev = u;
    }
}
} // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    const std::vector<uint32_t> sp = specials();

    for (uint32_t a : sp) {
        checkOne(a);
        for (uint32_t b : sp) checkPair(a, b);
    }
    // Every float32 next to a power of two and next to the denormal boundary.
    for (uint32_t e = 0; e < 256u; ++e)
        for (int32_t d = -3; d <= 3; ++d) {
            const uint32_t u = (e << 23) + static_cast<uint32_t>(d);
            checkOne(u); checkOne(u | 0x80000000u);
            checkPair(u, 0x3F7FFFFFu); checkPair(u | 0x80000000u, u ^ 1u);
        }
    for (uint32_t i = 0; i < 20000000u; ++i) {
        const uint32_t a = randomBits(0, 0), b = randomBits(a, i & 1u);
        checkPair(a, b);
        checkPair(a, sp[i % sp.size()]);
        checkOne(a);
    }
    // truncToF32 on doubles that are not products or sums of two floats: any bit pattern, halfway
    // points between two float32 values, and the neighbourhood of the float32 range's edges.
    for (uint32_t i = 0; i < 20000000u; ++i) {
        checkDouble(next64());
        const uint32_t f = static_cast<uint32_t>(next64());
        const double exact = static_cast<double>(floatOf(f));
        if (std::isfinite(exact)) {
            const uint64_t eb = bitsOf64(exact);
            checkDouble(eb); checkDouble(eb + 1u); checkDouble(eb - 1u);
            checkDouble(eb + (uint64_t(1) << 28)); checkDouble(eb + (uint64_t(1) << 28) + 1u); checkDouble(eb + (uint64_t(1) << 28) - 1u);
        }
    }
    for (double edge : {3.4028234663852886e+38, 3.4028235677973366e+38, 1.17549435082228750797e-38, 1.4012984643248171e-45,
                        7.0064923216240854e-46, 0.0})
        for (int64_t d = -4; d <= 4; ++d) {
            checkDouble(bitsOf64(edge) + static_cast<uint64_t>(d));
            checkDouble((bitsOf64(edge) + static_cast<uint64_t>(d)) | 0x8000000000000000ull);
        }

    for (uint32_t i = 0; i < 2000000u; ++i) {
        float a[16], b[16], got[16], want[16];
        fillMatrix(a, sp, i & 1u);
        fillMatrix(b, sp, (i >> 1) & 1u);
        rrv_hle::vu_exact::mulMatVu0Exact(a, b, got);
        ref::mulMatVu0Exact(a, b, want);
        for (int k = 0; k < 16; ++k)
            expect("mulMatVu0Exact", bitsOf(got[k]), bitsOf(want[k]), i, static_cast<uint64_t>(k));
    }

    if (bench) {
        std::vector<float> data(16u * 2u * 4096u);
        float m[16];
        for (size_t i = 0; i < data.size(); i += 16) { fillMatrix(m, sp, 1); std::memcpy(&data[i], m, sizeof(m)); }
        for (int which = 0; which < 2; ++which) {
            uint32_t sink = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 500; ++rep)
                for (size_t i = 0; i + 32 <= data.size(); i += 32) {
                    float a[16], b[16], out[16];
                    std::memcpy(a, &data[i], sizeof(a)); std::memcpy(b, &data[i + 16], sizeof(b));
                    if (which == 0) ref::mulMatVu0Exact(a, b, out); else rrv_hle::vu_exact::mulMatVu0Exact(a, b, out);
                    sink += bitsOf(out[5]);
                }
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%s: %.1f ns per matrix multiply (sink %08x)\n", which == 0 ? "comparison + nextafterf" : "rrv_vu_exact.h",
                        ns / (500.0 * 4096.0), sink);
        }
    }

    if (!kNanChoiceIsChecked)
        std::printf("vu_exact: %llu results are a different NaN (the compiler's operand order; not checked)\n",
                    (unsigned long long)g_nanChoice);
    std::printf("vu_exact: %llu checks, %llu failures\n", (unsigned long long)g_checks, (unsigned long long)g_failures);
    return g_failures == 0 ? 0 : 1;
}
