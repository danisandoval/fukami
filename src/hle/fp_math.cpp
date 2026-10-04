// fp_math.cpp — Host math redirections for PS2 FP/int library stubs.
//
// Calling conventions (MIPS O32, R5900):
//   float args   : $f12 (ctx->f[12]), $f13 (ctx->f[13])
//   float return : $f0  (ctx->f[0])
//   64-bit int   : lo word in even reg ($a0/$a2), hi in odd ($a1/$a3)
//   64-bit return: lo in $v0 (r[2]), hi in $v1 (r[3]) — setReturnU64 handles this

#include "rrv_hle.h"
#include "ps2_runtime.h"
#include <cmath>
#include <cstdint>
#include <bit>
#include <cstring>
#include <algorithm>

namespace rrv_hle {

// ── Register helpers ─────────────────────────────────────────────────────

// Reconstruct 64-bit int from O32 register pair (lo in reg, hi in reg+1).
static inline int64_t getReg64Pair(const R5900Context* ctx, int lo_reg) {
    uint32_t lo = getRegU32(ctx, lo_reg);
    uint32_t hi = getRegU32(ctx, lo_reg + 1);
    return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
}
static inline uint64_t getRegU64Pair(const R5900Context* ctx, int lo_reg) {
    return static_cast<uint64_t>(getReg64Pair(ctx, lo_reg));
}

// Reinterpret two float regs as a double (little-endian: f[N] = low bits).
static inline double getDoublePair(const R5900Context* ctx, int lo_reg) {
    uint64_t bits = (static_cast<uint64_t>(std::bit_cast<uint32_t>(ctx->f[lo_reg + 1])) << 32)
                  |  static_cast<uint64_t>(std::bit_cast<uint32_t>(ctx->f[lo_reg]));
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}
static inline void setDoublePair(R5900Context* ctx, int lo_reg, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    ctx->f[lo_reg]     = std::bit_cast<float>(static_cast<uint32_t>(bits & 0xFFFFFFFFu));
    ctx->f[lo_reg + 1] = std::bit_cast<float>(static_cast<uint32_t>(bits >> 32));
}

// ── Single-precision ─────────────────────────────────────────────────────

void cosf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::cosf(ctx->f[12]);
}
void sinf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::sinf(ctx->f[12]);
}
void sqrtf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::sqrtf(ctx->f[12]);
}
void floorf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::floorf(ctx->f[12]);
}
void atanf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::atanf(ctx->f[12]);
}
void atan2f_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::atan2f(ctx->f[12], ctx->f[13]);
}

// Soft-double atan2: doubles arrive as raw bit patterns in the 64-bit GPRs $a0
// and $a1 and the result leaves in $v0. The runtime's ps2_stubs::atan2 assumes
// the hardware-FPU single-precision ABI instead, which left $v0 untouched — so
// RR5's callers stored whatever the preceding `fptodp` had returned there.
// See src/patches.cpp patch_0x2cb4c8 and docs/TESTING.md T-FLY-TREES-ATAN2.
static inline double getSoftDouble(const R5900Context* ctx, int reg) {
    const uint64_t bits = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[reg], 0));
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
static inline void setSoftDoubleReturn(R5900Context* ctx, double value) {
    uint64_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(bits));  // $v0
}

void atan2_soft_double_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setSoftDoubleReturn(ctx, ::atan2(getSoftDouble(ctx, 4), getSoftDouble(ctx, 5)));
}

// pow(double,double) — guest 0x2CB730. Its call site builds the literal 10.0
// straight into a GPR (`ori $a0,$zero,0x8048` + `dsll32 $a0,$a0,15` =
// 0x4024000000000000), which is only meaningful under the soft-double ABI.
void pow_soft_double_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setSoftDoubleReturn(ctx, ::pow(getSoftDouble(ctx, 4), getSoftDouble(ctx, 5)));
}

// atan(double) — guest 0x2CE618. One argument: `$a0` in, `$v0` out.
void atan_soft_double_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setSoftDoubleReturn(ctx, ::atan(getSoftDouble(ctx, 4)));
}

// fabs(double) — guest 0x2CEA28. `$a0` in, `$v0` out; its result feeds atan's
// `$a0` directly at 0x2cc00c -> 0x2cc014.
void fabs_soft_double_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setSoftDoubleReturn(ctx, ::fabs(getSoftDouble(ctx, 4)));
}

// ── ieee754 helpers ──────────────────────────────────────────────────────
// These are double-precision ops; the PS2 passes doubles in float register pairs.

void ieee754_atan2_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double y = getDoublePair(ctx, 12);
    double x = getDoublePair(ctx, 14);
    setDoublePair(ctx, 0, ::atan2(y, x));
}
void ieee754_atan2f_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::atan2f(ctx->f[12], ctx->f[14]);
}
void ieee754_log_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double x = getDoublePair(ctx, 12);
    setDoublePair(ctx, 0, ::log(x));
}
void ieee754_log10_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double x = getDoublePair(ctx, 12);
    setDoublePair(ctx, 0, ::log10(x));
}
void ieee754_pow_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double base = getDoublePair(ctx, 12);
    double exp  = getDoublePair(ctx, 14);
    setDoublePair(ctx, 0, ::pow(base, exp));
}
void ieee754_sqrt_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double x = getDoublePair(ctx, 12);
    setDoublePair(ctx, 0, ::sqrt(x));
}
void ieee754_sqrtf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ::sqrtf(ctx->f[12]);
}
void ieee754_rem_pio2f_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // Returns residual of x - n*(pi/2); result in $f0, integer n in $v0.
    float x = ctx->f[12];
    float quotient;
    ctx->f[0] = ::remquof(x, static_cast<float>(M_PI / 2.0), reinterpret_cast<int*>(&quotient));
    int n = static_cast<int>(quotient);
    setReturnS32(ctx, n);
}
void kernel_rem_pio2f_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // Used in range-reduction for sin/cos; defer to ieee754 variant.
    ieee754_rem_pio2f_stub(nullptr, ctx, nullptr);
}

