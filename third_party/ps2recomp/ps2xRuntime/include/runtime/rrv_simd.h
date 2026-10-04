#ifndef RRV_SIMD_H
#define RRV_SIMD_H

// rrv_simd.h — host-agnostic 128-bit / 4-lane SIMD abstraction.
//
// WHY THIS EXISTS
//   The VU1 interpreter is the frame-time gate (measured 2026-08-21: 13.2 ms of
//   a 33.3 ms frame, 13-16 ns per instruction slot, ~1.1 M slots per display
//   list).  Every VU FMAC is a 4-lane float operation, so the interpreter wants
//   SIMD — but this port targets macOS/ARM64, Windows/x86-64 and Linux/x86-64,
//   and VU semantics must be identical on all three.  So NO host intrinsic ever
//   appears in VU logic: the VU code is written against this vocabulary, and
//   exactly one of the three backends below implements it.
//
//   Backends: NEON (AArch64) · SSE2 (x86-64) · portable scalar.
//   The scalar backend is the SEMANTIC REFERENCE.  If a vector backend cannot
//   express an operation with bit-identical results, the operation does not
//   belong in this header.
//
// SEMANTIC RULES (these are correctness, not style)
//
//   1. NO FUSED MULTIPLY-ADD.  The VU's FMAC rounds the multiply before the
//      add; an FMA does not, and that difference reaches the GS as different
//      vertices.  ps2_vu1.cpp is compiled -ffp-contract=off for exactly this
//      reason (ps2xRuntime/CMakeLists.txt).  This header therefore offers
//      mul() and add() and deliberately offers no fma(): intrinsics are never
//      contracted by the compiler, so composing them stays unfused on every
//      host.
//
//   2. max_c/min_c ARE NOT THE HOST'S max/min.  The VU interpreter's rule is
//      C's `(a > b) ? a : b`.  x86 MAXPS happens to match it, but AArch64
//      FMAXNM/vmaxq_f32 does not: it differs on NaN and on the +0/-0 pair.
//      Both are implemented here as cmpgt + select, which is the C rule
//      exactly, on every backend, for two extra ops.
//      On AArch64 the operands are also hidden from the optimiser: when one of
//      them is a known constant (a compiled VU block knows VF0), LLVM rewrites
//      the compare + select as FMAXNM/FMINNM, which it may do because it treats
//      every NaN as quiet. The instruction does not: for a SIGNALLING NaN it
//      returns a NaN where the C rule returns the other operand (observed
//      2026-10-02: 21 FMAXNM sites in the compiled blocks, and compiled blocks
//      differing from the interpreter only on signalling-NaN inputs;
//      tests/vu_lean_diff_tests.cpp, blockDiff).
//
//   3. ROUNDING MODE IS THE HOST FPU's.  VU1 runs under FE_TOWARDZERO
//      (ScopedVuRounding in ps2_vu1.cpp).  AArch64 ASIMD honours FPCR and SSE
//      honours MXCSR, so vector arithmetic sees the same rounding mode the
//      scalar code saw.  Nothing here sets or assumes a mode.
//
//   4. cvt_f32_to_s32_trunc IS HOST-DEPENDENT FOR OUT-OF-RANGE INPUTS, and
//      that is deliberate: AArch64 FCVTZS saturates, x86 CVTTPS2DQ yields
//      0x80000000, and those are exactly what `(int32_t)f` already compiled to
//      on each host in the scalar interpreter.  This op preserves each host's
//      existing behaviour rather than inventing a new one.  (The PS2's own
//      FTOI saturates; making that uniform is a separate, semantic change and
//      must be measured against PCSX2, not smuggled in here.)
//
// LANE ORDER
//   Lane 0 is x, lane 3 is w, matching float vf[4] in memory.  mask_bits()
//   returns lane i in bit i; mask_bits_rev() returns lane i in bit (3-i),
//   which is the order the VU's MAC/dest fields use (x is the HIGH bit).

