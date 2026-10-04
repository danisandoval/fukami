// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Contains logic adapted from PCSX2 (GPL-3.0+); the upstream revision, file and function of each adaptation
// are cited at the adapted code and in third_party/ps2recomp/RRV_CHANGES.md. Rest: PS2Recomp (GPL-3.0).
#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// RRV_EE_FPCLAMP (scripts/ee_fpclamp_overlay.py) -- the PS2 has no Inf/NaN.
// VU0 macro mode follows PCSX2 pcsx2/VUops.cpp and COP1 compares follow
// pcsx2/FPU.cpp (both @ d5f75c9e4, GPL-3.0). DEFAULT ON; RRV_EE_FPCLAMP=0
// restores the raw host expressions exactly.
#include <cstdlib>
inline const bool g_ps2EeFpClamp = [] {
    const char *v = std::getenv("RRV_EE_FPCLAMP");
    return !(v && v[0] == '0' && v[1] == '\0');
}();

inline uint32_t ps2VuBits(float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; }
inline float ps2VuFromBits(uint32_t u) { float f; std::memcpy(&f, &u, sizeof(f)); return f; }

// vuDouble()'s overflow rule, four lanes: exponent 255 -> sign|0x7F7FFFFF.
// The same rule is VU_MAC_UPDATE's result saturation (VU0 overflow on).
inline __m128 ps2VuFmaxV(__m128 v)
{
    const __m128i u = _mm_castps_si128(v);
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i isE = _mm_cmpeq_epi32(_mm_and_si128(u, expMask), expMask);
    const __m128i sat = _mm_or_si128(_mm_and_si128(u, _mm_set1_epi32((int)0x80000000u)),
                                     _mm_set1_epi32(0x7F7FFFFF));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(isE, sat), _mm_andnot_si128(isE, u)));
}
inline __m128 ps2VuClampV(__m128 v) { return g_ps2EeFpClamp ? ps2VuFmaxV(v) : v; }
inline float ps2VuClampF(float f)
{
    const uint32_t u = ps2VuBits(f);
    return (u & 0x7F800000u) == 0x7F800000u ? ps2VuFromBits((u & 0x80000000u) | 0x7F7FFFFFu) : f;
}

// PCSX2 fp_max/fp_min: a signed-integer compare of the raw bits.
inline __m128 ps2VuMax(__m128 a, __m128 b)
{
    if (!g_ps2EeFpClamp)
        return _mm_max_ps(a, b);
    const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b);
    const __m128i bothNeg = _mm_srai_epi32(_mm_and_si128(ia, ib), 31);
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bothNeg, _mm_min_epi32(ia, ib)),
                                         _mm_andnot_si128(bothNeg, _mm_max_epi32(ia, ib))));
}
inline __m128 ps2VuMin(__m128 a, __m128 b)
{
    if (!g_ps2EeFpClamp)
        return _mm_min_ps(a, b);
    const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b);
    const __m128i bothNeg = _mm_srai_epi32(_mm_and_si128(ia, ib), 31);
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bothNeg, _mm_max_epi32(ia, ib)),
                                         _mm_andnot_si128(bothNeg, _mm_min_epi32(ia, ib))));
}

// PCSX2 floatToInt<n>: scale the raw value, saturate by sign when the scaled
// exponent field is >= 0x4F000000, else truncate.
inline __m128 ps2VuFtoi(__m128 src, float scale)
{
    const __m128 f = _mm_mul_ps(src, _mm_set1_ps(scale));
    const __m128i t = _mm_cvttps_epi32(f);
    if (!g_ps2EeFpClamp)
        return _mm_castsi128_ps(t);
    const __m128i u = _mm_castps_si128(f);
    const __m128i big = _mm_cmpgt_epi32(_mm_and_si128(u, _mm_set1_epi32(0x7F800000)),
                                        _mm_set1_epi32(0x4EFFFFFF));
    const __m128i sat = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), _mm_srai_epi32(u, 31));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(big, sat), _mm_andnot_si128(big, t)));
}

