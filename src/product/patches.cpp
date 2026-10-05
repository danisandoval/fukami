// patches.cpp — Register address-level fixups for functions the analyzer
// incorrectly grouped with neighbors or that need explicit re-entry points.
//
// Call registerPatches() from main.cpp AFTER registerAllFunctions().
// Add entries here as the run-time log reveals new bad-pc addresses.

#include "patches.h"
#include <atomic>
#include <map>
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/rrv_fp_rounding.h"
#include "ps2_stubs.h"
#include "ps2_recompiled_functions.h"
#include "rrv_hle.h"
#include "rrv_gs_record_hooks.h" // bounded girl-scene GS capture (RRV_GS_RECORD_ARM)
#include "rrv_gs_trace_hooks.h"  // authoritative GS-consumption trace (RRV_GS_TRACE_ARM)
#include "rrv_guest_state_trace.h" // matched phase-6 guest-state oracle (default off)
#include "rrv_m2_neutrality.h" // bounded semantic control for M2 receipt neutrality
#include "rrv_snapshot.h"        // developer checkpoints (docs/SNAPSHOTS.md)
#include "rrv_window_title.h"    // window title: GS backend + scene phase + rate

#include <chrono>
#include <deque>
#include <future>
#include <memory>

void rrvDumpFnHits(const char* why); // RRV_FN_HITS counter, defined below
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Runtime interrupt-worker primitive (Kernel/Syscalls/Interrupt.cpp). Blocks the
// calling (guest) thread until the next emulated VBlank tick, releasing guest
// execution while it waits. Declared here to avoid pulling the private Kernel
// header into the tracked source tree.
namespace ps2_syscalls { uint64_t WaitForNextVSyncTick(uint8_t* rdram, PS2Runtime* runtime); }
namespace ps2_syscalls { uint64_t GetCurrentVSyncTick(); }

// ── Patched function bodies ───────────────────────────────────────────────

// 0x2000c0 — C++ global constructor, grouped inside sub_002000B0.
// The shim at 0x2000b0 (j + syscall) is dead code relative to 0x2000c0.
// The real body starts here; since there's no label_2000c0 in the generated
// code we register a no-op.  The .ctors loop dispatch code checks
// ctx->pc == __entryPc and will advance normally if we don't change it.
static void patch_0x2000c0(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2000c0");
}

// ── Phase-7 matrix candidates (docs/HANDOFF_PHASE7_MATRIX.md) ─────────────
// Two UNVALIDATED candidates were preserved from the 2026-08-13 handoff. They
// are opt-in, so the committed semantics remain the measured baseline and the
// two can never be active in the same arm without saying so:
//   RRV_P7_ROT_IMPL = hle (default, committed) | generated
//   RRV_P7_MUL_IMPL = hle (default, committed) | rtz | generated
static bool p7RotGenerated() {
    static const bool v = [] {
        const char* e = std::getenv("RRV_P7_ROT_IMPL");
        return e && std::string_view(e) == "generated";
    }();
    return v;
}

// ── libvu0 matrix ops: generated guest body vs host HLE ───────────────────
// Measured 2026-08-13: sceVu0MulMatrix (0x2CA250), sceVu0InversMatrix
// (0x2CA360), sceVu0TransMatrix (0x2CA498), sceVu0NormalLightMatrix (0x2CA8E8)
// and sceVu0LightColorMatrix (0x2CA9A0) all have REAL generated guest bodies —
// VU0 macro (VMULA/VMADDA/VSUB) plus MMI. Only the rotation leaves
// (0x2CA5E0/0x2CA680/0x2CA720) and sceVu0UnitMatrix (0x2CA540) are stub
// forwarders with no guest body. registerPatches force-overwrote all of them
// with host approximations; the playbook's HLE rule says test the generated
// original first. `RRV_VU0_MATRIX_GENERATED` selects, by comma-separated name
// or `all`: mul, invers, trans, rot, normallight, lightcolor.
static bool vu0Generated(std::string_view name) {
    static const std::string selection = [] {
        const char* e = std::getenv("RRV_VU0_MATRIX_GENERATED");
        return std::string(e ? e : "");
    }();
    if (selection.empty()) return false;
    if (selection == "all") return true;
    std::string_view s{selection};
    for (size_t start = 0; start <= s.size();) {
        const size_t end = std::min(s.find(',', start), s.size());
        if (s.substr(start, end - start) == name) return true;
        start = end + 1;
    }
    return false;
}

// PCSX2 is the oracle for PS2 FP semantics and its own configuration answers
// this: DEFAULT_VU_FP_CONTROL_REGISTER (and DEFAULT_FPU_FP_CONTROL_REGISTER)
// are DisableExceptions + DenormalsAreZero + FlushToZero + ChopZero, and
// SLUS-20002 carries no GameDB eeRoundMode/eeDivRoundMode override. So VU0
// macro-mode arithmetic rounds toward zero. Scope it to the call, never
// globally — EE COP1 DIV/SQRT use FPUDivFPCR, which is round-to-nearest.
// The scope is runtime/rrv_fp_rounding.h: direct FPCR/MXCSR rounding bits, not std::fesetround
// (glibc's fesetround rewrites the x87 control word too; Steam Deck profile, T-LINUX-DECK).
struct ScopedVu0Rounding : rrv::fp::ScopedRoundTowardZero {
    ScopedVu0Rounding() : rrv::fp::ScopedRoundTowardZero(true) {}
};

template <void (*Generated)(uint8_t*, R5900Context*, PS2Runtime*),
          void (*Hle)(uint8_t*, R5900Context*, PS2Runtime*)>
static void vu0MatrixOp(const char* name, const char* costName, uint8_t* rdram,
                        R5900Context* ctx, PS2Runtime* runtime) {
    if (!vu0Generated(name)) {
        runtime->gate3ChargeNamedHleV1(ctx, costName);
        Hle(rdram, ctx, runtime);
        return;
    }
    const ScopedVu0Rounding rounding;
    Generated(rdram, ctx, runtime);
}

// The libvu0 leaves are called straight from generated guest code, not through
// registerPatches, so they inherit whatever rounding mode the EE dispatch loop
// is in (round-to-nearest). Wrapping their entry points puts the whole libvu0
// call graph in VU0's own mode. Nested scopes save/restore, so an outer wrapper
// and an inner one compose. `RRV_VU0_RTZ=1` enables it.
static bool vu0RtzEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("RRV_VU0_RTZ");
        return e && e[0] && e[0] != '0';
    }();
    return v;
}

template <void (*Fn)(uint8_t*, R5900Context*, PS2Runtime*)>
static void vu0Rtz(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const ScopedVu0Rounding rounding;
    Fn(rdram, ctx, runtime);
}

// sceVu0Normalize (0x2CA2E0). The recompiler leaves this leaf as a forwarder
// into ps2_stubs' host port, whose `std::sqrt` + `1.0f/len` is one to two ULP
// off the guest. Proven at the phase-7 camera boundary (docs/TESTING.md
// T-P7-NORMALIZE): with a bit-identical input vector the host port's output
// diverges, and an exact round-toward-zero model of the guest's own VU0
// sequence reproduces hardware exactly. `rrv_hle::vu0Normalize` is that
// sequence. Rollback: RRV_VU0_NORMALIZE=stub.
static bool vu0NormalizeStub() {
    static const bool v = [] {
        const char* e = std::getenv("RRV_VU0_NORMALIZE");
        return e && std::string_view(e) == "stub";
    }();
    return v;
}

static void patch_0x2ca2e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == 0x2CA2E0u)
        rrv::guesttrace::normalize(rdram, ctx, "in");
    const uint32_t ra = GPR_U32(ctx, 31);
    if (vu0NormalizeStub())
        sub_002CA2E0_0x2ca2e0(rdram, ctx, runtime);
    else {
        runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2ca2e0");
        rrv_hle::vu0Normalize(rdram, ctx, runtime);
    }
    if (ra == 0x2CA88Cu || ra == 0x2CA89Cu)
        rrv::guesttrace::normalize(rdram, ctx, "out");
}

// sceVu0OuterProduct (0x2CA298). The generated body is real guest code but its
// inline COP2 runs in the host rounding mode; this is the same VU0 RTZ
// semantics as sceVu0Normalize. Rollback: RRV_VU0_OUTER=generated.
static void patch_0x2ca298(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static const bool useGenerated = [] {
        const char* e = std::getenv("RRV_VU0_OUTER");
        return e && std::string_view(e) == "generated";
    }();
    if (useGenerated)
        sub_002CA298_0x2ca298(rdram, ctx, runtime);
    else {
        runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2ca298");
        rrv_hle::vu0OuterProduct(rdram, ctx, runtime);
    }
}

// sceVu0ApplyMatrix (0x2CA220). VIS-005 wheel tilt: an input lane with
// exponent 255 (a stale NaN w in RR5's wheel-steer scratch vector) must act as
// a huge finite number, as on the VU, not poison x/y/z through 0 * NaN. Only
// those calls leave the ps2_stubs host port; all others are unchanged.
// docs/TESTING.md T-VIS005-APPLY-NAN. Rollback: RRV_VU0_APPLY=raw.
static void patch_0x2ca220(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static const bool raw = [] {
        const char* e = std::getenv("RRV_VU0_APPLY");
        return e && std::string_view(e) == "raw";
    }();
    if (raw || !rrv_hle::vu0ApplyMatrixHasNonFinite(rdram, ctx))
        sub_002CA220_0x2ca220(rdram, ctx, runtime);
    else
        rrv_hle::vu0ApplyMatrixClamped(rdram, ctx, runtime);
}

static void patch_0x2ca360(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool entry = (ctx->pc == 0x2CA360u);
    if (entry)
        rrv::guesttrace::inversMatrix(rdram, ctx, "in");
    const uint32_t ra = GPR_U32(ctx, 31);
    vu0MatrixOp<sub_002CA360_0x2ca360, rrv_hle::vu0InversMatrix>("invers", "patch:0x2ca360", rdram, ctx, runtime);
    if (ra == 0x2CA8C8u && ctx->pc == ra)
        rrv::guesttrace::inversMatrix(rdram, ctx, "out");
}
static void patch_0x2ca498(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA498_0x2ca498, rrv_hle::vu0TransMatrix>("trans", "patch:0x2ca498", rdram, ctx, runtime);
}
static void patch_0x2ca8e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA8E8_0x2ca8e8, rrv_hle::vu0NormalLightMatrix>("normallight", "patch:0x2ca8e8", rdram, ctx, runtime);
}
static void patch_0x2ca9a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA9A0_0x2ca9a0, rrv_hle::vu0LightColorMatrix>("lightcolor", "patch:0x2ca9a0", rdram, ctx, runtime);
}

enum class P7MulImpl { Hle, Rtz, Generated };
static P7MulImpl p7MulImpl() {
    static const P7MulImpl v = [] {
        const char* e = std::getenv("RRV_P7_MUL_IMPL");
        const std::string_view s = e ? std::string_view(e) : std::string_view("hle");
        if (s == "rtz") return P7MulImpl::Rtz;
        if (s == "generated") return P7MulImpl::Generated;
        return P7MulImpl::Hle;
    }();
    return v;
}

// atan2 (0x2CB4C8) — RR5 links the SOFT-DOUBLE libm, and the runtime's
// ps2_stubs::atan2 host port assumes the hardware-FPU single-precision ABI
// (`$f12`,`$f14` in, `$f0` out). RR5's `atan2` is `double atan2(double,double)`
// with each IEEE-754 double carried whole in one 64-bit GPR: `$a0`, `$a1` in,
// `$v0` out. Read off the guest's own call site and its neighbours, which are
// NOT stubbed and therefore state the convention themselves:
//
//   0x22cda0  jal fptodp          ; float -> double
//   0x22cda4    daddu $s0, $v0    ; (delay slot) $s0 = the PREVIOUS fptodp result
//   0x22cda8  daddu $a0, $s0      ;   => fptodp returns its double in $v0
//   0x22cdac  jal atan2
//   0x22cdb0    daddu $a1, $v0    ; (delay slot) second double
//   0x22cdb8  daddu $a0, $v0      ;   => atan2 returns its double in $v0
//   0x22cdbc  jal dptofp          ; 0x2D1A70's generated body opens with
//                                 ; `sd $a0, 0x20($sp)` => double arrives in $a0
//
// With the float ABI the stub wrote `$f0` and left `$v0` holding the second
// `fptodp` result, so the caller stored **the raw second argument** where the
// arctangent belonged. That value is the flyover's tree-billboard facing angle
// (EE 0x1DE6D44 -> 0x1DE0E14 -> sceVu0RotMatrixY -> VU1 mem 924..926 -> the
// foliage microprogram at entry 0x00c0): an unwrapped ~496 rad ramp instead of
// hardware's bounded ~-1.23 rad yaw, which is why the trees spun.
// docs/TESTING.md T-FLY-TREES-ATAN2.
//
// The audit that followed (docs/TESTING.md T-LIBM-SOFT-DOUBLE) found three more
// entry points on the same wrong ABI — `pow`, `atan` and `fabs` — each
// confirmed against its own guest call sites, plus two that are correctly on
// the float ABI and are left alone (`__kernel_sinf`, `__kernel_cosf`).
// `tan`/`exp`/`log`/`log10` are not routed anywhere in this build.
//
// RRV_LIBM_ABI=float restores the old single-precision-FPU behaviour for all
// four as an A/B control; RRV_LIBM_ATAN2_ABI=float is kept as the older name.
// Unset (or `gpr`) is the fix.
static bool libmFloatAbi() {
    static const bool v = [] {
        for (const char* name : {"RRV_LIBM_ABI", "RRV_LIBM_ATAN2_ABI"}) {
            const char* e = std::getenv(name);
            if (e && std::string_view(e) == "float")
                return true;
        }
        return false;
    }();
    return v;
}

// One shim per soft-double libm entry point. The generated bodies at these
// addresses are themselves forwarders into ps2_stubs, so the registrations must
// overwrite, and each must set `ctx->pc` because it never returns through a
// generated epilogue.
template <void (*SoftDouble)(uint8_t*, R5900Context*, PS2Runtime*),
          void (*FloatAbi)(uint8_t*, R5900Context*, PS2Runtime*)>
static void libmSoftDoubleShim(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool floatAbi = libmFloatAbi();
    const char* costName = nullptr;
    if constexpr (FloatAbi == ps2_stubs::atan2)
        costName = floatAbi ? "ps2_stubs::atan2" : "rrv_hle::atan2_soft_double_stub";
    else if constexpr (FloatAbi == ps2_stubs::pow)
        costName = floatAbi ? "ps2_stubs::pow" : "rrv_hle::pow_soft_double_stub";
    else if constexpr (FloatAbi == ps2_stubs::atan)
        costName = floatAbi ? "ps2_stubs::atan" : "rrv_hle::atan_soft_double_stub";
    else if constexpr (FloatAbi == ps2_stubs::fabs)
        costName = floatAbi ? "ps2_stubs::fabs" : "rrv_hle::fabs_soft_double_stub";
    else
        static_assert(FloatAbi != FloatAbi, "unmapped soft-double HLE entry");
    runtime->gate3ChargeNamedHleV1(ctx, costName);
    if (floatAbi)
        FloatAbi(rdram, ctx, runtime);
    else
        SoftDouble(rdram, ctx, runtime);
    ctx->pc = GPR_U32(ctx, 31);
}

constexpr auto patch_0x2cb4c8 =
    libmSoftDoubleShim<rrv_hle::atan2_soft_double_stub, ps2_stubs::atan2>;
constexpr auto patch_0x2cb730 =
    libmSoftDoubleShim<rrv_hle::pow_soft_double_stub, ps2_stubs::pow>;
constexpr auto patch_0x2ce618 =
    libmSoftDoubleShim<rrv_hle::atan_soft_double_stub, ps2_stubs::atan>;
constexpr auto patch_0x2cea28 =
    libmSoftDoubleShim<rrv_hle::fabs_soft_double_stub, ps2_stubs::fabs>;

// sceVu0RotMatrix (0x2CA7C0). NOTE, measured 2026-08-13: the generated body is
// not "the game's own trig" — it dispatches to func_2CA5E0/2CA720/2CA680, which
// the recompiler emitted as forwarders into ps2_stubs::sceVu0RotMatrix{Z,Y,X},
// i.e. the runtime's host std::sin/std::cos implementations. Both arms are host
// trig; the candidate changes rounding mode and multiply order, nothing else.
static void patch_0x2ca7c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == 0x2CA7C0u)
        rrv::guesttrace::rotMatrix(rdram, ctx);
    if (!p7RotGenerated() && !vu0Generated("rot")) {
        runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2ca7c0");
        rrv_hle::vu0RotMatrix(rdram, ctx, runtime);
        return;
    }
    const ScopedVu0Rounding rounding;
    sub_002CA7C0_0x2ca7c0(rdram, ctx, runtime);
}

// sceVu0MulMatrix (0x2CA250). The `in`/`out` records are format-matched to the
// scratch PCSX2 oracle's hooks at guest 0x2CA250 and 0x23E80C, so the two
// traces diff line-for-line at the phase-7 matrix boundary.
static void patch_0x2ca250(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool entry = (ctx->pc == 0x2CA250u);
    if (entry)
        rrv::guesttrace::mulMatrix(rdram, ctx, "in");
    const uint32_t ra = GPR_U32(ctx, 31);
    switch (vu0Generated("mul") ? P7MulImpl::Generated : p7MulImpl()) {
    case P7MulImpl::Rtz:
        runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2ca250");
        rrv_hle::vu0MulMatrixRtz(rdram, ctx, runtime);
        break;
    case P7MulImpl::Generated: {
        const ScopedVu0Rounding rounding;
        sub_002CA250_0x2ca250(rdram, ctx, runtime);
        break;
    }
    case P7MulImpl::Hle:
    default:
        runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2ca250");
        rrv_hle::vu0MulMatrix(rdram, ctx, runtime);
        break;
    }
    // The oracle's `out` edge is the instruction after sub_0023E720's call, so
    // emit it for that return address only. a0/a1/a2 in an `out` record are the
    // guest's post-call registers on PCSX2 but untouched under an HLE arm —
    // compare the base dumps, not the argument registers.
    if (ra == 0x23E80Cu && ctx->pc == ra)
        rrv::guesttrace::mulMatrix(rdram, ctx, "out");
}

// 0x294460 — SIF RPC end-callback: sets a global "IOP load done" flag.
// Grouped inside sub_00294440 (which ends with jr $ra at 0x294458).
// Generated body:
//   addiu $v0, $zero, 0x1      → $v0 = 1
//   sw    $v0, -0x5628($gp)    → [gp-22056] = 1   (done flag)
//   jr    $ra
static void patch_0x294460(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "patch:0x294460");
    // Sets $v0 = 1, stores it as the "IOP module load done" flag, then jr $ra.
    // When called via rpcInvokeFunction, $ra = 0x00FFF000 (sentinel).
    // Setting ctx->pc = $ra lets the mini-interpreter exit with Returned.
    SET_GPR_S32(ctx, 2, 1);
    WRITE32(ADD32(GPR_U32(ctx, 28), static_cast<uint32_t>(-22056)), 1u);
    ctx->pc = GPR_U32(ctx, 31); // jr $ra
}

// 0x21fdd8 — DMA-hold acquire (sceDmaHold-style helper).
// Reads D_ENABLER (0x1000F520) bit 16 (CPND); if the hold is already set it
// returns, otherwise it writes bit 16 to D_ENABLEW (0x1000F590) and *polls
// D_ENABLER until bit 16 reads back set*.  On real hardware writing D_ENABLEW
// propagates to D_ENABLER; the runtime doesn't model that alias, so the poll
// reads 0 forever and the EE deadlocks here (gsw=0, dma stuck).
//
// Our DMA executes synchronously at submit time — there is no background
// transfer to suspend — so acquiring the hold is a no-op that always succeeds.
// Return with $v0 = bit16 set (matching the hardware "held" status) and jr $ra.
static void patch_0x21fdd8(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "patch:0x21fdd8");
    SET_GPR_S32(ctx, 2, 0x10000); // $v0 = D_ENABLER & CPND  (hold acquired)
    ctx->pc = GPR_U32(ctx, 31);   // jr $ra
}

// 0x2c3000 — sceGsResetGraph: enable the CRT read-circuit (PMODE) the upstream
// runtime never sets.  The runtime's ps2_stubs::sceGsResetGraph configures the
// display by hand-building a GIF A+D packet that writes PMODE/SMODE2/DISPFB/
// DISPLAY — but the packet's register addresses (0xE pmode, 0x41 smode2, …) are
// not decoded by GS::writeRegister (no PMODE case; the only display-reg cases
// are 0x59..0x5f), so PMODE stays 0.  With PMODE.EN1=0 the presenter treats no
// CRT circuit as enabled and shows the magenta clear every frame, even though
// the GS rasterises sprites correctly into VRAM.
//
// The GS object's m_privRegs aliases PS2Memory::gs_regs (m_gs.init(.., &gs())),
// so we enable a CRT read-circuit directly via the GS privileged-register MMIO:
// write PMODE @ 0x12000000.  DISPFB1/DISPLAY1 already hold valid 640x448
// defaults, so this is enough to latch a real frame for presentation.
//
// The value written is HARDWARE'S: PMODE = 0x66, i.e. EN1=0 EN2=1 CRTMD=1
// MMOD=1 AMOD=1 SLBG=0 ALP=0, read out of local/gt/gt_A2_girl_clean_2026-08-02
// at the A2 anchor. It replaces the bring-up value 0x00008001 (EN1=1,
// ALP=0x80), which was picked only to get *a* circuit enabled and stop the
// magenta clear.
//
// That bring-up value was a real image defect, measured 2026-08-13 on a
// phase-matched A2 display list replayed through the pinned pcsx2-gsrunner
// (docs/TESTING.md T-G1-PMODE): mean luminance 31.23 with EN1 against 63.02 for
// hardware, i.e. our whole frame came out ~2x too dark, and pale rectangular
// blocks appeared in the sky. Selecting circuit 2 gives 67.77 and the blocks
// disappear; MAE against the hardware frame falls 37.54 -> 17.05.
//
// EN2 is the operative bit, not the mode/alpha fields: PMODE 0x02 (EN2 alone,
// every mode bit zero) measures identical to 0x66, and 0x0001 measures
// identical to 0x8001, so ALP is irrelevant here. Selecting the circuit is
// safe because applyGsDispEnv (Support.h) mirrors dispfb/display into BOTH
// circuits, so circuit 2 always carries the same buffer circuit 1 did.
//
// RRV_GS_PMODE_CRT1=1 restores the old EN1 value for A/B.
static inline void rrvEnablePmodeCrt(PS2Runtime& runtime) {
    static const uint32_t value = [] {
        const char* v = std::getenv("RRV_GS_PMODE_CRT1");
        return (v && v[0] != '\0' && v[0] != '0') ? 0x00008001u : 0x00000066u;
    }();
    runtime.memory().writeIORegister(0x12000000u, value);
}