#include <cstdint>
#include <cstring>

// Fast-math re-associates and contracts float arithmetic; either one changes VU
// results.  A TU that includes this header must not be built with it.
#if defined(__FAST_MATH__)
#  error "rrv_simd.h: -ffast-math changes VU float semantics; build this TU without it"
#endif

// The scalar fallback's arithmetic is marked contract(off) below, so a compiler
// running at -ffp-contract=fast cannot fuse a mul() with a following add() after
// inlining.  The vector backends use intrinsics, which are never contracted.
#if defined(__clang__)
#  define RRV_SIMD_NO_CONTRACT _Pragma("clang fp contract(off)")
#else
#  define RRV_SIMD_NO_CONTRACT
#endif

// RRV_SIMD_FORCE_SCALAR selects the portable fallback on any host.  It exists
// so the conformance test can exercise the fallback on the developer's own
// machine (tests/vu_simd_tests.cpp is built twice), not as a runtime option.
#if defined(RRV_SIMD_FORCE_SCALAR)
#  define RRV_SIMD_SCALAR 1
#  define RRV_SIMD_BACKEND_NAME "scalar"
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#  define RRV_SIMD_NEON 1
#  include <arm_neon.h>
#  define RRV_SIMD_BACKEND_NAME "neon"
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define RRV_SIMD_SSE2 1
#  include <emmintrin.h>
#  define RRV_SIMD_BACKEND_NAME "sse2"
#else
#  define RRV_SIMD_SCALAR 1
#  define RRV_SIMD_BACKEND_NAME "scalar"
#endif

#if defined(_MSC_VER)
#  define RRV_SIMD_INLINE __forceinline
#else
#  define RRV_SIMD_INLINE inline __attribute__((always_inline))
#endif

namespace rrv::simd
{

#if defined(RRV_SIMD_NEON)
    using f32x4_t = float32x4_t;
    using u32x4_t = uint32x4_t;
#elif defined(RRV_SIMD_SSE2)
    using f32x4_t = __m128;
    using u32x4_t = __m128i;
#else
    struct f32x4_t { float v[4]; };
    struct u32x4_t { uint32_t v[4]; };
#endif

// Wrapper structs keep f32x4 and u32x4 distinct types on every backend (on SSE2
// the native float/int vectors are already distinct; on NEON they are too; the
// scalar fallback needs the help).  They are trivially copyable and the
// compilers keep them in registers.
struct f32x4
{
    f32x4_t v;
};
struct u32x4
{
    u32x4_t v;
};

// ---------------------------------------------------------------- load/store

RRV_SIMD_INLINE f32x4 loadu(const float *p)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vld1q_f32(p)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_loadu_ps(p)};
#else
    f32x4 r;
    std::memcpy(r.v.v, p, 16);
    return r;
#endif
}

RRV_SIMD_INLINE void storeu(float *p, f32x4 a)
{
#if defined(RRV_SIMD_NEON)
    vst1q_f32(p, a.v);
#elif defined(RRV_SIMD_SSE2)
    _mm_storeu_ps(p, a.v);
#else
    std::memcpy(p, a.v.v, 16);
#endif
}

RRV_SIMD_INLINE u32x4 loadu_u(const uint32_t *p)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vld1q_u32(p)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_loadu_si128(reinterpret_cast<const __m128i *>(p))};
#else
    u32x4 r;
    std::memcpy(r.v.v, p, 16);
    return r;
#endif
}

RRV_SIMD_INLINE void storeu_u(uint32_t *p, u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    vst1q_u32(p, a.v);
#elif defined(RRV_SIMD_SSE2)
    _mm_storeu_si128(reinterpret_cast<__m128i *>(p), a.v);
#else
    std::memcpy(p, a.v.v, 16);
#endif
}

RRV_SIMD_INLINE f32x4 splat(float x)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vdupq_n_f32(x)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_set1_ps(x)};
#else
    return f32x4{{x, x, x, x}};