// PCSX2 _vuDIV / _vuSQRT / _vuRSQRT (flags are not modelled here).
inline float ps2Vu0Div(float fsRaw, float ftRaw)
{
    if (!g_ps2EeFpClamp)
        return (ftRaw != 0.0f) ? (fsRaw / ftRaw) : copysignf(3.402823466e+38F, fsRaw * ftRaw);
    const float fs = ps2VuClampF(fsRaw), ft = ps2VuClampF(ftRaw);
    if (ft == 0.0f)
        return ps2VuFromBits(((ps2VuBits(fsRaw) ^ ps2VuBits(ftRaw)) & 0x80000000u) | 0x7F7FFFFFu);
    return ps2VuClampF(fs / ft);
}
inline float ps2Vu0Sqrt(float ftRaw)
{
    if (!g_ps2EeFpClamp)
        return sqrtf(fabsf(ftRaw));
    return ps2VuClampF(sqrtf(fabsf(ps2VuClampF(ftRaw))));
}
inline float ps2Vu0Rsqrt(float fsRaw, float ftRaw)
{
    if (!g_ps2EeFpClamp)
    {
        const float root = sqrtf(fabsf(ftRaw));
        return (root != 0.0f) ? (fsRaw / root) : copysignf(3.402823466e+38F, fsRaw * ftRaw);
    }
    const float fs = ps2VuClampF(fsRaw), ft = ps2VuClampF(ftRaw);
    const uint32_t sign = (ps2VuBits(fsRaw) ^ ps2VuBits(ftRaw)) & 0x80000000u;
    if (ft == 0.0f)
        return ps2VuFromBits(fs != 0.0f ? (sign | 0x7F7FFFFFu) : sign);
    return ps2VuClampF(fs / sqrtf(fabsf(ft)));
}

// PCSX2 _vuCLIP: integer compare against |w| (a denormal w compares as the
// largest denormal). Bit 0 = x > +|w|, bit 1 = x < -|w|, then y, z.
inline uint32_t ps2Vu0Clip(__m128 fs, __m128 ft)
{
    if (!g_ps2EeFpClamp)
    {
        // The pre-RRV_EE_FPCLAMP emitter, bit order included.
        __m128 neg_ft = _mm_xor_ps(ft, _mm_castsi128_ps(_mm_set1_epi32(0x80000000)));
        uint32_t gt_mask = (uint32_t)_mm_movemask_ps(_mm_cmpgt_ps(fs, ft));
        uint32_t lt_mask = (uint32_t)_mm_movemask_ps(_mm_cmplt_ps(fs, neg_ft));
        return ((lt_mask & 0x1) << 0) | ((gt_mask & 0x1) << 1) |
               ((lt_mask & 0x2) << 1) | ((gt_mask & 0x2) << 2) |
               ((lt_mask & 0x4) << 2) | ((gt_mask & 0x4) << 3);
    }
    uint32_t v[4], w[4];
    std::memcpy(v, &fs, sizeof(v));
    std::memcpy(w, &ft, sizeof(w));
    int32_t value = (int32_t)w[0];
    value = (value & 0x7F800000) ? (value & 0x7FFFFFFF) : 0x007FFFFF;
    uint32_t flags = 0;
    for (int c = 0; c < 3; ++c)
    {
        if ((int32_t)(v[c] ^ 0x00000000u) > value) flags |= 0x01u << (c * 2);
        if ((int32_t)(v[c] ^ 0x80000000u) > value) flags |= 0x02u << (c * 2);
    }
    return flags;
}

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) ps2VuClampV(_mm_add_ps(ps2VuClampV((__m128)(a)), ps2VuClampV((__m128)(b))))
#define PS2_VSUB(a, b) ps2VuClampV(_mm_sub_ps(ps2VuClampV((__m128)(a)), ps2VuClampV((__m128)(b))))
#define PS2_VMUL(a, b) ps2VuClampV(_mm_mul_ps(ps2VuClampV((__m128)(a)), ps2VuClampV((__m128)(b))))
#define PS2_VDIV(a, b) ps2VuClampV(_mm_div_ps(ps2VuClampV((__m128)(a)), ps2VuClampV((__m128)(b))))
#define PS2_VMULQ(a, q) ps2VuClampV(_mm_mul_ps(ps2VuClampV((__m128)(a)), ps2VuClampV(_mm_set1_ps(q))))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

// The fast accessors run for every guest load and store outside the special segments. The compiler
// did not inline them (host disassembly of the native hot functions, 2026-10-02: a call per access),
// so the common case is forced inline (mask, range test, one load or store) and the case of an access
// that wraps around the end of RAM, which no sane guest code produces, is one out-of-line helper.
// Same bytes read and written, in the same order, as the previous per-width functions.
__attribute__((noinline, cold, unused)) static void Ps2FastReadWrapped(const uint8_t *rdram, uint32_t offset, void *out,
                                                              uint32_t bytes)
{
    uint8_t *dst = static_cast<uint8_t *>(out);
    for (uint32_t i = 0; i < bytes; ++i)
        dst[i] = rdram[(offset + i) & PS2_RAM_MASK];
}

