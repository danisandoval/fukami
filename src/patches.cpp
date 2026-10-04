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
static void patch_0x2000c0(uint8_t*, R5900Context*, PS2Runtime*) {}

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
struct ScopedVu0Rounding {
    const int previous = std::fegetround();
    ScopedVu0Rounding() { std::fesetround(FE_TOWARDZERO); }
    ~ScopedVu0Rounding() { std::fesetround(previous); }
};

template <void (*Generated)(uint8_t*, R5900Context*, PS2Runtime*),
          void (*Hle)(uint8_t*, R5900Context*, PS2Runtime*)>
static void vu0MatrixOp(const char* name, uint8_t* rdram, R5900Context* ctx,
                        PS2Runtime* runtime) {
    if (!vu0Generated(name)) {
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
    else
        rrv_hle::vu0Normalize(rdram, ctx, runtime);
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
    else
        rrv_hle::vu0OuterProduct(rdram, ctx, runtime);
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
    vu0MatrixOp<sub_002CA360_0x2ca360, rrv_hle::vu0InversMatrix>("invers", rdram, ctx, runtime);
    if (ra == 0x2CA8C8u && ctx->pc == ra)
        rrv::guesttrace::inversMatrix(rdram, ctx, "out");
}
static void patch_0x2ca498(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA498_0x2ca498, rrv_hle::vu0TransMatrix>("trans", rdram, ctx, runtime);
}
static void patch_0x2ca8e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA8E8_0x2ca8e8, rrv_hle::vu0NormalLightMatrix>("normallight", rdram, ctx, runtime);
}
static void patch_0x2ca9a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    vu0MatrixOp<sub_002CA9A0_0x2ca9a0, rrv_hle::vu0LightColorMatrix>("lightcolor", rdram, ctx, runtime);
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
    if (libmFloatAbi())
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
        rrv_hle::vu0MulMatrixRtz(rdram, ctx, runtime);
        break;
    case P7MulImpl::Generated: {
        const ScopedVu0Rounding rounding;
        sub_002CA250_0x2ca250(rdram, ctx, runtime);
        break;
    }
    case P7MulImpl::Hle:
    default:
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
static void patch_0x21fdd8(uint8_t*, R5900Context* ctx, PS2Runtime*) {
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
    ps2_stubs::sceGsPutDispEnv(rdram, ctx, runtime);
    if (runtime) {
        rrvEnablePmodeCrt(*runtime);
    }
    ctx->pc = GPR_U32(ctx, 31); // jr $ra
}