#endif
}

RRV_SIMD_INLINE u32x4 splat_u(uint32_t x)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vdupq_n_u32(x)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_set1_epi32(static_cast<int>(x))};
#else
    return u32x4{{x, x, x, x}};
#endif
}

RRV_SIMD_INLINE f32x4 zero_f() { return splat(0.0f); }

// Broadcast one lane of a 4-float array in memory.  The VU's `bc` field selects
// the lane at runtime, so this takes the index as a value, not a template
// parameter, and both vector backends resolve it as a scalar load + duplicate.
RRV_SIMD_INLINE f32x4 dup_lane(const float *base, unsigned lane)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vld1q_dup_f32(base + (lane & 3u))};
#else
    return splat(base[lane & 3u]);
#endif
}

// --------------------------------------------------------------- reinterpret

RRV_SIMD_INLINE u32x4 as_u(f32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vreinterpretq_u32_f32(a.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_castps_si128(a.v)};
#else
    u32x4 r;
    std::memcpy(r.v.v, a.v.v, 16);
    return r;
#endif
}

RRV_SIMD_INLINE f32x4 as_f(u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vreinterpretq_f32_u32(a.v)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_castsi128_ps(a.v)};
#else
    f32x4 r;
    std::memcpy(r.v.v, a.v.v, 16);
    return r;
#endif
}

// ---------------------------------------------------------------- arithmetic
// Plain IEEE-754 single-precision operations under the current rounding mode.
// See rule 1: there is no fma() here, on purpose.

RRV_SIMD_INLINE f32x4 add(f32x4 a, f32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vaddq_f32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_add_ps(a.v, b.v)};
#else
    RRV_SIMD_NO_CONTRACT
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] + b.v.v[i];
    return r;
#endif
}

RRV_SIMD_INLINE f32x4 sub(f32x4 a, f32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vsubq_f32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_sub_ps(a.v, b.v)};
#else
    RRV_SIMD_NO_CONTRACT
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] - b.v.v[i];
    return r;
#endif
}

RRV_SIMD_INLINE f32x4 mul(f32x4 a, f32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vmulq_f32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_mul_ps(a.v, b.v)};
#else
    RRV_SIMD_NO_CONTRACT
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] * b.v.v[i];
    return r;
#endif
}

RRV_SIMD_INLINE f32x4 div(f32x4 a, f32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vdivq_f32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_div_ps(a.v, b.v)};
#else
    RRV_SIMD_NO_CONTRACT
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] / b.v.v[i];
    return r;
#endif
}

// ----------------------------------------------------------------- compares
// All compares return a lane mask: 0xFFFFFFFF where true, 0 where false.
// Float compares are ORDERED (false if either operand is NaN), matching C's
// `>` and `<`.