__attribute__((noinline, cold, unused)) static void Ps2FastWriteWrapped(uint8_t *rdram, uint32_t offset, const void *in,
                                                               uint32_t bytes)
{
    const uint8_t *src = static_cast<const uint8_t *>(in);
    for (uint32_t i = 0; i < bytes; ++i)
        rdram[(offset + i) & PS2_RAM_MASK] = src[i];
}

template <typename T>
__attribute__((always_inline)) static inline T Ps2FastReadT(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    T value;
    if (__builtin_expect(!Ps2FastRangeIsContiguous(offset, sizeof(T)), 0))
        Ps2FastReadWrapped(rdram, offset, &value, sizeof(T));
    else
        std::memcpy(&value, rdram + offset, sizeof(T));
    return value;
}

template <typename T>
__attribute__((always_inline)) static inline void Ps2FastWriteT(uint8_t *rdram, uint32_t addr, T value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (__builtin_expect(!Ps2FastRangeIsContiguous(offset, sizeof(T)), 0))
        Ps2FastWriteWrapped(rdram, offset, &value, sizeof(T));
    else
        std::memcpy(rdram + offset, &value, sizeof(T));
}

__attribute__((always_inline)) static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}
__attribute__((always_inline)) static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    return Ps2FastReadT<uint16_t>(rdram, addr);
}
__attribute__((always_inline)) static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    return Ps2FastReadT<uint32_t>(rdram, addr);
}
__attribute__((always_inline)) static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    return Ps2FastReadT<uint64_t>(rdram, addr);
}
__attribute__((always_inline)) static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    return Ps2FastReadT<__m128i>(rdram, addr);
}

