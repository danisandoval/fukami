// vu0_math.cpp — Host implementations of libvu0 macro-mode matrix/vector ops
// that the PS2Recomp runtime leaves as throwing TODO_NAMED stubs.
//
// RRV drives its whole transform pipeline through these (sceVu0MulMatrix etc.),
// so a throw kills the render thread.  We implement them in the rrv_hle layer
// (tools/PS2Recomp is gitignored, so we cannot patch the runtime's VU.cpp) and
// install them as force-overwrite overrides in patches.cpp::registerPatches().
// (rrv_stubs.cpp / generate_stubs.py routing is dead code — never registered.)
//
// Conventions match the runtime's VU.cpp exactly:
//   * MATRIX = float[16], column-major; element [4*col + row]; translation in
//     [12..15] (4th column).  VECTOR = float[4] (x,y,z,w).
//   * EE ABI: a0=dst pointer, a1/a2=source pointers (regs 4/5/6); scalar/angle
//     float args arrive in $f12 (ctx->f[12]).
//   * mulVuMatrix(lhs,rhs,out) computes out = lhs * rhs in this layout, and the
//     sceVu0RotMatrixX/Y/Z helpers post-multiply: out = src * R.  We follow the
//     same composition order here.

#include "rrv_hle.h"
#include "rrv_vu_exact.h"
#include "ps2_runtime.h"
#include "runtime/rrv_fp_rounding.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cfenv>