RRV_SIMD_INLINE u32x4 cmpgt(f32x4 a, f32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vcgtq_f32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_castps_si128(_mm_cmpgt_ps(a.v, b.v))};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = (a.v.v[i] > b.v.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

RRV_SIMD_INLINE u32x4 cmplt(f32x4 a, f32x4 b) { return cmpgt(b, a); }

RRV_SIMD_INLINE u32x4 cmpeq_u(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vceqq_u32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_cmpeq_epi32(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = (a.v.v[i] == b.v.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

// Signed 32-bit compare on the bit patterns — the VU's CLIP rule compares
// sign-magnitude patterns as s32 (PCSX2 _vuCLIP).
RRV_SIMD_INLINE u32x4 cmpgt_s(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vcgtq_s32(vreinterpretq_s32_u32(a.v), vreinterpretq_s32_u32(b.v))};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_cmpgt_epi32(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i)
        r.v.v[i] = (static_cast<int32_t>(a.v.v[i]) > static_cast<int32_t>(b.v.v[i])) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

// ------------------------------------------------------------------ bitwise

RRV_SIMD_INLINE u32x4 and_u(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vandq_u32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_and_si128(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] & b.v.v[i];
    return r;
#endif
}

RRV_SIMD_INLINE u32x4 or_u(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vorrq_u32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_or_si128(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] | b.v.v[i];
    return r;
#endif
}

RRV_SIMD_INLINE u32x4 xor_u(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{veorq_u32(a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_xor_si128(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] ^ b.v.v[i];
    return r;
#endif
}

// (~a) & b
RRV_SIMD_INLINE u32x4 andnot_u(u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vbicq_u32(b.v, a.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_andnot_si128(a.v, b.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = (~a.v.v[i]) & b.v.v[i];
    return r;
#endif
}

template <int N>
RRV_SIMD_INLINE u32x4 shr_u(u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vshrq_n_u32(a.v, N)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_srli_epi32(a.v, N)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] >> N;
    return r;
#endif
}

template <int N>
RRV_SIMD_INLINE u32x4 shl_u(u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vshlq_n_u32(a.v, N)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_slli_epi32(a.v, N)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = a.v.v[i] << N;
    return r;
#endif
}

// --------------------------------------------------------------- select/mask

// mask ? a : b, bitwise, per lane.  `mask` lanes must be all-ones or all-zero.
RRV_SIMD_INLINE u32x4 select_u(u32x4 mask, u32x4 a, u32x4 b)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vbslq_u32(mask.v, a.v, b.v)};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_or_si128(_mm_and_si128(mask.v, a.v), _mm_andnot_si128(mask.v, b.v))};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = (a.v.v[i] & mask.v.v[i]) | (b.v.v[i] & ~mask.v.v[i]);
    return r;
#endif
}

RRV_SIMD_INLINE f32x4 select(u32x4 mask, f32x4 a, f32x4 b)
{
    return as_f(select_u(mask, as_u(a), as_u(b)));
}

// Lane i -> bit i.
RRV_SIMD_INLINE uint32_t mask_bits(u32x4 m)
{
#if defined(RRV_SIMD_NEON)
    static const uint32_t kW[4] = {1u, 2u, 4u, 8u};
    return vaddvq_u32(vandq_u32(m.v, vld1q_u32(kW)));
#elif defined(RRV_SIMD_SSE2)
    return static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(m.v)));
#else
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (m.v.v[i] & 1u) << i;
    return r;
#endif
}

// Lane i -> bit (3-i).  This is the VU's own order: x is the high bit of a
// dest mask (x=8, y=4, z=2, w=1) and of every MAC nibble.
RRV_SIMD_INLINE uint32_t mask_bits_rev(u32x4 m)
{
#if defined(RRV_SIMD_NEON)
    static const uint32_t kW[4] = {8u, 4u, 2u, 1u};
    return vaddvq_u32(vandq_u32(m.v, vld1q_u32(kW)));
#elif defined(RRV_SIMD_SSE2)
    static const uint8_t kRev[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    return kRev[_mm_movemask_ps(_mm_castsi128_ps(m.v)) & 15];
#else
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (m.v.v[i] & 1u) << (3 - i);
    return r;
#endif
}

// Sum of the four lanes, as a scalar.
//
// Exists so a caller that needs SEVERAL lane->bit reductions of the same
// vectors can do ONE vector-to-GPR transfer instead of one per mask. Weight
// each mask vector with the bit positions it owns, OR the weighted vectors
// together, and reduce once: when the weights are disjoint the sum IS the OR.
// The four mask_bits_rev() calls in the VU's MAC update were four separate
// `addv` + vector-to-GPR moves per FMAC, which is the most expensive kind of
// operation to repeat on a NEON pipeline.
RRV_SIMD_INLINE uint32_t reduce_add_u32(u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return vaddvq_u32(a.v);
#elif defined(RRV_SIMD_SSE2)
    __m128i t = _mm_add_epi32(a.v, _mm_shuffle_epi32(a.v, _MM_SHUFFLE(1, 0, 3, 2)));
    t = _mm_add_epi32(t, _mm_shuffle_epi32(t, _MM_SHUFFLE(2, 3, 0, 1)));
    return static_cast<uint32_t>(_mm_cvtsi128_si32(t));
#else
    return a.v.v[0] + a.v.v[1] + a.v.v[2] + a.v.v[3];
#endif
}

// A lane mask built from a VU dest field (bit 3 = x ... bit 0 = w).
RRV_SIMD_INLINE u32x4 dest_mask(uint8_t dest)
{
    // 16 masks, indexed by the 4-bit dest field.  A table load beats four
    // compares and is the same cost on both vector backends.
    alignas(16) static const uint32_t kTable[16][4] = {
        {0u, 0u, 0u, 0u},                                     // ....
        {0u, 0u, 0u, 0xFFFFFFFFu},                            // ...w
        {0u, 0u, 0xFFFFFFFFu, 0u},                            // ..z.
        {0u, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu},                   // ..zw
        {0u, 0xFFFFFFFFu, 0u, 0u},                            // .y..
        {0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu},                   // .y.w
        {0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u},                   // .yz.
        {0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},          // .yzw
        {0xFFFFFFFFu, 0u, 0u, 0u},                            // x...
        {0xFFFFFFFFu, 0u, 0u, 0xFFFFFFFFu},                   // x..w
        {0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u},                   // x.z.
        {0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu},          // x.zw
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u},                   // xy..
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu},          // xy.w
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u},          // xyz.
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, // xyzw
    };
    return loadu_u(kTable[dest & 15]);
}