// 0x220100 — per-frame VBlank-sync wait (sub_00220100).  Generated body:
//   sceGsSyncPath(); v1 = 0x2000000;
//   do { v0 = [0x346E84]; if (v0 >= 5) break; } while (--v1 > 0);
// i.e. it busy-waits up to 33.5M iterations for the VBlank field counter at
// guest RAM 0x346E84 to reach 5.  On hardware that counter is bumped once per
// VBlank by the game's INTC VBlank handler; under our HLE that ISR never fires
// (it isn't registered through AddIntcHandler), so [0x346E84] — reset to {0,2,4}
// each frame by sub_00220470 — never climbs, and every frame exhausts the full
// timeout (~5 s of spin, observed as pc pinned at 0x220120, ~0.2 fps).
//
// Emulate the missing ISR: pace on the runtime's real VBlank worker
// (WaitForNextVSyncTick) and advance the guest counter one step per tick until
// the game's >=5 condition holds.  This both unblocks boot and gives correct
// ~60 Hz frame pacing (the game asks for 5-N more fields; N = its reset value).
static void patch_0x220100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ps2_stubs::sceGsSyncPath(rdram, ctx, runtime); // GIF/GS path idle (no-op: sync DMA)

    constexpr uint32_t kFieldCounter = 0x346E84u; // guest RAM: VBlank field counter

    // On hardware the field counter is bumped by the VBlank ISR *continuously* —
    // including while the frame's own rendering/compute runs — so by the time the
    // game reaches this wait the counter has usually already climbed most of the
    // way to its target, and the wait is short. Our HLE has no registered ISR, so
    // first CREDIT the counter with however many real VBlank ticks elapsed since
    // the previous frame (the ticks that passed during this frame's compute). Only
    // then block for any remaining fields. This removes the artificial 3-waits-
    // per-frame serialization (the counter no longer advances *only* inside this
    // loop) and matches HW pacing: a slow frame that already overran its field
    // budget waits zero, a fast frame still blocks to the target.
    static uint64_t s_lastRealTick = ps2_syscalls::GetCurrentVSyncTick();
    const uint64_t nowTick = ps2_syscalls::GetCurrentVSyncTick();
    const uint32_t elapsed = static_cast<uint32_t>(nowTick - s_lastRealTick);
    s_lastRealTick = nowTick;

    // RRV_FRAME_BARRIER_DIAG — default-off, log-only. Records the counter value
    // seen on ENTRY, before this patch credits or fakes anything, plus how many
    // fields the wait loop below actually blocked for.
    //
    // It was added to test this patch's premise, and it FALSIFIED it (measured
    // 2026-08-02, docs/HANDOFF_GIRL_TIMING_FRAMING.md §1.2). Across scene phases
    // 2-8, 100% of frames enter with [0x346E84] already >= 5 and the loop below
    // blocks ZERO times — the emulated pacing is inert on the current build.
    // [0x346E84] is also not a VBlank field counter: it is the game's own
    // display-list STAGE counter, advanced by the DMA/GIF completion handler
    // sub_0021FE88, which reads it, indexes a jump table on it (`sll $v1, $s1, 2`
    // at 0x21ff44) and stores back the literals 1..5 (0x21ff68/78/9c/b4/c8 ->
    // 0x21ffd0); sub_00220470 seeds it (0x22059c) with the selected queue
    // variant's start stage. `>= 5` therefore means "this frame's chain sequence
    // completed", a completion barrier rather than a sleep.
    //
    // Retiring the credit+sleep is deliberately NOT done here: it is milestone T4
    // in the handoff and needs its own boot/logo/flyover/demo-race regression pass.
    // Keep this probe as the gate for that change. Metadata only.
    static const bool frameBarrierDiag = [] {
        const char* value = std::getenv("RRV_FRAME_BARRIER_DIAG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    const uint32_t barrierOnEntry = frameBarrierDiag ? READ32(kFieldCounter) : 0u;

    // Credit VBlanks that elapsed while the guest was rendering.
    if (elapsed != 0u) {
        WRITE32(kFieldCounter, ADD32(READ32(kFieldCounter), elapsed));
    }

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
    // Cap the number of real fields we'll block so a stuck/!=reset counter can
    // never hang the game thread; 8 covers the worst legitimate case (reset 0).
    int barrierWaits = 0;
    for (int i = 0; i < 8; ++i) {
        if (READ32(kFieldCounter) >= 5u) break;
        if (runtime && runtime->isStopRequested()) break;
        ps2_syscalls::WaitForNextVSyncTick(rdram, runtime); // ~1 field, releases guest exec
        s_lastRealTick = ps2_syscalls::GetCurrentVSyncTick();
        WRITE32(kFieldCounter, ADD32(READ32(kFieldCounter), 1u)); // emulate ISR increment
        ++barrierWaits;
    }

    if (frameBarrierDiag) {
        // Per-second summary, bucketed by the value seen on entry. Bucket 5 also
        // absorbs anything above it, so "already satisfied" is one number.
        static uint64_t entryBuckets[7]{};
        static uint64_t waitBuckets[9]{};
        static uint64_t frames = 0u;
        static uint32_t lastPhase = UINT32_MAX;
        static auto lastReport = std::chrono::steady_clock::now();
        ++frames;
        ++entryBuckets[barrierOnEntry <= 5u ? barrierOnEntry : 6u];
        ++waitBuckets[barrierWaits < 9 ? barrierWaits : 8];
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count();
        if (ms >= 1000 || scenePhase != lastPhase) {
            std::fprintf(stderr,
                         "[frame-barrier] phase=%u wall=%.3f frames=%llu build_hz=%.2f "
                         "entry[0|1|2|3|4|5|>5]=%llu %llu %llu %llu %llu %llu %llu "
                         "waits[0..8]=%llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
                         scenePhase, static_cast<double>(ms) / 1000.0,
                         static_cast<unsigned long long>(frames),
                         ms > 0 ? static_cast<double>(frames) * 1000.0 / static_cast<double>(ms) : 0.0,
                         (unsigned long long)entryBuckets[0], (unsigned long long)entryBuckets[1],
                         (unsigned long long)entryBuckets[2], (unsigned long long)entryBuckets[3],
                         (unsigned long long)entryBuckets[4], (unsigned long long)entryBuckets[5],
                         (unsigned long long)entryBuckets[6],
                         (unsigned long long)waitBuckets[0], (unsigned long long)waitBuckets[1],
                         (unsigned long long)waitBuckets[2], (unsigned long long)waitBuckets[3],
                         (unsigned long long)waitBuckets[4], (unsigned long long)waitBuckets[5],
                         (unsigned long long)waitBuckets[6], (unsigned long long)waitBuckets[7],
                         (unsigned long long)waitBuckets[8]);
            lastReport = now;
            lastPhase = scenePhase;
            frames = 0u;
            std::memset(entryBuckets, 0, sizeof(entryBuckets));
            std::memset(waitBuckets, 0, sizeof(waitBuckets));
        }
    }

    ctx->pc = GPR_U32(ctx, 31); // jr $ra
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

// address -> resolved ps2_stubs handler (populated in registerPatches()).
static std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction>& rrvStubMap() {
    static std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> m;
    return m;
}

static void rrvStubTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    auto& m = rrvStubMap();
    if (auto it = m.find(entry); it != m.end() && it->second) {
        it->second(rdram, ctx, runtime);
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

void registerPatches(PS2Runtime& runtime)
{
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
    runtime.registerFunction(0x2C5AC0u, rrv_hle::spu2Remote_hle);

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

    // Render/stream completion busy-wait (0x29a508): emulate the EE-Timer0
    // timeout our runtime never advances, so the attract loop no longer hard-
    // hangs at demo phase 0x27 (the FMV/stream restart segment whose IPU decode
    // is HLE-stubbed and never marks its job "done"). Default on; set
    // RRV_FMV_COMPLETE_HLE=0 to reproduce the stock hang for debugging.
    if (const char* e = std::getenv("RRV_FMV_COMPLETE_HLE"); !(e && e[0] == '0')) {
        runtime.registerFunction(0x29A508u, patch_0x29a508);
    }

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
            rrvStubMap()[s.addr] = fn;
            runtime.registerFunction(s.addr, rrvStubTrampoline);
        }
    }
}