namespace rrv_hle {
namespace {

// ── guest memory helpers ──────────────────────────────────────────────────────
// Route through the runtime's guest-pointer resolver: RRV passes matrix
// pointers in SCRATCHPAD (0x70000000-0x70003FFF) as well as main RAM, and a
// raw `rdram + (addr & PS2_RAM_MASK)` silently aliases SPR onto main RAM
// (0x700000c0 -> 0xc0), making every scratchpad-resident sceVu0 matrix op
// read zeros and write to the wrong memory.
bool readVec4(uint8_t* rdram, uint32_t addr, float (&out)[4]) {
    if (!addr) return false;
    const uint8_t* p = getConstMemPtr(rdram, addr);
    if (!p) return false;
    std::memcpy(out, p, sizeof(out));
    return true;
}
bool writeVec4(uint8_t* rdram, uint32_t addr, const float (&in)[4]) {
    if (!addr) return false;
    uint8_t* p = getMemPtr(rdram, addr);
    if (!p) return false;
    std::memcpy(p, in, sizeof(in));
    return true;
}

bool readMat4(uint8_t* rdram, uint32_t addr, float (&out)[16]) {
    if (!addr) return false;
    const uint8_t* p = getConstMemPtr(rdram, addr);
    if (!p) return false;
    std::memcpy(out, p, sizeof(out));
    return true;
}
void writeMat4(uint8_t* rdram, uint32_t addr, const float (&in)[16]) {
    if (!addr) return;
    uint8_t* p = getMemPtr(rdram, addr);
    if (!p) return;
    std::memcpy(p, in, sizeof(in));
}

void traceMat4Write(uint8_t* rdram, R5900Context* ctx, const char* origin,
                    uint32_t addr, const float (&in)[16]) {
    if (!ps2PathWatchIntersects(addr & PS2_RAM_MASK, sizeof(in))) return;
    uint32_t raw[16]{};
    std::memcpy(raw, in, sizeof(raw));
    uint32_t phase = 0xffffffffu;
    if (rdram) std::memcpy(&phase, rdram + 0x334E94u, sizeof(phase));
    std::fprintf(stderr,
                 "[vu0-hle-matrix] phase=%u origin=%s pc=%08x ra=%08x dst=%08x "
                 "q0=%08x/%08x/%08x/%08x q1=%08x/%08x/%08x/%08x\n",
                 phase, origin, ctx ? ctx->pc : 0u, ctx ? getRegU32(ctx, 31) : 0u, addr,
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
}

void traceMulInputs(uint8_t* rdram, R5900Context* ctx, uint32_t dst,
                    uint32_t a, const float (&m1)[16], uint32_t b, const float (&m2)[16]) {
    if (!ps2PathWatchIntersects(dst & PS2_RAM_MASK, sizeof(m1))) return;
    uint32_t x[16]{}, y[16]{};
    std::memcpy(x, m1, sizeof(x));
    std::memcpy(y, m2, sizeof(y));
    uint32_t phase = 0xffffffffu;
    if (rdram) std::memcpy(&phase, rdram + 0x334E94u, sizeof(phase));
    std::fprintf(stderr,
                 "[vu0-hle-input] phase=%u ra=%08x dst=%08x a=%08x "
                 "aq=%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x "
                 "b=%08x bq=%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x\n",
                 phase, ctx ? getRegU32(ctx, 31) : 0u, dst, a,
                 x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7],x[8],x[9],x[10],x[11],x[12],x[13],x[14],x[15], b,
                 y[0],y[1],y[2],y[3],y[4],y[5],y[6],y[7],y[8],y[9],y[10],y[11],y[12],y[13],y[14],y[15]);
}

// VU0 round-toward-zero arithmetic (flushF32, truncToF32, vuMul, vuAdd, vuSub, vuSqrt, vuDiv) and the
// exact sceVu0MulMatrix body (mulMatVu0Exact): rrv_vu_exact.h.
using namespace vu_exact;

// out = lhs * rhs  (column-major; identical to VU.cpp's mulVuMatrix)
void mulMat(const float (&lhs)[16], const float (&rhs)[16], float (&out)[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += rhs[4 * k + j] * lhs[4 * i + k];
            out[4 * i + j] = s;
        }
}

// VU multiply-adds round toward zero after every multiply and add. Keep the
// operations as explicit SIMD instructions so the host compiler cannot fuse a
// multiply with the following add or reassociate the four terms.
void mulMatVu0(const float (&columns)[16], const float (&vectors)[16], float (&out)[16]) {
    const rrv::fp::ScopedRoundTowardZero rounding; // runtime/rrv_fp_rounding.h
    const __m128 c0 = _mm_loadu_ps(columns + 0);
    const __m128 c1 = _mm_loadu_ps(columns + 4);
    const __m128 c2 = _mm_loadu_ps(columns + 8);
    const __m128 c3 = _mm_loadu_ps(columns + 12);
    for (int i = 0; i < 4; ++i) {
        const float* v = vectors + 4 * i;
        __m128 acc = _mm_mul_ps(c0, _mm_set1_ps(v[0]));
        acc = _mm_add_ps(acc, _mm_mul_ps(c1, _mm_set1_ps(v[1])));
        acc = _mm_add_ps(acc, _mm_mul_ps(c2, _mm_set1_ps(v[2])));
        acc = _mm_add_ps(acc, _mm_mul_ps(c3, _mm_set1_ps(v[3])));
        _mm_storeu_ps(out + 4 * i, acc);
    }
}

void identity(float (&m)[16]) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Axis rotation matrices, matching VU.cpp's sceVu0RotMatrixX/Y/Z sign layout.
void rotX(float a, float (&m)[16]) {
    identity(m); const float c = std::cos(a), s = std::sin(a);
    m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
}
void rotY(float a, float (&m)[16]) {
    identity(m); const float c = std::cos(a), s = std::sin(a);
    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
}
void rotZ(float a, float (&m)[16]) {
    identity(m); const float c = std::cos(a), s = std::sin(a);
    m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
}

// These are registered as overrides in patches.cpp, so they may be reached by
// an indirect (function-pointer) call where dispatchLoop dispatches the entry
// directly.  Emulate `jr $ra` (return 0 in $v0) so pc advances either way —
// matching the recompiler's generated stub wrapper.
void finishVu0(R5900Context* ctx) {
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}

} // namespace

// sceVu0MulMatrix(m0, m1, m2): m0 = m2 x m1 in flat row-qword terms — the
// FIRST source operand is the RIGHT factor. Ground-truthed against PCSX2
// (region-matched USA ELF) at two independent sites: the compound at EE
// 0x01e24860 equals flat viewinv(m2) x proj(m1) lane-for-lane (e.g. q0 =
// (-1915.69, -1979.43, 23.10, -0.9392) = 0.0290*266.88 - 0.9392*2048, ...),
// and sceVu0ViewScreenMatrix's internal multiply only reproduces PCSX2's
// projection layout (raw 2048 centre terms in q2, az in q3) with this order.
// The previous m1 x m2 order scrambled every proj*view compound.
void vu0MulMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), a = getRegU32(ctx, 5), b = getRegU32(ctx, 6);
    float m1[16]{}, m2[16]{}, out[16]{};
    if (readMat4(rdram, a, m1) && readMat4(rdram, b, m2)) {
        traceMulInputs(rdram, ctx, dst, a, m1, b, m2);
        mulMatVu0Exact(m1, m2, out);
        traceMat4Write(rdram, ctx, "MulMatrix", dst, out);
        writeMat4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// UNVALIDATED phase-7 candidate (docs/HANDOFF_PHASE7_MATRIX.md), reachable only
// via RRV_P7_MUL_IMPL=rtz. Same operand roles and same accumulation order as the
// generated guest body sub_002CA250_0x2ca250 — `columns` is $a1, `vectors` is
// $a2, so mulMatVu0(m1, m2) is arithmetically the same routing as
// mulMat(m2, m1); the only semantic change is round-toward-zero plus explicit
// SIMD so the host cannot contract or reassociate.
void vu0MulMatrixRtz(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), a = getRegU32(ctx, 5), b = getRegU32(ctx, 6);
    float m1[16]{}, m2[16]{}, out[16]{};
    if (readMat4(rdram, a, m1) && readMat4(rdram, b, m2)) {
        traceMulInputs(rdram, ctx, dst, a, m1, b, m2);
        mulMatVu0(m1, m2, out);
        traceMat4Write(rdram, ctx, "MulMatrix", dst, out);
        writeMat4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// sceVu0TransMatrix(m0, m1, t): m0 = m1 * Translate(t)
void vu0TransMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5), tv = getRegU32(ctx, 6);
    float m1[16]{}, t[4]{}, tm[16]{}, out[16]{};
    if (readMat4(rdram, src, m1) && readVec4(rdram, tv, t)) {
        identity(tm);
        tm[12] = t[0]; tm[13] = t[1]; tm[14] = t[2];
        mulMat(m1, tm, out);
        traceMat4Write(rdram, ctx, "TransMatrix", dst, out);
        writeMat4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// sceVu0RotMatrix(m0, m1, rot): m0 = m1 * Rz(rot.z) * Ry(rot.y) * Rx(rot.x).
// Ground-truthed against the game's own recompiled sceVu0RotMatrix (0x2CA7C0):
// it decomposes into three leaf calls in this exact order — RotMatrixZ (rot.z,
// vector offset +8), then RotMatrixY (rot.y, +4), then RotMatrixX (rot.x, +0) —
// each post-multiplying via mulVuMatrix(src,rot,out)=src*rot (VU.cpp:80-93,
// sub_002CA5E0/2CA720/2CA680 forwarders). The previous Rx*Ry*Rz order here was
// backwards; it happened to be a no-op for the attract flyover (rot=(x,0,0),
// so Rz=Ry=I there) but would misorient any scene rotating about >1 axis.
void vu0RotMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5), rv = getRegU32(ctx, 6);
    float m1[16]{}, r[4]{};
    if (readMat4(rdram, src, m1) && readVec4(rdram, rv, r)) {
        float rx[16], ry[16], rz[16], t0[16], t1[16], out[16];
        rotX(r[0], rx); rotY(r[1], ry); rotZ(r[2], rz);
        mulMat(m1, rz, t0);
        mulMat(t0, ry, t1);
        mulMat(t1, rx, out);
        traceMat4Write(rdram, ctx, "RotMatrix", dst, out);
        writeMat4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// sceVu0NormalLightMatrix(m0, l0, l1, l2): build a local-light matrix from three
// light *direction* vectors.  Applying it to a normal n (sceVu0ApplyMatrix) must
// yield (l0·n, l1·n, l2·n) in xyz.  ApplyMatrix computes out[i]=dot(column_i,n)
// with column_i = {m[i],m[4+i],m[8+i],m[12+i]}, so each light occupies a column;
// only xyz contribute (w-row zeroed), 4th column = (0,0,0,1).
//   ABI: a0=dst matrix, a1/a2/a3 = l0/l1/l2.
void vu0NormalLightMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    // Ground truth = the game's own 0x2CA8E8 (disasm + PCSX2 live trace): for
    // each light it does tmp = l_i * -1 (ScaleVector 0x2CA480), row_i =
    // normalize_xyz(tmp) with w=0 (Normalize 0x2CA2E0), zeroes ALL of row 3,
    // then TransposeMatrix(m0, m0) — so columns end up normalize(-l_i) and the
    // 4th row/column are 0 (not 1 in [15]).  The previous version copied the
    // raw vectors unnormalized, which left boot-logo light rows ~87x too large
    // (B-5 overbright white letters).
    const uint32_t dst = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
    float l0[4]{}, l1[4]{}, l2[4]{}, m[16]{};
    if (readVec4(rdram, a1, l0) && readVec4(rdram, a2, l1) && readVec4(rdram, a3, l2)) {
        float* ls[3] = {l0, l1, l2};
        for (int i = 0; i < 3; ++i) {
            float x = -ls[i][0], y = -ls[i][1], z = -ls[i][2];
            const float len = std::sqrt(x * x + y * y + z * z);
            // Match sceVu0Normalize: sub-epsilon inputs produce a zero vector,
            // not an unstable full-strength light direction.
            if (len > 1.0e-6f) {
                x /= len; y /= len; z /= len;
            } else {
                x = y = z = 0.0f;
            }
            m[0 + i] = x; m[4 + i] = y; m[8 + i] = z; m[12 + i] = 0.0f;
        }
        m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 0.0f;
        traceMat4Write(rdram, ctx, "NormalLightMatrix", dst, m);
        writeMat4(rdram, dst, m);
    }
    finishVu0(ctx);
}

// sceVu0LightColorMatrix(m0, c0, c1, c2, c3): build a light-colour matrix from
// FOUR RGB vectors.  Ground truth = the game's own 0x2CA9A0: four
// sceVu0CopyVector (0x2CA4C8) calls copying c0/c1/c2 into rows 0-2 and a
// fourth vector — passed in $t0 (reg 8), the ambient term — into row 3,
// verbatim, no transpose.  The previous version dropped the ambient row
// (wrote 0,0,0,1), which capped the boot-logo letter brightness (B-5).
// ABI: a0=dst matrix, a1/a2/a3 = c0/c1/c2, t0 = c3 (ambient).
void vu0LightColorMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
    const uint32_t t0 = getRegU32(ctx, 8);
    float c0[4]{}, c1[4]{}, c2[4]{}, c3[4]{}, m[16]{};
    if (readVec4(rdram, a1, c0) && readVec4(rdram, a2, c1) &&
        readVec4(rdram, a3, c2) && readVec4(rdram, t0, c3)) {
        std::memcpy(&m[0],  c0, sizeof(c0));  // row 0 = c0
        std::memcpy(&m[4],  c1, sizeof(c1));  // row 1 = c1
        std::memcpy(&m[8],  c2, sizeof(c2));  // row 2 = c2
        std::memcpy(&m[12], c3, sizeof(c3));  // row 3 = c3 (ambient, from $t0)
        traceMat4Write(rdram, ctx, "LightColorMatrix", dst, m);
        writeMat4(rdram, dst, m);
    }
    finishVu0(ctx);
}