// ------------------------------------------------------------ min/max (rule 2)

// Rule 2: keeps LLVM from turning the compare + select below into FMAXNM/FMINNM.
RRV_SIMD_INLINE void opaque_operands(f32x4 &a, f32x4 &b)
{
#if defined(RRV_SIMD_NEON) && (defined(__clang__) || defined(__GNUC__))
    __asm__ volatile("" : "+w"(a.v), "+w"(b.v));
#else
    (void)a;
    (void)b;
#endif
}

RRV_SIMD_INLINE f32x4 max_c(f32x4 a, f32x4 b)
{
    opaque_operands(a, b);
    return select(cmpgt(a, b), a, b);
}
RRV_SIMD_INLINE f32x4 min_c(f32x4 a, f32x4 b)
{
    opaque_operands(a, b);
    return select(cmplt(a, b), a, b);
}

RRV_SIMD_INLINE f32x4 abs_f(f32x4 a)
{
    return as_f(and_u(as_u(a), splat_u(0x7FFFFFFFu)));
}

// ---------------------------------------------------------------- conversion
// See rule 4 for the out-of-range behaviour of the float->int direction.

RRV_SIMD_INLINE f32x4 cvt_s32_to_f32(u32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return f32x4{vcvtq_f32_s32(vreinterpretq_s32_u32(a.v))};
#elif defined(RRV_SIMD_SSE2)
    return f32x4{_mm_cvtepi32_ps(a.v)};
#else
    f32x4 r;
    for (int i = 0; i < 4; ++i) r.v.v[i] = static_cast<float>(static_cast<int32_t>(a.v.v[i]));
    return r;
#endif
}

RRV_SIMD_INLINE u32x4 cvt_f32_to_s32_trunc(f32x4 a)
{
#if defined(RRV_SIMD_NEON)
    return u32x4{vreinterpretq_u32_s32(vcvtq_s32_f32(a.v))};
#elif defined(RRV_SIMD_SSE2)
    return u32x4{_mm_cvttps_epi32(a.v)};
#else
    u32x4 r;
    for (int i = 0; i < 4; ++i)
        r.v.v[i] = static_cast<uint32_t>(static_cast<int32_t>(a.v.v[i]));
    return r;
#endif
}

} // namespace rrv::simd

#endif // RRV_SIMD_H
