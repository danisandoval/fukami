#pragma once
// rrv_hle.h — Game-specific HLE function declarations.
// Implementations are in fp_math.cpp, dev_hw.cpp, safe_stubs.cpp.
// The generator routes TODO_NAMED stubs to these instead of throwing.

#include "ps2_runtime.h"
#include <cstdint>

namespace rrv_hle {

// ── Floating-point math ──────────────────────────────────────────────────
// Single-precision: arg in $f12, return in $f0.
void cosf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void sinf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void sqrtf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void floorf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void atanf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void atan2f_stub(uint8_t*, R5900Context*, PS2Runtime*);   // $f12,$f13

// RR5 links the SOFT-DOUBLE libm: each IEEE-754 double is carried whole in one
// 64-bit GPR — `$a0`(,`$a1`) in, `$v0` out — not in the hardware FPU registers.
// Every entry point below was confirmed against its own guest call sites.
// docs/TESTING.md T-FLY-TREES-ATAN2 and T-LIBM-SOFT-DOUBLE.
void atan2_soft_double_stub(uint8_t*, R5900Context*, PS2Runtime*);  // 0x2CB4C8
void pow_soft_double_stub(uint8_t*, R5900Context*, PS2Runtime*);    // 0x2CB730
void atan_soft_double_stub(uint8_t*, R5900Context*, PS2Runtime*);   // 0x2CE618
void fabs_soft_double_stub(uint8_t*, R5900Context*, PS2Runtime*);   // 0x2CEA28

// Double-precision ieee754 helpers: arg(s) in $f12/$f13 (or reg pairs), return $f0.
void ieee754_atan2_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_atan2f_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_log_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_log10_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_pow_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_sqrt_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_sqrtf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ieee754_rem_pio2f_stub(uint8_t*, R5900Context*, PS2Runtime*);
void kernel_rem_pio2f_stub(uint8_t*, R5900Context*, PS2Runtime*);

// Soft-float/conversion helpers.
// 64-bit int convention: O32 ABI — hi in odd reg, lo in even reg.
void muldi3_stub(uint8_t*, R5900Context*, PS2Runtime*);
void divdi3_stub(uint8_t*, R5900Context*, PS2Runtime*);
void moddi3_stub(uint8_t*, R5900Context*, PS2Runtime*);
void udivdi3_stub(uint8_t*, R5900Context*, PS2Runtime*);
void umoddi3_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fixunsdfdi_stub(uint8_t*, R5900Context*, PS2Runtime*);
void floatdidf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void floatdisf_stub(uint8_t*, R5900Context*, PS2Runtime*);
void negdf2_stub(uint8_t*, R5900Context*, PS2Runtime*);
void negsf2_stub(uint8_t*, R5900Context*, PS2Runtime*);

// Double fp ops (operate on f[12]/f[13] as little-endian 64-bit pair).
void dpadd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dpcmp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dpdiv_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dpmul_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dpsub_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dptofp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dptoli_stub(uint8_t*, R5900Context*, PS2Runtime*);
void dptoul_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fptodp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fptosi_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fptoui_stub(uint8_t*, R5900Context*, PS2Runtime*);
void litodp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void sitofp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void ftoi_stub(uint8_t*, R5900Context*, PS2Runtime*);
void itof_stub(uint8_t*, R5900Context*, PS2Runtime*);
void rint_stub(uint8_t*, R5900Context*, PS2Runtime*);

// FP comparison helpers.
void fpcmp_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpadd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpdiv_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpmul_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpsub_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpcmp_parts_stub(uint8_t*, R5900Context*, PS2Runtime*);
void fpadd_parts_stub(uint8_t*, R5900Context*, PS2Runtime*);

// Misc math.
void matherr_stub(uint8_t*, R5900Context*, PS2Runtime*);
void exponent_stub(uint8_t*, R5900Context*, PS2Runtime*);

// ── sceDevGif / sceDevVif / sceDevVu  ───────────────────────────────────
void devGifPutFifo_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devGifSync_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devGifGetCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devGifContinue_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devGifPutImtMode_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devGifPutP3msk_stub(uint8_t*, R5900Context*, PS2Runtime*);

void devVif0PutFifo_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif0Sync_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif0GetCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif0Continue_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif0Pause_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif0PutErr_stub(uint8_t*, R5900Context*, PS2Runtime*);

void devVif1PutFifo_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1Sync_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1GetCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1GetFifo_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1Continue_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1Pause_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1PutErr_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVif1Reset_stub(uint8_t*, R5900Context*, PS2Runtime*);

void devVu0Sync_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu0GetCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu0Pause_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu0PutCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu0PutDBit_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu0PutTBit_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1Sync_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1GetCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1GetTpc_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1Pause_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1PutCnd_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1PutDBit_stub(uint8_t*, R5900Context*, PS2Runtime*);
void devVu1PutTBit_stub(uint8_t*, R5900Context*, PS2Runtime*);

void vu0MemReadQ_stub(uint8_t*, R5900Context*, PS2Runtime*);
void vu0MemWriteQ_stub(uint8_t*, R5900Context*, PS2Runtime*);
void vuCheckBusy_stub(uint8_t*, R5900Context*, PS2Runtime*);

// ── VU0 macro-mode math (libvu0) ────────────────────────────────────────
// Implementations the runtime leaves as throwing TODO_NAMED. See vu0_math.cpp.
// Column-major MATRIX float[16]; a0=dst, a1/a2=src pointers.
void vu0Normalize(uint8_t*, R5900Context*, PS2Runtime*); // exact VU0 RTZ transcription
void vu0OuterProduct(uint8_t*, R5900Context*, PS2Runtime*); // exact VU0 RTZ transcription
bool vu0ApplyMatrixHasNonFinite(uint8_t*, R5900Context*);   // any exponent-255 input lane?
void vu0ApplyMatrixClamped(uint8_t*, R5900Context*, PS2Runtime*); // VU operand clamp (VIS-005)
void vu0MulMatrix(uint8_t*, R5900Context*, PS2Runtime*);
void vu0MulMatrixRtz(uint8_t*, R5900Context*, PS2Runtime*); // phase-7 candidate, opt-in
void vu0TransMatrix(uint8_t*, R5900Context*, PS2Runtime*);
void vu0RotMatrix(uint8_t*, R5900Context*, PS2Runtime*);
void vu0InversMatrix(uint8_t*, R5900Context*, PS2Runtime*);
void vu0NormalLightMatrix(uint8_t*, R5900Context*, PS2Runtime*);
void vu0LightColorMatrix(uint8_t*, R5900Context*, PS2Runtime*);

// ── SPU2 sound driver (sceSpu2Remote) ───────────────────────────────────
// Host model of the RSPU2DRV IOP register-RPC, enough to clear the boot-time
// SPU2 init/verify wait without a real IOP. See spu2.cpp.
void spu2Remote_hle(uint8_t*, R5900Context*, PS2Runtime*);

// ── Safe no-ops (return 0, never throw) ─────────────────────────────────
void ret0(uint8_t*, R5900Context*, PS2Runtime*);

} // namespace rrv_hle