__attribute__((always_inline)) static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}
__attribute__((always_inline)) static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    Ps2FastWriteT<uint16_t>(rdram, addr, value);
}
__attribute__((always_inline)) static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    Ps2FastWriteT<uint32_t>(rdram, addr, value);
}
__attribute__((always_inline)) static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    Ps2FastWriteT<uint64_t>(rdram, addr, value);
}
__attribute__((always_inline)) static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    Ps2FastWriteT<__m128i>(rdram, addr, value);
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
#define PS2_PABSW(a) _mm_abs_epi32((__m128i)(a))
#define PS2_PABSH(a) _mm_abs_epi16((__m128i)(a))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
#define PS2_PSLLVW(a, b) _mm_custom_sllv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRLVW(a, b) _mm_custom_srlv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRAVW(a, b) _mm_custom_srav_epi32((__m128i)(a), (__m128i)(b))

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// ── R5900 COP1 exact-semantics helpers ──────────────────────────────────────
//
// The EE FPU is NOT IEEE 754: it has no infinities and no NaNs, denormal
// operands read as zero, and an overflow saturates to +/-Fmax. Reference:
// PCSX2 pcsx2/FPU.cpp @ d5f75c9e4 (fpuDouble, checkDivideByZero, CVT_W).
// Anything that lets an Inf or a NaN into guest state is a divergence that
// then propagates silently -- that is how a wrong sqrt froze the demo race.
inline uint32_t ps2FpuBits(float f)
{
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float ps2FpuFromBits(uint32_t u)
{
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// PCSX2 FPU.cpp fpuDouble(): denormal -> signed zero, Inf/NaN -> signed Fmax.
inline float ps2FpuOperand(float f)
{
    const uint32_t u = ps2FpuBits(f);
    switch (u & 0x7F800000u)
    {
    case 0x00000000u: return ps2FpuFromBits(u & 0x80000000u);
    case 0x7F800000u: return ps2FpuFromBits((u & 0x80000000u) | 0x7F7FFFFFu);
    default:          return f;
    }
}

// checkOverflow + checkUnderflow: Inf -> +/-Fmax, denormal result -> +/-0.
inline float ps2FpuResult(float f)
{
    const uint32_t u = ps2FpuBits(f);
    if ((u & ~0x80000000u) >= 0x7F800000u)
        return ps2FpuFromBits((u & 0x80000000u) | 0x7F7FFFFFu);
    if ((u & 0x7F800000u) == 0u)
        return ps2FpuFromBits(u & 0x80000000u);
    return f;
}

// DIV.S. A zero OR DENORMAL divisor is the divide-by-zero case, and the result
// is a signed Fmax -- never an infinity.
inline float ps2FpuDivS(float fs, float ft, uint32_t &fcr31)
{
    const uint32_t us = ps2FpuBits(fs), ut = ps2FpuBits(ft);
    if ((ut & 0x7F800000u) == 0u)
    {
        fcr31 |= ((us & 0x7F800000u) == 0u) ? 0x00020040u   // 0/0: I | SI
                                            : 0x00010020u;  // x/0: D | SD
        return ps2FpuFromBits(((us ^ ut) & 0x80000000u) | 0x7F7FFFFFu);
    }
    return ps2FpuResult(ps2FpuOperand(fs) / ps2FpuOperand(ft));
}

// CVT.W.S truncates (the EE FPU has no other rounding mode here) and saturates
// instead of wrapping. PCSX2 CVT_W().
inline int32_t ps2FpuCvtWS(float fs)
{
    const uint32_t u = ps2FpuBits(fs);
    if ((u & 0x7F800000u) <= 0x4E800000u)
        return (int32_t)fs;
    return (u & 0x80000000u) == 0u ? (int32_t)0x7FFFFFFF : (int32_t)0x80000000;
}

// RRV_EE_FPCLAMP: C.EQ/C.LT/C.LE.S compare fpuDouble() values (PCSX2
// FPU.cpp C_cond_S; comparePrecision is undefined there), so 0x7FFFFFFF is
// +Fmax, not a NaN that makes every compare false.
inline float ps2FpuCmp(float f) { return g_ps2EeFpClamp ? ps2FpuOperand(f) : f; }
// SQRT.S: a zero/denormal input returns a signed zero, else sqrt(|fpuDouble|).
inline float ps2FpuSqrtS(float f)
{
    if (!g_ps2EeFpClamp)
        return sqrtf(fabsf(f));
    const uint32_t u = ps2FpuBits(f);
    if ((u & 0x7F800000u) == 0u)
        return ps2FpuFromBits(u & 0x80000000u);
    return sqrtf(fabsf(ps2FpuOperand(f)));
}

// FPU (COP1) operations
// Operands are flushed the way the EE FPU reads them (denormal -> 0,
// Inf/NaN -> +/-Fmax) and results are saturated back into the representable
// range, so no infinity or NaN can ever enter guest state. PCSX2 FPU.cpp
// ADD_S/SUB_S/MUL_S = fpuDouble(fs) op fpuDouble(ft) + checkOverflow +
// checkUnderflow.
#define FPU_ADD_S(a, b) ps2FpuResult(ps2FpuOperand((float)(a)) + ps2FpuOperand((float)(b)))
#define FPU_SUB_S(a, b) ps2FpuResult(ps2FpuOperand((float)(a)) - ps2FpuOperand((float)(b)))
#define FPU_MUL_S(a, b) ps2FpuResult(ps2FpuOperand((float)(a)) * ps2FpuOperand((float)(b)))
#define FPU_DIV_S(a, b) ((float)(a) / (float)(b))
// PCSX2 FPU.cpp SQRT_S: a negative operand yields sqrt(|ft|) (and sets the I
// flag) rather than NaN; +/-0 keeps its sign. sqrtf() alone returns NaN for
// negatives, which then poisons everything downstream.
#define FPU_SQRT_S(a) ps2FpuSqrtS((float)(a))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ((float)(int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
#define FPU_CVT_W_S(a) ps2FpuCvtWS((float)(a))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_EQ_S(a, b) (ps2FpuCmp((float)(a)) == ps2FpuCmp((float)(b)))
#define FPU_C_UEQ_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLT_S(a, b) (ps2FpuCmp((float)(a)) < ps2FpuCmp((float)(b)))
#define FPU_C_ULT_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLE_S(a, b) (ps2FpuCmp((float)(a)) <= ps2FpuCmp((float)(b)))
#define FPU_C_ULE_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SEQ_S(a, b) ((float)(a) == (float)(b))
#define FPU_C_NGL_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LT_S(a, b) ((float)(a) < (float)(b))
#define FPU_C_NGE_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LE_S(a, b) ((float)(a) <= (float)(b))
#define FPU_C_NGT_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(0, 3, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