// sceVu0InversMatrix(m0, m1): m0 = inverse(m1) for a rigid (rotation +
// translation) transform, matching the real libvu0 microcode: transpose the
// 3x3 block and negate the back-transformed translation, ignoring the 4th
// row entirely.  RRV passes camera matrices whose 4th row is NOT (0,0,0,1)
// (it carries unrelated data), so a general 4x4 cofactor inverse computes
// det=0 and any full-matrix approach breaks — the camera/view matrix must be
// inverted the way the hardware library does it.
void vu0InversMatrix(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5);
    float m[16]{}, inv[16]{};
    if (readMat4(rdram, src, m)) {
        identity(inv);
        // Rᵀ: inv(r,c) = m(c,r) with element (row,col) stored at [4*col+row].
        // The guest does this with PEXTLW/PEXTUW/PCPYLD/PCPYUD on integer
        // registers — a pure bit shuffle, so it must stay a copy, not a
        // multiply by an identity.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                inv[4 * c + r] = m[4 * r + c];
        // NOTE: hardware's `sqc2 vf4` writes the SOURCE row-3 w here, not 1.0,
        // because `vsub.xyz` never touches w. Propagating that (inv[15] =
        // m[15]) was tried on 2026-08-13 and BREAKS BOOT — RRV reaches no
        // attract phase at all. The phase-7 camera path has m[15] == 1.0 on
        // both sides, so there is no boundary evidence for the change; keep
        // identity's 1.0 until some boundary actually disagrees.
        // t' = 0 - (Rᵀ · t), as VMULAx / VMADDAy / VMADDz then VSUB from a zero
        // register — round-toward-zero at every step, and `0 - s` rather than
        // `-s`, which is what preserves hardware's negative zero.
        for (int r = 0; r < 3; ++r) {
            const float s = vuAdd(vuAdd(vuMul(inv[r], m[12]), vuMul(inv[4 + r], m[13])),
                                  vuMul(inv[8 + r], m[14]));
            inv[12 + r] = vuSub(0.0f, s);
        }
        traceMat4Write(rdram, ctx, "InversMatrix", dst, inv);
        writeMat4(rdram, dst, inv);
    }
    finishVu0(ctx);
}