// ── 64-bit integer helpers (O32: lo in even reg, hi in odd reg) ──────────

void muldi3_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    int64_t a = getReg64Pair(ctx, 4); // $a0/$a1
    int64_t b = getReg64Pair(ctx, 6); // $a2/$a3
    setReturnU64(ctx, static_cast<uint64_t>(a * b));
}
void divdi3_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    int64_t a = getReg64Pair(ctx, 4);
    int64_t b = getReg64Pair(ctx, 6);
    setReturnU64(ctx, b != 0 ? static_cast<uint64_t>(a / b) : 0u);
}
void moddi3_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    int64_t a = getReg64Pair(ctx, 4);
    int64_t b = getReg64Pair(ctx, 6);
    setReturnU64(ctx, b != 0 ? static_cast<uint64_t>(a % b) : 0u);
}
void udivdi3_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    uint64_t a = getRegU64Pair(ctx, 4);
    uint64_t b = getRegU64Pair(ctx, 6);
    setReturnU64(ctx, b != 0 ? a / b : 0u);
}
void umoddi3_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    uint64_t a = getRegU64Pair(ctx, 4);
    uint64_t b = getRegU64Pair(ctx, 6);
    setReturnU64(ctx, b != 0 ? a % b : 0u);
}

// ── Float/int conversion helpers ─────────────────────────────────────────

void fixunsdfdi_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // (unsigned long long)(double x)
    double x = getDoublePair(ctx, 12);
    setReturnU64(ctx, x >= 0.0 ? static_cast<uint64_t>(x) : 0u);
}
void floatdidf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // (double)(long long x)
    int64_t x = getReg64Pair(ctx, 4);
    setDoublePair(ctx, 0, static_cast<double>(x));
}
void floatdisf_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // (float)(long long x)
    int64_t x = getReg64Pair(ctx, 4);
    ctx->f[0] = static_cast<float>(x);
}
void negdf2_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double x = getDoublePair(ctx, 12);
    setDoublePair(ctx, 0, -x);
}
void negsf2_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = -ctx->f[12];
}

// ── Double FP ops ($f12/$f13 = lo/hi word of double arg, $f0/$f1 = result) ─

void dpadd_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, getDoublePair(ctx, 12) + getDoublePair(ctx, 14));
}
void dpcmp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double a = getDoublePair(ctx, 12), b = getDoublePair(ctx, 14);
    setReturnS32(ctx, (a > b) - (a < b));
}
void dpdiv_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double b = getDoublePair(ctx, 14);
    setDoublePair(ctx, 0, b != 0.0 ? getDoublePair(ctx, 12) / b : 0.0);
}
void dpmul_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, getDoublePair(ctx, 12) * getDoublePair(ctx, 14));
}
void dpsub_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, getDoublePair(ctx, 12) - getDoublePair(ctx, 14));
}
void dptofp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = static_cast<float>(getDoublePair(ctx, 12));
}
void dptoli_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnU64(ctx, static_cast<uint64_t>(static_cast<int64_t>(getDoublePair(ctx, 12))));
}
void dptoul_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    double x = getDoublePair(ctx, 12);
    setReturnU64(ctx, x >= 0.0 ? static_cast<uint64_t>(x) : 0u);
}
void fptodp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, static_cast<double>(ctx->f[12]));
}
void fptosi_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, static_cast<int32_t>(ctx->f[12]));
}
void fptoui_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnU32(ctx, static_cast<uint32_t>(ctx->f[12]));
}
void litodp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, static_cast<double>(getReg64Pair(ctx, 4)));
}
void sitofp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = static_cast<float>(static_cast<int32_t>(getRegU32(ctx, 4)));
}
void ftoi_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, static_cast<int32_t>(ctx->f[12]));
}
void itof_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = static_cast<float>(static_cast<int32_t>(getRegU32(ctx, 4)));
}
void rint_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setDoublePair(ctx, 0, ::rint(getDoublePair(ctx, 12)));
}

// ── FP op/cmp helpers (single-precision variant used by soft-fp layer) ──

void fpcmp_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    float a = ctx->f[12], b = ctx->f[14];
    setReturnS32(ctx, (a > b) - (a < b));
}
void fpadd_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ctx->f[12] + ctx->f[14];
}
void fpdiv_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ctx->f[14] != 0.0f ? ctx->f[12] / ctx->f[14] : 0.0f;
}
void fpmul_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ctx->f[12] * ctx->f[14];
}
void fpsub_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->f[0] = ctx->f[12] - ctx->f[14];
}
void fpcmp_parts_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}
void fpadd_parts_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}

// ── Misc ─────────────────────────────────────────────────────────────────

void matherr_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}
void exponent_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // Returns exponent of float arg; used internally by dtoa.
    ctx->f[0] = ::logbf(ctx->f[12]);
}

} // namespace rrv_hle