static void patch_0x2c3000(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceGsResetGraph");
    ps2_stubs::sceGsResetGraph(rdram, ctx, runtime); // real CRT/gparam setup
    if (runtime) {
        rrvEnablePmodeCrt(*runtime); // PMODE: hardware's read circuit
    }
    ctx->pc = GPR_U32(ctx, 31); // jr $ra (also correct under indirect dispatch)
}

// 0x2c33b8 — sceGsPutDispEnv: applies a display environment to the GS privileged
// registers via applyGsDispEnv(), which does `regs.pmode = env.pmode`.  The env
// is built by sceGsSetDefDispEnv -> writeGsDispEnv, which sets DISPFB/DISPLAY but
// never PMODE, so env.pmode is 0 and applying it *clears* the EN1 bit we set in
// ResetGraph — killing presentation after the first frame.  Re-assert EN1 after
// the real apply so the read-circuit stays enabled.
static void patch_0x2c33b8(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceGsPutDispEnv");
    ps2_stubs::sceGsPutDispEnv(rdram, ctx, runtime);
    if (runtime) {
        rrvEnablePmodeCrt(*runtime);
    }
    ctx->pc = GPR_U32(ctx, 31); // jr $ra
}

// 0x220100 — guest DMA/GIF display-list completion barrier.
// [0x346E84] is a stage index written by the real completion handler, not a
// VBlank counter. Keep the diagnostic and FLAG_A controls below, then execute
// the original finite poll with modeled instruction time and real DMA service.
static void patch_0x220100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    constexpr uint32_t kDmaStage = 0x346E84u;
    const auto instruction = [&](uint32_t pc) {
        ctx->pc = pc;
        // The branch and its delay slot are one checkpoint unit.
        if (!ctx->in_delay_slot) runtime->gate3CheckpointV1(ctx);
        runtime->gate3BeginInstructionV1(ctx);
    };

    const bool resumingPoll = ctx->pc == 0x220120u;
    const bool resumingAfterSyncPath = ctx->pc == 0x220114u;
    static const bool frameBarrierDiag = [] {
        const char* value = std::getenv("RRV_FRAME_BARRIER_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    uint32_t barrierOnEntry = 0u;
    if (!resumingPoll) {
    if (!resumingAfterSyncPath) {

    // Preserve the original prologue and sceGsSyncPath(0) call. The real stub
    // services pending transfers; its result is not the stage-5 predicate.
    instruction(0x220100u);
    SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 4294967280u));
    instruction(0x220104u);
    SET_GPR_U64(ctx, 4, 0u);
    instruction(0x220108u);
    WRITE64(GPR_U32(ctx, 29), GPR_U64(ctx, 31));
    instruction(0x22010Cu);
    SET_GPR_U32(ctx, 31, 0x220114u);
    ctx->in_delay_slot = true;
    ctx->branch_pc = 0x22010Cu;
    instruction(0x220110u);
    SET_GPR_U64(ctx, 5, 0u);
    ctx->in_delay_slot = false;
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceGsSyncPath");
    ps2_stubs::sceGsSyncPath(rdram, ctx, runtime);
    }

    // This probe observes the real stage after SyncPath service. Host time is
    // diagnostic only and cannot mutate the stage or the guest poll budget.
    barrierOnEntry = frameBarrierDiag ? READ32(kDmaStage) : 0u;

    // The FLAG_A pin. FLAG_A ([gp-0x588C]) selects sub_00220470's short
    // two-entry DMA queue; forcing it to 2 makes the guest take that reduced
    // path, which is why it was once treated as a "60 fps fix".
    //
    // IT IS NOT A PERFORMANCE OPTIMISATION AND MUST NOT BE ONE AGAIN.
    // Measured 2026-08-13 (docs/TESTING.md T-MS-SPRITES): the reduced queue is a
    // different *workload*, not the same workload rendered faster. Pinned, the
    // guest emits primitive mix tristrip 0.97 / sprite 0.00; with the pin
    // released it emits 0.53 / 0.47 against hardware's 0.63 / 0.37. The pin
    // drops the entire 2D/sprite/cinematic workload — the character, the
    // cinematic letterbox bars and the attract overlays are one defect with
    // three symptoms. Any frame-rate number measured under the pin is measuring
    // a game that is not drawing what it should; do not build performance work
    // on it. See docs/KNOWN_ISSUES.md #26.
    //
    // The pin survives only where it is still load-bearing for *correctness*:
    // during boot it prevents the guest from selecting the full queue before the
    // boot queue's source table is stable (matching its pointers here re-enables
    // the pin on alternating frames), and it is not yet proven removable in the
    // phase-1 flyover. `RRV_FLAG_A_PIN` below is the scope control.
    constexpr uint32_t kScenePhase = 0x334E94u;
    constexpr uint32_t kAttractCursorPtr = 0x3348ACu;
    // The attract character's per-object record array: 44 records of 0x88 bytes,
    // renderable when the signed halfword at +0x82 is zero (PCSX2 at the A2
    // anchor: 33 of 44). Used by the reveal anchor below and by the queue diag.
    constexpr uint32_t kGirlRecordArray = 0x368500u;
    constexpr uint32_t kGirlRecordStride = 0x88u;
    constexpr uint32_t kGirlRecordEnableOfs = 0x82u;
    constexpr uint32_t kGirlRenderableRecords = 33u;
    const uint32_t scenePhase = READ32(kScenePhase);
    const bool bootPhase = scenePhase == 0u;
    // Surface the scene phase in the window title (one relaxed store per frame).
    rrv::hostwin::noteScenePhase(scenePhase);

    // ── PHASE-39 DIAGNOSTIC (RRV_PHASE39_DIAG=1, default off) ────────────────
    // Prints every change of the demo-VM state at scenePhase 39, so our own
    // walk can be diffed against hardware's (PINE, 17 kHz, see ANCHORS.md).
    //   sub   [0x334EA0] demo-VM substate  — hardware walks 0->1..6->0
    //   ready [0x334F00] the flag the retracted RRV_PHASE39_DEMO_READY forced —
    //                    hardware keeps it at 0 for the whole scene
    //   gate  [0x3683A0] & 0x800 — sub_0023E3C8's entry test
    //   scur  [0x334BA4] attract-script cursor, opcode = *(s16*)scur
    static const bool phase39Diag = [] {
        const char* v = std::getenv("RRV_PHASE39_DIAG");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    if (phase39Diag) {
        static bool once = false;
        if (!once) {
            once = true;
            char buf[40]{};
            for (int i = 0; i < 32; ++i) buf[i] = (char)rdram[(0x00082010u + i) & 0x01FFFFFFu];
            std::fprintf(stderr, "[boot-argv] [0x82000]=0x%08X str@0x82010=\"%s\" argc[0x335100]=%u argv[0x335104]=0x%08X\n",
                         READ32(0x00082000u), buf, READ32(0x00335100u), READ32(0x00335104u));
        }
        static uint64_t key = ~0ull;
        const uint32_t sub   = READ32(0x334EA0u);
        const uint32_t ready = READ32(0x334F00u);
        const uint32_t gate  = READ32(0x3683A0u) & 0xFFFFu;
        const uint32_t scur  = READ32(0x334BA4u);
        const uint32_t op    = (scur && scur < 0x02000000u) ? READ32(scur) : 0xFFFFFFFFu;
        // The top-level selector is what the PCSX2 reference reports as `sel`;
        // the demo race is 0x00020005. Keyed on so the log marks its edges.
        const uint32_t sel   = READ32(0x334E04u);
        static uint32_t selKey = ~0u;
        const uint64_t k = (uint64_t)scenePhase << 48 | (uint64_t)(sub & 0xFFu) << 40 |
                           (uint64_t)(ready != 0) << 39 | (uint64_t)(gate & 0xFFFFu) << 16 |
                           (uint64_t)(op & 0xFFFFu);
        if (k != key || sel != selKey) {
            rrvDumpFnHits("phase-change");
            ps2PathWatchHistDump("phase-change");
            std::fprintf(stderr,
                         "[p39] phase=%u sub=%u sel=0x%08X ready=0x%X gate16=0x%04X scur=0x%X op=0x%X\n",
                         scenePhase, sub, sel, ready, gate, scur, op);
            selKey = sel;
            key = k;
        }
        // RRV_P39_RAMDUMP=<prefix> — two raw dumps of the same guest ranges the
        // PINE side samples, taken 60 frames apart once the race selector
        // [0x334E04] == 0x20005 is live and has settled. Diffing "which words
        // changed" between the two, on each engine, names the subsystem that is
        // running on hardware and dead here.
        static const char* ramDump = std::getenv("RRV_P39_RAMDUMP");
        if (ramDump && ramDump[0]) {
            static const struct { uint32_t lo, hi; } ranges[] = {
                {0x00330000u, 0x00340000u}, {0x01D00000u, 0x01F00000u},
            };
            static int settle = -1, taken = 0;
            if (READ32(0x334E04u) == 0x00020005u) {
                if (settle < 0) settle = 0; else ++settle;
                const bool wantZ = (settle == 1   && taken == 0);
                const bool wantA = (settle == 300 && taken == 1);
                const bool wantB = (settle == 360 && taken == 2);
                if (wantZ || wantA || wantB) {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s.%c.bin", ramDump, wantZ ? 'z' : (wantA ? 'a' : 'b'));
                    if (FILE* fp = std::fopen(path, "wb")) {
                        for (const auto& r : ranges)
                            for (uint32_t a = r.lo; a < r.hi; a += 4u) {
                                const uint32_t w = READ32(a);
                                std::fwrite(&w, 4, 1, fp);
                            }
                        std::fclose(fp);
                    }
                    ++taken;
                    std::fprintf(stderr, "[p39-ramdump] wrote %s\n", path);
                }
            }
        }

        // Camera path rate, same probe scripts/cam_rate_pcsx2.py reads on the
        // reference: the view-inverse translation column (eye world position).
        static uint32_t camTick = 0u;
        if ((camTick % 180u) == 0u) {
            rrvDumpFnHits("periodic");
            ps2PathWatchHistDump("periodic");
            // RRV_GT_MEMDUMP=<hexaddr>:<hexlen> — same name and format as the
            // scratch PCSX2 probe, so the two dumps diff line for line. The
            // attract picks a different demo run to run, so only the STRUCTURE
            // (which words move at all) is comparable, never the values.
            static const char* memDump = std::getenv("RRV_GT_MEMDUMP");
            if (memDump && memDump[0]) {
                char* end = nullptr;
                const uint32_t base = (uint32_t)std::strtoul(memDump, &end, 16);
                const uint32_t len  = (end && *end == ':') ? (uint32_t)std::strtoul(end + 1, nullptr, 16) : 0u;
                std::fprintf(stderr, "[memdump] sel=0x%08X phase=%u\n", READ32(0x334E04u), scenePhase);
                for (uint32_t o = 0; o < len; o += 32) {
                    std::fprintf(stderr, "  mem %08x:", base + o);
                    for (uint32_t w = 0; w < 32 && o + w < len; w += 4)
                        std::fprintf(stderr, " %08x", READ32(base + o + w));
                    std::fputc('\n', stderr);
                }
            }
        }
        if ((camTick++ % 60u) == 0u) {
            const auto f32 = [&](uint32_t a) {
                const uint32_t bits = READ32(a); float v; std::memcpy(&v, &bits, 4); return v;
            };
            std::fprintf(stderr, "[p39-cam] phase=%u eye=%.3f,%.3f,%.3f\n",
                         scenePhase, f32(0x01E24EB0u), f32(0x01E24EB4u), f32(0x01E24EB8u));
        }
    }

    // RRV_FLAG_A_PIN=<scope> — how much of the attract sequence still runs under
    // the FLAG_A pin described above. This variable is named for what it
    // controls; its predecessor `RRV_GIRL_QUEUE_FIX` was named for the first
    // symptom anyone noticed and is accepted below only as a deprecated alias.
    //
    //   none   DEFAULT since 2026-08-14. Never pin, including boot: the guest
    //          owns FLAG_A everywhere, which is what hardware does.
    //   early  Release the pin at the A2 cinematic anchor and for every phase
    //          after it. Boot and the phase-1 flyover still run pinned. This
    //          was the default until 2026-08-14 and it SUPPRESSES CONTENT —
    //          see the evidence below.
    //   a2     Release it only at the exact A2 anchor (phase 2 / script 1 /
    //          step 2). Bisecting hatch: this is `early` minus the post-A2 half.
    //   all    Pin in every non-boot phase — the original "60 fps" behaviour.
    //          Diagnostic rollback ONLY. This does not render the game.
    //   boot   Pin during boot only (what the retired `RRV_DISABLE_60FPS=1`
    //          did). Isolates the phase-1 flyover's dependency on the pin.
    //
    // Why the default moved from `early` to `none` (T-FLAGA-PIN-NONE). `early`
    // still pins boot and the phase-1 flyover, and that pin was measured
    // deleting guest content in both:
    //
    //   * the NAMCO logo plays white -> red -> "PRODUCED BY namco" on hardware.
    //     Under `early` we render only the red stage; under `none` we render
    //     the whole sequence, hardware's order and hardware's stages.
    //   * the "RIDGE CITY FM 76.5MHz" overlay is absent from the phase-1
    //     flyover under `early` and present under `none`.
    //
    // The comment here used to say the boot pin was "not known to be removable".
    // It is: `none` boots and runs phases 0, 1, 2, 6 and 7, phase 6 plays its
    // full "A New Beginning" title-card animation, and phase 2 replays against
    // the hardware capture at FRAME MAE 0.00 with every region ratio 1.00.
    //
    // Evidence for the default (docs/TESTING.md T-MS-SPRITES), primitive mix on
    // the authoritative consumed stream, post-A2 band, vs the hardware A3
    // capture:
    //     all    tristrip 0.97 / sprite 0.00   mix distance 0.35 (NOT COMPARABLE)
    //     early  tristrip 0.53 / sprite 0.47   mix distance 0.10 (WEAK)
    //     hardware A3  tristrip 0.63 / sprite 0.37
    // Under `all`, a full 1,400-present attract loop contains ZERO letterboxed
    // cinematic frames — the A2 section does not render at all, rather than
    // rendering without its character.
    enum class FlagAPinScope { None, Boot, A2, Early, All };
    static const char* const kFlagAPinNames[] = {"none", "boot", "a2", "early", "all"};
    static const FlagAPinScope flagAPinScope = [] {
        const char* value = std::getenv("RRV_FLAG_A_PIN");
        if (value && value[0] != '\0') {
            for (int i = 0; i < 5; ++i) {
                if (std::strcmp(value, kFlagAPinNames[i]) == 0)
                    return static_cast<FlagAPinScope>(i);
            }
            std::fprintf(stderr, "[flag-a] unknown RRV_FLAG_A_PIN=%s; using 'none'\n", value);
            return FlagAPinScope::None;
        }
        // Deprecated aliases, kept so existing capture scripts keep their
        // meaning instead of silently changing it.
        const char* legacy = std::getenv("RRV_GIRL_QUEUE_FIX");
        if (legacy && legacy[0] != '\0') {
            const FlagAPinScope mapped = legacy[0] == '0'   ? FlagAPinScope::All
                                         : legacy[0] == '2' ? FlagAPinScope::Early
                                                            : FlagAPinScope::A2;
            std::fprintf(stderr,
                         "[flag-a] RRV_GIRL_QUEUE_FIX=%s is deprecated; use "
                         "RRV_FLAG_A_PIN=%s\n",
                         legacy, kFlagAPinNames[static_cast<int>(mapped)]);
            return mapped;
        }
        if (std::getenv("RRV_DISABLE_60FPS")) {
            std::fprintf(stderr, "[flag-a] RRV_DISABLE_60FPS is deprecated (and was "
                                 "never a frame-rate control); use RRV_FLAG_A_PIN=boot\n");
            return FlagAPinScope::Boot;
        }
        return FlagAPinScope::None;
    }();
    // `boot`/`none` release the pin unconditionally after boot, so the A2 and
    // post-A2 releases are implied by them too.
    const bool releaseAtA2 = flagAPinScope != FlagAPinScope::All;
    const bool releaseAfterA2 = flagAPinScope != FlagAPinScope::All &&
                                flagAPinScope != FlagAPinScope::A2;
    static const bool girlQueueDiagEnabled = [] {
        const char* value = std::getenv("RRV_GIRL_QUEUE_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();

    // RRV_ATTRACT_TIMELINE=1 — log every new (phase, script, step) the attract
    // sequence enters. This is how a checkpoint's semantic coordinates are
    // discovered before it can be anchored on; it is the forward-sweep
    // equivalent of the A2 girl-queue diagnostic.
    static const bool attractTimeline = [] {
        const char* value = std::getenv("RRV_ATTRACT_TIMELINE");
        return value && value[0] != '\0' && value[0] != '0';
    }();

    // Inert unless a bounded M2 manifest is configured. The observer uses the
    // established attract cursor but performs no file I/O during guest work.
    const bool m2NeutralityEnabled = rrv::m2neutral::enabled();

    // RRV_GS_RECORD_ANCHOR=<phase>[:<script>:<step>] — arm the recorder (and the
    // snapshot checkpoint) at an arbitrary attract checkpoint instead of the A2
    // girl anchor, so the lockstep sweep can capture A1/A3 without a new patch
    // per scene. Unset keeps the A2 behaviour exactly.
    struct RecordAnchor { bool set; uint32_t phase, script, step; };
    static const RecordAnchor recordAnchor = [] {
        RecordAnchor a{false, 0u, UINT32_MAX, UINT32_MAX};
        const char* value = std::getenv("RRV_GS_RECORD_ANCHOR");
        if (!value || value[0] == '\0') return a;
        char* end = nullptr;
        a.phase = static_cast<uint32_t>(std::strtoul(value, &end, 0));
        a.set = true;
        if (end && *end == ':') {
            a.script = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
            if (end && *end == ':') {
                a.step = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0));
            }
        }
        std::fprintf(stderr, "[anchor] recorder armed on phase=%u script=%u step=%u\n",
                     a.phase, a.script, a.step);
        return a;
    }();

    uint32_t attractCursor = 0u;
    uint32_t attractScript = UINT32_MAX;
    uint32_t attractStep = UINT32_MAX;
    if ((releaseAtA2 || girlQueueDiagEnabled || attractTimeline ||
         recordAnchor.set || m2NeutralityEnabled) && scenePhase >= 1u) {
        attractCursor = READ32(kAttractCursorPtr);
        if (attractCursor != 0u && (attractCursor & 3u) == 0u &&
            attractCursor <= 0x01FFFFF8u) {
            attractScript = READ32(attractCursor);
            attractStep = READ32(attractCursor + 4u);
        }
    }
    if (m2NeutralityEnabled) {
        // Selector observation is guest state at this established patch seam.
        // The M2 control arms only on its one-shot non-target -> target edge.
        const uint32_t selector = READ32(0x334E04u);
        rrv::m2neutral::observeSemanticState(
            scenePhase, attractScript, attractStep, selector,
            ps2_syscalls::GetCurrentVSyncTick());
    }

    if (attractTimeline) {
        static uint32_t lastP = UINT32_MAX, lastSc = UINT32_MAX, lastSt = UINT32_MAX;
        if (scenePhase != lastP || attractScript != lastSc || attractStep != lastSt) {
            std::fprintf(stderr, "[attract] phase=%u script=%d step=%d\n",
                         scenePhase, static_cast<int>(attractScript),
                         static_cast<int>(attractStep));
            // Stamp the same coordinates into any open .gstrace, so ONE long
            // capture can be sliced per checkpoint offline instead of needing a
            // fresh live cold boot for every scene in the sweep.
            rrv::gstrace::sceneMark(scenePhase, attractScript, attractStep);
            lastP = scenePhase; lastSc = attractScript; lastSt = attractStep;
        }
    }

    // True wherever the guest, not this patch, owns FLAG_A for this frame.
    bool guestOwnsFlagA = false;
    if (releaseAtA2 && scenePhase == 2u && attractScript == 1u &&
        attractStep == 2u) {
        guestOwnsFlagA = true;
    } else if (releaseAfterA2 && scenePhase > 2u) {
        guestOwnsFlagA = true;
    }

    // Bounded GS capture of the girl scene. With RRV_GS_RECORD_ARM=1 the
    // recorder writes nothing until this point, which is what makes a diffable
    // capture possible at all: recording from launch produces a ~1.5 GB file
    // because this scene is thousands of frames into the attract loop. Combine
    // with RRV_GS_RECORD_FRAMES=N to auto-stop. No-op unless RRV_GS_RECORD and
    // RRV_GS_RECORD_ARM are both set.
    // The A2_girl anchor condition, evaluated once and shared by the two
    // consumers below. This is the same guest-state predicate every B-3 capture
    // has been aligned on (scene phase 2, attract cursor {script=1, step=2}) and
    // the same moment as PCSX2 savestate slot 3 / local/gt/gt_A2_girl.gsr.
    bool atGirlAnchor = false;
    if (recordAnchor.set) {
        atGirlAnchor =
            scenePhase == recordAnchor.phase &&
            (recordAnchor.script == UINT32_MAX || attractScript == recordAnchor.script) &&
            (recordAnchor.step == UINT32_MAX || attractStep == recordAnchor.step);
    } else if (scenePhase == 2u) {
        const uint32_t cursor = READ32(kAttractCursorPtr);
        atGirlAnchor = cursor != 0u && (cursor & 3u) == 0u && cursor <= 0x01FFFFF8u &&
                       READ32(cursor) == 1u && READ32(cursor + 4u) == 2u;
    }

    // RRV_GIRL_ANCHOR=reveal moves both consumers below from the *scene-step*
    // anchor to the *reveal* anchor: the first frame on which the character's
    // model records are actually renderable. Measured 2026-08-01: the step-2
    // anchor is ~158 presents BEFORE that — its whole record array is disabled
    // (`enabled=0/44`), so a capture taken there contains no character at all,
    // while PCSX2 savestate slot 3 / local/gt/gt_A2_girl.gsr was saved with her
    // on screen. Comparing the two aligns nothing, which is what
    // scripts/gsr_comparable.py reports as NOT COMPARABLE.
    //
    // The predicate is the same one the queue diagnostic counts: a record is
    // renderable when its signed halfword at +0x82 is zero, and hardware has
    // 33 of 44 in that state at the anchor moment. Present index cannot serve
    // as the alignment key — it is wall-clock paced against a ~20 Hz game loop,
    // so enabling the GS recorder alone moves the reveal by ~100 presents.
    static const bool girlRevealAnchor = [] {
        const char* value = std::getenv("RRV_GIRL_ANCHOR");
        return value && std::strcmp(value, "reveal") == 0;
    }();
    // The reveal refinement is A2-specific; an explicit checkpoint anchor
    // replaces it rather than being ANDed with it.
    if (girlRevealAnchor && !recordAnchor.set) {
        bool revealed = false;
        if (atGirlAnchor) {
            uint32_t renderable = 0u;
            for (uint32_t i = 0u; i < 44u; ++i) {
                if (static_cast<int16_t>(READ16(kGirlRecordArray + i * kGirlRecordStride +
                                                kGirlRecordEnableOfs)) == 0) {
                    ++renderable;
                }
            }
            revealed = renderable >= kGirlRenderableRecords;
        }
        atGirlAnchor = revealed;
    }

    // Stand down while a snapshot capture/restore is armed: the checkpoint
    // instant (the next dispatch safepoint) is then the authoritative start of
    // the recording, so a continuous and a restored run record the exact same
    // window. Triggering here as well would start the continuous run's
    // recording a few hundred guest instructions early and offset the two
    // packet streams against each other. See rrv::snapshot::ownsRecorderArming().
    if (rrv::gsrecord::rrv_gs_record_armed() && !rrv::snapshot::ownsRecorderArming()) {
        // Arm on the A2 anchor itself, independent of the queue-fix experiment,
        // so a default-build capture of the same scene is possible too.
        if (atGirlAnchor) {
            rrv::gsrecord::rrv_gs_record_trigger();
        }
    }

    // The authoritative GS-consumption trace (KNOWN_ISSUES #22) arms on the same
    // checkpoint predicate, so a .gsr and a .gstrace of the same scene start at
    // the same guest instant and can be compared directly.
    if (atGirlAnchor) {
        rrv::gstrace::trigger();
    }

    // Developer checkpoint: `--snapshot-save girl --snapshot-at latch` captures
    // at the first dispatch safepoint after this same anchor. The condition
    // lives here because this is where the game-specific knowledge already is;
    // the snapshot framework itself stays free of any scene table
    // (docs/SNAPSHOTS.md §5). No-op unless a latch capture was armed.
    if (atGirlAnchor) {
        rrv::snapshot::requestCapture("a2_girl_anchor:phase2,script1,step2");
    }

    // RRV_FIX_SCAN=<n> — bounded provenance scan for the A2 feedback weight.
    // Dumps the guest global segment (0 .. kFixScanBytes) once per game-loop
    // iteration for the first <n> iterations after the A2 anchor, so an offline
    // correlator can find the guest word whose value drives the ALPHA.FIX of the
    // aMode=0x68 feedback pass. Dump k corresponds to display list k of a .gsr
    // recorded in the same run (one list is built per iteration of this loop).
    // Diagnostic only, default off; delete-safe.
    static const int fixScanFrames = [] {
        const char* value = std::getenv("RRV_FIX_SCAN");
        return value ? std::atoi(value) : 0;
    }();
    if (fixScanFrames > 0) {
        // Default covers every known global; widen with RRV_FIX_SCAN_BYTES when
        // the target lives in a display-list arena higher in RAM.
        static const uint32_t kFixScanBytes = [] {
            const char* value = std::getenv("RRV_FIX_SCAN_BYTES");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
                         : 0x400000u;
        }();
        static int scanned = -1;
        if (scanned < 0 && atGirlAnchor) {
            scanned = 0;
        }
        if (scanned >= 0 && scanned < fixScanFrames) {
            char path[256];
            const char* dir = std::getenv("RRV_FIX_SCAN_DIR");
            std::snprintf(path, sizeof(path), "%s/ram_%03d.bin",
                          dir ? dir : ".", scanned);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fwrite(rdram, 1, kFixScanBytes, f);
                std::fclose(f);
            }
            // The scratchpad too (16 KB, cheap): a PATH3 chain can source from
            // it, so a display-list search that skips SPR can report a false
            // absence.
            std::snprintf(path, sizeof(path), "%s/spr_%03d.bin",
                          dir ? dir : ".", scanned);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fwrite(runtime->memory().getScratchpad(), 1, 16u * 1024u, f);
                std::fclose(f);
            }
            ++scanned;
        }
    }

    if (girlQueueDiagEnabled) {
        const uint32_t flagA = READ32(0x334DE4u);
        const uint32_t auxSelector = READ32(0x334DC8u);
        static uint32_t lastPhase = UINT32_MAX;
        static uint32_t lastCursor = UINT32_MAX;
        static uint32_t lastScript = UINT32_MAX;
        static uint32_t lastStep = UINT32_MAX;
        static uint32_t lastFlagA = UINT32_MAX;
        static uint32_t lastAuxSelector = UINT32_MAX;
        if (scenePhase != lastPhase || attractCursor != lastCursor ||
            attractScript != lastScript || attractStep != lastStep ||
            flagA != lastFlagA || auxSelector != lastAuxSelector) {
            std::fprintf(stderr,
                         "[girl-queue] phase=%u cursor=%08x script=%u step=%u "
                         "flagA=%u aux=%u preserve=%u\n",
                         scenePhase, attractCursor, attractScript, attractStep,
                         flagA, auxSelector, guestOwnsFlagA ? 1u : 0u);
            lastPhase = scenePhase;
            lastCursor = attractCursor;
            lastScript = attractScript;
            lastStep = attractStep;
            lastFlagA = flagA;
            lastAuxSelector = auxSelector;
        }
    }

    // `none` releases FLAG_A even during boot; `boot` pins there and nowhere
    // else. Boot is the only place the pin is still known to be load-bearing.
    const bool pinThisFrame =
        flagAPinScope != FlagAPinScope::None &&
        (bootPhase ? true
                   : (flagAPinScope != FlagAPinScope::Boot && !guestOwnsFlagA));
    if (pinThisFrame) {
        const uint32_t gp = GPR_U32(ctx, 28);
        rrv::guesttrace::flagWrite(rdram, ctx, 0x220100u, 2u); // host pin, not a guest writer
        WRITE32(ADD32(gp, 4294944628u), 2u); // [gp-0x588C] (FLAG_A) = 2
    }
    // Original 0x220114..0x220140 sequence: a finite 0x02000000-poll
    // budget, stage >= 5 success, and timeout return with v1 == 0. A stage
    // write may come only from the guest's DMA/GIF completion route.
    instruction(0x220114u);
    SET_GPR_S32(ctx, 4, (int32_t)(52u << 16));
    instruction(0x220118u);
    SET_GPR_S32(ctx, 3, (int32_t)(512u << 16));
    instruction(0x22011Cu); // architectural NOP
    }
    uint32_t pollIterations = 0u;
    for (;;) {
        instruction(0x220120u);
        runtime->memory().processPendingTransfers();
        SET_GPR_S32(ctx, 2, (int32_t)READ32(kDmaStage));
        ++pollIterations;
        instruction(0x220124u);
        SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) < 5u ? 1u : 0u);
        instruction(0x220128u);
        const bool stageComplete = GPR_U64(ctx, 2) == 0u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x220128u;
        instruction(0x22012Cu);
        SET_GPR_U64(ctx, 31, READ64(GPR_U32(ctx, 29)));
        ctx->in_delay_slot = false;
        if (stageComplete) break;
        instruction(0x220130u);
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), 4294967295u));
        instruction(0x220134u);
        const bool keepPolling = GPR_S64(ctx, 3) > 0;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x220134u;
        instruction(0x220138u); // architectural branch-delay NOP
        ctx->in_delay_slot = false;
        if (!keepPolling) break;
        ctx->pc = 0x220120u;
        if (runtime->shouldPreemptGuestExecution()) return;
    }

    if (frameBarrierDiag && !resumingPoll) {
        static uint64_t entryBuckets[7]{};
        static uint64_t pollBuckets[9]{};
        static uint64_t frames = 0u;
        static uint32_t lastPhase = UINT32_MAX;
        static auto lastReport = std::chrono::steady_clock::now();
        ++frames;
        ++entryBuckets[barrierOnEntry <= 5u ? barrierOnEntry : 6u];
        ++pollBuckets[pollIterations <= 8u ? pollIterations : 8u];
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
        const uint32_t scenePhaseForBarrierDiag = READ32(0x334E94u);
        if (ms >= 1000 || scenePhaseForBarrierDiag != lastPhase) {
            std::fprintf(stderr,
                         "[frame-barrier] phase=%u wall=%.3f frames=%llu build_hz=%.2f "
                         "entry[0|1|2|3|4|5|>5]=%llu %llu %llu %llu %llu %llu %llu "
                         "polls[0..7|>=8]=%llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
                         scenePhaseForBarrierDiag, static_cast<double>(ms) / 1000.0,
                         static_cast<unsigned long long>(frames),
                         ms > 0 ? static_cast<double>(frames) * 1000.0 / static_cast<double>(ms) : 0.0,
                         (unsigned long long)entryBuckets[0], (unsigned long long)entryBuckets[1],
                         (unsigned long long)entryBuckets[2], (unsigned long long)entryBuckets[3],
                         (unsigned long long)entryBuckets[4], (unsigned long long)entryBuckets[5],
                         (unsigned long long)entryBuckets[6],
                         (unsigned long long)pollBuckets[0], (unsigned long long)pollBuckets[1],
                         (unsigned long long)pollBuckets[2], (unsigned long long)pollBuckets[3],
                         (unsigned long long)pollBuckets[4], (unsigned long long)pollBuckets[5],
                         (unsigned long long)pollBuckets[6], (unsigned long long)pollBuckets[7],
                         (unsigned long long)pollBuckets[8]);
            lastReport = now;
            lastPhase = scenePhaseForBarrierDiag;
            frames = 0u;
            std::memset(entryBuckets, 0, sizeof(entryBuckets));
            std::memset(pollBuckets, 0, sizeof(pollBuckets));
        }
    }

    instruction(0x22013Cu);
    const uint32_t returnAddress = GPR_U32(ctx, 31);
    ctx->in_delay_slot = true;
    ctx->branch_pc = 0x22013Cu;
    instruction(0x220140u);
    SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 16u));
    ctx->in_delay_slot = false;
    ctx->pc = returnAddress;

}