// sceVu0OuterProduct(v0, v1, v2): v0.xyz = v1 x v2, v0.w = 0.
//
// Transcription of the guest body at 0x2CA298, which the recompiler DOES emit
// as real code — but as inline COP2 through PS2_VMUL/PS2_VSUB, which run in the
// host's default rounding mode because RRV has no VU0 FP-control model:
//
//   vopmula.xyz ACC, vf4, vf5    ACC = (a.y*b.z, a.z*b.x, a.x*b.y)
//   vopmsub.xyz vf6, vf5, vf4    vf6 = ACC - (b.y*a.z, b.z*a.x, b.x*a.y)
//   vsub.w      vf6, vf6, vf6    w cleared, not computed
//
// Same round-toward-zero rule as vu0Normalize, and measured at the same
// boundary: this vector is the input to normalize(cross), which is the
// phase-7 camera basis row 0. Rollback: RRV_VU0_OUTER=generated.
void vu0OuterProduct(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), av = getRegU32(ctx, 5), bv = getRegU32(ctx, 6);
    float a[4]{}, b[4]{}, out[4]{};
    if (readVec4(rdram, av, a) && readVec4(rdram, bv, b)) {
        out[0] = vuSub(vuMul(a[1], b[2]), vuMul(b[1], a[2]));
        out[1] = vuSub(vuMul(a[2], b[0]), vuMul(b[2], a[0]));
        out[2] = vuSub(vuMul(a[0], b[1]), vuMul(b[0], a[1]));
        out[3] = 0.0f;
        writeVec4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// sceVu0ApplyMatrix(v0, m, v1): v0 = m * v1. Guest body at 0x2CA220:
//
//   vmulax.xyzw  ACC, c0, v1x
//   vmadday.xyzw ACC, c1, v1y
//   vmaddaz.xyzw ACC, c2, v1z
//   vmaddw.xyzw  v0,  c3, v1w
//
// VIS-005 (wheel tilt): the VU has no Inf/NaN. An exponent-255 lane is a huge
// finite number (PCSX2 vuDouble reads it as sign|0x7F7FFFFF, the rule
// RRV_VU_OPCLAMP already uses for VU1), so `0 * it` is 0 and only the lanes it
// really scales saturate. The ps2_stubs host port does IEEE math instead: RR5's
// wheel-steer helper (0x207570) builds (0,0,1) in scratchpad without writing
// w, the stale w is a NaN pattern, and 0 * NaN turned x/y/z into NaN — the
// front-left wheel then came out at exactly -90 / 135 degrees.
//
// Only called when an input lane has exponent 255 (vu0ApplyMatrixHasNonFinite),
// so every other call keeps the host port's exact bits. Same operation order
// and round-to-nearest as that port; one product or sum per statement, so no
// FMA can form.
namespace {
inline float vuOperandClamp(float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    if ((b & 0x7F800000u) == 0x7F800000u)
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}
inline float vuSaturate(float v) {
    return std::isinf(v) ? std::copysignf(3.4028234663852886e+38f, v) : v;
}
inline bool hasExp255(const float* p, int n) {
    for (int i = 0; i < n; ++i) {
        uint32_t b;
        std::memcpy(&b, &p[i], sizeof(b));
        if ((b & 0x7F800000u) == 0x7F800000u)
            return true;
    }
    return false;
}
} // namespace

bool vu0ApplyMatrixHasNonFinite(uint8_t* rdram, R5900Context* ctx) {
    float m[16]{}, v[4]{};
    if (!readMat4(rdram, getRegU32(ctx, 5), m) || !readVec4(rdram, getRegU32(ctx, 6), v))
        return false;
    return hasExp255(m, 16) || hasExp255(v, 4);
}

void vu0ApplyMatrixClamped(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), ma = getRegU32(ctx, 5), va = getRegU32(ctx, 6);
    float m[16]{}, v[4]{}, out[4]{};
    if (readMat4(rdram, ma, m) && readVec4(rdram, va, v)) {
        for (float& x : m) x = vuOperandClamp(x);
        for (float& x : v) x = vuOperandClamp(x);
        for (int r = 0; r < 4; ++r) {
            float acc = vuSaturate(m[r] * v[0]);
            float p = vuSaturate(m[4 + r] * v[1]);
            acc = vuSaturate(acc + p);
            p = vuSaturate(m[8 + r] * v[2]);
            acc = vuSaturate(acc + p);
            p = vuSaturate(m[12 + r] * v[3]);
            out[r] = vuSaturate(acc + p);
        }
        writeVec4(rdram, dst, out);
    }
    finishVu0(ctx);
}

