// rrv_vu_exact.h -- VU0 arithmetic for the libvu0 host implementations (vu0_math.cpp), without
// depending on the FP environment.
//
// PCSX2's DEFAULT_VU_FP_CONTROL_REGISTER is DAZ + FTZ + ChopZero, so every VU
// arithmetic result truncates toward zero. Scoping `fesetround` around host
// code does NOT reliably deliver that: without `#pragma STDC FENV_ACCESS ON`
// the compiler may fold, hoist or reorder FP operations across the call, and
// measurement showed exactly that (one call site matched hardware, another 1-2
// ULP away, same function, same scope). So do the rounding explicitly.
//
// A float32 multiply or add is exact in double, and double is wide enough that
// a correctly-rounded double sqrt/divide of float32 operands determines the
// float32 result. Truncating that double toward zero is therefore the VU's
// result, computed identically on every compiler and optimisation level.
// The VU has no denormals: DenormalsAreZero on the way in, FlushToZero on the
// way out. Measured, not assumed — without it the phase-7 source matrix keeps a
// 0x00000002 where hardware has +0.
//
// The helpers work on the bit patterns and have no branches and no library call: sceVu0MulMatrix
// alone is 112 of them, and it was 6.5% of the game thread in an 8-car race (host `sample`,
// 2026-10-04) while the step back toward zero was a call to nextafterf. tests/vu_exact_tests.cpp
// compares every helper with the comparison-and-nextafterf formulation it replaces.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rrv_hle::vu_exact {

inline uint32_t bitsOf(float v) { uint32_t u; std::memcpy(&u, &v, sizeof(u)); return u; }
inline float floatOf(uint32_t u) { float v; std::memcpy(&v, &u, sizeof(v)); return v; }

// Zero and denormals become a zero of the same sign; everything else (NaN and infinity included) is kept.
inline uint32_t flushBits(uint32_t u) { return (u & 0x7FFFFFFFu) < 0x00800000u ? (u & 0x80000000u) : u; }
inline float flushF32(float v) { return floatOf(flushBits(bitsOf(v))); }

// The float32 nearest to `v`, moved one step toward zero when rounding went away from it, then
// flushed. A NaN or an infinity is returned as it is. One step toward zero of a finite, nonzero
// float32 is its bit pattern minus one, and a value that overshot is never zero.
inline float truncToF32(double v) {
    const float nearest = static_cast<float>(v);
    const uint32_t u = bitsOf(nearest);
    const bool finite = (u & 0x7FFFFFFFu) < 0x7F800000u;
    const bool overshot = std::fabs(static_cast<double>(nearest)) > std::fabs(v);
    return finite ? floatOf(flushBits(u - ((overshot ? 1u : 0u)))) : nearest;
}

inline float vuMul(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) * static_cast<double>(flushF32(b))); }
inline float vuAdd(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) + static_cast<double>(flushF32(b))); }
inline float vuSub(float a, float b) { return truncToF32(static_cast<double>(flushF32(a)) - static_cast<double>(flushF32(b))); }
inline float vuSqrt(float a) { return truncToF32(std::sqrt(std::fabs(static_cast<double>(flushF32(a))))); }

// VDIV: PS2 yields +/-Fmax on a zero divisor (PCSX2 microVU_Lower.inl mVU_DIV
// ORs in maxvals), it does not trap or produce an infinity.
inline float vuDiv(float a, float b) {
    a = flushF32(a);
    b = flushF32(b);
    if (b == 0.0f)
        return std::copysignf(3.4028234663852886e+38f, a * b);
    return truncToF32(static_cast<double>(a) / static_cast<double>(b));
}

// Exact transcription of the generated sceVu0MulMatrix body (0x2CA250):
//
//   lqc2 vf4..vf7, 0x00/0x10/0x20/0x30($a1)   the four columns
//   loop x4:
//     lqc2      vf8, 0($a2)                   one row of coefficients
//     vmulax.xyzw  ACC, vf4, vf8x
//     vmadday.xyzw ACC, vf5, vf8y
//     vmaddaz.xyzw ACC, vf6, vf8z
//     vmaddw.xyzw  vf9, vf7, vf8w
//     sqc2      vf9, 0($a0)
//
// Round-toward-zero after every multiply and every add, in this order. That
// ordering is guest-visible: it is what produces hardware's negative zero when
// a column term is signed and the rest are zero.
//
// Which NaN comes out when two operands of one multiply or add are NaN is the first operand of the
// machine instruction, and a compiler is free to hand a commutative instruction its operands in either
// order. So the loop keeps, per architecture, a shape that tests/vu_exact_tests.cpp shows equal to the
// formulation this replaces on that architecture:
//   * ARM64: the four lanes of a row go through each step together (the lanes do not depend on each
//     other), which lets the compiler use vector instructions: 232 -> 139 ns per multiply on an M-series
//     Mac. Each lane's operations and their order are the ones listed above.
//   * elsewhere: one lane at a time, as before (232 -> 207 ns on the same Mac). On x86-64 the row-wise
//     form gave the other NaN in 32,791 of 32 M matrix elements (clang, Linux container, 2026-10-05);
//     spelling the choice out per operation ("the first operand") matched even fewer, because the scalar
//     code's own choice there is the compiler's.
inline void mulMatVu0Exact(const float (&columns)[16], const float (&rows)[16], float (&out)[16]) {
    for (int i = 0; i < 4; ++i) {
        const float* v = rows + 4 * i;
#if defined(__aarch64__)
        float acc[4];
        for (int j = 0; j < 4; ++j)
            acc[j] = vuMul(columns[j], v[0]);
        for (int k = 1; k < 4; ++k)
            for (int j = 0; j < 4; ++j)
                acc[j] = vuAdd(acc[j], vuMul(columns[4 * k + j], v[k]));
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = acc[j];
#else
        for (int j = 0; j < 4; ++j) {
            float acc = vuMul(columns[j], v[0]);
            acc = vuAdd(acc, vuMul(columns[4 + j], v[1]));
            acc = vuAdd(acc, vuMul(columns[8 + j], v[2]));
            acc = vuAdd(acc, vuMul(columns[12 + j], v[3]));
            out[4 * i + j] = acc;
        }
#endif
    }
}

} // namespace rrv_hle::vu_exact