// Diagnostic wrapper at sub_00220470's entry, immediately before its queue
// selection. It is registered only with RRV_GIRL_QUEUE_DIAG=1 and delegates to
// the original generated body, so it cannot affect default execution. Log only
// changes during A2's exact phase/script/step window; these two arrays are the
// source and selected DMA queue roots copied by 0x2204BC..0x220590.
static void patch_0x220470_girl_queue_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t cursor = READ32(0x3348ACu);
    const bool validCursor = cursor != 0u && (cursor & 3u) == 0u &&
                             cursor <= 0x01FFFFF8u;
    if (READ32(0x334E94u) == 2u && validCursor &&
        READ32(cursor) == 1u && READ32(cursor + 4u) == 2u) {
        uint32_t enabledGirlRecords = 0u;
        for (uint32_t i = 0u; i < 44u; ++i) {
            enabledGirlRecords +=
                static_cast<int16_t>(READ16(0x368500u + i * 0x88u + 0x82u)) == 0
                    ? 1u : 0u;
        }
        static bool dumpedEeRam = false;
        // 0x22D390 considers a record renderable when its signed halfword at
        // +0x82 is zero.  PCSX2's A2 state has 33/44 such records.  The earlier
        // diagnostic accidentally counted +0x86, which is nonzero for all 44
        // records in both PCSX2 and native and therefore dumped too early.
        if (!dumpedEeRam && READ32(0x334DE4u) == 0u &&
            enabledGirlRecords >= 33u) {
            if (const char* dumpPath = std::getenv("RRV_GIRL_EE_DUMP");
                dumpPath && dumpPath[0] != '\0') {
                if (std::FILE* dump = std::fopen(dumpPath, "wb")) {
                    constexpr size_t kEeRamBytes = 0x02000000u;
                    const size_t written = std::fwrite(rdram, 1u, kEeRamBytes, dump);
                    std::fclose(dump);
                    std::fprintf(stderr,
                                 "[girl-roots] EE RAM dump %s: %zu/%zu bytes\n",
                                 dumpPath, written, kEeRamBytes);
                } else {
                    std::fprintf(stderr,
                                 "[girl-roots] failed to open EE RAM dump %s\n",
                                 dumpPath);
                }
                dumpedEeRam = true;
            }
        }

        uint32_t source[8];
        uint32_t selected[8];
        for (uint32_t i = 0; i < 8u; ++i) {
            source[i] = READ32(0x347100u + i * 4u);
            selected[i] = READ32(0x3470C0u + i * 4u);
        }
        static uint32_t lastSource[8] = {};
        static uint32_t lastSelected[8] = {};
        static uint32_t lastEnabledGirlRecords = UINT32_MAX;
        static bool haveLast = false;
        bool changed = !haveLast || enabledGirlRecords != lastEnabledGirlRecords;
        for (uint32_t i = 0; i < 8u; ++i) {
            changed = changed || source[i] != lastSource[i] ||
                      selected[i] != lastSelected[i];
        }
        if (changed) {
            std::fprintf(stderr,
                         "[girl-roots] flagA=%u aux=%u enabled82=%u/44 "
                         "src=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                         "sel=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                         READ32(0x334DE4u), READ32(0x334DC8u),
                         enabledGirlRecords,
                         source[0], source[1], source[2], source[3],
                         source[4], source[5], source[6], source[7],
                         selected[0], selected[1], selected[2], selected[3],
                         selected[4], selected[5], selected[6], selected[7]);
            for (uint32_t i = 0; i < 8u; ++i) {
                lastSource[i] = source[i];
                lastSelected[i] = selected[i];
            }
            haveLast = true;
            lastEnabledGirlRecords = enabledGirlRecords;
        }
    }

    sub_00220470_0x220470(rdram, ctx, runtime);
}

// Generic REF-tag splicer used by the EE-built DMA/VIF chains.  The A2 PCSX2
// chain contains 34 large REF blocks that the native chain omits.  Trace the
// splicer's caller and arguments only during the exact girl script step so the
// producer can be identified without enabling the very noisy global function
// logger.  This wrapper is diagnostic-only and always delegates unchanged.
static thread_local bool s_girlGeometryFlushActive = false;
static uint32_t s_girlGeometryFlushCount = 0u;
static uint32_t s_girlGeometryRefCount = 0u;
static bool rrvExactGirlStep(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);

static void patch_0x21f9c0_girl_ref_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t cursor = READ32(0x3348ACu);
    const bool exactGirlStep = READ32(0x334E94u) == 2u &&
        cursor != 0u && (cursor & 3u) == 0u && cursor <= 0x01FFFFF8u &&
        READ32(cursor) == 1u && READ32(cursor + 4u) == 2u;
    static const bool verboseDiag = [] {
        const char* value = std::getenv("RRV_GIRL_QUEUE_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    if (exactGirlStep && verboseDiag) {
        static uint32_t callCount = 0u;
        if (callCount < 512u) {
            std::fprintf(stderr,
                         "[girl-ref] n=%u ra=%08x dst=%08x splice=%08x "
                         "src=%08x qwc=%u\n",
                         callCount, GPR_U32(ctx, 31), GPR_U32(ctx, 4),
                         GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7));
        }
        ++callCount;
        if (s_girlGeometryFlushActive && s_girlGeometryRefCount < 96u) {
            const uint32_t geometryQwc = GPR_U32(ctx, 7);
            std::fprintf(stderr,
                         "[girl-geometry-ref] n=%u dst=%08x splice=%08x "
                         "src=%08x qwc=%u\n",
                         s_girlGeometryRefCount, GPR_U32(ctx, 4),
                         GPR_U32(ctx, 5), GPR_U32(ctx, 6), geometryQwc);
            static bool dumpedGeometryFrame = false;
            if (!dumpedGeometryFrame && geometryQwc == 0x2E1Au) {
                if (const char* dumpPath = std::getenv("RRV_GIRL_GEOMETRY_DUMP");
                    dumpPath && dumpPath[0] != '\0') {
                    if (std::FILE* dump = std::fopen(dumpPath, "wb")) {
                        std::fwrite(rdram, 1u, 0x02000000u, dump);
                        std::fclose(dump);
                        std::fprintf(stderr,
                                     "[girl-geometry-ref] dumped EE RAM -> %s\n",
                                     dumpPath);
                        dumpedGeometryFrame = true;
                    }
                }
            }
            ++s_girlGeometryRefCount;
        }
    }
    sub_0021F9C0_0x21f9c0(rdram, ctx, runtime);
}

// Trace the producer that flushes each scratch packet through 0x21FCE8 before
// it reaches the generic REF splicer above.  Its incoming $ra identifies the
// actual renderer/model routine; $a0 is the queue node being spliced.
static void patch_0x21fce8_girl_flush_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t cursor = READ32(0x3348ACu);
    const bool exactGirlStep = READ32(0x334E94u) == 2u &&
        cursor != 0u && (cursor & 3u) == 0u && cursor <= 0x01FFFFF8u &&
        READ32(cursor) == 1u && READ32(cursor + 4u) == 2u;
    const uint32_t caller = GPR_U32(ctx, 31);
    const bool geometryFlush = exactGirlStep && caller == 0x0022E154u;
    const uint32_t geometryNode = GPR_U32(ctx, 4);
    const uint32_t scratchCurrent = READ32(0x003470A0u);
    const uint32_t scratchEnd = READ32(0x003470A4u);
    const uint32_t geometryQwc =
        ((scratchCurrent & 0x0FFFFFFFu) - (scratchEnd & 0x0FFFFFFFu) + 15u) >> 4u;
    static const bool verboseDiag = [] {
        const char* value = std::getenv("RRV_GIRL_QUEUE_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    if (exactGirlStep && verboseDiag) {
        static uint32_t callCount = 0u;
        if (callCount < 512u) {
            std::fprintf(stderr, "[girl-flush] n=%u ra=%08x node=%08x\n",
                         callCount, caller, GPR_U32(ctx, 4));
        }
        ++callCount;
    }
    const uint32_t geometryFlushIndex = s_girlGeometryFlushCount;
    if (geometryFlush && geometryFlushIndex < 96u) {
        std::fprintf(stderr,
                     "[girl-geometry-flush] n=%u node=%08x begin=%08x "
                     "end=%08x qwc=%u root-before=%08x\n",
                     geometryFlushIndex, geometryNode, scratchCurrent,
                     scratchEnd, geometryQwc, READ32(geometryNode));
    }
    if (geometryFlush) ++s_girlGeometryFlushCount;

    // Once the large model REF has been prepended, follow every later splice
    // into that same queue root. The first call whose root-before no longer
    // points through the geometry head identifies the producer that replaced
    // the model chain instead of extending it.
    static uint32_t watchedNode = 0u;
    static uint32_t watchedGeometryHead = 0u;
    static uint32_t watchedCalls = 0u;
    if (geometryFlush && geometryQwc >= 0x1000u) {
        watchedNode = geometryNode;
        watchedGeometryHead = scratchCurrent & 0x0FFFFFFFu;
        watchedCalls = 0u;
        std::fprintf(stderr,
                     "[girl-root-watch-start] node=%08x head=%08x old-target=%08x "
                     "qwc=%u\n",
                     watchedNode, watchedGeometryHead,
                     READ32(watchedNode + 4u) & 0x0FFFFFFFu, geometryQwc);
    }
    const bool watchThisCall = exactGirlStep && watchedNode == geometryNode &&
        watchedGeometryHead != 0u && watchedCalls < 96u;
    if (watchThisCall) {
        std::fprintf(stderr,
                     "[girl-root-watch] n=%u ra=%08x geometry=%u qwc=%u "
                     "before=%08x expected-head=%08x\n",
                     watchedCalls, caller, geometryFlush ? 1u : 0u,
                     geometryQwc, READ32(geometryNode + 4u) & 0x0FFFFFFFu,
                     watchedGeometryHead);
    }
    const bool previousGeometryFlush = s_girlGeometryFlushActive;
    s_girlGeometryFlushActive = geometryFlush;
    sub_0021FCE8_0x21fce8(rdram, ctx, runtime);
    s_girlGeometryFlushActive = previousGeometryFlush;

    if (geometryFlush && geometryFlushIndex < 96u) {
        std::fprintf(stderr,
                     "[girl-geometry-flush-post] n=%u node=%08x root-after=%08x "
                     "target-after=%08x scratch=%08x,%08x\n",
                     geometryFlushIndex, geometryNode, READ32(geometryNode),
                     READ32(geometryNode + 4u) & 0x0FFFFFFFu,
                     READ32(0x003470A0u), READ32(0x003470A4u));
    }
    if (watchThisCall) {
        std::fprintf(stderr,
                     "[girl-root-watch-post] n=%u ra=%08x after=%08x\n",
                     watchedCalls, caller,
                     READ32(geometryNode + 4u) & 0x0FFFFFFFu);
        ++watchedCalls;
    }

    if (geometryFlush) {
        // The first active model pass has 33 records. Capture after its final
        // splice, when the complete native model chain is rooted and can be
        // compared directly with PCSX2's 0x3472A0 chain.
        static bool dumpedPostModelChain = false;
        if (!dumpedPostModelChain && geometryQwc >= 0x1000u) {
            if (const char* dumpPath = std::getenv("RRV_GIRL_MODEL_POST_DUMP");
                dumpPath && dumpPath[0] != '\0') {
                if (std::FILE* dump = std::fopen(dumpPath, "wb")) {
                    constexpr size_t kEeRamBytes = 0x02000000u;
                    const size_t written =
                        std::fwrite(rdram, 1u, kEeRamBytes, dump);
                    std::fclose(dump);
                    std::fprintf(stderr,
                                 "[girl-geometry-flush-post] EE RAM dump %s: "
                                 "%zu/%zu bytes\n",
                                 dumpPath, written, kEeRamBytes);
                    dumpedPostModelChain = true;
                }
            }
        }
    }
}