// sceVu0Normalize(v0, v1): v0.xyz = v1.xyz / |v1.xyz|, v0.w = 0.
//
// Instruction-for-instruction transcription of the guest body at 0x2CA2E0,
// which the recompiler leaves as a stub forwarder because libvu0's leaves are
// in `stubs`:
//
//   vmul.xyz  vf5, vf4, vf4     x*x, y*y, z*z
//   vaddy.x   vf5, vf5, vf5y    sum = x2 + y2
//   vaddz.x   vf5, vf5, vf5z    sum = sum + z2
//   vsqrt     Q, vf5x           len = sqrt(|sum|)
//   vaddq.x   vf5, vf0, Q       vf0.x is 0, so this just moves len
//   vdiv      Q, vf0w, vf5x     Q = 1.0 / len
//   vsub.xyzw vf6, vf0, vf0     zero, so w is cleared not scaled
//   vmulq.xyz vf6, vf4, Q
//
// Verified against PCSX2 at the phase-7 camera boundary: the accumulation
// order, the reciprocal-then-multiply (not a divide per component) and
// round-toward-zero at every step are all load-bearing — changing any one of
// them moves the result by 1-2 ULP, which is enough to shift the environment
// draw in screen space. There is no host `std::sqrt`+`1.0f/len` arrangement
// that reproduces hardware; this ordering is the semantics.
void vu0Normalize(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5);
    float v[4]{}, out[4]{};
    if (readVec4(rdram, src, v)) {
        const float sum = vuAdd(vuAdd(vuMul(v[0], v[0]), vuMul(v[1], v[1])),
                                vuMul(v[2], v[2]));
        const float q = vuDiv(1.0f, vuSqrt(sum));
        out[0] = vuMul(v[0], q);
        out[1] = vuMul(v[1], q);
        out[2] = vuMul(v[2], q);
        out[3] = 0.0f;
        writeVec4(rdram, dst, out);
    }
    finishVu0(ctx);
}

} // namespace rrv_hle