// Append a CALL tag to the current model queue.  sub_0021FD98 reads the
// current scratch cursor, delegates to 0x21F980, and advances both scratch
// pointers.  During A2 the calls from 0x22D390 should supply the CALL(0x6786)
// and CALL(0x067f) prefixes that surround the already-verified geometry refs.
static void patch_0x21fd98_girl_call_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool exactGirlStep = rrvExactGirlStep(rdram, ctx, runtime);
    const uint32_t caller = GPR_U32(ctx, 31);
    const uint32_t node = GPR_U32(ctx, 4);
    const uint32_t packet = GPR_U32(ctx, 5);
    const uint32_t scratchBefore = READ32(0x003470A0u);
    const uint32_t targetBefore =
        node <= 0x01FFFFF8u ? (READ32(node + 4u) & 0x0FFFFFFFu) : 0u;

    sub_0021FD98_0x21fd98(rdram, ctx, runtime);

    uint32_t liveEnabledRecords = 0u;
    if (exactGirlStep) {
        for (uint32_t i = 0u; i < 44u; ++i) {
            liveEnabledRecords +=
                static_cast<int16_t>(READ16(0x00368500u + i * 0x88u + 0x82u)) == 0
                    ? 1u : 0u;
        }
    }
    if (exactGirlStep && caller == 0x0022D51Cu && liveEnabledRecords >= 33u) {
        static uint32_t callCount = 0u;
        if (callCount < 96u) {
            std::fprintf(stderr,
                         "[girl-call] n=%u enabled=%u node=%08x packet=%08x "
                         "scratch=%08x->%08x target=%08x->%08x\n",
                         callCount, liveEnabledRecords, node, packet, scratchBefore,
                         READ32(0x003470A0u), targetBefore,
                         node <= 0x01FFFFF8u
                             ? (READ32(node + 4u) & 0x0FFFFFFFu)
                             : 0u);
        }
        ++callCount;
    }

    // 0x23C6D8 appends the final setup CALL (target 0x12BD1C0) after the
    // 14 REF(0x1f)/REF(0x08) pairs.  At this exact point the queue root is the
    // complete frame chain that sub_00220470 will submit to VIF1 next.
    static bool dumpedPreSendChain = false;
    if (exactGirlStep && caller == 0x0023C6E0u &&
        packet == 0x012BD1C0u && liveEnabledRecords >= 33u &&
        !dumpedPreSendChain) {
        if (const char* dumpPath = std::getenv("RRV_GIRL_PRESEND_DUMP");
            dumpPath && dumpPath[0] != '\0') {
            if (std::FILE* dump = std::fopen(dumpPath, "wb")) {
                constexpr size_t kEeRamBytes = 0x02000000u;
                const size_t written = std::fwrite(rdram, 1u, kEeRamBytes, dump);
                std::fclose(dump);
                std::fprintf(stderr,
                             "[girl-presend] root=%08x target=%08x dump=%s "
                             "bytes=%zu/%zu\n",
                             node, READ32(node + 4u) & 0x0FFFFFFFu, dumpPath,
                             written, kEeRamBytes);
                dumpedPreSendChain = true;
            }
        }
    }

    // Diagnostic oracle: replace only the payload bytes of the fully-built
    // native chain with the same-address payloads from a user-provided PCSX2
    // EE-RAM snapshot.  Tags/control flow remain native.  If this makes the
    // girl correct, divergence is in EE packet contents; if it does not, the
    // remaining fault is DMAC flattening, VIF1/VU1, or later.
    if (exactGirlStep && caller == 0x0023C6E0u &&
        packet == 0x012BD1C0u && liveEnabledRecords >= 33u) {
        const char* oraclePath = std::getenv("RRV_GIRL_PCSX2_CHAIN_ORACLE");
        if (oraclePath && oraclePath[0] != '\0') {
            static std::vector<uint8_t> oracle;
            static bool attemptedLoad = false;
            if (!attemptedLoad) {
                attemptedLoad = true;
                if (std::FILE* file = std::fopen(oraclePath, "rb")) {
                    oracle.resize(0x02000000u);
                    const size_t read =
                        std::fread(oracle.data(), 1u, oracle.size(), file);
                    std::fclose(file);
                    if (read != oracle.size()) oracle.clear();
                    std::fprintf(stderr,
                                 "[girl-oracle] load=%s bytes=%zu valid=%u\n",
                                 oraclePath, read, oracle.empty() ? 0u : 1u);
                }
            }

            if (!oracle.empty() && node <= 0x01FFFFF0u) {
                uint32_t tagPc = node;
                uint32_t returnStack[2] = {};
                uint32_t depth = 0u;
                uint32_t copiedBlocks = 0u;
                size_t copiedBytes = 0u;
                size_t differingBytes = 0u;
                bool valid = true;
                for (uint32_t tagCount = 0u; tagCount < 4096u; ++tagCount) {
                    if (tagPc > 0x01FFFFF0u) {
                        valid = false;
                        break;
                    }
                    const uint32_t word0 = READ32(tagPc);
                    const uint32_t qwc = word0 & 0xFFFFu;
                    const uint32_t id = (word0 >> 28u) & 7u;
                    const uint32_t target = READ32(tagPc + 4u) & 0x0FFFFFFFu;
                    const uint32_t sequential = tagPc + 16u + qwc * 16u;
                    const uint32_t payload =
                        (id == 0u || id == 3u || id == 4u)
                            ? target
                            : tagPc + 16u;
                    const size_t bytes = static_cast<size_t>(qwc) * 16u;
                    if (bytes != 0u) {
                        if (payload > 0x02000000u ||
                            bytes > 0x02000000u - payload) {
                            valid = false;
                            break;
                        }
                        for (size_t i = 0u; i < bytes; ++i) {
                            differingBytes += rdram[payload + i] != oracle[payload + i]
                                ? 1u : 0u;
                        }
                        std::memcpy(rdram + payload, oracle.data() + payload, bytes);
                        ++copiedBlocks;
                        copiedBytes += bytes;
                    }

                    if (id == 0u || id == 7u) break;
                    if (id == 1u) {
                        tagPc = sequential;
                    } else if (id == 2u) {
                        tagPc = target;
                    } else if (id == 3u || id == 4u) {
                        tagPc += 16u;
                    } else if (id == 5u) {
                        if (depth >= 2u) {
                            valid = false;
                            break;
                        }
                        returnStack[depth++] = sequential;
                        tagPc = target;
                    } else if (id == 6u) {
                        if (depth == 0u) break;
                        tagPc = returnStack[--depth];
                    }
                }
                static uint32_t oracleFrames = 0u;
                if (oracleFrames < 16u) {
                    std::fprintf(stderr,
                                 "[girl-oracle] frame=%u root=%08x valid=%u "
                                 "blocks=%u bytes=%zu differing=%zu\n",
                                 oracleFrames, node, valid ? 1u : 0u,
                                 copiedBlocks, copiedBytes, differingBytes);
                }
                ++oracleFrames;
            }
        }
    }
}

// Trace the model-list gate immediately above the missing 0x22E154 flushes.
// 0x233240 walks the active scene-object list and calls 0x22D390 for each
// object.  The latter reads its record count at object+0xA710 and skips any
// 0x88-byte record whose signed halfword at +0x82 is non-zero.  Capturing that
// compact state tells us whether the missing geometry is caused by an absent
// object, an empty list, a visibility/animation flag, or a deeper renderer
// early-out.  These wrappers are diagnostic-only and delegate unchanged.
static bool rrvExactGirlStep(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t cursor = READ32(0x3348ACu);
    return READ32(0x334E94u) == 2u && cursor != 0u &&
           (cursor & 3u) == 0u && cursor <= 0x01FFFFF8u &&
           READ32(cursor) == 1u && READ32(cursor + 4u) == 2u;
}

static void patch_0x233240_girl_model_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (rrvExactGirlStep(rdram, ctx, runtime)) {
        static uint32_t callCount = 0u;
        if (callCount < 16u) {
            const uint32_t list = READ32(0x01DF8460u);
            std::fprintf(stderr,
                         "[girl-scene] n=%u ra=%08x list0=%08x list1=%08x "
                         "gate=%08x\n",
                         callCount, GPR_U32(ctx, 31), list,
                         READ32(0x01DF8464u),
                         READ32(ADD32(GPR_U32(ctx, 28), 4294944900u)));
        }
        ++callCount;
    }
    sub_00233240_0x233240(rdram, ctx, runtime);
}

static void patch_0x22d390_girl_model_list_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool exactGirlStep = rrvExactGirlStep(rdram, ctx, runtime);
    const uint32_t entryObject = GPR_U32(ctx, 4);
    const uint32_t entryReturnPc = GPR_U32(ctx, 31);
    uint32_t entryEnabledRecords = 0u;
    if (exactGirlStep && entryObject == 0x00368500u) {
        for (uint32_t i = 0u; i < 44u; ++i) {
            entryEnabledRecords +=
                static_cast<int16_t>(READ16(entryObject + i * 0x88u + 0x82u)) == 0
                    ? 1u : 0u;
        }
    }
    if (exactGirlStep) {
        static uint32_t callCount = 0u;
        const uint32_t object = GPR_U32(ctx, 4);
        const bool validObject = object <= 0x01FF58F0u;
        const uint32_t count = validObject ? READ32(object + 0xA710u) : UINT32_MAX;
        if (callCount < 64u) {
            uint32_t enabled = 0u;
            uint32_t skipped = 0u;
            uint32_t nullData = 0u;
            uint32_t disabled86 = 0u;
            if (validObject && count <= 1024u) {
                for (uint32_t i = 0u; i < count; ++i) {
                    const uint32_t record = object + i * 0x88u;
                    if (static_cast<int16_t>(READ16(record + 0x82u)) != 0) {
                        ++skipped;
                        continue;
                    }
                    ++enabled;
                    if (READ32(record + 0x0Cu) == 0u) ++nullData;
                    if (static_cast<int16_t>(READ16(record + 0x86u)) != 0) {
                        ++disabled86;
                    }
                }
            }
            std::fprintf(stderr,
                         "[girl-model-list] n=%u ra=%08x object=%08x "
                         "count=%u enabled=%u skip82=%u nullData=%u flag86=%u\n",
                         callCount, GPR_U32(ctx, 31), object, count, enabled,
                         skipped, nullData, disabled86);
        }
        ++callCount;
    }
    const char* completeDumpPath = std::getenv("RRV_GIRL_MODEL_COMPLETE_DUMP");
    const bool captureCompleteModel = exactGirlStep &&
        entryObject == 0x00368500u && entryEnabledRecords >= 33u &&
        completeDumpPath && completeDumpPath[0] != '\0';
    if (captureCompleteModel) {
        // Generated loop bodies may return early at a back-edge and resume via
        // an internal PC.  Keep this one diagnostic invocation contiguous so
        // "post" really means the function reached its caller.
        PS2Runtime::BackEdgeYieldSuppressionScope suppressBackEdgeYields;
        sub_0022D390_0x22d390(rdram, ctx, runtime);
    } else {
        sub_0022D390_0x22d390(rdram, ctx, runtime);
    }

    // Capture only after all 44 records have been considered.  Unlike the
    // earlier post-REF dump, this includes the CALL prefixes appended by the
    // enabled records and is therefore directly comparable with PCSX2's full
    // 0x3472A0/0x347180 VIF1 chain.
    static bool dumpedCompleteModel = false;
    if (exactGirlStep && entryObject == 0x00368500u &&
        entryEnabledRecords >= 33u && ctx->pc == entryReturnPc &&
        !dumpedCompleteModel) {
        if (const char* dumpPath = completeDumpPath;
            dumpPath && dumpPath[0] != '\0') {
            if (std::FILE* dump = std::fopen(dumpPath, "wb")) {
                constexpr size_t kEeRamBytes = 0x02000000u;
                const size_t written = std::fwrite(rdram, 1u, kEeRamBytes, dump);
                std::fclose(dump);
                std::fprintf(stderr,
                             "[girl-model-complete] object=%08x root=%08x "
                             "dump=%s bytes=%zu/%zu\n",
                             entryObject, READ32(0x00334DFCu), dumpPath,
                             written, kEeRamBytes);
                dumpedCompleteModel = true;
            }
        }
    }
}

// A2 diagnostic A/B: keep only the model-list DMA builder contiguous.  Global
// RRV_NO_BACKEDGE_YIELD changed the girl corruption but also perturbed unrelated
// city/car work.  This wrapper tests the narrower hardware invariant suggested
// by that result without changing scheduling outside sub_0022D390.
static void patch_0x22d390_girl_atomic(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (!rrvExactGirlStep(rdram, ctx, runtime)) {
        sub_0022D390_0x22d390(rdram, ctx, runtime);
        return;
    }

    PS2Runtime::BackEdgeYieldSuppressionScope suppressBackEdgeYields;
    sub_0022D390_0x22d390(rdram, ctx, runtime);
}

// The scene command interpreter at 0x23E3C8 consumes setup/texture commands in
// a resumable loop and only then calls 0x233240 to append model geometry.  A
// dispatcher yield at its 0x23E568 back-edge lets another guest thread submit
// or recycle the shared queue between those two halves.  Keep this full command
// batch contiguous only at the exact A2 girl checkpoint.
static void patch_0x23e3c8_girl_atomic(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (!rrvExactGirlStep(rdram, ctx, runtime)) {
        sub_0023E3C8_0x23e3c8(rdram, ctx, runtime);
        return;
    }

    PS2Runtime::BackEdgeYieldSuppressionScope suppressBackEdgeYields;
    sub_0023E3C8_0x23e3c8(rdram, ctx, runtime);
}

static void patch_0x22d810_girl_geometry_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (rrvExactGirlStep(rdram, ctx, runtime)) {
        static uint32_t callCount = 0u;
        if (callCount < 128u) {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t index = GPR_U32(ctx, 5);
            const uint32_t record = ADD32(object, index * 0x88u);
            std::fprintf(stderr,
                         "[girl-geometry] n=%u ra=%08x object=%08x index=%u "
                         "record=%08x data=%08x flag82=%d flag86=%d "
                         "a2=%08x a3=%08x t0=%08x t1=%08x t2=%08x t3=%08x\n",
                         callCount, GPR_U32(ctx, 31), object, index, record,
                         READ32(record + 0x0Cu),
                         static_cast<int16_t>(READ16(record + 0x82u)),
                         static_cast<int16_t>(READ16(record + 0x86u)),
                         GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8),
                         GPR_U32(ctx, 9), GPR_U32(ctx, 10), GPR_U32(ctx, 11));
        }
        ++callCount;
    }
    sub_0022D810_0x22d810(rdram, ctx, runtime);
}

static void patch_0x2c3ff0_girl_vif0_send_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (rrvExactGirlStep(rdram, ctx, runtime)) {
        static uint32_t callCount = 0u;
        if (callCount < 128u) {
            const uint32_t payload = GPR_U32(ctx, 5);
            std::fprintf(stderr,
                         "[girl-vif0-send] n=%u ra=%08x chan=%08x "
                         "payload=%08x qwc=%u words=%08x,%08x,%08x,%08x\n",
                         callCount, GPR_U32(ctx, 31), GPR_U32(ctx, 4), payload,
                         GPR_U32(ctx, 6), READ32(payload), READ32(payload + 4u),
                         READ32(payload + 8u), READ32(payload + 12u));
        }
        ++callCount;
    }
    const uint32_t returnAddress = GPR_U32(ctx, 31);
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceDmaSendN");
    ps2_stubs::sceDmaSendN(rdram, ctx, runtime);
    ctx->pc = returnAddress;
}

static void patch_0x2c3f18_girl_spr_chain_diag(
    uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (rrvExactGirlStep(rdram, ctx, runtime)) {
        static uint32_t callCount = 0u;
        if (callCount < 128u) {
            const uint32_t chain = GPR_U32(ctx, 5);
            std::fprintf(stderr,
                         "[girl-spr-chain] n=%u ra=%08x chan=%08x chain=%08x "
                         "arg2=%08x tag=%08x,%08x,%08x,%08x\n",
                         callCount, GPR_U32(ctx, 31), GPR_U32(ctx, 4), chain,
                         GPR_U32(ctx, 6), READ32(chain), READ32(chain + 4u),
                         READ32(chain + 8u), READ32(chain + 12u));
        }
        ++callCount;
    }
    const uint32_t returnAddress = GPR_U32(ctx, 31);
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceDmaSend");
    ps2_stubs::sceDmaSend(rdram, ctx, runtime);
    ctx->pc = returnAddress;
}

// 0x29a508 — render/stream completion busy-wait (sub_0029A508).  Generated body:
//   s0 = T0_COUNT(0x10000000);  s1 = 0xEFFF;        // EE Timer0 start, timeout
//   do {
//       if (sub_002A07C8() != 0) break;             // all queued jobs done?
//       cur = T0_COUNT(0x10000000);
//       if (cur <  s0)          break;              // timer wrapped
//       if (cur - s0 > 0xEFFF)  break;              // ** timeout escape **
//   } while (1);
// The poll (sub_002A07C8 -> sub_00298F28 / sub_00296F48) waits for the render/
// stream job table at guest RAM 0x1D7BD80 (per-job status byte at +0x27 reaches
// 4 = done).  It is a bounded wait on hardware: after 0xEFFF EE-Timer0 ticks it
// gives up and proceeds.  Our runtime never advances EE Timer0 (0x10000000), so
// `cur - s0` stays 0, the timeout never fires, and when a job never reaches
// "done" the whole EE game thread spins here forever.
//
// This is what stalls the attract loop: demo phase 0x27 (the attract-restart
// FMV/stream segment) queues a stream job whose IPU/stream decode is HLE-stubbed
// (Stubs/IPU.cpp is a success no-op), so its status never becomes 4.  The demo
// timer that gates phase 0x27 itself completes fine (frame ~0xDBB, counter 0xF0 =
// duration 240), but immediately afterwards the render path enters this wait and
// hangs (frame counter 0x334DE0 frozen, pad polling drops to 0).  Same class as
// patch_0x220100: a busy-wait on a counter no HLE ISR/clock advances.
//
// Fix: emulate the hardware timeout that the frozen Timer0 suppresses.  Because
// our DMA/GS execute synchronously at submit time, every job that CAN complete is
// already complete by the time this wait is reached, and nothing on this single
// game thread can flip a job's status while we spin — so one poll is definitive.
// Run the real poll once (this preserves its all-jobs-done finalize side effect,
// sub_00298E40, in the normal case) then return, whether the job finished or not.
// Escape hatch RRV_FMV_COMPLETE_HLE=0 leaves the stock (hanging) body in place.
static void patch_0x29a508(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "patch:0x29a508");
    const uint32_t retAddr = GPR_U32(ctx, 31); // caller return address (jr $ra)
    if (runtime && runtime->hasFunction(0x2A07C8u)) {
        constexpr uint32_t kSentinel = 0x00FFF000u; // return sentinel for the poll
        auto poll = runtime->lookupFunction(0x2A07C8u);
        SET_GPR_U32(ctx, 31, kSentinel);            // poll's $ra
        ctx->pc = 0x2A07C8u;
        // Drive the poll to completion, honoring cooperative back-edge preemption
        // (the callee re-enters at ctx->pc via its switch).  Guarded so a truly
        // stuck callee can never hang this patch.
        for (int guard = 0; guard < 1000000 && ctx->pc != kSentinel; ++guard) {
            poll(rdram, ctx, runtime);
        }
    }
    ctx->pc = retAddr; // jr $ra — return (job done, or emulated Timer0 timeout)
}

// ── RRV_P39_EULER=1 — one-shot euler-extraction probe (default off) ─────────
//
// func_207438 turns a car's orientation matrix into the euler triple that
// sub_00203560 then clamps into car+0x20. Our pitch comes out at a computed
// ~pi/4 where hardware's is a small varying angle, so this prints the two
// input matrices (both live in scratchpad) and the triple it produced, and
// reports whether each input is a well-formed rotation (orthonormal rows).
// Diagnostic only; forwards to the generated body untouched.
namespace {
PS2Runtime::RecompiledFunction g_euler207438Orig = nullptr;

float rrvReadF32(uint8_t* rdram, uint32_t addr) {
    const uint8_t* p = getConstMemPtr(rdram, addr);
    if (!p) return 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void rrvDumpRotation(uint8_t* rdram, const char* tag, uint32_t addr) {
    float m[16];
    for (int i = 0; i < 16; ++i) m[i] = rrvReadF32(rdram, addr + (uint32_t)i * 4u);
    double n[3], d[3];
    for (int r = 0; r < 3; ++r)
        n[r] = std::sqrt((double)m[r*4+0]*m[r*4+0] + (double)m[r*4+1]*m[r*4+1] + (double)m[r*4+2]*m[r*4+2]);
    d[0] = (double)m[0]*m[4] + (double)m[1]*m[5] + (double)m[2]*m[6];
    d[1] = (double)m[0]*m[8] + (double)m[1]*m[9] + (double)m[2]*m[10];
    d[2] = (double)m[4]*m[8] + (double)m[5]*m[9] + (double)m[6]*m[10];
    std::fprintf(stderr, "  [euler-in] %s@%08x norms=%.4f,%.4f,%.4f dots=%.4f,%.4f,%.4f rows=", tag, addr, n[0], n[1], n[2], d[0], d[1], d[2]);
    for (int i = 0; i < 12; ++i) std::fprintf(stderr, "%s%.4f", (i % 4) ? "," : "|", (double)m[i]);
    std::fputc('\n', stderr);
}

void patch_0x207438_euler(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int budget = 6;
    const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6);
    const bool show = (budget > 0) && (READ32(0x334E04u) == 0x00020005u);
    if (show) {
        --budget;
        std::fprintf(stderr, "[euler] call a0=%08x a1=%08x out=%08x\n", a0, a1, a2);
        // a0/a1 are 0x10 apart: they are VECTORS, not matrices. Print them as
        // such, plus the 4x4 at 0x70000170 that func_207570 transformed them
        // by, which is where a zero third column would come from.
        std::fprintf(stderr, "  [euler-vec] a0=%.5f,%.5f,%.5f,%.5f  a1=%.5f,%.5f,%.5f,%.5f\n",
                     (double)rrvReadF32(rdram, a0), (double)rrvReadF32(rdram, a0+4u),
                     (double)rrvReadF32(rdram, a0+8u), (double)rrvReadF32(rdram, a0+12u),
                     (double)rrvReadF32(rdram, a1), (double)rrvReadF32(rdram, a1+4u),
                     (double)rrvReadF32(rdram, a1+8u), (double)rrvReadF32(rdram, a1+12u));
        rrvDumpRotation(rdram, "xform", 0x70000170u);
    }
    if (g_euler207438Orig) g_euler207438Orig(rdram, ctx, runtime);
    if (show) {
        std::fprintf(stderr, "  [euler-out] %.6f, %.6f, %.6f, %.6f\n",
                     (double)rrvReadF32(rdram, a2), (double)rrvReadF32(rdram, a2 + 4u),
                     (double)rrvReadF32(rdram, a2 + 8u), (double)rrvReadF32(rdram, a2 + 12u));
    }
}
} // namespace

// ── Untracked library-import stub binding ─────────────────────────────────
//
// The recompiler emits C++ bodies for rrv.toml `stubs` but not `untracked_stubs`.
// We bind the missing ones to their ps2_stubs:: handlers at runtime.  The raw
// ps2_stubs functions do NOT advance ctx->pc, so when one is reached via direct
// dispatch the dispatch loop re-enters it forever ("PC not updating").  Wrap each
// in a trampoline that runs the handler then performs jr $ra, exactly like the
// generated stub wrappers do.

// Untracked imports bypass generated HLE wrappers. Keep each binding's exact
// name so the trampoline can consume the same explicit cost as a generated
// import, including when reached through nested lookupFunction() dispatch.
struct RrvBoundStub { PS2Runtime::RecompiledFunction fn; const char* name; };
static std::unordered_map<uint32_t, RrvBoundStub>& rrvStubMap() {
    static std::unordered_map<uint32_t, RrvBoundStub> m;
    return m;
}

static void rrvStubTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    auto& m = rrvStubMap();
    if (auto it = m.find(entry); it != m.end() && it->second.fn) {
        const auto& bound = it->second;
        std::string costName = "ps2_stubs::";
        costName += bound.name;
        if (std::string_view{bound.name} == "sceGsSyncV") {
            bound.fn(rdram, ctx, runtime); // Charge only after the modeled wait returns.
            runtime->gate3ChargeNamedHleV1(ctx, costName.c_str());
        } else {
            runtime->gate3ChargeNamedHleV1(ctx, costName.c_str());
            bound.fn(rdram, ctx, runtime);
        }
    }
    if (ctx->pc == entry) {
        ctx->pc = GPR_U32(ctx, 31); // jr $ra — avoid dispatch-loop re-entry
    }
}

// Resolve a stub name to its ps2_stubs:: function pointer via the shared call
// list.  Returns nullptr for names without an implementation (compiler-rt,
// unimplemented imports) so they are left unregistered rather than mis-bound.
static PS2Runtime::RecompiledFunction rrvResolveStub(std::string_view n) {
#define RRV_RESOLVE_STUB(name) if (n == std::string_view{#name}) return &ps2_stubs::name;
    PS2_STUB_LIST(RRV_RESOLVE_STUB)
#undef RRV_RESOLVE_STUB
    return nullptr;
}

// ── Registration entry point ──────────────────────────────────────────────

// ── RRV_FN_HITS=<hex>[,<hex>...] — guest-function call counter ───────────────
// A trampoline over the listed guest addresses that counts entries and forwards
// to whatever body was registered before it, so "is this routine running in
// this scene at all?" is one run instead of a bisect. Counts are dumped by the
// per-frame patch whenever scenePhase changes (and at exit). Default off.
namespace {
std::map<uint32_t, PS2Runtime::RecompiledFunction>& rrvFnHitOrig() {
    static std::map<uint32_t, PS2Runtime::RecompiledFunction> m;
    return m;
}
std::map<uint32_t, std::atomic<uint64_t>>& rrvFnHitCount() {
    static std::map<uint32_t, std::atomic<uint64_t>> m;
    return m;
}
void rrvFnHitTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t pc = ctx->pc;
    auto& counts = rrvFnHitCount();
    auto c = counts.find(pc);
    if (c != counts.end())
        c->second.fetch_add(1, std::memory_order_relaxed);
    auto o = rrvFnHitOrig().find(pc);
    if (o != rrvFnHitOrig().end() && o->second)
        o->second(rdram, ctx, runtime);
}
} // namespace

void rrvDumpFnHits(const char* why) {
    if (rrvFnHitCount().empty())
        return;
    std::fprintf(stderr, "[fn-hits] %s:", why);
    for (auto& kv : rrvFnHitCount())
        std::fprintf(stderr, " %08x=%llu", kv.first,
                     (unsigned long long)kv.second.load(std::memory_order_relaxed));
    std::fputc('\n', stderr);
}

static void patch_0x2c5ac0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->gate3ChargeNamedHleV1(ctx, "patch:0x2c5ac0");
    rrv_hle::spu2Remote_hle(rdram, ctx, runtime);
}

// RR5 enhancement (opt-in, owner direction 2026-09-27): car model detail.
// func_226EB0 is the draw-object auto-LOD selector (reached from func_201540
// when a1 < 0). It divides the object's view depth by the scale at 0x1DE0CB0
// and walks the row [gp-0x6af0] of the table at 0x2F5460 (7 rows of 5 floats:
// four LOD thresholds, zero = level unused, then the cull distance): the level
// is the first used i with distance < t[i], else 4; beyond the cull distance
// the object is not drawn. RRV_RR5_CAR_LOD=<factor> scales every used
// threshold of rows 0-6 by the factor, capped at that row's cull distance
// (the cull distance itself is kept); RRV_RR5_CAR_LOD=best caps them all at
// the cull distance. The original table is read once, so the scale never
// compounds. Off by default: it changes guest-visible data, so reference
// runs never set it. This is an RR5-specific enhancement, not emulation.
//
// Display-list room (owner report 2026-09-30: x4 crashed at the start of the
// second race; measured 2026-10-03 on local/replays/crash-021). The game
// builds each field's DMA list in one of two buffers that func_21E658(size)
// places just below 0x1D68500: buffer 1 = 0x1D68500 - size, buffer 0 =
// buffer 1 - size, size kept at 0x334DF8. func_220768 points the packet
// cursor [0x3470A0] at buffer [0x334DD0 + 4 * [0x347084]] each field, and
// nothing checks its end. The sizes the game asks for are 0x300000 (races
// and most menus), 0x400000 (func_266570's menus) and 0x180000 (the attract
// cinematic, which draws no LOD objects). A car at its most detailed level
// adds about 0x55000 bytes (the EE vertex builder func_222EB8, two GIF
// streams), against 0x6000-0xF000 at the far levels. On the first field of
// race 2 the camera sees the whole grid: stock uses 0x121460, x4 puts the
// cars at the top level and passes 0x300000 at the 11th of 14 objects; the
// fromSPR destination chain then writes past 0x1D68500 and reaches the sound
// RPC server record at 0x1DAD4A0 ("Gate3 missing admitted callback target").
// Two parts, both only while the factor is above 1:
//   * Room: while func_217628 (race setup) runs, its func_21E658(0x300000)
//     becomes the game's own 0x400000 request, so a race builds its lists in
//     0x1568500 / 0x1968500 like func_266570's menus do; boot, menus and the
//     attract keep their sizes, and leaving the race is the game's ordinary
//     size change (its two-field wait included). The extra 0x200000 below
//     the stock buffer 0 is not free outside races: the boot loads data at
//     0x1568500 that the attract changes and keeps, and no race writes it
//     (stock RAM at the start and the end of a race are equal there). Those
//     bytes are saved when the race takes the room and put back at the next
//     mode change, before func_21E658 runs; the lists in them were copied at
//     their DMA kick, so nothing reads them later. A change to the game's own
//     0x400000 mode keeps the layout and the stock game also overwrites the
//     bytes then, so nothing is put back.
//   * Guard: an object selected while less than kCarLodReserve bytes remain
//     in the current buffer uses the stock table, so detail can never push
//     the list past the end; the rest of the field is then stock work.
// Measured (crash-021, x4, 16:9, draw distance +2): the replay runs to its end
// (it crashed at start 14945), largest list 0x36B280 of 0x400000, 8 of
// 161,297 selections kept stock; with `best` 1,920 of 161,297. The attract after an x4 race has the stock bytes at 0x1568500.
// RRV_RR5_CAR_LOD_GUARD=0 (developer A/B) turns both off; RRV_RR5_CAR_LOD_LOG=1
// prints the guard count and the largest lists at exit.
namespace {
PS2Runtime::RecompiledFunction g_lod226EB0Orig = nullptr, g_lodRace217628Orig = nullptr, g_lodDl21E658Orig = nullptr;
float g_rr5CarLodFactor = 1.0f;
bool g_carLodGuard = true, g_carLodLog = false;
constexpr uint32_t kCarLodDlTop = 0x1D68500u;    // end of display-list buffer 1 (func_21E658)
constexpr uint32_t kCarLodDlBase0 = 0x334DD0u;   // buffer 0, buffer 1 at +4
constexpr uint32_t kCarLodDlIndex = 0x347084u;   // buffer of the field being built
constexpr uint32_t kCarLodDlCursor = 0x3470A0u;  // packet cursor (| 0x20000000 uncached)
constexpr uint32_t kCarLodRaceSize = 0x300000u, kCarLodRoomSize = 0x400000u;
constexpr uint32_t kCarLodReserve = 0x180000u;
uint64_t g_carLodGuarded = 0, g_carLodRaised = 0;
uint32_t g_carLodMaxUsed[3] = {}; // at a selector call, by buffer size: 0x300000, 0x400000, other

uint32_t rrvCarLodRead32(uint8_t* rdram, uint32_t addr) {
    const uint8_t* p = getMemPtr(rdram, addr);
    uint32_t v = 0;
    if (p) std::memcpy(&v, p, 4);
    return v;
}

void rrvRr5CarLodTable(uint8_t* rdram, bool raised) {
    static float orig[7][5];
    static bool have = false;
    if (!have) {
        for (uint32_t row = 0; row < 7u; ++row)
            for (uint32_t i = 0; i < 5u; ++i) {
                const uint8_t* t = getMemPtr(rdram, 0x2F5460u + row * 0x14u + i * 4u);
                if (!t) return;
                std::memcpy(&orig[row][i], t, sizeof(float));
            }
        have = true;
    }
    for (uint32_t row = 0; row < 7u; ++row) {
        const float cull = orig[row][4];
        for (uint32_t i = 0; i < 4u; ++i) {
            const float v = orig[row][i];
            if (!(v > 0.0f)) continue;
            const float value = raised ? std::min(v * g_rr5CarLodFactor, cull) : v;
            std::memcpy(getMemPtr(rdram, 0x2F5460u + row * 0x14u + i * 4u), &value, sizeof(float));
        }
    }
}

// Bytes left in the buffer the cursor is in; 0 when the cursor is not inside it.
uint32_t rrvCarLodRoomLeft(uint8_t* rdram) {
    const uint32_t buffer0 = rrvCarLodRead32(rdram, kCarLodDlBase0) & 0x0FFFFFFFu;
    const uint32_t buffer1 = rrvCarLodRead32(rdram, kCarLodDlBase0 + 4u) & 0x0FFFFFFFu;
    const uint32_t size = buffer1 - buffer0; // func_21E658 puts them back to back
    const uint32_t base = (rrvCarLodRead32(rdram, kCarLodDlIndex) & 1u) ? buffer1 : buffer0;
    const uint32_t cursor = rrvCarLodRead32(rdram, kCarLodDlCursor) & 0x0FFFFFFFu;
    if (buffer1 <= buffer0 || buffer1 + size != kCarLodDlTop || cursor < base || cursor > base + size) return 0u;
    if (g_carLodLog) {
        uint32_t& max = g_carLodMaxUsed[size == kCarLodRaceSize ? 0 : size == kCarLodRoomSize ? 1 : 2];
        max = std::max(max, cursor - base);
    }
    return base + size - cursor;
}

void patch_0x226eb0_car_lod(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool raised = !g_carLodGuard || rrvCarLodRoomLeft(rdram) >= kCarLodReserve;
    ++(raised ? g_carLodRaised : g_carLodGuarded);
    rrvRr5CarLodTable(rdram, raised);
    if (g_lod226EB0Orig) g_lod226EB0Orig(rdram, ctx, runtime);
}

// Room (see above): func_217628's request, and the bytes under it.
uint32_t g_carLodRaceSetup = 0;
bool g_carLodRoomActive = false;
std::vector<uint8_t> g_carLodRoomSaved;
constexpr uint32_t kCarLodRoomLow = kCarLodDlTop - 2u * kCarLodRoomSize;   // 0x1568500
constexpr uint32_t kCarLodRoomHigh = kCarLodDlTop - 2u * kCarLodRaceSize;  // 0x1768500
void patch_0x217628_car_lod_room(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ++g_carLodRaceSetup;
    g_lodRace217628Orig(rdram, ctx, runtime);
    --g_carLodRaceSetup;
}
void patch_0x21e658_car_lod_room(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t requested = GPR_U32(ctx, 4) & ~0x7Fu;
    const bool room = g_carLodRaceSetup > 0u && requested == kCarLodRaceSize;
    uint8_t* low = getMemPtr(rdram, kCarLodRoomLow);
    if (room && !g_carLodRoomActive && low) {
        g_carLodRoomSaved.assign(low, low + (kCarLodRoomHigh - kCarLodRoomLow));
        g_carLodRoomActive = true;
    } else if (!room && g_carLodRoomActive) {
        if (requested != kCarLodRoomSize && low)
            std::memcpy(low, g_carLodRoomSaved.data(), g_carLodRoomSaved.size());
        g_carLodRoomActive = false;
    }
    if (room) SET_GPR_U32(ctx, 4, kCarLodRoomSize);
    g_lodDl21E658Orig(rdram, ctx, runtime);
    if (room) SET_GPR_U32(ctx, 4, kCarLodRaceSize); // a0 as the stock call leaves it
}

// RRV_RR5_CAR_LOD_LOG=1 only: the list each field ended with (func_220768
// moves the cursor to the next field's buffer).
PS2Runtime::RecompiledFunction g_lodField220768Orig = nullptr;
uint32_t g_carLodMaxField = 0, g_carLodMaxFieldSize = 0;
void patch_0x220768_car_lod_log(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t buffer0 = rrvCarLodRead32(rdram, kCarLodDlBase0) & 0x0FFFFFFFu;
    const uint32_t buffer1 = rrvCarLodRead32(rdram, kCarLodDlBase0 + 4u) & 0x0FFFFFFFu;
    const uint32_t cursor = rrvCarLodRead32(rdram, kCarLodDlCursor) & 0x0FFFFFFFu;
    const uint32_t base = cursor >= buffer1 ? buffer1 : buffer0;
    if (cursor >= base && cursor - base > g_carLodMaxField) {
        g_carLodMaxField = cursor - base;
        g_carLodMaxFieldSize = buffer1 - buffer0;
    }
    g_lodField220768Orig(rdram, ctx, runtime);
}

void rrvCarLodReport() {
    std::fprintf(stderr, "[rr5-enhance] car LOD: %llu selections raised, %llu kept stock by the display-list guard; "
                         "largest list at a selection 0x%X (0x300000 buffers), 0x%X (0x400000), 0x%X (other)\n",
                 (unsigned long long)g_carLodRaised, (unsigned long long)g_carLodGuarded, g_carLodMaxUsed[0],
                 g_carLodMaxUsed[1], g_carLodMaxUsed[2]);
    std::fprintf(stderr, "[rr5-enhance] car LOD: largest list at a field end 0x%X (buffers 0x%X)\n", g_carLodMaxField,
                 g_carLodMaxFieldSize);
}
} // namespace

// RR5 enhancement (opt-in, owner direction 2026-09-27): real widescreen.
// The camera block at 0x1DE0CB0 holds p[0] (screen distance), p[1] (X
// factor) and p[2] (Y factor); func_220A90/220B88 turn it into the projection
// (screen x = p1*p0*x/z, y = p2*p0*y/z). Mechanism from the community hack for
// SLUS-20002 by "No.47" (cheat codes 20332690..20332F80: screen distances x0.75,
// Y factor 0.46 x4/3): p[0] x0.75 widens the view and p[2] x4/3 restores the
// height, so only the horizontal field of view grows (by 4/3), and every
// system derived from p[0] (VU1 clip constants in func_21BF08, cull planes in
// func_21B408, ...) widens with it. Measured 2026-09-27: scaling only the
// projection matrix left scenery clipped at the old 4:3 edge, and scaling
// p[0] for func_21BF08 alone corrupted the scene; p[0] must change for every
// reader. No.47 patches seven init constants, which misses cameras set up
// elsewhere (the car-select camera came out ~10% zoomed). Here the block is
// normalised at the entry of every function that reads it (the camera build
// func_220E10 and the direct p[0] readers): a value the game wrote is scaled
// once; a value this code produced is left alone (a ring of produced values
// also covers func_2AAA60's save/restore). The car LOD selector (func_226EB0)
// divides depth by p[0]; it gets the original p[0] back for its call so car
// detail is unchanged. RRV_RR5_WIDESCREEN=16:9|16:10|21:9 (1 = 16:9) picks
// the target ratio A: p[0] x(4/3)/A, p[2] xA/(4/3). The launcher shows the
// image at A (RRV_PCSX2_GS_ASPECT=A); 2D (HUD, text) is widened by that. Off by
// default: it changes guest data, so reference runs never set it.
//
// Intro profile (owner direction 2026-10-02). The attract cinematic is not a
// gameplay camera. Attract main (func_23E3C8) writes the block every frame in
// func_23E720 with its own factors, p[1] = 0.8 ([0x333240]) and p[2] = 0.368
// ([0x333244]) against 1.0 and 0.46 in a race: the view is already 1.25x
// wider, and func_23D408 (called from func_233240) covers rows 0..23 and
// 203..224 with two black quads, leaving a 640x180 picture (1.66:1).
// Characters and objects use a second projection, func_22CB88's
// sceVu0ViewScreenMatrix(scrz, ax = [0x332F80] = 0.8, ay = [0x332F84] = 0.368)
// into 0x1DF06F0, which the block normaliser never sees. Measured 2026-10-03
// on v155 at 16:9 (neutral workload, fields 400..2950): every field keeps the
// bars, the course is widened by a further 4/3 (a 2.2:1 strip; the owner saw
// geometry missing at its sides) and the characters are not widened at all,
// so they are stretched and do not register with the course behind them.
// In the intro profile the horizontal state is the stock game's and only the
// Y factors change: p[0] is left alone, p[2] and ay are scaled by A/(4/3), and
// the two bar quads are given zero height. The 4:3 frame shown at A is then
// geometrically correct, fills the screen, and every system derived from p[0]
// sees the values the scene was built for. At 16:9 the screen shows 168 of
// the 180 picture rows of the stock cinematic. The 2D title cards ("A New
// Beginning", ...) are not camera scenes and keep their own black bands.
// The profile holds inside attract main, and after it returns for as long as
// the block is still the camera the attract left (the same p[0], X factor
// 0.8). The X factor alone cannot tell the cameras apart: func_220150's
// mode 1 (a small viewport at the top centre, assumed to be the rear-view
// mirror) uses the same 0.8 / 0.368, and in attract
// scenes 13 and 36 the block holds 1.0 / 0.46 with the attract's p[0] when the
// spark effect (func_2A2EF0) reads it inside attract main. When the attract
// ends, the next mode sets p[1] = 1.0 and the block is a gameplay camera
// again, with the p[0] the gameplay path would have produced.
// func_22CB88's other caller (func_2364F0) keeps the stock ay.
// RRV_RR5_WIDESCREEN_INTRO=gameplay (developer A/B) treats the intro as a
// gameplay camera again; RRV_RR5_WIDESCREEN_LOG=1 logs every profile change
// and, in the intro profile, every change of the X factor.
namespace {
constexpr uint32_t kWsBlockP0 = 0x1DE0CB0u;
constexpr uint32_t kWsBlockP1 = 0x1DE0CB4u;
constexpr uint32_t kWsBlockP2 = 0x1DE0CB8u;
constexpr uint32_t kWsAttractP1Bits = 0x3F4CCCCDu; // 0.8f
constexpr uint32_t kWsAttractAy = 0x332F84u;
constexpr uint32_t kWsPacketCursor = 0x3470A0u;    // func_23D408 builds its packets here
constexpr uint32_t kWsScenePhase = 0x334E94u;
constexpr uint32_t kWsReaders[] = {0x220E10u, 0x21BF08u, 0x21B408u, 0x21B098u, 0x222620u,
                                   0x2423E0u, 0x29F990u, 0x2A2EF0u, 0x2AAA60u};
constexpr uint32_t kWsReaderCount = sizeof(kWsReaders) / sizeof(kWsReaders[0]);
PS2Runtime::RecompiledFunction g_wsReaderOrig[kWsReaderCount] = {};
PS2Runtime::RecompiledFunction g_wsLodOrig = nullptr;

struct WsProduced {
    uint32_t scaled[16] = {}, orig[16] = {};
    uint32_t next = 0;
    bool find(uint32_t v, uint32_t* o) const {
        for (uint32_t i = 0; i < 16u; ++i)
            if (scaled[i] == v && v != 0u) { if (o) *o = orig[i]; return true; }
        return false;
    }
    void add(uint32_t s, uint32_t o) { scaled[next] = s; orig[next] = o; next = (next + 1u) % 16u; }
};
WsProduced g_wsP0, g_wsP2;
float g_wsFactor = 0.75f; // (4/3) / target aspect

void rrvWsNormalizeOne(uint8_t* rdram, uint32_t addr, WsProduced& produced, float factor) {
    uint8_t* p = getMemPtr(rdram, addr);
    if (!p) return;
    uint32_t bits;
    std::memcpy(&bits, p, 4);
    if (bits == 0u || produced.find(bits, nullptr)) return;
    float v;
    std::memcpy(&v, &bits, 4);
    const float s = v * factor;
    uint32_t sbits;
    std::memcpy(&sbits, &s, 4);
    std::memcpy(p, &sbits, 4);
    produced.add(sbits, bits);
}

PS2Runtime::RecompiledFunction g_wsAttractOrig = nullptr, g_wsViewScreenOrig = nullptr, g_wsBarsOrig = nullptr;
bool g_wsIntroPath = false; // the intro profile is registered
bool g_wsIntro = false;     // the block is the attract's camera
bool g_wsLog = false;
uint32_t g_wsAttractDepth = 0;
uint32_t g_wsIntroP0 = 0;   // p[0] as the attract left it

uint32_t rrvWsRead32(uint8_t* rdram, uint32_t addr) {
    const uint8_t* p = getMemPtr(rdram, addr);
    uint32_t v = 0;
    if (p) std::memcpy(&v, p, 4);
    return v;
}

// `reader` and `ra` only name the call in the log.
bool rrvWsIntroProfile(uint8_t* rdram, uint32_t reader, uint32_t ra) {
    if (!g_wsIntroPath) return false;
    const uint32_t p0 = rrvWsRead32(rdram, kWsBlockP0), p1 = rrvWsRead32(rdram, kWsBlockP1);
    const bool intro = g_wsAttractDepth > 0u || (g_wsIntro && p0 == g_wsIntroP0 && p1 == kWsAttractP1Bits);
    if (intro) g_wsIntroP0 = p0;
    if (g_wsLog) {
        static uint32_t lastP1 = 0;
        if (intro != g_wsIntro || (intro && p1 != lastP1)) {
            float p[3];
            for (uint32_t i = 0; i < 3u; ++i) {
                const uint32_t bits = rrvWsRead32(rdram, kWsBlockP0 + i * 4u);
                std::memcpy(&p[i], &bits, 4);
            }
            std::fprintf(stderr, "[rr5-enhance] widescreen profile: %s (scene %u, p %.3f %.4f %.4f, reader %06X ra %06X, "
                                 "attract depth %u)\n", intro ? "intro" : "gameplay", rrvWsRead32(rdram, kWsScenePhase),
                         p[0], p[1], p[2], reader, ra, g_wsAttractDepth);
        }
        lastP1 = p1;
    }
    g_wsIntro = intro;
    return intro;
}

void rrvWsNormalize(uint8_t* rdram, uint32_t reader, uint32_t ra) {
    if (!rrvWsIntroProfile(rdram, reader, ra)) rrvWsNormalizeOne(rdram, kWsBlockP0, g_wsP0, g_wsFactor);
    rrvWsNormalizeOne(rdram, kWsBlockP2, g_wsP2, 1.0f / g_wsFactor);
}

template <uint32_t I>
void patch_ws_reader(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    rrvWsNormalize(rdram, kWsReaders[I], GPR_U32(ctx, 31));
    g_wsReaderOrig[I](rdram, ctx, runtime);
}
constexpr PS2Runtime::RecompiledFunction kWsReaderPatches[] = {
    patch_ws_reader<0>, patch_ws_reader<1>, patch_ws_reader<2>, patch_ws_reader<3>, patch_ws_reader<4>,
    patch_ws_reader<5>, patch_ws_reader<6>, patch_ws_reader<7>, patch_ws_reader<8>};
static_assert(sizeof(kWsReaderPatches) / sizeof(kWsReaderPatches[0]) == kWsReaderCount);

void patch_0x226eb0_ws_lod(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    rrvWsNormalize(rdram, 0x226EB0u, GPR_U32(ctx, 31));
    uint8_t* p = getMemPtr(rdram, kWsBlockP0);
    uint32_t scaled = 0, orig = 0;
    const bool swap = !g_wsIntro && p && (std::memcpy(&scaled, p, 4), g_wsP0.find(scaled, &orig));
    if (swap) std::memcpy(p, &orig, 4);
    g_wsLodOrig(rdram, ctx, runtime);
    if (swap) {
        uint32_t now;
        std::memcpy(&now, p, 4);
        if (now == orig) std::memcpy(p, &scaled, 4);
    }
}

void patch_0x23e3c8_ws_attract(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    struct Scope {
        Scope() { ++g_wsAttractDepth; }
        ~Scope() { --g_wsAttractDepth; }
    } scope;
    g_wsAttractOrig(rdram, ctx, runtime);
}

// func_22CB88 is the only reader of ay; the word is put back after the call.
void patch_0x22cb88_ws_viewscreen(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    uint8_t* p = g_wsAttractDepth > 0u ? getMemPtr(rdram, kWsAttractAy) : nullptr;
    if (!p) {
        g_wsViewScreenOrig(rdram, ctx, runtime);
        return;
    }
    struct Restore {
        uint8_t* p;
        uint32_t bits;
        ~Restore() { std::memcpy(p, &bits, 4); }
    } restore{p, 0u};
    std::memcpy(&restore.bits, p, 4);
    float ay;
    std::memcpy(&ay, p, 4);
    ay *= 1.0f / g_wsFactor;
    std::memcpy(p, &ay, 4);
    g_wsViewScreenOrig(rdram, ctx, runtime);
}

// One bar: GIF tag 0x5122400000008001 (one tristrip, PRIM 0x244), registers
// 0x55551 (RGBAQ, XYZ2 x4), Y words at +0x24/+0x34 (y0) and +0x44/+0x54 (y1).
bool rrvWsCollapseBar(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t packet, uint32_t y0,
                      uint32_t y1, uint32_t y) {
    if (READ64(packet) != 0x5122400000008001ull || READ64(packet + 8u) != 0x55551ull) return false;
    if (READ32(packet + 0x24u) != y0 || READ32(packet + 0x34u) != y0) return false;
    if (READ32(packet + 0x44u) != y1 || READ32(packet + 0x54u) != y1) return false;
    for (const uint32_t off : {0x24u, 0x34u, 0x44u, 0x54u}) WRITE32(packet + off, y);
    return true;
}

// func_23D408 writes the top bar at the packet cursor, links it (func_21FCE8:
// func_21F9C0 appends a ref and a next DMA tag, 0x20 bytes), then the bottom
// bar, then a register packet. The list keeps its length and order; the two
// quads get zero height.
void patch_0x23d408_ws_bars(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (g_wsAttractDepth == 0u) {
        g_wsBarsOrig(rdram, ctx, runtime);
        return;
    }
    const uint32_t first = READ32(kWsPacketCursor);
    g_wsBarsOrig(rdram, ctx, runtime);
    const bool top = rrvWsCollapseBar(rdram, ctx, runtime, first, 0x7900u, 0x7A70u, 0x7900u);
    const bool bottom = rrvWsCollapseBar(rdram, ctx, runtime, first + 0x80u, 0x85B0u, 0x8700u, 0x8700u);
    static bool reported = false;
    if ((!top || !bottom) && !reported) {
        reported = true;
        std::fprintf(stderr, "[rr5-enhance] widescreen intro: letterbox packets not found at 0x%08X (top %d, bottom %d); "
                             "the bars stay\n", first, top, bottom);
    }
}
} // namespace

// RR5 enhancement (opt-in, owner direction 2026-09-29): scenery draw distance.
// The course is not culled by distance at run time. func_21D850 maps the
// camera to a course section (position / 20, func_21AE80) and a heading
// octant, and func_21D0B0 copies that (section, octant) entry of the course's
// precomputed visibility table ([gp-0x58F8] = *0x334D78, entry pointer at
// +0xC + (section*8 + octant)*4; u16 block indices at entry+8; count = word 0,
// byte 5 in modes 2-3, byte 4 when mirrored ([gp-0x5898] = 0x334DD8 == 1);
// near count = byte 6, byte 7 mirrored; both capped at 256) into the draw
// list at 0x1DE08A0 (count), 0x1DE08A4 (near count), 0x1DE08A8 (u32 blocks).
// func_21D438 draws entries [near, count) with the plain far setup
// (func_21BDD8) and [0, near) after func_21BF08, which loads the clip planes.
// Whatever lies outside the baked list pops in when the camera enters the
// next section. RRV_RR5_DRAW_DISTANCE=N (sections, 1..32) adds the blocks of
// the same octant's lists for the N sections ahead of the camera (every block
// is resident: func_21D730 draws them all) to the clipped (near) group, so a
// block that crosses a plane is clipped, never drawn unclipped. The list keeps
// the game's 256-entry limit. Measured 2026-09-29 (wheel-v130 replay, 4x,
// headless unpaced): far buildings appear sections earlier; guest simulation
// unchanged (identical lap clocks); throughput 107 -> 89 (+2) -> 82 (+4) -> 68 (+8)
// fields/s. Off by default: it changes what the guest draws, so reference runs
// never set it. RRV_RR5_DRAW_DISTANCE_LOG=1 logs the list sizes.
namespace {
PS2Runtime::RecompiledFunction g_dd21D0B0Orig = nullptr;
uint32_t g_ddSections = 0;
bool g_ddLog = false;

uint32_t rrvDdRead32(uint8_t* rdram, uint32_t a) {
    const uint8_t* p = getMemPtr(rdram, a);
    uint32_t v = 0;
    if (p) std::memcpy(&v, p, 4);
    return v;
}
void rrvDdWrite32(uint8_t* rdram, uint32_t a, uint32_t v) {
    if (uint8_t* p = getMemPtr(rdram, a)) std::memcpy(p, &v, 4);
}

// The (section, octant) entry exactly as func_21AF20 reads it.
bool rrvDdEntry(uint8_t* rdram, uint32_t section, uint32_t octant, uint32_t* list, uint32_t* count) {
    const uint32_t table = rrvDdRead32(rdram, 0x334D78u);
    if (table == 0u) return false;
    const uint32_t entry = rrvDdRead32(rdram, table + (section * 8u + octant) * 4u + 0xCu);
    const uint8_t* e = entry ? getMemPtr(rdram, entry) : nullptr;
    if (!e) return false;
    const int32_t mode = static_cast<int32_t>(rrvDdRead32(rdram, 0x334DD8u));
    uint32_t n;
    if (mode == 1) n = e[4];
    else if (mode >= 2 && mode < 4) n = e[5];
    else std::memcpy(&n, e, 4);
    *list = entry + 8u;
    *count = std::min(n, 0x100u);
    return true;
}

void patch_0x21d0b0_draw_distance(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t section = GPR_U32(ctx, 4), octant = GPR_U32(ctx, 5) & 7u;
    g_dd21D0B0Orig(rdram, ctx, runtime);
    const uint32_t positions = rrvDdRead32(rdram, 0x334D44u); // [gp-0x592C], func_21D850's modulus
    const uint32_t sections = (positions + 19u) / 20u;
    const uint32_t count = std::min(rrvDdRead32(rdram, 0x1DE08A0u), 0x100u);
    const uint32_t nearCount = std::min(rrvDdRead32(rdram, 0x1DE08A4u), count);
    if (sections == 0u || section >= sections) return;
    uint32_t blocks[0x100];
    for (uint32_t i = 0; i < count; ++i) blocks[i] = rrvDdRead32(rdram, 0x1DE08A8u + i * 4u);
    uint32_t extra[0x100];
    uint32_t extraCount = 0;
    auto present = [&](uint32_t b) {
        for (uint32_t i = 0; i < count; ++i) if (blocks[i] == b) return true;
        for (uint32_t i = 0; i < extraCount; ++i) if (extra[i] == b) return true;
        return false;
    };
    // Which way along the section index the camera looks: a section behind
    // the camera (looking the same way) still sees the camera's own near
    // blocks; a section ahead does not. The side sharing fewer near blocks
    // is ahead.
    auto overlap = [&](uint32_t s) {
        uint32_t list = 0, n = 0, shared = 0;
        if (!rrvDdEntry(rdram, s, octant, &list, &n)) return 0u;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* p = getMemPtr(rdram, list + i * 2u);
            if (!p) break;
            uint16_t b;
            std::memcpy(&b, p, 2);
            for (uint32_t j = 0; j < nearCount; ++j) if (blocks[j] == b) { ++shared; break; }
        }
        return shared;
    };
    const uint32_t next = (section + 1u) % sections, prev = (section + sections - 1u) % sections;
    const int32_t step = overlap(next) <= overlap(prev) ? 1 : -1;
    for (uint32_t k = 1; k <= g_ddSections && count + extraCount < 0x100u; ++k) {
        const uint32_t s = (section + sections + static_cast<uint32_t>(step * static_cast<int32_t>(k % sections))) % sections;
        uint32_t list = 0, n = 0;
        if (!rrvDdEntry(rdram, s, octant, &list, &n)) continue;
        for (uint32_t i = 0; i < n && count + extraCount < 0x100u; ++i) {
            const uint8_t* p = getMemPtr(rdram, list + i * 2u);
            if (!p) break;
            uint16_t b;
            std::memcpy(&b, p, 2);
            if (!present(b)) extra[extraCount++] = b;
        }
    }
    if (g_ddLog) {
        static uint32_t calls = 0;
        if (calls++ % 120u == 0u)
            std::fprintf(stderr, "[rr5-enhance] draw distance: section %u/%u octant %u step %d blocks %u (near %u) + %u\n",
                         section, sections, octant, step, count, nearCount, extraCount);
    }
    if (extraCount == 0u) return;
    // New order: original near group, extras (clipped group), original far group.
    uint32_t out = nearCount;
    for (uint32_t i = 0; i < extraCount; ++i) rrvDdWrite32(rdram, 0x1DE08A8u + (out++) * 4u, extra[i]);
    for (uint32_t i = nearCount; i < count; ++i) rrvDdWrite32(rdram, 0x1DE08A8u + (out++) * 4u, blocks[i]);
    rrvDdWrite32(rdram, 0x1DE08A0u, count + extraCount);
    rrvDdWrite32(rdram, 0x1DE08A4u, nearCount + extraCount);
}
} // namespace
// RR5 enhancement (opt-in, owner direction 2026-09-30): fast unpack.
// func_221D68 is the game's LZSS unpacker (called only from func_222258). It
// can stop when the available input (a2, 0 = all of it) runs short and resume
// later; its state lives in the gp words -0x5B78..-0x5B50, -0x6B04, -0x6B00.
// The recompiled original costs one modeled EE cycle per instruction, so a
// 3.4 MB boot pack takes 20 fields of guest time with no drawing (the boot
// Namco line animation freezes twice, fields 62-80 and 83-102). This native
// port produces the same bytes, return value and state words, and charges
// 64 + (input + output bytes) / 16 cycles. RRV_RR5_FAST_UNPACK=1 turns it on;
// =verify runs the original too (guest time unchanged) and logs every call
// that differs. It changes guest time, not guest data. RR5-specific.
namespace {
PS2Runtime::RecompiledFunction g_unpack221D68Orig = nullptr;
int g_unpackMode = 0; // 1 fast, 2 verify
uint64_t g_unpackCalls = 0, g_unpackBad = 0, g_unpackFallback = 0;
uint64_t g_unpackHostNs = 0, g_unpackHostMaxNs = 0, g_unpackBytes = 0; // host cost of the native port
uint64_t g_unpackCommitNs = 0; // of which: copying the result into guest memory

// Unpack ahead (2026-10-05). The Steam Deck needs about 15 ms of host time for the largest boot packs even with
// the host-pointer loop: one late frame each. The game reads a whole pack with one sceCdRead and calls the
// unpacker at the earliest one field later, so a worker thread starts unpacking a copy of the pack when the read
// returns and the unpacker call takes the finished output. The copy is compared byte for byte with the guest
// input at the call and the worker records what the answer depends on (see RrvUnpackJob); anything else falls
// back to unpacking on the game thread. Host-only: guest data and guest time are the same either way.
struct RrvUnpackJob {
    // header (10 bytes) + packed data + 16 following guest bytes. The WORKER copies them from guest memory
    // (the copy cost the game thread about 2 ms per 3.4 MB on the Deck), so the copy can be torn if the guest
    // writes there meanwhile: the call compares it with the guest input after the worker is done, and a torn
    // copy then simply does not match.
    std::vector<uint8_t> in;
    const uint8_t* src = nullptr;     // host address of the guest input
    uint32_t guestAddr = 0, span = 0; // filter for the call, before it waits for the worker
    uint8_t head[10] = {};
    std::vector<uint8_t> out;
    uint32_t inUsed = 0, outSize = 0; // where the loop stopped
    uint32_t lastBoundary = 0;        // input offset at the last 64-item block boundary the loop continued from
    bool hasBoundary = false;
    int64_t maxLead = INT64_MIN;      // largest (bytes written - input offset) at any read
    bool valid = false;               // false: the pack reads output from before its own start
    std::future<void> done;
};
std::deque<std::shared_ptr<RrvUnpackJob>> g_unpackJobs;
std::shared_ptr<RrvUnpackJob> g_unpackJobHeld; // keeps the output of the job the last call used alive
PS2Runtime::RecompiledFunction g_unpackCdReadOrig = nullptr;
uint64_t g_unpackJobsStarted = 0, g_unpackJobsUsed = 0;

// The statements of the host-pointer loop in rrvUnpack, on offsets into the job's own buffers.
void rrvUnpackJobRun(RrvUnpackJob& j) {
    j.in.assign(j.src, j.src + j.span);
    std::memcpy(j.in.data(), j.head, sizeof j.head); // the header the read saw decides the layout
    const uint8_t* const in = j.in.data();
    const uint32_t s6 = in[0], flagBytes = in[1];
    uint32_t usize, csize; std::memcpy(&usize, in + 2, 4); std::memcpy(&csize, in + 6, 4);
    const uint32_t mask = (1u << (s6 & 31u)) - 1u, t4 = 1u << ((16u - s6) & 31u), a3 = mask + 1u, shift = s6 & 31u;
    j.out.resize(size_t(usize) + t4 + 64u);
    uint8_t* const out = j.out.data();
    uint32_t p = 10u, q = 0u;
    const uint32_t srcEnd = 10u + csize, dstEnd = usize;
    for (;;) {
        j.maxLead = std::max(j.maxLead, int64_t(q) - int64_t(p));
        uint64_t s2 = 0;
        for (uint32_t t1 = 0; t1 < flagBytes; ++t1) {
            s2 += uint64_t(in[p]) << ((t1 * 8u) & 63u); ++p;
            if (!(p < srcEnd)) break;
        }
        bool stop = false;
        for (int32_t t1 = int32_t(flagBytes * 8u); t1 > 0; --t1) {
            j.maxLead = std::max(j.maxLead, int64_t(q) - int64_t(p));
            if ((s2 & 1u) != 0) {
                const uint32_t run = std::min<uint32_t>(~s2 ? uint32_t(__builtin_ctzll(~s2)) : 64u, uint32_t(t1));
                const uint32_t lim = std::min(srcEnd - p, dstEnd - q), k = std::min(run, lim);
                for (uint32_t i = 0; i < k; i += 8u) std::memcpy(out + q + i, in + p + i, 8);
                p += k; q += k;
                s2 = k >= 64u ? 0 : s2 >> k;
                t1 -= int32_t(k) - 1;
                if (k == lim) { stop = true; break; }
                continue;
            }
            s2 >>= 1;
            const uint32_t v1 = uint32_t(in[p + 1u]) | (uint32_t(in[p]) << 8);
            p += 2u;
            uint32_t off = v1 & mask, len = uint32_t(int32_t(v1) >> shift);
            if (off == 0) off = a3;
            if (len == 0) len = t4;
            if (off > q) return; // copies bytes from before this pack's output: not self-contained (valid stays false)
            uint8_t* d = out + q; const uint8_t* c = d - off;
            if (off >= 16u) for (uint32_t i = 0; i < len; i += 16u) std::memcpy(d + i, c + i, 16);
            else for (uint32_t i = 0; i < len; ++i) d[i] = c[i];
            q += len;
            if (!(p < srcEnd) || !(q < dstEnd)) { stop = true; break; }
        }
        if (stop) break;
        j.lastBoundary = p; j.hasBoundary = true;
    }
    j.inUsed = p; j.outSize = q; j.valid = true;
}

// sceCdRead (func_2C7780: lbn, sectors, buffer): after a successful read into EE memory that starts with a
// plausible pack header and holds the whole pack, start unpacking a copy.
void patch_0x2c7780_unpack_ahead(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t sectors = GPR_U32(ctx, 5), buf = GPR_U32(ctx, 6);
    g_unpackCdReadOrig(rdram, ctx, runtime);
    if (GPR_U32(ctx, 2) != 1u || sectors == 0 || sectors > 0x8000u) return;
    const uint8_t* head = getMemPtr(rdram, buf);
    if (!head || !getMemPtr(rdram, buf + 9u) || getMemPtr(rdram, buf + 9u) - head != 9) return;
    uint32_t usize, csize; std::memcpy(&usize, head + 2, 4); std::memcpy(&csize, head + 6, 4);
    if (!(uint32_t(head[0]) - 1u < 14u) || head[1] != 8u || usize == 0 || usize > 0x2000000u || csize == 0 ||
        uint64_t(csize) + 10u > uint64_t(sectors) * 2048u || csize < 0x8000u) return; // small packs are cheap inline
    const uint32_t span = 10u + csize + 16u;
    const uint8_t* last = getMemPtr(rdram, buf + span - 1u);
    if (!last || last - head != std::ptrdiff_t(span - 1u)) return;
    auto job = std::make_shared<RrvUnpackJob>();
    job->src = head; job->guestAddr = buf; job->span = span;
    std::memcpy(job->head, head, sizeof job->head);
    job->done = std::async(std::launch::async, [job] { rrvUnpackJobRun(*job); });
    g_unpackJobs.push_back(std::move(job));
    ++g_unpackJobsStarted;
    while (g_unpackJobs.size() > 4u) g_unpackJobs.pop_front(); // a dropped job is waited for by its future
}

struct RrvUnpackResult {
    bool ok = false;
    uint32_t v0 = 0, base = 0, inBytes = 0;
    std::vector<uint8_t> out;           // output of the byte-at-a-time loop
    const uint8_t* data = nullptr;      // the output of either loop (out, or the reused scratch buffer)
    size_t size = 0;
    std::map<uint32_t, uint32_t> words; // gp offset -> value written
};

bool rrvUnpack(uint8_t* rdram, R5900Context* ctx, RrvUnpackResult& r) {
    const uint32_t gp = GPR_U32(ctx, 28);
    std::map<uint32_t, uint32_t>& w = r.words;
    bool bad = false;
    auto get = [&](uint32_t off) -> uint32_t {
        if (auto it = w.find(off); it != w.end()) return it->second;
        const uint8_t* p = getMemPtr(rdram, gp - off);
        if (!p) { bad = true; return 0; }
        uint32_t v; std::memcpy(&v, p, 4); return v;
    };
    auto put = [&](uint32_t off, uint32_t v) { w[off] = v; };
    auto rd8 = [&](uint32_t a) -> uint32_t {
        if (a - r.base < r.out.size()) return r.out[a - r.base];
        const uint8_t* p = getMemPtr(rdram, a);
        if (!p) { bad = true; return 0; }
        return *p;
    };
    auto reset = [&] { put(0x6B04, 0); put(0x6B00, 0); };
    uint32_t s0 = GPR_U32(ctx, 4), s1 = GPR_U32(ctx, 5);
    const uint32_t s5 = GPR_U32(ctx, 6), a3in = GPR_U32(ctx, 7), t0 = GPR_U32(ctx, 8);
    const uint32_t src0 = s0;
    r.base = s1;
    auto done = [&](uint32_t v0) {
        r.v0 = v0; r.inBytes = s0 - src0; r.ok = !bad;
        if (!r.data) { r.data = r.out.data(); r.size = r.out.size(); }
        return r.ok;
    };
    if (s0 == s5 && s5 != 0) return done(s1);
    if (get(0x5B70) == s5 && s5 != 0) return done(s1);
    const uint32_t t5 = get(0x6B04);
    uint32_t s6 = get(0x5B6C), s7, t4, s3, s4, fp;
    if (t5 == 0) {
        s6 = rd8(s0++);
        s7 = (1u << (s6 & 31u)) - 1u;
        t4 = 1u << ((16u - s6) & 31u);
        put(0x5B6C, s6); put(0x5B68, s7); put(0x5B64, t4);
        s3 = rd8(s0++); put(0x5B60, s3);
        uint32_t a2 = 0, a1 = 0;
        for (uint32_t i = 0; i < 4; ++i) a2 += rd8(s0++) << (i * 8u);
        put(0x5B5C, a2);
        for (uint32_t i = 0; i < 4; ++i) a1 += rd8(s0++) << (i * 8u);
        fp = s1 + a2; s4 = s0 + a1;
        put(0x5B58, a1); put(0x5B54, s4); put(0x5B50, fp);
        if (!(s6 - 1u < 14u) || s3 != 8u || a2 != a3in || a1 != t0 - 10u) { put(0x6B00, 1); return done(s1); }
        if (get(0x6B00) != 0) put(0x6B00, 0xFFFFFFFFu);
    } else {
        s7 = get(0x5B68); t4 = get(0x5B64); s3 = get(0x5B60); s4 = get(0x5B54); fp = get(0x5B50);
        s0 = get(0x5B78); s1 = get(0x5B74);
        r.base = s1;
    }
    put(0x6B04, t5 + 1u);
    const auto shortInput = [&] { return s5 != 0 && s5 - (s3 * 16u + 1u) < s0; };
    if (shortInput()) { put(0x5B78, s0); put(0x5B70, s5); put(0x5B74, s1); return done(s1); }
    const uint32_t a3 = s7 + 1u;
    // Host-speed path (2026-10-05: the byte-at-a-time loop below took 115 ms for
    // the 3.4 MB boot pack on the Steam Deck, a visible hitch in real time).
    // When everything the loop can touch (input, output plus the longest copy,
    // the furthest back-reference) is one contiguous host range, it runs on
    // host pointers. Same statements as the loop below; output still goes to
    // r.out first, so verify mode and the fallback are unchanged.
    if (!bad && s0 < s4 && s1 < fp && s1 > a3 && fp - s1 <= 0x4000000u && s4 - s0 <= 0x4000000u) {
        const uint32_t lo = std::min(s0, s1 - a3), hi = std::max(s4 + 16u, fp + t4 + 1u);
        const uint8_t* first = hi > lo ? getMemPtr(rdram, lo) : nullptr;
        const uint8_t* last = first ? getMemPtr(rdram, hi - 1u) : nullptr;
        if (first && last && last - first == std::ptrdiff_t(hi - 1u - lo)) {
            const uint8_t* const mem = first - lo; // mem + guest address, valid in [lo, hi)
            // A fresh call whose input is byte for byte a pack the worker has unpacked (or is unpacking).
            if (t5 == 0 && s0 == src0 + 10u) for (auto it = g_unpackJobs.rbegin(); it != g_unpackJobs.rend(); ++it) {
                const std::shared_ptr<RrvUnpackJob> job = *it;
                const uint32_t span = job->span;
                if (job->guestAddr != src0 || span != 10u + (s4 - s0) + 16u ||
                    std::memcmp(mem + src0, job->head, sizeof job->head) != 0) continue;
                job->done.wait();
                g_unpackJobs.erase(std::next(it).base());
                if (std::memcmp(mem + src0, job->in.data(), span) != 0) break;
                // The loop on the game thread reads this call's own output where output has overtaken input in
                // guest memory, and stops early at a block boundary on short input: the worker's result stands
                // only if neither can have happened.
                const int64_t gap = int64_t(src0) - int64_t(s1);
                const bool noOverlapRead = gap >= job->maxLead || gap + int64_t(span) <= 0;
                const bool noShortInput = !job->hasBoundary || !(s5 != 0 && s5 - (s3 * 16u + 1u) < src0 + job->lastBoundary);
                if (!job->valid || !noOverlapRead || !noShortInput) break;
                g_unpackJobHeld = job; ++g_unpackJobsUsed;
                r.data = job->out.data(); r.size = job->outSize;
                s0 = src0 + job->inUsed; s1 += job->outSize;
                reset();
                return done(s1);
            }
            const uint32_t base = s1;
            // One scratch buffer for every call (a fresh 5 MB vector per call cost page faults and a zero fill),
            // with slack for the 16-byte copies below.
            static std::vector<uint8_t> scratch;
            const size_t want = size_t(fp - s1) + t4 + 64u;
            if (scratch.size() < want) scratch.resize(want);
            uint8_t* const out = scratch.data() - base; // out + guest address
            // Where input and output ranges overlap, a read sees this call's own output.
            const bool overlap = s0 < fp + t4 + 1u && base < s4 + 16u;
            // The loop state lives in locals no lambda captures, so it stays in registers.
            uint32_t p = s0, q = s1;
            const uint32_t srcEnd = s4, dstEnd = fp, mask = s7, shift = s6 & 31u, flagBytes = s3;
            const uint32_t need = s3 * 16u + 1u;
            bool resetState = false, saveState = false;
#define RRV_UNPACK_RD(a) (overlap && (a) >= base && (a) < q ? out[(a)] : mem[(a)])
            for (;;) {
                uint64_t s2 = 0;
                for (uint32_t t1 = 0; t1 < flagBytes; ++t1) {
                    s2 += uint64_t(RRV_UNPACK_RD(p)) << ((t1 * 8u) & 63u); ++p;
                    if (!(p < srcEnd)) { resetState = true; break; }
                }
                bool stop = false;
                for (int32_t t1 = int32_t(flagBytes * 8u); t1 > 0; --t1) {
                    if ((s2 & 1u) != 0 && !overlap) {
                        // A run of literal flags at once. The byte loop stops after the item that reaches the
                        // end of the input or of the output: that is item `lim` of the run.
                        const uint32_t run = std::min<uint32_t>(~s2 ? uint32_t(__builtin_ctzll(~s2)) : 64u, uint32_t(t1));
                        const uint32_t lim = std::min(srcEnd - p, dstEnd - q), k = std::min(run, lim);
                        for (uint32_t i = 0; i < k; i += 8u) std::memcpy(out + q + i, mem + p + i, 8);
                        p += k; q += k;
                        s2 = k >= 64u ? 0 : s2 >> k;
                        t1 -= int32_t(k) - 1;
                        if (k == lim) { resetState = true; stop = true; break; }
                        continue;
                    }
                    const bool literal = (s2 & 1u) != 0; s2 >>= 1;
                    if (literal) {
                        out[q] = uint8_t(RRV_UNPACK_RD(p)); ++q; ++p;
                    } else {
                        const uint32_t hi8 = RRV_UNPACK_RD(p), lo8 = RRV_UNPACK_RD(p + 1u);
                        p += 2u;
                        const uint32_t v1 = lo8 | (hi8 << 8);
                        uint32_t off = v1 & mask, len = uint32_t(int32_t(v1) >> shift);
                        if (off == 0) off = a3;
                        if (len == 0) len = t4;
                        uint32_t a1 = q - off;
                        if (a1 >= base && off >= 16u) {
                            // The usual case. Source and destination are 16 or more bytes apart, so 16-byte
                            // chunks in order give the bytes of the forward byte copy; the bytes written past
                            // len are scratch the following output overwrites.
                            uint8_t* d = out + q; const uint8_t* c = out + a1;
                            for (uint32_t i = 0; i < len; i += 16u) std::memcpy(d + i, c + i, 16);
                            q += len;
                        } else if (a1 >= base) {
                            uint8_t* d = out + q; const uint8_t* c = out + a1;
                            for (uint32_t i = 0; i < len; ++i) d[i] = c[i];
                            q += len;
                        } else {
                            for (uint32_t i = 0; i < len; ++i, ++a1) out[q++] = uint8_t(a1 >= base ? out[a1] : mem[a1]);
                        }
                    }
                    if (!(p < srcEnd) || !(q < dstEnd)) { resetState = true; stop = true; break; }
                }
                if (stop) break;
                if (s5 != 0 && s5 - need < p) { saveState = true; break; }
            }
#undef RRV_UNPACK_RD
            s0 = p; s1 = q;
            // The slow loop's order: a flag-byte reset is overwritten by nothing later, so one reset at the end is
            // the same state (reset only clears two words; saveState writes three others).
            if (resetState) reset();
            if (saveState) { put(0x5B78, s0); put(0x5B70, s5); put(0x5B74, s1); }
            r.data = scratch.data(); r.size = s1 - base;
            return done(s1);
        }
    }
    for (;;) {
        if (bad) return done(s1);
        uint64_t s2 = 0;
        for (uint32_t t1 = 0; t1 < s3; ++t1) {
            s2 += uint64_t(rd8(s0++)) << ((t1 * 8u) & 63u);
            if (!(s0 < s4)) { reset(); break; }
        }
        for (int32_t t1 = int32_t(s3 * 8u); t1 > 0; --t1) {
            const bool literal = (s2 & 1u) != 0; s2 >>= 1;
            if (literal) {
                r.out.push_back(uint8_t(rd8(s0++))); ++s1;
            } else {
                const uint32_t hi = rd8(s0++), lo = rd8(s0++), v1 = lo | (hi << 8);
                uint32_t off = v1 & s7, len = uint32_t(int32_t(v1) >> (s6 & 31u));
                if (off == 0) off = a3;
                if (len == 0) len = t4;
                uint32_t a1 = s1 - off;
                for (uint32_t i = 0; i < len; ++i) { r.out.push_back(uint8_t(rd8(a1++))); ++s1; }
            }
            if (bad) return done(s1);
            if (!(s0 < s4) || !(s1 < fp)) { reset(); return done(s1); }
        }
        if (shortInput()) { put(0x5B78, s0); put(0x5B70, s5); put(0x5B74, s1); return done(s1); }
    }
}

void rrvUnpackCommit(uint8_t* rdram, R5900Context* ctx, const RrvUnpackResult& r) {
    const uint32_t gp = GPR_U32(ctx, 28), sp = GPR_U32(ctx, 29) - 0x110u;
    // The original's register saves below sp (guest-visible stack bytes).
    static const struct { uint32_t off; int reg; } saves[] = {
        {0xC0, 21}, {0x80, 17}, {0x100, 31}, {0x70, 16}, {0xF0, 30}, {0xE0, 23},
        {0xD0, 22}, {0xB0, 20}, {0xA0, 19}, {0x90, 18}};
    for (const auto& s : saves)
        if (uint8_t* p = getMemPtr(rdram, sp + s.off)) { const uint64_t v = GPR_U64(ctx, s.reg); std::memcpy(p, &v, 8); }
    if (r.size) {
        uint8_t* first = getMemPtr(rdram, r.base);
        uint8_t* last = getMemPtr(rdram, r.base + uint32_t(r.size - 1u));
        if (first && last && last - first == std::ptrdiff_t(r.size - 1u)) std::memcpy(first, r.data, r.size);
        else for (size_t i = 0; i < r.size; ++i) *getMemPtr(rdram, r.base + uint32_t(i)) = r.data[i];
    }
    for (const auto& [off, v] : r.words) std::memcpy(getMemPtr(rdram, gp - off), &v, 4);
    SET_GPR_S32(ctx, 2, int32_t(r.v0));
    ctx->pc = GPR_U32(ctx, 31);
}

void patch_0x221d68_fast_unpack(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ++g_unpackCalls;
    RrvUnpackResult r;
    const auto hostT0 = std::chrono::steady_clock::now();
    const bool ok = rrvUnpack(rdram, ctx, r);
    bool inRam = ok;
    for (size_t i = 0; inRam && i < r.size; i += 4096)
        inRam = getMemPtr(rdram, r.base + uint32_t(i)) && getMemPtr(rdram, r.base + uint32_t(r.size - 1));
    if (!inRam) { ++g_unpackFallback; g_unpack221D68Orig(rdram, ctx, runtime); return; }
    if (g_unpackMode == 2) {
        const uint32_t gp = GPR_U32(ctx, 28);
        g_unpack221D68Orig(rdram, ctx, runtime);
        bool same = GPR_U32(ctx, 2) == r.v0;
        for (size_t i = 0; same && i < r.size; ++i) same = *getMemPtr(rdram, r.base + uint32_t(i)) == r.data[i];
        for (const auto& [off, v] : r.words) {
            uint32_t g; std::memcpy(&g, getMemPtr(rdram, gp - off), 4);
            same = same && g == v;
        }
        if (!same && g_unpackBad++ < 20)
            std::fprintf(stderr, "[rr5-enhance] fast unpack MISMATCH call %llu base=0x%x out=%zu v0 native=0x%x guest=0x%x\n",
                         (unsigned long long)g_unpackCalls, r.base, r.size, r.v0, GPR_U32(ctx, 2));
        if (g_unpackCalls % 64u == 0u)
            std::fprintf(stderr, "[rr5-enhance] fast unpack verify: %llu calls, %llu mismatches\n",
                         (unsigned long long)g_unpackCalls, (unsigned long long)g_unpackBad);
        return;
    }
    const auto commitT0 = std::chrono::steady_clock::now();
    rrvUnpackCommit(rdram, ctx, r);
    g_unpackCommitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - commitT0).count());
    const uint64_t hostNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - hostT0).count());
    g_unpackHostNs += hostNs; g_unpackHostMaxNs = std::max(g_unpackHostMaxNs, hostNs); g_unpackBytes += r.size;
    runtime->gate3ChargeHleV1(64u + (uint64_t(r.inBytes) + r.size) / 16u);
}
} // namespace
// RR5 native hot functions (owner direction 2026-09-30), RRV_RR5_NATIVE_HOT=1.
// The most expensive guest functions (host profile of the game thread, boot
// through the demo race) run as native versions made by tools/ee-native from
// their own generated code: every instruction statement is kept, but the two
// runtime calls the generated code makes before each instruction
// (gate3CheckpointV1, gate3BeginInstructionV1) become a local cycle counter
// inside the lazy window, committed before any runtime interaction. The guest
// sees the same state at every interaction, so guest time and guest data are
// unchanged; only host time drops. A function whose code in guest RAM differs
// from the words it was generated from falls back to the generated original.
#include <array>
#include <utility>
namespace rrv_native {
struct Word { uint32_t pc, word; };
using Fn = void (*)(uint8_t*, R5900Context*, PS2Runtime*);
struct Entry { uint32_t address; const char* name; Fn original; Fn native; const Word* words; size_t count; };
// Same switch as the runtime's rrvSprFastV1 (RRV_GATE4_SPR_FAST=0 disables).
inline const bool kSprFast = [] { const char* v = std::getenv("RRV_GATE4_SPR_FAST"); return !(v && v[0] == '0'); }();
// The clock of one native function. Its fields must stay in host registers: a native function runs
// the checkpoint test before every guest instruction, and guest stores (through uint8_t*) may alias
// anything whose address has escaped. So no member function that is not inlined takes `this`, and
// nothing outside is handed a reference: the slow paths receive and return the counters by value
// (State), and hooks get the scalars they need. Host disassembly of 2026-10-02 (the address did
// escape, through the out-of-line slow path): five loads and three stores of clock fields per
// guest instruction.
struct Clock {
    struct State { uint64_t c, base, lim, gen, lastck; bool any; };
    PS2Runtime* rt; R5900Context* ctx; uint8_t* spr_;
    uint64_t c = 0, base = 0, lim = 0, gen = 0, lastck = 0; bool any = false;
    __attribute__((always_inline)) Clock(PS2Runtime* r, R5900Context* x) : rt(r), ctx(x), spr_(r->memory().getScratchpad()) { reload(); }
    __attribute__((always_inline)) ~Clock() { sync(); }
    Clock(const Clock&) = delete; Clock& operator=(const Clock&) = delete;
    __attribute__((always_inline)) inline State state() const { return State{c, base, lim, gen, lastck, any}; }
    __attribute__((always_inline)) inline void set(const State& s) { c = s.c; base = s.base; lim = s.lim; gen = s.gen; lastck = s.lastck; any = s.any; }
    // Re-read the runtime after it may have run: cycle count and lazy window.
    __attribute__((always_inline)) inline void reload() { uint64_t g = 0; c = base = rt->gate4NativeCyclesV1(); lim = rt->gate4NativeLimitV1(ctx, g); gen = g; any = false; }
    // Publish the local count exactly as the per-instruction calls would have.
    __attribute__((always_inline)) inline void sync() { if (c != base || any) { rt->gate4NativeCommitV1(ctx, c, lastck, any); base = c; any = false; } }
    __attribute__((always_inline)) inline bool quiet() const { return rrv::guest_time::quiet_generation.load(std::memory_order_relaxed) == gen; }
    // gate3CheckpointV1: the lazy fast path, else the real checkpoint.
    // Always inline: in the largest function the compiler called it instead (199 of its sites, Steam Deck
    // profile 2026-10-02), a call and a return around two compares.
    __attribute__((always_inline)) inline void ck() { if (__builtin_expect(c < lim && quiet(), true)) { lastck = c; any = true; } else set(slow(rt, ctx, state())); }
    __attribute__((always_inline)) inline void begin() { ++c; }
    // Back-edge scheduler check: a no-op while the armed window's generation holds.
    __attribute__((always_inline)) inline bool preempt() {
        if (lim != 0 && quiet()) return false;
        sync(); const bool r = rt->shouldPreemptGuestExecution(); reload(); return r;
    }
    __attribute__((always_inline)) inline void signal(R5900Context* x, PS2Exception e) { set(signalSlow(rt, ctx, x, e, state())); }
    __attribute__((always_inline)) inline void vu0(uint8_t* rdram, R5900Context* x, uint32_t address) { sync(); rt->executeVU0Microprogram(rdram, x, address); reload(); }
    __attribute__((always_inline)) inline void brk(uint8_t* rdram, R5900Context* x) { set(brkSlow(rt, ctx, rdram, x, state())); }
    // The runtime's scratchpad fast path (rrvSprFastV1), inline.
    __attribute__((always_inline)) inline uint8_t* spr(uint32_t vaddr, uint32_t size) const {
        const uint32_t offset = vaddr - PS2_SCRATCHPAD_BASE;
        if (!kSprFast || g_ps2PathWatchArmed || offset >= PS2_SCRATCHPAD_SIZE || (vaddr & (size - 1u)) != 0u) return nullptr;
        return spr_ ? spr_ + offset : nullptr;
    }
private:
    // The slow paths: commit, call the runtime, re-read. By value in and out (see above).
    static inline void commit(PS2Runtime* rt, R5900Context* ctx, State& s) {
        if (s.c != s.base || s.any) { rt->gate4NativeCommitV1(ctx, s.c, s.lastck, s.any); s.base = s.c; s.any = false; }
    }
    static inline void reread(PS2Runtime* rt, R5900Context* ctx, State& s) {
        s.c = s.base = rt->gate4NativeCyclesV1(); s.lim = rt->gate4NativeLimitV1(ctx, s.gen); s.any = false;
    }
    __attribute__((noinline)) static State slow(PS2Runtime* rt, R5900Context* ctx, State s) { commit(rt, ctx, s); rt->gate3CheckpointV1(ctx); reread(rt, ctx, s); return s; }
    __attribute__((noinline)) static State signalSlow(PS2Runtime* rt, R5900Context* ctx, R5900Context* x, PS2Exception e, State s) { commit(rt, ctx, s); rt->SignalException(x, e); reread(rt, ctx, s); return s; }
    __attribute__((noinline)) static State brkSlow(PS2Runtime* rt, R5900Context* ctx, uint8_t* rdram, R5900Context* x, State s) { commit(rt, ctx, s); rt->handleBreak(rdram, x); reread(rt, ctx, s); return s; }
};
} // namespace rrv_native
// Hand-written native routines the generated native functions hook (tools/ee-native --hook).
#include "rr5_car_builder.h" // func_222EB8's block loop: one VCALLMS per car vertex
#pragma push_macro("READ8")
#pragma push_macro("READ16")
#pragma push_macro("READ32")
#pragma push_macro("READ64")
#pragma push_macro("READ128")
#pragma push_macro("WRITE8")
#pragma push_macro("WRITE16")
#pragma push_macro("WRITE32")
#pragma push_macro("WRITE64")
#pragma push_macro("WRITE128")
#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128
#define RRV_NATIVE_READ(T, N, addr) ([&]() -> T { const uint32_t _a = (uint32_t)(addr); \
    if (!PS2Runtime::isSpecialAddress(_a)) return FAST_READ##N(_a); \
    if (uint8_t* _p = rrvNative.spr(_a, sizeof(T))) { T _v; std::memcpy(&_v, _p, sizeof(T)); return _v; } \
    rrvNative.sync(); const T _v = runtime->Load##N(rdram, ctx, _a); rrvNative.reload(); return _v; }())
#define READ8(addr) RRV_NATIVE_READ(uint8_t, 8, addr)
#define READ16(addr) RRV_NATIVE_READ(uint16_t, 16, addr)
#define READ32(addr) RRV_NATIVE_READ(uint32_t, 32, addr)
#define READ64(addr) RRV_NATIVE_READ(uint64_t, 64, addr)
#define READ128(addr) RRV_NATIVE_READ(__m128i, 128, addr)
#define RRV_NATIVE_WRITE(T, N, NAME, addr, val) do { const uint32_t _a = (addr); \
    if (PS2Runtime::isSpecialAddress(_a)) { \
        if (uint8_t* _p = rrvNative.spr(_a, sizeof(T))) { const T _v = (T)(val); std::memcpy(_p, &_v, sizeof(T)); } \
        else { rrvNative.sync(); runtime->Store##N(rdram, ctx, _a, (val)); rrvNative.reload(); } } \
    else { ps2TraceGuestWrite(rdram, _a, sizeof(T), (T)(val), 0u, NAME, ctx); FAST_WRITE##N(_a, (val)); } } while (0)
#define WRITE8(addr, val) RRV_NATIVE_WRITE(uint8_t, 8, "WRITE8", addr, val)
#define WRITE16(addr, val) RRV_NATIVE_WRITE(uint16_t, 16, "WRITE16", addr, val)
#define WRITE32(addr, val) RRV_NATIVE_WRITE(uint32_t, 32, "WRITE32", addr, val)
#define WRITE64(addr, val) RRV_NATIVE_WRITE(uint64_t, 64, "WRITE64", addr, val)
#define WRITE128(addr, val) do { const uint32_t _a = (addr); const __m128i _value = (val); \
    if (PS2Runtime::isSpecialAddress(_a)) { \
        if (uint8_t* _p = rrvNative.spr(_a, 16u)) { std::memcpy(_p, &_value, 16u); } \
        else { rrvNative.sync(); runtime->Store128(rdram, ctx, _a, _value); rrvNative.reload(); } } \
    else { const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
        const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
        ps2TraceGuestWrite(rdram, _a, 16u, _lo, _hi, "WRITE128", ctx); FAST_WRITE128(_a, _value); } } while (0)
#include "rrv_ee_native.inc"  // game-derived native hot functions: generated/rr5/native (RRV_GENERATED_INCLUDE_DIR)
#undef RRV_NATIVE_READ
#undef RRV_NATIVE_WRITE
#pragma pop_macro("READ8")
#pragma pop_macro("READ16")
#pragma pop_macro("READ32")
#pragma pop_macro("READ64")
#pragma pop_macro("READ128")
#pragma pop_macro("WRITE8")
#pragma pop_macro("WRITE16")
#pragma pop_macro("WRITE32")
#pragma pop_macro("WRITE64")
#pragma pop_macro("WRITE128")
namespace rrv_native {
constexpr size_t kCount = sizeof(rrv_native_entries) / sizeof(rrv_native_entries[0]);
uint64_t g_calls[kCount] = {}, g_fallbacks = 0;
bool wordsMatch(const uint8_t* rdram, const Entry& e) {
    for (size_t i = 0; i < e.count; ++i) {
        const uint8_t* p = getMemPtr(const_cast<uint8_t*>(rdram), e.words[i].pc);
        uint32_t w = 0;
        if (!p) return false;
        std::memcpy(&w, p, 4);
        if (w != e.words[i].word) return false;
    }
    return true;
}
template <size_t I>
void thunk(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int state = 0; // 0 unchecked, 1 native, 2 original
    const Entry& e = rrv_native_entries[I];
    if (__builtin_expect(state != 1, false)) {
        if (state == 0) {
            state = wordsMatch(rdram, e) ? 1 : 2;
            if (state == 2) {
                ++g_fallbacks;
                std::fprintf(stderr, "[rr5-native] %s: guest code differs from its generation words; original kept\n", e.name);
            }
        }
        if (state == 2) { e.original(rdram, ctx, runtime); return; }
    }
    ++g_calls[I];
    e.native(rdram, ctx, runtime);
}
template <size_t... I>
constexpr std::array<Fn, sizeof...(I)> thunks(std::index_sequence<I...>) { return {&thunk<I>...}; }
void registerAll(PS2Runtime& runtime) {
    static constexpr auto table = thunks(std::make_index_sequence<kCount>{});
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const Entry& e = rrv_native_entries[i];
        // Only replace the generated original, never another patch.
        if (!runtime.hasFunction(e.address) || runtime.lookupFunction(e.address) != e.original) continue;
        runtime.registerFunction(e.address, table[i]);
        ++n;
    }
    std::fprintf(stderr, "[rr5-native] native hot functions: %zu of %zu registered\n", n, kCount);
    std::fprintf(stderr, "[rr5-car] native vertex block loop (func_222EB8): %s\n",
                 !rrv_car::kEnabled ? "off (RRV_RR5_NATIVE_BUILDER=0)" : rrv_car::kVerify ? "verify" : "on");
    static const bool atExit = [] { std::atexit([] {
        std::fprintf(stderr, "[rr5-native] calls:");
        for (size_t i = 0; i < kCount; ++i)
            std::fprintf(stderr, " %s=%llu", rrv_native_entries[i].name, (unsigned long long)g_calls[i]);
        std::fprintf(stderr, " fallbacks=%llu\n", (unsigned long long)g_fallbacks);
        rrv_car::reportAtExit();
    }); return true; }();
    (void)atExit;
}
} // namespace rrv_native
// 0x298eb0 job-table wait: idle skip in its poll 0x298f28 (stage wait-idle,
// scripts/wait_idle_overlay.py; RR5-specific, owner authorisation 2026-09-29).
namespace {
PS2Runtime::RecompiledFunction g_jobPoll298f28 = nullptr;
constexpr uint64_t kMaxIdleSkip = 4096; // EE cycles per step (~14 us)
}

static void patch_0x298f28(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool fromWait = ctx->pc == 0x298F28u && GPR_U32(ctx, 31) == 0x298EE0u;
    g_jobPoll298f28(rdram, ctx, runtime);
    if (!fromWait || (ctx->pc != 0x298EE0u && ctx->pc != 0x298F28u) || GPR_U32(ctx, 2) != 0u)
        return;
    // Not done: nothing on this thread can change a job status before the
    // next modeled event. Commit counted cycles, then move towards it.
    runtime->gate3TemporalCheckpointV1(ctx);
    if (runtime->gate3AtCutV1())
        return;
    auto& temporal = runtime->gate3TemporalV1();
    const uint64_t now = temporal.now(), next = temporal.NextDeadline();
    if (next > now && next != UINT64_MAX)
        runtime->gate3ChargeHleV1(std::min<uint64_t>(next - now, kMaxIdleSkip));
}

void registerPatches(PS2Runtime& runtime)
{
    // CD payload digests cost host time at every disc read and only feed the semantic trace (ps2_runtime.h).
    { const char* trace = std::getenv("RRV_GATE3_SEMANTIC_TRACE"); runtime.gate3SetCdDigestV1(trace && trace[0]); }
    struct Patch { uint32_t addr; PS2Runtime::RecompiledFunction fn; };
    static const Patch kPatches[] = {
        { 0x2000c0u, patch_0x2000c0 },
        { 0x294460u, patch_0x294460 },
    };

    for (const auto& p : kPatches) {
        if (!runtime.hasFunction(p.addr)) {
            runtime.registerFunction(p.addr, p.fn);
        }
    }

    // sceSpu2Remote (0x2C5AC0): override the real SIF-RPC body with a host
    // SPU2 register model so the boot-time sound verify clears without an IOP.
    // Must overwrite — the generated body is already registered here.
    // Gate 8: with the host sound driver linked, the game's own sceSpu2Remote
    // marshals real SIF RPCs to it; the echo HLE remains only without it.
    if (!g_gate8AudioHooksV1.sifCallRpc)
        runtime.registerFunction(0x2C5AC0u, patch_0x2c5ac0);

    // Job-table wait 0x298eb0: idle skip in its poll (stage wait-idle).
    g_jobPoll298f28 = runtime.hasFunction(0x298F28u) ? runtime.lookupFunction(0x298F28u) : nullptr;
    if (g_jobPoll298f28)
        runtime.registerFunction(0x298F28u, patch_0x298f28);

    // libvu0 matrix helpers which still need host implementations. MulMatrix's
    // HLE mirrors its generated VMULA/VMADDA sequence explicitly, including
    // per-operation VU round-toward-zero semantics (see vu0_math.cpp).
    // Soft-double libm entry points whose ps2_stubs host ports had the
    // single-precision-FPU ABI. Must overwrite: the generated bodies at these
    // addresses are themselves forwarders into ps2_stubs. See libmSoftDoubleShim.
    runtime.registerFunction(0x2CB4C8u, patch_0x2cb4c8);           // atan2(double,double)
    runtime.registerFunction(0x2CB730u, patch_0x2cb730);           // pow(double,double)
    runtime.registerFunction(0x2CE618u, patch_0x2ce618);           // atan(double)
    runtime.registerFunction(0x2CEA28u, patch_0x2cea28);           // fabs(double)

    runtime.registerFunction(0x2CA220u, patch_0x2ca220);           // sceVu0ApplyMatrix (NaN lanes)
    runtime.registerFunction(0x2CA298u, patch_0x2ca298);           // sceVu0OuterProduct
    runtime.registerFunction(0x2CA2E0u, patch_0x2ca2e0);           // sceVu0Normalize
    runtime.registerFunction(0x2CA250u, patch_0x2ca250);           // sceVu0MulMatrix
    runtime.registerFunction(0x2CA360u, patch_0x2ca360);           // sceVu0InversMatrix
    runtime.registerFunction(0x2CA498u, patch_0x2ca498);           // sceVu0TransMatrix
    // Keep sceVu0RotMatrix (0x2CA7C0) on its generated guest body. Its leaf
    // calls use the game's PS2 trig implementation; host std::sin/std::cos
    // differs by one ULP and corrupts the phase-7 VU1 projection matrix.
    runtime.registerFunction(0x2CA7C0u, patch_0x2ca7c0);
    runtime.registerFunction(0x2CA8E8u, patch_0x2ca8e8);           // sceVu0NormalLightMatrix
    runtime.registerFunction(0x2CA9A0u, patch_0x2ca9a0);           // sceVu0LightColorMatrix

    // The libvu0 *leaves* (RotMatrixX/Y/Z, Normalize, UnitMatrix, ApplyMatrix,
    // TransposeMatrix, CopyMatrix, InnerProduct) stay on their ps2_stubs host
    // ports. Un-stubbing them in config/rrv.toml so the recompiler emits the
    // real ELF bodies was tried on 2026-08-13 and REGRESSED: the guest dies a
    // few dozen MulMatrix calls into phase 7, reproducibly, with every other
    // arm at its default. Reverted; see docs/HANDOFF_PHASE7_MATRIX.md.
    if (vu0RtzEnabled()) {
        // Only the entry points that already have real generated bodies.
        runtime.registerFunction(0x2CA838u, vu0Rtz<sub_002CA838_0x2ca838>); // CameraMatrix
        runtime.registerFunction(0x2CAA08u, vu0Rtz<sub_002CAA08_0x2caa08>); // ViewScreenMatrix
        runtime.registerFunction(0x2CAB10u, vu0Rtz<sub_002CAB10_0x2cab10>); // DropShadowMatrix
    }

    // DMA-hold acquire helper (0x21fdd8): no-op in synchronous-DMA HLE.
    // Must overwrite — the generated body busy-waits on an unmodeled register.
    runtime.registerFunction(0x21FDD8u, patch_0x21fdd8);

    // sceGsResetGraph (0x2c3000) / sceGsPutDispEnv (0x2c33b8): keep PMODE.EN1 set
    // so the presenter shows the rendered frame instead of the magenta clear.
    runtime.registerFunction(0x2C3000u, patch_0x2c3000);
    runtime.registerFunction(0x2C33B8u, patch_0x2c33b8);

    // VBlank-sync wait (0x220100): emulate the missing per-VBlank ISR so the
    // field counter at 0x346E84 advances; otherwise every frame busy-waits the
    // full 33.5M-iter timeout (~0.2 fps). Must overwrite the generated spin.
    runtime.registerFunction(0x220100u, patch_0x220100);

    // Optional A2 queue-root diagnostic. Wrap the real function entry because
    // 0x2204BC is normally reached by a generated C++ goto without returning to
    // the runtime dispatcher.
    const char* girlQueueDiag = std::getenv("RRV_GIRL_QUEUE_DIAG");
    const bool girlQueueDiagEnabled = girlQueueDiag && girlQueueDiag[0] != '\0' &&
        girlQueueDiag[0] != '0';
    const char* girlQueueLightDiag = std::getenv("RRV_GIRL_QUEUE_LIGHT_DIAG");
    const bool girlQueueLightDiagEnabled = girlQueueLightDiag &&
        girlQueueLightDiag[0] != '\0' && girlQueueLightDiag[0] != '0';
    const char* girlChainDiag = std::getenv("RRV_GIRL_CHAIN_DIAG");
    const bool girlChainDiagEnabled = girlChainDiag &&
        girlChainDiag[0] != '\0' && girlChainDiag[0] != '0';
    const char* girlChainOracle = std::getenv("RRV_GIRL_PCSX2_CHAIN_ORACLE");
    const bool girlChainOracleEnabled = girlChainOracle &&
        girlChainOracle[0] != '\0' && girlChainOracle[0] != '0';
    if (girlQueueDiagEnabled || girlQueueLightDiagEnabled) {
        runtime.registerFunction(0x220470u, patch_0x220470_girl_queue_diag);
    }
    if (girlQueueDiagEnabled || girlChainDiagEnabled) {
        runtime.registerFunction(0x21F9C0u, patch_0x21f9c0_girl_ref_diag);
        runtime.registerFunction(0x21FCE8u, patch_0x21fce8_girl_flush_diag);
        runtime.registerFunction(0x21FD98u, patch_0x21fd98_girl_call_diag);
        runtime.registerFunction(0x22D390u, patch_0x22d390_girl_model_list_diag);
    }
    if (girlChainOracleEnabled && !girlQueueDiagEnabled && !girlChainDiagEnabled) {
        runtime.registerFunction(0x21FD98u, patch_0x21fd98_girl_call_diag);
    }
    if (girlQueueDiagEnabled) {
        runtime.registerFunction(0x233240u, patch_0x233240_girl_model_diag);
        runtime.registerFunction(0x22D810u, patch_0x22d810_girl_geometry_diag);
        runtime.registerFunction(0x2C3FF0u, patch_0x2c3ff0_girl_vif0_send_diag);
        runtime.registerFunction(0x2C3F18u, patch_0x2c3f18_girl_spr_chain_diag);
    }

    if (const char* e = std::getenv("RRV_GIRL_ATOMIC_MODEL");
        e && e[0] != '\0' && e[0] != '0') {
        runtime.registerFunction(0x22D390u, patch_0x22d390_girl_atomic);
    }

    if (const char* e = std::getenv("RRV_GIRL_ATOMIC_SCENE");
        e && e[0] != '\0' && e[0] != '0') {
        runtime.registerFunction(0x23E3C8u, patch_0x23e3c8_girl_atomic);
    }

    // Render/stream completion busy-wait (0x29a508): Gate-3 stand-in for the
    // stubbed IPU/stream completion; always registered (no host env input).
    runtime.registerFunction(0x29A508u, patch_0x29a508);

    if (const char* e = std::getenv("RRV_RR5_CAR_LOD"); e && e[0]) {
        const float factor = std::strcmp(e, "best") == 0 ? 1e30f : std::strtof(e, nullptr);
        g_lod226EB0Orig = runtime.hasFunction(0x226EB0u) ? runtime.lookupFunction(0x226EB0u) : nullptr;
        if (g_lod226EB0Orig && factor > 1.0f) {
            g_rr5CarLodFactor = factor;
            runtime.registerFunction(0x226EB0u, patch_0x226eb0_car_lod);
            const char* guard = std::getenv("RRV_RR5_CAR_LOD_GUARD");
            g_carLodGuard = !(guard && guard[0] == '0') && runtime.hasFunction(0x217628u) &&
                            runtime.hasFunction(0x21E658u);
            if (g_carLodGuard) {
                g_lodRace217628Orig = runtime.lookupFunction(0x217628u);
                runtime.registerFunction(0x217628u, patch_0x217628_car_lod_room);
                g_lodDl21E658Orig = runtime.lookupFunction(0x21E658u);
                runtime.registerFunction(0x21E658u, patch_0x21e658_car_lod_room);
            }
            const char* log = std::getenv("RRV_RR5_CAR_LOD_LOG");
            g_carLodLog = log && log[0] && log[0] != '0';
            if (g_carLodLog && runtime.hasFunction(0x220768u)) {
                g_lodField220768Orig = runtime.lookupFunction(0x220768u);
                runtime.registerFunction(0x220768u, patch_0x220768_car_lod_log);
                std::atexit(rrvCarLodReport);
            }
            std::fprintf(stderr, "[rr5-enhance] car LOD: thresholds x%s (table 0x2F5460, capped at cull distance); "
                                 "display list %s\n", e,
                         g_carLodGuard ? "race buffers 0x400000 (func_217628), stock table below 0x180000 left"
                                       : "unguarded (RRV_RR5_CAR_LOD_GUARD=0)");
        }
    }

    if (const char* e = std::getenv("RRV_RR5_DRAW_DISTANCE"); e && e[0]) {
        const long n = std::strtol(e, nullptr, 10);
        g_dd21D0B0Orig = runtime.hasFunction(0x21D0B0u) ? runtime.lookupFunction(0x21D0B0u) : nullptr;
        if (g_dd21D0B0Orig && n >= 1) {
            g_ddSections = static_cast<uint32_t>(std::min(n, 32L));
            const char* log = std::getenv("RRV_RR5_DRAW_DISTANCE_LOG");
            g_ddLog = log && log[0] && log[0] != '0';
            runtime.registerFunction(0x21D0B0u, patch_0x21d0b0_draw_distance);
            std::fprintf(stderr, "[rr5-enhance] draw distance: +%u course sections ahead "
                                 "(visibility lists, func_21D0B0; clipped group)\n", g_ddSections);
        }
    }

    float wsAspect = 0.0f;
    if (const char* e = std::getenv("RRV_RR5_WIDESCREEN"); e && e[0]) {
        if (std::strcmp(e, "1") == 0 || std::strcmp(e, "16:9") == 0) wsAspect = 16.0f / 9.0f;
        else if (std::strcmp(e, "16:10") == 0) wsAspect = 16.0f / 10.0f;
        else if (std::strcmp(e, "21:9") == 0) wsAspect = 21.0f / 9.0f;
        else std::fprintf(stderr, "[rr5-enhance] ignoring RRV_RR5_WIDESCREEN=%s (want 16:9, 16:10 or 21:9)\n", e);
    }
    if (wsAspect > 0.0f) {
        g_wsFactor = (4.0f / 3.0f) / wsAspect;
        bool ok = runtime.hasFunction(0x226EB0u);
        for (uint32_t i = 0; i < kWsReaderCount; ++i) ok = ok && runtime.hasFunction(kWsReaders[i]);
        if (ok) {
            for (uint32_t i = 0; i < kWsReaderCount; ++i) {
                g_wsReaderOrig[i] = runtime.lookupFunction(kWsReaders[i]);
                runtime.registerFunction(kWsReaders[i], kWsReaderPatches[i]);
            }
            g_wsLodOrig = runtime.lookupFunction(0x226EB0u); // after the car LOD wrapper, if any
            runtime.registerFunction(0x226EB0u, patch_0x226eb0_ws_lod);
            std::fprintf(stderr, "[rr5-enhance] widescreen %s: camera p[0] x%.4f, p[2] x%.4f at %u readers "
                                 "(No.47 mechanism); car LOD sees the original p[0]\n",
                         std::getenv("RRV_RR5_WIDESCREEN"), g_wsFactor, 1.0f / g_wsFactor, kWsReaderCount);
            const char* intro = std::getenv("RRV_RR5_WIDESCREEN_INTRO");
            const char* log = std::getenv("RRV_RR5_WIDESCREEN_LOG");
            g_wsLog = log && log[0] && log[0] != '0';
            g_wsIntroPath = !(intro && std::strcmp(intro, "gameplay") == 0) && runtime.hasFunction(0x23E3C8u) &&
                            runtime.hasFunction(0x22CB88u) && runtime.hasFunction(0x23D408u);
            if (g_wsIntroPath) {
                g_wsAttractOrig = runtime.lookupFunction(0x23E3C8u);
                runtime.registerFunction(0x23E3C8u, patch_0x23e3c8_ws_attract);
                g_wsViewScreenOrig = runtime.lookupFunction(0x22CB88u);
                runtime.registerFunction(0x22CB88u, patch_0x22cb88_ws_viewscreen);
                g_wsBarsOrig = runtime.lookupFunction(0x23D408u);
                runtime.registerFunction(0x23D408u, patch_0x23d408_ws_bars);
                std::fprintf(stderr, "[rr5-enhance] widescreen intro: attract camera keeps p[0], p[2] and ay x%.4f, "
                                     "no letterbox (func_23E3C8, func_22CB88, func_23D408)\n", 1.0f / g_wsFactor);
            } else {
                std::fprintf(stderr, "[rr5-enhance] widescreen intro: off, the attract uses the gameplay camera path\n");
            }
        }
    }

    if (const char* e = std::getenv("RRV_RR5_FAST_UNPACK"); e && e[0] && e[0] != '0') {
        g_unpack221D68Orig = runtime.hasFunction(0x221D68u) ? runtime.lookupFunction(0x221D68u) : nullptr;
        if (g_unpack221D68Orig) {
            g_unpackMode = std::strcmp(e, "verify") == 0 ? 2 : 1;
            runtime.registerFunction(0x221D68u, patch_0x221d68_fast_unpack);
            const char* ahead = std::getenv("RRV_RR5_FAST_UNPACK_AHEAD"); // diagnostic: 0 = no worker thread
            if (!(ahead && ahead[0] == '0') && runtime.hasFunction(0x2C7780u)) {
                g_unpackCdReadOrig = runtime.lookupFunction(0x2C7780u);
                runtime.registerFunction(0x2C7780u, patch_0x2c7780_unpack_ahead);
            }
            std::fprintf(stderr, "[rr5-enhance] fast unpack: native func_221D68 (%s)\n",
                         g_unpackMode == 2 ? "verify against the original" : "64 + bytes/16 cycles");
            static const bool atExit = [] { std::atexit([] {
                std::fprintf(stderr, "[rr5-enhance] fast unpack: calls=%llu mismatches=%llu fallbacks=%llu "
                                     "host_ms=%.1f host_max_ms=%.1f commit_ms=%.1f out_mb=%.1f ahead=%llu/%llu\n",
                             (unsigned long long)g_unpackCalls, (unsigned long long)g_unpackBad,
                             (unsigned long long)g_unpackFallback, g_unpackHostNs / 1e6, g_unpackHostMaxNs / 1e6, g_unpackCommitNs / 1e6,
                             g_unpackBytes / 1048576.0, (unsigned long long)g_unpackJobsUsed,
                             (unsigned long long)g_unpackJobsStarted); }); return true; }();
            (void)atExit;
        }
    }

    if (const char* e = std::getenv("RRV_RR5_NATIVE_HOT"); e && e[0] && e[0] != '0')
        rrv_native::registerAll(runtime);

    if (const char* e = std::getenv("RRV_P39_EULER"); e && e[0] && e[0] != '0') {
        g_euler207438Orig = runtime.hasFunction(0x207438u) ? runtime.lookupFunction(0x207438u) : nullptr;
        runtime.registerFunction(0x207438u, patch_0x207438_euler);
    }

    // Library-import stubs the recompiler did not emit bodies for (rrv.toml
    // `untracked_stubs`). Without these, the first such call (e.g.
    // scePadSetActDirect@0x2C68D8 on the render/input path) hits an unmapped PC
    // and kills the thread. Bind each to its ps2_stubs:: handler, but only when
    // (a) no generated body already exists and (b) the name actually resolves —
    // so real code (compiler-rt, etc.) is never masked. See rrv_stub_addrs.inc.
    if (const char* spec = std::getenv("RRV_FN_HITS"); spec && spec[0]) {
        // RRV_FN_HITS=all arms EVERY registered entry point. That is the
        // apples-to-apples counterpart of the scratch PCSX2 build's
        // RRV_GT_BLOCKS=1 block-entry profiler: diffing the two executed sets
        // for one scene names the guest code one engine runs and the other
        // does not, without having to guess a candidate list first.
        if (std::strcmp(spec, "all") == 0) {
            const std::vector<uint32_t> addrs = runtime.allFunctionAddresses();
            for (const uint32_t addr : addrs) {
                if (!addr) continue;
                rrvFnHitOrig()[addr] = runtime.lookupFunction(addr);
                (void)rrvFnHitCount()[addr];
                runtime.registerFunction(addr, rrvFnHitTrampoline);
            }
            std::fprintf(stderr, "[fn-hits] armed ALL %zu functions\n", addrs.size());
        }
        for (const char* p = (std::strcmp(spec, "all") == 0) ? "" : spec; *p;) {
            char* end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p) break;
            if (addr) {
                rrvFnHitOrig()[addr] = runtime.hasFunction(addr) ? runtime.lookupFunction(addr) : nullptr;
                (void)rrvFnHitCount()[addr];
                runtime.registerFunction(addr, rrvFnHitTrampoline);
                std::fprintf(stderr, "[fn-hits] armed %08x (orig=%s)\n", addr,
                             rrvFnHitOrig()[addr] ? "yes" : "none");
            }
            p = (*end == ',') ? end + 1 : end;
        }
    }

    struct StubBind { uint32_t addr; const char* name; };
    static const StubBind kStubBinds[] = {
#include "rrv_stub_addrs.inc"
    };
    for (const auto& s : kStubBinds) {
        if (runtime.hasFunction(s.addr)) {
            continue; // generated body or an explicit override above wins
        }
        if (auto fn = rrvResolveStub(s.name)) {
            rrvStubMap()[s.addr] = {fn, s.name};
            runtime.registerFunction(s.addr, rrvStubTrampoline);
        }
    }
}
