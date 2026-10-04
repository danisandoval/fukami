// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Contains logic adapted from PCSX2 (GPL-3.0+); the upstream revision, file and function of each adaptation
// are cited at the adapted code and in third_party/ps2recomp/RRV_CHANGES.md. Rest: PS2Recomp (GPL-3.0).
#include "ps2_syscalls.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/rrv_fp_rounding.h"
#include "ps2_log.h"
#include "runtime/diag_counters.h"
#include "rrv_cadence_diag.h" // P0-cadence (C0) frame-budget probe; default off
#include <atomic>
#include <chrono>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <map>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <algorithm>

namespace
{
    // DIAG-TEMP (T-P7-VUIN) — VU1 microprogram input trace, default off.
    //
    // Mirrors RRVVu1Trace in the patched PCSX2 reference build
    // (tools/patches/pcsx2-vu1-input-trace.patch) byte for byte, so the two
    // engines' dumps of the SAME invocation can be diffed directly:
    //   float vf[32][4]  int32 vi[16]  float q  float i  u32 mac/status/clip
    //   uint8 mem[dataSize]                              = 16,980 B for VU1.
    //
    //   RRV_VU_INTRACE=<hex entry PC in BYTES>   enable + select microprogram
    //   RRV_VU_INTRACE_PHASE=7                   guest scene phase ([0x334E94])
    //   RRV_VU_INTRACE_NLOOP / _PRIM / _NREG      GIFtag that identifies the kick
    //   RRV_VU_INTRACE_N / _DIR / _LOG
    struct VuInTrace
    {
        bool enabled = false;
        uint32_t entry = 0xFFFFFFFFu;
        int32_t phase = 7;
        uint32_t nloop = 101u;
        uint32_t prim = 0x4cu;
        uint32_t nreg = 0u;   // 0 = any; a program whose first kick is a bare
                              // A+D register write needs this to reach geometry
        uint32_t max = 8u;
        uint32_t written = 0u;
        uint32_t seq = 0u;
        std::string dir = "/tmp";
        FILE *log = nullptr;

        bool pending = false;
        uint32_t pendingSeq = 0u;
        uint32_t pendTop = 0u, pendItop = 0u;
        float vf[32][4]{};
        int32_t vi[16]{};
        float q = 0.0f;
        float i = 0.0f;
        uint32_t mac = 0u, status = 0u, clip = 0u;
        std::vector<uint8_t> mem;
        std::vector<uint8_t> micro;
    };

    uint32_t vuInTraceEnvU32(const char *name, uint32_t fallback)
    {
        const char *v = std::getenv(name);
        return (v && v[0]) ? (uint32_t)std::strtoul(v, nullptr, 0) : fallback;
    }

    VuInTrace &rrvVuInTrace()
    {
        static VuInTrace t = [] {
            VuInTrace s;
            const char *v = std::getenv("RRV_VU_INTRACE");
            if (!v || !v[0])
                return s;
            s.enabled = true;
            s.entry = (uint32_t)std::strtoul(v, nullptr, 0);
            s.phase = (int32_t)vuInTraceEnvU32("RRV_VU_INTRACE_PHASE", 7u);
            s.nloop = vuInTraceEnvU32("RRV_VU_INTRACE_NLOOP", 101u);
            s.prim = vuInTraceEnvU32("RRV_VU_INTRACE_PRIM", 0x4cu);
            s.nreg = vuInTraceEnvU32("RRV_VU_INTRACE_NREG", 0u);
            s.max = vuInTraceEnvU32("RRV_VU_INTRACE_N", 8u);
            if (const char *d = std::getenv("RRV_VU_INTRACE_DIR"); d && d[0])
                s.dir = d;
            if (const char *l = std::getenv("RRV_VU_INTRACE_LOG"); l && l[0] && l[0] != '0')
            {
                s.log = (std::strcmp(l, "1") == 0) ? stderr : std::fopen(l, "w");
                if (s.log)
                    std::setvbuf(s.log, nullptr, _IOLBF, 0);
            }
            return s;
        }();
        return t;
    }


    // DIAG-TEMP (T-P7-VUSTEP) — per-instruction step trace, default off.
    //
    // RRV_VU_STEPTRACE=<file> writes one line per executed instruction for the
    // microprogram selected by RRV_VU_INTRACE, listing only the architectural
    // state that CHANGED since the previous instruction. The patched PCSX2
    // reference build emits byte-identical text (RRV_VU1_STEPTRACE,
    // tools/patches/pcsx2-vu1-input-trace.patch), so the two files diff directly
    // and the first differing line is the first cross-engine execution
    // divergence. Pair it with the input injector so both engines run the SAME
    // input and camera phase cannot enter the comparison.
    struct VuStepTrace
    {
        FILE *out = nullptr;
        bool active = false;
        bool used = false;
        uint32_t n = 0;
        bool havePrev = false;
        uint32_t vf[32][4]{};
        uint32_t vi[16]{};
        uint32_t acc[4]{};
        uint32_t q = 0, i = 0, p = 0, mac = 0, status = 0, clip = 0;
        std::vector<uint8_t> mem;
    };

    VuStepTrace &rrvVuStep()
    {
        static VuStepTrace t = [] {
            VuStepTrace s;
            const char *v = std::getenv("RRV_VU_STEPTRACE");
            if (v && v[0] && v[0] != '0')
            {
                s.out = std::fopen(v, "w");
                if (s.out)
                    std::setvbuf(s.out, nullptr, _IOFBF, 1 << 20);
            }
            return s;
        }();
        return t;
    }

    // VU arithmetic rounds toward zero (PCSX2 ChopZero), scoped to the call. The scope itself lives in
    // runtime/rrv_fp_rounding.h: direct FPCR (aarch64) or MXCSR (x86-64) bits, not std::fesetround.
    using ScopedVuRounding = rrv::fp::ScopedRoundTowardZero;
    thread_local int32_t s_p7TraceRun = -1;
    std::atomic<uint32_t> s_debugVu1XgkickCount{0};

    // RRV_VU1_DIAG: env-gated geometry-gap diagnostic. Attributes VU1 activity to
    // the mscal start-PC that launched it, so we can tell whether a scene's missing
    // object batches are (a) never dispatched (few mscals), or (b) dispatched but
    // produce no XGKICK geometry (empty runs). Zero cost unless RRV_VU1_DIAG is set.
    struct Vu1Diag
    {
        bool enabled = false;
        std::mutex mtx;
        std::unordered_map<uint32_t, uint64_t> mscalByPc;   // mscal launches per startPC
        std::unordered_map<uint32_t, uint64_t> emptyByPc;   // launches that emitted 0 XGKICK
        std::unordered_map<uint32_t, uint64_t> xgkickByPc;  // XGKICK submissions per xgkickPc
        // B-1: same histograms restricted to when the demo car slot 0x334E98 is
        // live. If the car-live set == the environment-only set, the car is NOT
        // dispatched through a distinct VU1 program (⇒ it is PATH3 data-driven,
        // shares an environment program, or is not submitted at all).
        std::unordered_map<uint32_t, uint64_t> mscalByPcCar;
        std::unordered_map<uint32_t, uint64_t> xgkickByPcCar;
        uint64_t mscalTotalCar = 0;
        uint64_t xgkickSkipOverflow = 0;                    // XGKICK dropped: tag pktSize > dataSize
        uint64_t xgkickSkipEmpty = 0;                       // XGKICK dropped: totalBytes == 0
        uint64_t mscalTotal = 0;
        // Stage-1 sprite-reachability probe: count instructions whose PC lands in the
        // loop bodies immediately preceding the never-executed sprite XGKICKs (0x9f0 /
        // 0x19e8). >0 => execution DOES approach them but branches away (Class B).
        // ==0 over the whole scene => never reached at runtime (needs a dedicated
        // mscal our EE never issues -> Class A).
        std::atomic<uint64_t> approach9f0{0};
        std::atomic<uint64_t> approach19e8{0};
        std::chrono::steady_clock::time_point last;
    };
    Vu1Diag g_vu1Diag;
    std::atomic<uint32_t> s_curRunXgkicks{0};
    // Car-live latch for the CURRENT mscal run (sampled at execMicroProgram
    // entry, stable for the whole run incl. its XGKICKs). B-1.
    thread_local bool s_curRunCarLive = false;

    struct CarDmaVu1Scope
    {
        bool active = false;
        uint64_t chainId = 0u;
        uint32_t vifByteOffset = 0u;
        ps2_diag::CarDmaVu1Result result;
    };
    thread_local CarDmaVu1Scope s_carDmaVu1Scope;

    void vu1DiagInit()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            const char *v = std::getenv("RRV_VU1_DIAG");
            g_vu1Diag.enabled = (v && v[0] && v[0] != '0');
            g_vu1Diag.last = std::chrono::steady_clock::now();
        });
    }

    void vu1DiagDumpLocked()
    {
        auto histLine = [](const char *label, std::unordered_map<uint32_t, uint64_t> &m) {
            std::vector<std::pair<uint32_t, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first < b.first; });
            fprintf(stderr, "[vu1diag] %s:", label);
            for (auto &p : v)
                fprintf(stderr, " 0x%x=%llu", p.first, (unsigned long long)p.second);
            fprintf(stderr, "\n");
        };
        fprintf(stderr, "[vu1diag] ---- mscalTotal=%llu xgkickSkipOverflow=%llu xgkickSkipEmpty=%llu approach9f0=%llu approach19e8=%llu ----\n",
                (unsigned long long)g_vu1Diag.mscalTotal,
                (unsigned long long)g_vu1Diag.xgkickSkipOverflow,
                (unsigned long long)g_vu1Diag.xgkickSkipEmpty,
                (unsigned long long)g_vu1Diag.approach9f0.load(std::memory_order_relaxed),
                (unsigned long long)g_vu1Diag.approach19e8.load(std::memory_order_relaxed));
        histLine("mscalByPc ", g_vu1Diag.mscalByPc);
        histLine("emptyByPc ", g_vu1Diag.emptyByPc);
        histLine("xgkickByPc", g_vu1Diag.xgkickByPc);
        fprintf(stderr, "[vu1diag] ==== CAR-LIVE only: mscalTotalCar=%llu ====\n",
                (unsigned long long)g_vu1Diag.mscalTotalCar);
        histLine("mscalByPcCar ", g_vu1Diag.mscalByPcCar);
        histLine("xgkickByPcCar", g_vu1Diag.xgkickByPcCar);
    }

    // RRV_VU_FLAG_PIPELINE (B-5, DEFAULT ON since the white-ST gate passed:
    // Pass B S/T MAD 33114/66742 -> 0.00014/0.00055 vs PCSX2 ground truth,
    // 60s regression smoke clean): 4-slot delayed MAC/status/clip visibility
    // for lower-pipe flag readers, matching hardware flag latency. Set
    // RRV_VU_FLAG_PIPELINE=0 to fall back to instant flags. See
    // docs/HANDOFF_BOOT_NAMCO_WHITE.md.
    // RRV_VU_MAC_EXACT (DEFAULT ON since 2026-08-20): PCSX2's VU_MAC_UPDATE
    // (VUflags.cpp, d5f75c9e4) on every FMAC result -- the full O/U/S/Z MAC word,
    // a denormal result flushed to signed zero, and an exp==255 result clamped to
    // +/-0x7f7fffff. The VU has no infinities; PCSX2 runs this on VU1 by default
    // (vu1Overflow = true, Pcsx2Config.cpp:463). Before this we stored the raw
    // IEEE result and left U/O at 0, so NaN/Inf entered vf and propagated -- that
    // is how a NaN reached CLIP in T-P7-CLIPFIX. RRV_VU_MAC_EXACT=0 restores the
    // old Z/S-only, no-sanitise behaviour.
    // DIAG-TEMP (T-P7-CLIPWHY, default off): RRV_VU_CLIP_DIAG=<N> computes BOTH
    // CLIP forms on every CLIP and logs the first N disagreements with the raw
    // operand bit patterns, so "why does the integer form matter" is measured
    // rather than argued. Costs one extra compare per CLIP when unset.
    uint32_t vuClipDiagLimit()
    {
        static const uint32_t limit = []() -> uint32_t {
            const char *v = std::getenv("RRV_VU_CLIP_DIAG");
            return (v && v[0]) ? (uint32_t)std::strtoul(v, nullptr, 10) : 0u;
        }();
        return limit;
    }

    bool vuMacExactEnabled()
    {
        static const bool enabled = []() {
            const char *v = std::getenv("RRV_VU_MAC_EXACT");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return enabled;
    }

    bool vuFlagPipelineEnabled()
    {
        static const bool enabled = []() {
            const char *v = std::getenv("RRV_VU_FLAG_PIPELINE");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return enabled;
    }

    // RRV_VU0_FMAND_DIAG (B-5 step-1 confirmation probe, default off): logs the
    // instant vs. 4-slots-ago MAC flags at the boot VU0 FMAND (byte PC 0x2D0)
    // that gates the white Namco animation's front/side ST path. Diagnostic
    // only; does not change interpreter behavior by itself. Also causes the
    // flag-history ring to be maintained even when RRV_VU_FLAG_PIPELINE=0, so
    // the probe can observe real delayed values before the behavior change is
    // enabled.
    bool vu0FmandDiagEnabled()
    {
        static const bool enabled = []() {
            const char *v = std::getenv("RRV_VU0_FMAND_DIAG");
            return v && v[0] && v[0] != '0';
        }();
        return enabled;
    }

    // RRV_VU_Q_LATENCY (DEFAULT OFF, opt-in) — model the FDIV pipe's latency
    // before a DIV/SQRT/RSQRT result becomes VISIBLE in Q. Provenance: PCSX2
    // d5f75c9e4, pcsx2/VUops.cpp — _vuFDIVAdd() stages the result with
    // sCycle/Cycle (7 cycles for DIV and SQRT, 13 for RSQRT), _vuFDIVflush()
    // commits it once `cycle - sCycle >= Cycle`, WAITQ and _vuFlushAll() drain
    // it early. Writing Q at the DIV, as this file did before, is only
    // equivalent for microprograms that do NOT software-pipeline the
    // perspective divide.
    //
    // Default OFF for two measured reasons, not for doubt about the semantics:
    //   * it is NOT causal for the phase-39 race defect (docs/KNOWN_ISSUES.md
    //     item 17): on-screen-vertex fraction 1.1% with it, 1.1% without, and
    //     the projected-coordinate distribution barely moves;
    //   * enabling it forces interpretation, because the ARM64 recompiler
    //     writes Q at the DIV and has no FDIV pipe of its own.
    // Turn it on with RRV_VU_Q_LATENCY=1 when investigating a Q-dependent
    // divergence, or make it default once the recompiler models the pipe too.
    bool vuQLatencyEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("RRV_VU_Q_LATENCY");
            return v && v[0] && v[0] != '0';
        }();
        return on;
    }

    // RRV_VU_FASTPATH (DEFAULT ON) — execute the upper pipe from the decoded
    // slot IR with rrv::simd, instead of re-decoding and computing lane by
    // lane.  RRV_VU_FASTPATH=0 is the rollback to the scalar reference path,
    // which stays in this file unchanged.
    bool vuFastPathEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("RRV_VU_FASTPATH");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return on;
    }

    // RRV_VU_VERIFY=1 — run BOTH upper-pipe implementations on every
    // instruction and compare the architectural state they produce.  The
    // acceptance gate for the fast path (docs/TESTING.md T-VU-FASTPATH); ~3x
    // slower, so it is a verification build knob, not a shipping one.
    bool vuVerifyEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("RRV_VU_VERIFY");
            return v && v[0] && v[0] != '0';
        }();
        return on;
    }

    // Branch hints for the hot loop: the diagnostics below are compiled in but
    // armed only by env vars, so the common case is "none of them".
#if defined(__GNUC__) || defined(__clang__)
#  define RRV_VU_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define RRV_VU_UNLIKELY(x) (x)
#endif
}

void ps2_diag::beginCarDmaVu1Scope(uint64_t chainId, uint32_t vifByteOffset)
{
    s_carDmaVu1Scope = {};
    s_carDmaVu1Scope.active = true;
    s_carDmaVu1Scope.chainId = chainId;
    s_carDmaVu1Scope.vifByteOffset = vifByteOffset;
}

ps2_diag::CarDmaVu1Result ps2_diag::endCarDmaVu1Scope()
{
    const CarDmaVu1Result result = s_carDmaVu1Scope.result;
    s_carDmaVu1Scope = {};
    return result;
}

// Defined further down with the FMAC helpers; run() resolves it once per call.
static inline bool vuDenormFlushEnabled();
static inline bool vuOpClampEnabled();
// Same.
static inline bool vuClipExactEnabled();
static inline bool vuDivExactEnabled();

// Instruction field extraction helpers
static inline uint8_t DEST(uint32_t i) { return (uint8_t)((i >> 21) & 0xF); }
static inline uint8_t FT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t FS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t FD(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline uint8_t BC(uint32_t i) { return (uint8_t)(i & 0x3); }

// Lower instruction field helpers
static inline uint8_t LIT(uint32_t i) { return (uint8_t)((i >> 16) & 0x1F); }
static inline uint8_t LIS(uint32_t i) { return (uint8_t)((i >> 11) & 0x1F); }
static inline uint8_t LID(uint32_t i) { return (uint8_t)((i >> 6) & 0x1F); }
static inline int16_t IMM11(uint32_t i) { return (int16_t)(int32_t)((int32_t)(i << 21) >> 21); }
static inline int16_t IMM15(uint32_t i)
{
    uint32_t lo11 = i & 0x7FF;
    uint32_t hi4 = (i >> 21) & 0xF;
    uint32_t raw = (hi4 << 11) | lo11;
    return (int16_t)(int32_t)((int32_t)(raw << 17) >> 17);
}

VU1Interpreter::VU1Interpreter()
{
    reset();
}

VU1Interpreter::~VU1Interpreter()
{
    progReport("total");
}

// RRV_VU_AOT (statically recompiled microcode) is defined at the end of
// this file, in rrv_vu_aot_engine.inc (scripts/vu_aot_overlay.py).


// RRV_VU_AOT_VERIFY=1 — the acceptance gate for the statically recompiled
// microcode. One microprogram invocation is executed TWICE from an identical
// snapshot: first by the interpreter, whose results the game keeps and whose
// GIF packets are the ones actually submitted; then by the recompiler with its
// side effects suppressed. Everything the VU can observe afterwards — the whole
// of VU1State and the whole of VU data memory — is compared.
//
// This is deliberately coarser than the per-slot RRV_VU_VERIFY: it catches a
// divergence anywhere in the program including one that only shows up through
// memory, and it costs three 16 KB copies and a second execution per mscal,
// which is a verification-build price and not a shipping one.
void VU1Interpreter::runVerified(uint8_t *vuCode, uint32_t codeSize,
                                 uint8_t *vuData, uint32_t dataSize,
                                 GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    static uint64_t s_invocations = 0;
    static uint64_t s_mismatches = 0;
    static uint32_t s_reported = 0;

    const VU1State state0 = m_state;
    FlagSnapshot ring0[4];
    std::memcpy(ring0, m_flagRing, sizeof(ring0));
    const int ringPos0 = m_flagRingPos;
    const FlagSnapshot visible0 = m_flagVisible;
    std::vector<uint8_t> data0(vuData, vuData + dataSize);

    // Pass 1 — the oracle. Side effects are real; this is the run the game sees.
    m_aotForceInterp = true;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    m_aotForceInterp = false;

    const VU1State state1 = m_state;
    FlagSnapshot ring1[4];
    std::memcpy(ring1, m_flagRing, sizeof(ring1));
    const int ringPos1 = m_flagRingPos;
    const FlagSnapshot visible1 = m_flagVisible;
    std::vector<uint8_t> data1(vuData, vuData + dataSize);

    // Rewind and run the same invocation through the recompiler.
    m_state = state0;
    std::memcpy(m_flagRing, ring0, sizeof(ring0));
    m_flagRingPos = ringPos0;
    m_flagVisible = visible0;
    std::memcpy(vuData, data0.data(), dataSize);

    m_suppressSideEffects = true;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    m_suppressSideEffects = false;

    ++s_invocations;
    const bool sameState = std::memcmp(&m_state, &state1, sizeof(VU1State)) == 0;
    const bool sameData = std::memcmp(vuData, data1.data(), dataSize) == 0;
    if (!sameState || !sameData)
    {
        ++s_mismatches;
        if (s_reported < 16u)
        {
            ++s_reported;
            std::fprintf(stderr,
                         "[vu-aot-verify] MISMATCH #%llu entry=%04x (state=%s data=%s)\n",
                         (unsigned long long)s_mismatches, m_entryPC,
                         sameState ? "ok" : "DIFF", sameData ? "ok" : "DIFF");
            for (uint32_t r = 0; r < 32u; ++r)
                if (std::memcmp(m_state.vf[r], state1.vf[r], 16) != 0)
                {
                    const auto *g = reinterpret_cast<const uint32_t *>(m_state.vf[r]);
                    const auto *e = reinterpret_cast<const uint32_t *>(state1.vf[r]);
                    std::fprintf(stderr,
                                 "[vu-aot-verify]   vf%02u jit=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                                 r, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
                }
            for (uint32_t r = 0; r < 16u; ++r)
                if (m_state.vi[r] != state1.vi[r])
                    std::fprintf(stderr, "[vu-aot-verify]   vi%02u jit=%08x ref=%08x\n",
                                 r, (uint32_t)m_state.vi[r], (uint32_t)state1.vi[r]);
            if (m_state.pc != state1.pc)
                std::fprintf(stderr, "[vu-aot-verify]   pc jit=%04x ref=%04x\n",
                             m_state.pc, state1.pc);
            if (m_state.mac != state1.mac || m_state.status != state1.status ||
                m_state.clip != state1.clip)
                std::fprintf(stderr,
                             "[vu-aot-verify]   flags jit mac=%08x status=%08x clip=%06x | "
                             "ref mac=%08x status=%08x clip=%06x\n",
                             m_state.mac, m_state.status, m_state.clip,
                             state1.mac, state1.status, state1.clip);
            for (uint32_t o = 0; o + 16u <= dataSize; o += 16u)
                if (std::memcmp(vuData + o, data1.data() + o, 16) != 0)
                {
                    const auto *g = reinterpret_cast<const uint32_t *>(vuData + o);
                    const auto *e = reinterpret_cast<const uint32_t *>(data1.data() + o);
                    std::fprintf(stderr,
                                 "[vu-aot-verify]   mem[%04x] jit=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                                 o, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
                    break;
                }
        }
    }
    if ((s_invocations % 100000ull) == 0ull)
    {
        std::fprintf(stderr,
                     "[vu-aot-verify] %s: %llu invocations, %llu mismatches; "
                     "%llu M slots by compiled blocks, %llu M interpreted\n",
                     m_isVu0 ? "vu0" : "vu1",
                     (unsigned long long)s_invocations, (unsigned long long)s_mismatches,
                     (unsigned long long)(m_aotSlots >> 20),
                     (unsigned long long)(m_interpSlots >> 20));
    }

    // The game continues from the ORACLE's results, never the recompiler's.
    m_state = state1;
    std::memcpy(m_flagRing, ring1, sizeof(ring1));
    m_flagRingPos = ringPos1;
    m_flagVisible = visible1;
    std::memcpy(vuData, data1.data(), dataSize);
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f; // VF0.w = 1.0
    m_state.q = 1.0f;
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return vf[bc & 3];
}

// VU MAC flag register: 4 bits each of O/U/S/Z over x,y,z,w. Per component c
// (x=0..w=3): Z bit = (3-c), S bit = (7-c), U bit = (11-c), O bit = (15-c).
// Only the dest components contribute; the rest are cleared.
//
// This is the LEGACY Z/S-only path, kept for RRV_VU_MAC_EXACT=0. It leaves U/O
// at 0 and does not sanitise the result, which let NaN/Inf into vf. The default
// path is macUpdate() below.
// One component of PCSX2's VU_MAC_UPDATE(shift, VU, f), VUflags.cpp @ d5f75c9e4.
// `c` is 0..3 for x..w, i.e. PCSX2's shift = 3 - c, so the bit positions below
// are its 0x0001/0x0010/0x0100/0x1000 << shift written out.
//
// Returns the value the VU STORES. The order matters and is PCSX2's: the sign
// bit alone drives S, the `f == 0` test comes BEFORE the exponent switch (so a
// true zero is Z-only and keeps its sign), and only the exp==255 case is the
// saturation.
static inline float vuMacUpdateComponent(float f, int c, uint32_t &mac)
{
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    const uint32_t sign = v & 0x80000000u;

    if (sign)
        mac |= (1u << (7 - c));                       // S

    if ((v & 0x7FFFFFFFu) == 0u)                      // PCSX2: if (f == 0)
    {
        mac |= (1u << (3 - c));                       // Z; O and U stay clear
        return f;                                     // -0.0 is preserved
    }

    const uint32_t exponent = (v >> 23) & 0xFFu;
    if (exponent == 0u)                               // denormal -> underflow
    {
        mac |= (1u << (11 - c)) | (1u << (3 - c));    // U | Z, O clear
        float flushed;
        std::memcpy(&flushed, &sign, sizeof(flushed));
        return flushed;                               // signed zero
    }
    if (exponent == 0xFFu)                            // Inf/NaN -> overflow
    {
        mac |= (1u << (15 - c));                      // O, U and Z clear
        const uint32_t clamped = sign | 0x7F7FFFFFu;  // largest representable
        float saturated;
        std::memcpy(&saturated, &clamped, sizeof(saturated));
        return saturated;
    }
    return f;                                         // O, U and Z clear
}

void VU1Interpreter::macUpdate(float *out, const float *result, uint8_t dest)
{
    uint32_t mac = 0u;
    for (int c = 0; c < 4; ++c)
    {
        // Non-dest components are PCSX2's VU_MACx_CLEAR: their four bits stay 0.
        out[c] = (dest & (0x8u >> c)) ? vuMacUpdateComponent(result[c], c, mac)
                                      : result[c];
    }
    m_state.mac = mac;
}

void VU1Interpreter::setMacFlags(const float *result, uint8_t dest)
{
    uint32_t mac = 0u;
    for (int c = 0; c < 4; ++c)
    {
        if (!(dest & (0x8u >> c)))
            continue;
        uint32_t bits;
        std::memcpy(&bits, &result[c], 4);
        if ((bits & 0x7FFFFFFFu) == 0u)
            mac |= (1u << (3 - c)); // Z (treat +/-0 as zero)
        if (bits & 0x80000000u)
            mac |= (1u << (7 - c)); // S (sign bit set)
    }
    m_state.mac = mac;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    // m_updateMac marks exactly the FMAC-arithmetic ops, which is exactly where
    // PCSX2 calls VU_MACx_UPDATE. MAX/MINI/ITOF/FTOI/ABS/CLIP/move clear it and
    // must NOT be sanitised -- PCSX2 does not run them through VU_MAC_UPDATE
    // either, and clamping them would be a superset of the hardware rule.
    float sanitised[4];
    const float *src = result;
    if (m_updateMac)
    {
        if (vuMacExactEnabled())
        {
            macUpdate(sanitised, result, dest);
            src = sanitised;
        }
        else
        {
            setMacFlags(result, dest);
        }
        m_updateMac = false; // consume: a paired lower-pipe applyDest must not re-set
    }

    if (dest & 0x8)
        dst[0] = src[0]; // x
    if (dest & 0x4)
        dst[1] = src[1]; // y
    if (dest & 0x2)
        dst[2] = src[2]; // z
    if (dest & 0x1)
        dst[3] = src[3]; // w
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

// Lower-pipe write-back: the same masked store as applyDest(), branchless.
//
// execLower() clears m_updateMac on entry and nothing inside it ever sets the
// flag again, so every lower-pipe applyDest() call took the plain four-
// conditional-store branch. Those four unpredictable branches showed up as
// applyDest self-time in a release profile, reached from LQ/LQI/LQD/MOVE/
// MR32/MFIR/ILW -- some of the most frequent lower ops in this microcode.
// Rewriting the untouched lanes with their own value is what makes it
// branchless, and the VU register file is only ever touched from this thread.
//
// This is a SEPARATE function on purpose: applyDest() itself stays scalar so
// the RRV_VU_VERIFY reference path keeps its independence from the SIMD
// helpers it is used to check (see docs/TESTING.md T-VU-FASTPATH). The lower
// pipe has no second implementation, so it has nothing to stay independent of.
void VU1Interpreter::applyDestNoMac(float *dst, const float *result, uint8_t dest)
{
    using namespace rrv::simd;
    storeu(dst, select(dest_mask(dest), loadu(result), loadu(dst)));
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t itop,
                             uint32_t maxCycles, uint32_t top)
{
    // RR5_TASKS A-12 follow-up: this used to be `ScopedVuRounding(!m_isVu0)`,
    // withholding FE_TOWARDZERO from VU0 micro mode so it ran under whatever
    // ambient rounding the caller left (observed: round-to-nearest, since
    // nothing on the executeVU0Microprogram/VCALLMS path sets it). VU0 and
    // VU1 are the SAME architectural unit and PCSX2 configures them
    // identically: `pcsx2/Pcsx2Config.cpp` sets `VU0FPCR = VU1FPCR =
    // DEFAULT_VU_FP_CONTROL_REGISTER` (DAZ+FTZ+ChopZero), and
    // `InterpVU0::Execute()` (pcsx2/VU0microInterp.cpp @ d5f75c9e4) scopes
    // `FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR)` around
    // every VU0 micro-mode run, exactly symmetric to InterpVU1::Execute()'s
    // VU1FPCR scope. There is no hardware or reference basis for VU0 micro
    // mode running under a different rounding mode than VU1 — always
    // toward-zero, unconditionally, for both.
    const ScopedVuRounding vuRounding(true);
    static const bool p7ForceVf3z = [] {
        const char *v = std::getenv("RRV_P7_FORCE_VF3Z");
        return v && v[0] && v[0] != '0';
    }();
    if (p7ForceVf3z && !m_isVu0 && memory && memory->read32(0x334E94u) == 7u)
    {
        constexpr uint32_t hardwareBits = 0xbefe203cu;
        std::memcpy(&m_state.vf[3][2], &hardwareBits, sizeof(hardwareBits));
    }
    static const bool p7RunTrace = [] {
        const char *v = std::getenv("RRV_P7_VU_TRACE");
        return v && v[0] && v[0] != '0';
    }();
    static uint32_t p7RunSeq = 0;
    if (p7RunTrace && !m_isVu0 && memory && memory->read32(0x334E94u) == 7u && p7RunSeq < 4096u)
    {
        auto fnv1a = [](const void *ptr, size_t size) {
            const auto *bytes = static_cast<const uint8_t *>(ptr);
            uint64_t hash = 0xcbf29ce484222325ULL;
            for (size_t n = 0; n < size; ++n)
            {
                hash ^= bytes[n];
                hash *= 0x100000001b3ULL;
            }
            return hash;
        };
        auto fnvCodeLayout = [](const uint8_t *bytes, size_t size, bool swapHalves, bool swapBytes) {
            uint64_t hash = 0xcbf29ce484222325ULL;
            for (size_t base = 0; base < size; base += 8u)
            {
                for (size_t out = 0; out < 8u; ++out)
                {
                    size_t pos = out;
                    if (swapHalves) pos ^= 4u;
                    if (swapBytes) pos = (pos & ~3u) | (3u - (pos & 3u));
                    hash ^= bytes[base + pos];
                    hash *= 0x100000001b3ULL;
                }
            }
            return hash;
        };
        std::fprintf(stderr,
                     "[p7-vu-run] seq=%u start=%04x itop=%03x top=%03x "
                     "code=%016llx data=%016llx state=%016llx live=%016llx/%016llx/%016llx/%016llx code_chunks=",
                     p7RunSeq++, startPC, itop, top,
                     static_cast<unsigned long long>(fnv1a(vuCode, codeSize)),
                     static_cast<unsigned long long>(fnv1a(vuData, dataSize)),
                     static_cast<unsigned long long>(fnv1a(&m_state, sizeof(m_state))),
                     static_cast<unsigned long long>(fnvCodeLayout(vuCode, 0x500u, false, false)),
                     static_cast<unsigned long long>(fnvCodeLayout(vuCode, 0x500u, true, false)),
                     static_cast<unsigned long long>(fnvCodeLayout(vuCode, 0x500u, false, true)),
                     static_cast<unsigned long long>(fnvCodeLayout(vuCode, 0x500u, true, true)));
        for (uint32_t offset = 0; offset < codeSize; offset += 0x400u)
            std::fprintf(stderr, "%s%016llx", offset ? "," : "", static_cast<unsigned long long>(fnv1a(vuCode + offset, 0x400u)));
        std::fprintf(stderr, " data_chunks=");
        for (uint32_t offset = 0; offset < dataSize; offset += 0x400u)
            std::fprintf(stderr, "%s%016llx", offset ? "," : "", static_cast<unsigned long long>(fnv1a(vuData + offset, 0x400u)));
        std::fputc('\n', stderr);
        if (p7RunSeq <= 4096u)
        {
            std::fprintf(stderr, "[p7-vu-state] seq=%u vf=", p7RunSeq - 1u);
            for (uint32_t reg = 0; reg < 32u; ++reg)
                std::fprintf(stderr, "%s%016llx", reg ? "," : "",
                             static_cast<unsigned long long>(fnv1a(m_state.vf[reg], sizeof(m_state.vf[reg]))));
            std::fprintf(stderr, " vi=");
            for (uint32_t reg = 0; reg < 16u; ++reg)
                std::fprintf(stderr, "%s%08x", reg ? "," : "", static_cast<uint32_t>(m_state.vi[reg]));
            uint32_t qBits = 0, pBits = 0, iBits = 0;
            std::memcpy(&qBits, &m_state.q, sizeof(qBits));
            std::memcpy(&pBits, &m_state.p, sizeof(pBits));
            std::memcpy(&iBits, &m_state.i, sizeof(iBits));
            std::fprintf(stderr, " acc=%016llx q=%08x p=%08x i=%08x mac=%08x clip=%08x status=%08x\n",
                         static_cast<unsigned long long>(fnv1a(m_state.acc, sizeof(m_state.acc))),
                         qBits, pBits, iBits, m_state.mac, m_state.clip, m_state.status);
            constexpr uint32_t selected[] = {3u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 21u, 24u, 26u, 27u, 29u, 30u};
            std::fprintf(stderr, "[p7-vu-values] seq=%u regs=", p7RunSeq - 1u);
            for (uint32_t n = 0; n < sizeof(selected) / sizeof(selected[0]); ++n)
            {
                const uint32_t reg = selected[n];
                const auto *bits = reinterpret_cast<const uint32_t *>(m_state.vf[reg]);
                std::fprintf(stderr, "%sr%02u:%08x/%08x/%08x/%08x", n ? "," : "",
                             reg, bits[0], bits[1], bits[2], bits[3]);
            }
            std::fputc('\n', stderr);
        }
    }
    m_state.pc = startPC;
    m_entryPC = startPC; // diagnostic only, see header
    m_state.ebit = false;
    m_state.itop = itop;
    m_state.top = top;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    // RRV_VU_FLAG_PIPELINE / RRV_VU0_FMAND_DIAG (B-5): seed the 4-deep flag
    // ring with the entry flag state (imported ctx flags for VU0 calls —
    // executeVU0Microprogram sets m_state.mac/status/clip from ctx before
    // calling execute(); current m_state values for VU1, which persist
    // across calls), so readers in the first 4 slots see pre-call flags.
    if (vuFlagPipelineEnabled() || vu0FmandDiagEnabled())
    {
        const FlagSnapshot seed{m_state.mac, m_state.clip, m_state.status};
        for (auto &slot : m_flagRing)
            slot = seed;
        m_flagRingPos = 0;
        m_flagVisible = seed;
    }
#if defined(_DEBUG)
    ps2_diag::g_vu1Mscals.fetch_add(1, std::memory_order_relaxed); // RRV_RUNTIME_LOG: VU1 profiling ([perf:vu1])
#endif
    vu1DiagInit();
    if (g_vu1Diag.enabled)
    {
        s_curRunXgkicks.store(0, std::memory_order_relaxed);
        // B-1: sample the demo car slot at mscal launch (we have PS2Memory here;
        // the GS decode thread does not). Stable for this run's XGKICKs.
        if (memory)
        {
            ps2_diag::noteCarSlot(memory->read32(ps2_diag::kCarSlotAddr));
            ps2_diag::noteDemoPhase(memory->read32(ps2_diag::kDemoPhaseAddr));
        }
        s_curRunCarLive = ps2_diag::carLive();
    }

    // DIAG-TEMP RRV_VU_INTRACE=<hex entry PC> — hold the INPUT state of a chosen
    // microprogram (every VF/VI register, Q/I, the three flag words and the whole
    // of VU data memory, before a single instruction of it runs) and write it out
    // only once that program's XGKICK is seen to emit the requested GIF packet.
    // Selecting the invocation by its OUTPUT is what lets this be compared with
    // PCSX2's identically-laid-out dump without the two engines having to sit at
    // the same frame index. See rrvVuInTrace() and the XGKICK site.
    if (!m_isVu0)
    {
        auto &t = rrvVuInTrace();
        if (t.enabled && t.written < t.max &&
            (t.phase < 0 || (memory && memory->read32(0x334E94u) == (uint32_t)t.phase)))
        {
            if (t.log)
                std::fprintf(t.log, "[rrv-vu1] mscal seq=%u entry=%04x top=%03x itop=%03x\n",
                             t.seq, m_entryPC, top, itop);
            ++t.seq;
            if (m_entryPC != t.entry)
            {
                t.pending = false;
            }
            else
            {
                std::memcpy(t.vf, m_state.vf, sizeof(t.vf));
                std::memcpy(t.vi, m_state.vi, sizeof(t.vi));
                t.q = m_state.q;
                t.i = m_state.i;
                t.mac = m_state.mac;
                t.status = m_state.status;
                t.clip = m_state.clip;
                t.mem.assign(vuData, vuData + dataSize);
                t.micro.assign(vuCode, vuCode + codeSize);
                t.pending = true;
                t.pendingSeq = t.seq - 1u;
                t.pendTop = top;
                t.pendItop = itop;
            }
        }
    }

    if (p7RunTrace && !m_isVu0)
        s_p7TraceRun = static_cast<int32_t>(p7RunSeq) - 1;
    {
        auto &st = rrvVuStep();
        if (st.out && !st.used && !m_isVu0 && rrvVuInTrace().pending &&
            rrvVuInTrace().written == 0u)
        {
            st.used = true;
            st.active = true;
            st.n = 0;
            st.havePrev = false;
        }
    }

    aotEnsure();
    if (m_aotVerify && m_aotEnabled)
        runVerified(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    else
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    {
        auto &st = rrvVuStep();
        if (st.active)
        {
            std::fflush(st.out);
            st.active = false;
        }
    }
    s_p7TraceRun = -1;

    if (g_vu1Diag.enabled)
    {
        const uint32_t produced = s_curRunXgkicks.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_vu1Diag.mtx);
        ++g_vu1Diag.mscalTotal;
        ++g_vu1Diag.mscalByPc[startPC];
        if (s_curRunCarLive)
        {
            ++g_vu1Diag.mscalTotalCar;
            ++g_vu1Diag.mscalByPcCar[startPC];
        }
        if (produced == 0u)
            ++g_vu1Diag.emptyByPc[startPC];
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_vu1Diag.last).count() >= 2000)
        {
            vu1DiagDumpLocked();
            g_vu1Diag.last = now;
            // Snapshot current VU1 microcode + data mem (overwrite; last dump = latest scene).
            if (const char *dp = std::getenv("RRV_VU1_DUMP"))
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s.code", dp);
                if (FILE *fc = std::fopen(path, "wb")) { std::fwrite(vuCode, 1, codeSize, fc); std::fclose(fc); }
                std::snprintf(path, sizeof(path), "%s.data", dp);
                if (FILE *fd = std::fopen(path, "wb")) { std::fwrite(vuData, 1, dataSize, fd); std::fclose(fd); }
                // Also snapshot EE main RAM to search for the sprite VIF1 chain
                // (MSCAL VIFcodes 0x1400013e / 0x1400033d) — is the display list
                // built-but-unwalked (present) or never built (absent)?
                if (memory)
                {
                    if (uint8_t *ram = memory->getRDRAM())
                    {
                        std::snprintf(path, sizeof(path), "%s.eeram", dp);
                        if (FILE *fr = std::fopen(path, "wb")) { std::fwrite(ram, 1, PS2_RAM_SIZE, fr); std::fclose(fr); }
                    }
                }
            }
        }
    }
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t itop, uint32_t maxCycles, uint32_t top)
{
    // See execute()'s A-12 comment: always toward-zero, for both units. Only
    // m_vu1 ever calls resume() today (VU0 has no MSCNT-continuation caller),
    // so this is currently a no-op for VU0, kept symmetric on purpose.
    const ScopedVuRounding vuRounding(true);
    m_state.ebit = false;
    m_state.itop = itop;
    m_state.top = top;
    // See execute(): reseed the flag ring at resume() start too (VU1's mid-
    // microprogram continuation via MSCNT).
    if (vuFlagPipelineEnabled() || vu0FmandDiagEnabled())
    {
        const FlagSnapshot seed{m_state.mac, m_state.clip, m_state.status};
        for (auto &slot : m_flagRing)
            slot = seed;
        m_flagRingPos = 0;
        m_flagVisible = seed;
    }
    aotEnsure();
    if (m_aotVerify && m_aotEnabled)
        runVerified(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    else
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    // C0 frame budget. Exclusive of GS: XGKICK submits GIF packets from inside
    // this loop, and that cost belongs to the `gs` bucket, not to VU1.
    auto cadenceVu1 = rrv::cadence::vu1Scope();
    // RRV_VU0_LEAN / RRV_VU1_LEAN: aotLean() may hand over with a branch still pending.
    if (!m_aotLeanResume)
        m_state.branchPending = false;
#if defined(_DEBUG)
    const auto perfVu1Start = std::chrono::steady_clock::now(); // RRV_RUNTIME_LOG: VU1 profiling ([perf:vu1])
    uint32_t perfVu1Instrs = 0;
#endif
    // B-5: maintain the flag-visibility ring whenever the pipeline model or its
    // confirmation probe is active (cached once per run() call; both env reads
    // are getenv-cached statics, so this is a cheap bool check per call).
    const bool flagRingActive = vuFlagPipelineEnabled() || vu0FmandDiagEnabled();
    static const bool matrixTrace = [] {
        const char *v = std::getenv("RRV_VU_MATRIX_TRACE");
        return v && v[0] && v[0] != '0';
    }();
    m_codeImage = vuCode;   // DIAG (T-P7-CLIPWHO): so the CLIP probe can dump the
    m_codeSize = codeSize;  // program it is actually inside, not whichever one a
                            // 2-second timer happened to catch.

    // Resolve the configuration ONCE per run() call (~556 instructions on
    // average, measured), so the loop below reads plain bools instead of
    // re-entering a function-local-static guard on every instruction.  Every
    // source here is already a getenv-cached static, so this is a copy, not a
    // re-read of the environment.
    m_cfgFast = vuFastPathEnabled();
    m_cfgVerify = vuVerifyEnabled();
    m_cfgMacExact = vuMacExactEnabled();
    m_cfgDenorm = vuDenormFlushEnabled();
    m_cfgFlagRing = flagRingActive;
    m_cfgPipeline = vuFlagPipelineEnabled();
    m_cfgClipDiag = (vuClipDiagLimit() != 0u);
    m_cfgQLatency = vuQLatencyEnabled();
    // A microprogram never starts with a result still in the FDIV pipe: the
    // previous one drained it (PCSX2 _vuFlushAll at end of program).
    m_qStageActive = false;
    // One test in the hot loop stands for every per-instruction diagnostic.
    // s_p7TraceRun is armed in execute() before run() and can only be turned
    // OFF inside the loop, never on, so sampling it here is sound.
    m_cfgAnyDiag = rrvVuStep().active || (s_p7TraceRun == 1) || g_vu1Diag.enabled ||
                   matrixTrace || m_cfgClipDiag || vu0FmandDiagEnabled();

    // ---- RRV_VU_AOT: statically recompiled microcode ------------------------
    //
    // A compiled block maintains exactly the state this loop maintains (it is
    // the same per-slot sequence over the same op handlers), so whatever it
    // executes this loop would have executed identically. The loop takes over
    // at anything the catalogue does not hold, at a branch in a delay slot, or
    // at an E bit in a delay slot, and hands back to compiled code at the next
    // slot boundary where a block matches.
    //
    // The eligibility test is not a preference, it is a correctness fence: the
    // per-instruction diagnostics, the scalar reference path, the per-slot
    // fast-path verifier and the opt-in FDIV latency model are not reproduced
    // by compiled code, so a run that arms any of them interprets. Blocks also
    // bake the default flag-visibility ring (RRV_VU_FLAG_PIPELINE).
    m_curData = vuData;
    m_curDataSize = dataSize;
    m_curGs = &gs;
    m_curMemory = memory;
    aotEnsure();
    const bool aotActive = m_aotEnabled && !m_aotForceInterp && m_cfgFast && !m_cfgVerify &&
                           !m_cfgAnyDiag && !m_cfgQLatency && m_cfgFlagRing && !m_cfgClipDiag &&
                           aotConfigIsDefault();
    // Blocks start at a program entry and after the delay slot of a branch,
    // so that is where the interpreter hands back (and where a miss is
    // recorded for the generator).
    bool aotBoundary = true;
    // False here except when aotLean() hands over inside a delay slot.
    bool aotPrevBranch = m_state.branchPending;

    // Pre-decoded microprogram (rrv_vu_ir.h).  Entries re-validate against the
    // raw instruction bits, so no invalidation is needed when VIF1 uploads a
    // new program over this one.
    const bool irCacheUsable = (codeSize <= kIrSlots * 8u);
    uint32_t cycle = 0;
    uint64_t interpreted = 0;
    for (; cycle < maxCycles; ++cycle)
    {
        if (aotActive && aotBoundary && !m_state.branchPending && !m_state.ebit)
        {
            uint32_t ran = 0;
            const bool ended = aotRun(vuCode, codeSize, maxCycles - cycle, ran);
            m_aotSlots += ran;
            cycle += ran;
            if (ended)
            {
                cycle = maxCycles;
                break;
            }
            if (cycle >= maxCycles)
                break;
            aotBoundary = false;
            aotPrevBranch = m_state.branchPending; // a block may end on a branch in a delay slot
        }
        if (m_state.pc + 8 > codeSize)
            break;
        ++interpreted;

        // FDIV pipe test, at the instruction boundary and BEFORE this slot can
        // read Q — PCSX2 runs _vuTestPipes() at the same point.
        m_curCycle = cycle;
        if (m_qStageActive)
            qTick();

        // ---- fetch + decode ----------------------------------------------
        // One 64-bit load of the slot, then the pre-decoded IR for it.  The
        // cache entry carries the raw bits it was decoded from, so a VIF1 MPG
        // upload that replaces this program is detected by the compare below
        // and re-decoded on first execution — stale IR cannot run.
        uint64_t rawSlot;
        std::memcpy(&rawSlot, vuCode + m_state.pc, sizeof(rawSlot));
        const uint32_t lower = static_cast<uint32_t>(rawSlot);
        const uint32_t upper = static_cast<uint32_t>(rawSlot >> 32);
        rrv::vu::SlotIR localIr;
        rrv::vu::SlotIR *irSlot;
        if (irCacheUsable)
        {
            irSlot = &m_ir[m_state.pc >> 3];
            if (RRV_VU_UNLIKELY(!irSlot->valid || irSlot->lower != lower ||
                                irSlot->upper != upper))
                rrv::vu::decodeSlot(lower, upper, *irSlot);
        }
        else
        {
            irSlot = &localIr;
            rrv::vu::decodeSlot(lower, upper, localIr);
        }
        const rrv::vu::SlotIR &ir = *irSlot;

        const uint32_t tracePc = m_state.pc;
        uint32_t matrixBefore[8];
        // Every per-instruction diagnostic in this loop is env-armed and off in
        // a normal run; one predicted-not-taken test now stands for all of them.
        if (RRV_VU_UNLIKELY(m_cfgAnyDiag))
        {
            if (auto &st = rrvVuStep(); st.active)
            {
                uint32_t lower = 0, upper = 0;
                std::memcpy(&lower, vuCode + m_state.pc, sizeof(lower));
                std::memcpy(&upper, vuCode + m_state.pc + 4, sizeof(upper));
                std::fprintf(st.out, "%05u pc=%04x U=%08x L=%08x", st.n, m_state.pc, upper, lower);
                uint32_t nvf[32][4], nvi[16], nacc[4], nq, ni, np;
                std::memcpy(nvf, m_state.vf, sizeof(nvf));
                for (uint32_t r = 0; r < 16u; ++r)
                    nvi[r] = static_cast<uint32_t>(m_state.vi[r]) & 0xFFFFu;
                std::memcpy(nacc, m_state.acc, sizeof(nacc));
                std::memcpy(&nq, &m_state.q, sizeof(nq));
                std::memcpy(&ni, &m_state.i, sizeof(ni));
                std::memcpy(&np, &m_state.p, sizeof(np));
                if (st.havePrev)
                {
                    for (uint32_t r = 0; r < 32u; ++r)
                        if (std::memcmp(nvf[r], st.vf[r], 16) != 0)
                            std::fprintf(st.out, " vf%02u=%08x,%08x,%08x,%08x", r,
                                         nvf[r][0], nvf[r][1], nvf[r][2], nvf[r][3]);
                    for (uint32_t r = 0; r < 16u; ++r)
                        if (nvi[r] != st.vi[r])
                            std::fprintf(st.out, " vi%02u=%08x", r, nvi[r]);
                    if (std::memcmp(nacc, st.acc, 16) != 0)
                        std::fprintf(st.out, " acc=%08x,%08x,%08x,%08x",
                                     nacc[0], nacc[1], nacc[2], nacc[3]);
                    if (nq != st.q) std::fprintf(st.out, " q=%08x", nq);
                    if (ni != st.i) std::fprintf(st.out, " i=%08x", ni);
                    if (np != st.p) std::fprintf(st.out, " p=%08x", np);
                    if (m_state.mac != st.mac) std::fprintf(st.out, " mac=%08x", m_state.mac);
                    if (m_state.status != st.status) std::fprintf(st.out, " status=%08x", m_state.status);
                    if (m_state.clip != st.clip) std::fprintf(st.out, " clip=%08x", m_state.clip);
                    for (uint32_t o = 0; o + 16u <= dataSize; o += 16u)
                        if (std::memcmp(vuData + o, st.mem.data() + o, 16) != 0)
                        {
                            uint32_t w[4];
                            std::memcpy(w, vuData + o, sizeof(w));
                            std::fprintf(st.out, " mem[%04x]=%08x,%08x,%08x,%08x", o, w[0], w[1], w[2], w[3]);
                        }
                }
                std::fputc('\n', st.out);
                std::memcpy(st.vf, nvf, sizeof(nvf));
                std::memcpy(st.vi, nvi, sizeof(nvi));
                std::memcpy(st.acc, nacc, sizeof(nacc));
                st.q = nq; st.i = ni; st.p = np;
                st.mac = m_state.mac; st.status = m_state.status; st.clip = m_state.clip;
                st.mem.assign(vuData, vuData + dataSize);
                st.havePrev = true;
                ++st.n;
            }
            if (s_p7TraceRun == 1)
            {
                auto fnv1a = [](const void *ptr, size_t size) {
                    const auto *bytes = static_cast<const uint8_t *>(ptr);
                    uint64_t hash = 0xcbf29ce484222325ULL;
                    for (size_t n = 0; n < size; ++n) { hash ^= bytes[n]; hash *= 0x100000001b3ULL; }
                    return hash;
                };
                std::fprintf(stderr, "[p7-vu-step] pc=%04x vf=", m_state.pc);
                for (uint32_t reg = 0; reg < 32u; ++reg)
                    std::fprintf(stderr, "%s%016llx", reg ? "," : "",
                                 static_cast<unsigned long long>(fnv1a(m_state.vf[reg], sizeof(m_state.vf[reg]))));
                const auto *accBits = reinterpret_cast<const uint32_t *>(m_state.acc);
                std::fprintf(stderr, " vi=%016llx acc=%016llx accv=%08x/%08x/%08x/%08x q=%016llx i=%016llx\n",
                             static_cast<unsigned long long>(fnv1a(m_state.vi, sizeof(m_state.vi))),
                             static_cast<unsigned long long>(fnv1a(m_state.acc, sizeof(m_state.acc))),
                             accBits[0], accBits[1], accBits[2], accBits[3],
                             static_cast<unsigned long long>(fnv1a(&m_state.q, sizeof(m_state.q))),
                             static_cast<unsigned long long>(fnv1a(&m_state.i, sizeof(m_state.i))));
                if (m_state.pc == 0x278u)
                    s_p7TraceRun = -1;
            }
            if (g_vu1Diag.enabled)
            {
                // Sprite-reachability probe: the loop bodies right before the never-run
                // sprite XGKICKs at 0x9f0 / 0x19e8 (see Vu1Diag). A hit here means we DID
                // branch into that routine at runtime (Class B); zero hits means we never
                // reach it (Class A).
                const uint32_t pc = m_state.pc;
                if (pc >= 0x950u && pc <= 0x9f0u)
                    g_vu1Diag.approach9f0.fetch_add(1, std::memory_order_relaxed);
                else if (pc >= 0x1948u && pc <= 0x19e8u)
                    g_vu1Diag.approach19e8.fetch_add(1, std::memory_order_relaxed);
            }
            if (matrixTrace && !m_isVu0)
            {
                std::memcpy(matrixBefore, m_state.vf[29], sizeof(matrixBefore));
            }
            if (s_p7TraceRun == 1 && m_state.pc >= 0x150u && m_state.pc <= 0x178u)
            {
                const uint8_t upperOp = static_cast<uint8_t>(upper & 0x3fu);
                const uint8_t upperOp2 = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7cu));
                std::fprintf(stderr,
                             "[p7-vu-decode] pc=%04x up=%02x up2=%02x dest=%x fd=%u fs=%u ft=%u "
                             "lo=%02x lit=%u lis=%u lid=%u\n",
                             m_state.pc, upperOp, upperOp2, DEST(upper), FD(upper), FS(upper), FT(upper),
                             static_cast<uint8_t>((lower >> 25) & 0x7fu), LIT(lower), LIS(lower), LID(lower));
            }
        }
#if defined(_DEBUG)
        ++perfVu1Instrs;
#endif

        const bool eBit = (ir.ubits & rrv::vu::UB_E) != 0u;

        // A branch taken by the PREVIOUS instruction transfers control only after
        // THIS instruction (its delay slot) has executed. Capture and clear the
        // pending branch before running the slot, so a branch here starts fresh.
        const bool takeBranch = m_state.branchPending;
        const uint32_t branchTo = m_state.branchTarget;
        m_state.branchPending = false;

        // LOI is signalled by the I bit (bit 31 of the UPPER word). When set, the
        // LOWER 32-bit slot is a float immediate loaded into the I register (the
        // upper FMAC still executes and may consume I), and there is no lower op.
        // Keying on a magic lower value (0x8000033C) was wrong: that constant is
        // the common NOP-lower paired with real upper FMACs (e.g. the per-vertex
        // transform's MULAx/MADDAy at lo=0x8000033C), so those uppers were dropped
        // -> the matrix multiply lost its X/Y rows. (XGKICK et al. set the LOWER
        // bit31, not the upper, so the I bit doesn't false-trigger on them.)
        // B-5 flag-visibility ring: before this slot's upper runs, capture the
        // snapshot from 4 slots ago (the one about to be overwritten below) —
        // that is what THIS slot's lower-pipe flag readers must see. Then push
        // this slot's own {mac,status,clip} right after the upper executes
        // (before the lower runs), so a future slot 4 iterations from now sees
        // this slot's upper results, not this slot's own lower-pipe writes
        // (e.g. FSSET) — matching the ~4 issue-slot hardware delay.
        if (m_cfgFlagRing)
        {
            m_flagVisible = m_flagRing[m_flagRingPos];
            // The clip shadow is only ever non-zero under RRV_VU_CLIP_DIAG.
            if (RRV_VU_UNLIKELY(m_cfgClipDiag))
                m_clipShadowVisible = m_clipShadowRing[m_flagRingPos];
        }
        if (RRV_VU_UNLIKELY(m_clipWatch != 0u))
            --m_clipWatch;

        const bool loi = (ir.ubits & rrv::vu::UB_I) != 0u;
        if (loi)
            std::memcpy(&m_state.i, &lower, 4);

        execUpperSlot(ir, upper);

        if (m_cfgFlagRing)
        {
            m_flagRing[m_flagRingPos] = FlagSnapshot{m_state.mac, m_state.clip, m_state.status};
            if (RRV_VU_UNLIKELY(m_cfgClipDiag))
                m_clipShadowRing[m_flagRingPos] = m_clipShadow;
            m_flagRingPos = (m_flagRingPos + 1) & 3;
        }

        if (!loi)
        {
            // A NOP lower is ~a third of all slots in this game's microcode;
            // skipping the call is exactly what execLower() would have done
            // after its own early-out, minus the call.
            if (ir.lclass == rrv::vu::LC_NOP)
                m_updateMac = false;
            else
                execLower(lower, vuData, dataSize, gs, memory, upper);
        }

        // Enforce VF0 invariant (one 16-byte store rather than four)
        {
            static const float kVf0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            rrv::simd::storeu(m_state.vf[0], rrv::simd::loadu(kVf0));
        }
        // Enforce VI0 invariant
        m_state.vi[0] = 0;

        if (RRV_VU_UNLIKELY(m_cfgAnyDiag))
        {
            if (matrixTrace && !m_isVu0 &&
                std::memcmp(matrixBefore, m_state.vf[29], sizeof(matrixBefore)) != 0)
            {
                const auto *matrixAfter = reinterpret_cast<const uint32_t *>(m_state.vf[29]);
                const uint32_t phase = memory ? memory->read32(0x334E94u) : 0xffffffffu;
                std::fprintf(stderr,
                             "[vu-matrix-write] phase=%u pc=%04x upper=%08x lower=%08x vi2=%04x "
                             "before=%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x "
                             "after=%08x/%08x/%08x/%08x,%08x/%08x/%08x/%08x\n",
                             phase, tracePc, upper, lower, static_cast<uint32_t>(m_state.vi[2]) & 0xffffu,
                             matrixBefore[0], matrixBefore[1], matrixBefore[2], matrixBefore[3],
                             matrixBefore[4], matrixBefore[5], matrixBefore[6], matrixBefore[7],
                             matrixAfter[0], matrixAfter[1], matrixAfter[2], matrixAfter[3],
                             matrixAfter[4], matrixAfter[5], matrixAfter[6], matrixAfter[7]);
            }
        }

        // RRV_VU_AOT: the slot after a branch's delay slot starts a block.
        aotBoundary = aotPrevBranch;
        aotPrevBranch = !loi && ir.lclass != rrv::vu::LC_NOP &&
                        (ir.lop == 0x20 || ir.lop == 0x21 || ir.lop == 0x24 || ir.lop == 0x25 ||
                         ir.lop == 0x28 || ir.lop == 0x29 || (ir.lop >= 0x2C && ir.lop <= 0x2F));
        if (takeBranch)
        {
            // This instruction was the delay slot of a taken branch; jump now.
            m_state.pc = branchTo & 0x3FFFu;
        }
        else
        {
            uint32_t nextPC = m_state.pc + 8;
            if (nextPC >= codeSize)
                nextPC = 0;
            m_state.pc = nextPC;
        }

        if (m_state.ebit)
            break;

        if (eBit)
            m_state.ebit = true;
    }
    // PCSX2 _vuFlushAll: a microprogram that ends with a DIV still in flight
    // leaves that result VISIBLE in Q for whatever runs next.
    qFlush();
    // Coverage: only the slots the interpreter had to take BACK from the
    // recompiler count here. The differential gate's oracle pass runs with the
    // recompiler forced off and is not a coverage miss.
    if (!m_aotForceInterp)
        m_interpSlots += interpreted;

#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: VU1 interpreter cost (wall-ns + instruction-pairs), reported
    // via [perf:vu1] in ps2_runtime.cpp's periodic diagnostic block.
    ps2_diag::g_vu1Ns.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - perfVu1Start).count()), std::memory_order_relaxed);
    ps2_diag::g_vu1Instrs.fetch_add(perfVu1Instrs, std::memory_order_relaxed);
#endif
}

// RRV_VU_DENORM — the VU has no denormals. PCSX2 models this by routing every
// arithmetic operand through vuDouble(), which flushes a denormal to signed zero
// (`case 0x0: f &= 0x80000000`); it does so at 51 call sites in VUops.cpp
// (d5f75c9e4). Our interpreter had no equivalent anywhere and multiplied raw IEEE
// floats, so denormals were produced and propagated. Flushing happens on operand
// READ, not on result write, exactly as PCSX2 does it: a denormal result stays in
// vf and is neutralised when something reads it.
//
// Opt-in while its effect on the phase-7 sky vertex is being measured
// (docs/TESTING.md T-P7-VUSEM step 1).
static inline bool vuDenormFlushEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("RRV_VU_DENORM");
        return v && v[0] && v[0] != '0';
    }();
    return on;
}

// RRV_VU_CLIP_EXACT — PCSX2-exact CLIP (integer compare + denormal fixup +
// 24-bit mask). DEFAULT ON since 2026-08-20; RRV_VU_CLIP_EXACT=0 is the
// rollback.
//
// The float form we used before is not the hardware rule. `x > fabs(w)` is
// FALSE for a NaN x, so the float path reports "not clipped" exactly where
// hardware reports "clipped". Those values reach CLIP because our FMAC has no
// exp==255 saturation — PCSX2 clamps every result to +/-0x7f7fffff in
// VU_MAC_UPDATE (VUflags.cpp) and we store the raw IEEE result. The visible
// consequence was near-plane triangle fans surviving into the GS and writing a
// Z that the scene never clears. docs/TESTING.md T-P7-POISON, T-P7-CLIPFIX.
//
// The FMAC saturation gap itself is still open; this makes CLIP behave like
// hardware even in its presence.
// RRV_VU_DIV_EXACT — PCSX2-exact DIV: divide-by-zero sign from the XOR of both
// operand sign bits, and the STATUS D/I bits. Q feeds the perspective divide, so
// this sits directly on the screen X/Y path (T-P7-VUIN step 3).
static inline bool vuDivExactEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("RRV_VU_DIV_EXACT");
        return v && v[0] && v[0] != '0';
    }();
    return on;
}

static inline bool vuClipExactEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("RRV_VU_CLIP_EXACT");
        // Unset, empty and any non-"0" value select the PCSX2-exact rule;
        // exactly "0" is the rollback. Same idiom as vuFlagPipelineEnabled().
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// RRV_VU_OPCLAMP (scripts/vu_opclamp_overlay.py) — the VU has no Inf/NaN.
// PCSX2 routes every FMAC operand through vuDouble() (pcsx2/VUops.cpp @
// d5f75c9e4, GPL-3.0): an exponent-255 operand becomes sign|0x7f7fffff before
// the arithmetic. Without it a raw integer bit pattern in a lane the program
// multiplies by zero is a host NaN (RR5 VU0 skinning, MADDw vf4,vf0,vf1w), and
// FTOI4 turns it into UV (0,0). DEFAULT ON; RRV_VU_OPCLAMP=0 is the rollback.
static inline bool vuOpClampEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("RRV_VU_OPCLAMP");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

static inline float vuClampOperand(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    if ((bits & 0x7F800000u) == 0x7F800000u)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&f, &bits, sizeof(f));
    }
    return f;
}

static inline float vuFlushDenorm(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    if ((bits & 0x7F800000u) == 0u)
    {
        bits &= 0x80000000u;
        std::memcpy(&f, &bits, sizeof(f));
    }
    return f;
}


// ---- FDIV (Q) pipeline -----------------------------------------------------
// Provenance: PCSX2 d5f75c9e4, pcsx2/VUops.cpp — _vuFDIVAdd / _vuFDIVflush /
// _vuFlushAll (GPL-3.0, the licence this tree already carries). See ps2_vu1.h
// for why a scene-dependent defect falls out of getting this wrong.
void VU1Interpreter::qWrite(float value, uint32_t latency)
{
    if (!m_cfgQLatency)
    {
        m_state.q = value;
        return;
    }
    // PCSX2 _vuTestFDIVStalls: a second FDIV op issued while one is still in
    // flight STALLS until the pipe drains, so the older result always becomes
    // visible first. Committing it here has the same effect on Q's value.
    if (m_qStageActive)
        m_state.q = m_qStage;
    m_qStage = value;
    m_qStageReadyCycle = m_curCycle + latency;
    m_qStageActive = true;
}

void VU1Interpreter::qFlush()
{
    if (!m_qStageActive)
        return;
    m_state.q = m_qStage;
    m_qStageActive = false;
}

void VU1Interpreter::qTick()
{
    if (!m_qStageActive)
        return;
    // No unsigned wrap: m_curCycle only advances by one per slot inside a
    // single run(), and the stage is drained at every exit from it.
    if (m_curCycle >= m_qStageReadyCycle)
    {
        m_state.q = m_qStage;
        m_qStageActive = false;
    }
}


// ---- EFU transcendentals ---------------------------------------------------
// Provenance: PCSX2 d5f75c9e4, pcsx2/VUops.cpp — _vuCalculateEATAN, _vuEATAN,
// _vuEATANxy, _vuEATANxz, _vuESIN, _vuEEXP, _vuESQRT, _vuERSQRT, _vuERCPR,
// _vuERSADD (GPL-3.0, the licence this tree already carries).
//
// The PS2's EFU does not compute libm's transcendentals; it evaluates fixed
// polynomials, and three of the differences against the host functions this
// file used before are not small:
//
//   * EATAN is a 15th-order odd series PLUS a constant pi/4 term. std::atan()
//     is therefore a systematic ~0.785 rad (45 degrees) out.
//   * EATANxy / EATANxz are EATAN(y/x) and EATAN(z/x) — a SINGLE-argument
//     arctangent of the ratio, not std::atan2(). They differ by pi whenever
//     x < 0, and hardware returns exactly 0 for x == 0 instead of +/-pi/2.
//   * ESQRT / ERSQRT pass a NEGATIVE input straight through unchanged;
//     std::sqrt(std::fabs(x)) silently rectifies it.
//
// The expression shapes below are kept identical to PCSX2's (float accumulator,
// std::pow promoting to double, the pi/4 term added AFTER the narrowing to
// float) so the two engines agree bit for bit, not just numerically.
static float vuCalculateEATAN(float inputvalue)
{
    static const float eatanconst[9] = {
        0.999999344348907f, -0.333298563957214f, 0.199465364217758f,
        -0.13085337519646f, 0.096420042216778f, -0.055909886956215f,
        0.021861229091883f, -0.004054057877511f, 0.785398185253143f};

    float result = static_cast<float>(
        (eatanconst[0] * inputvalue) + (eatanconst[1] * std::pow(inputvalue, 3)) +
        (eatanconst[2] * std::pow(inputvalue, 5)) + (eatanconst[3] * std::pow(inputvalue, 7)) +
        (eatanconst[4] * std::pow(inputvalue, 9)) + (eatanconst[5] * std::pow(inputvalue, 11)) +
        (eatanconst[6] * std::pow(inputvalue, 13)) + (eatanconst[7] * std::pow(inputvalue, 15)));

    result += eatanconst[8];
    return vuFlushDenorm(result);
}

// RRV_VU_EATAN_MVU (scripts/vu_eatan_overlay.py) — the EFU arctangent as
// PCSX2's microVU computes it (pcsx2/x86/microVU_Lower.inl mVU_EATAN_,
// mVU_EATAN, mVU_EATANxy, mVU_EATANxz; constants microVU_Misc.h mVUglob.T1..T8
// and Pi4; @ d5f75c9e4, GPL-3.0). PCSX2's interpreter (above) feeds the raw
// ratio to the series; microVU feeds (x-1)/(x+1), (y-x)/(y+x), (z-x)/(z+x), so
// series + pi/4 is a real arctangent, and applies T2..T8 in its own order. The
// reference ran microVU. RR5's glass reflection (VU1 entry 0x00a0) scrolls
// its texture by these angles (known_issues VIS-007). DEFAULT ON;
// RRV_VU_EATAN_MVU=0 restores the interpreter shape.
static bool vuEatanMicroVu()
{
    static const bool on = [] {
        const char *v = std::getenv("RRV_VU_EATAN_MVU");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

static float vuMvuBits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// mVU_EATAN_: PQ = t*T1, then for T2..T8: t2 *= t; t2 *= t; PQ += t2*Tn;
// finally PQ += Pi4. Every step is one single-precision op, in this order.
static float vuMvuEatanSeries(float t)
{
    static const uint32_t T[8] = {0x3f7ffff5u, 0x3e4c40a6u, 0xbe0e6c63u, 0x3dc577dfu,
                                  0xbeaaa61cu, 0xbd6501c4u, 0x3cb31652u, 0xbb84d7e7u};
    volatile float pq = t * vuMvuBits(T[0]);
    volatile float t2 = t;
    for (int n = 1; n < 8; ++n)
    {
        t2 = t2 * t;
        t2 = t2 * t;
        const float t1 = t2 * vuMvuBits(T[n]);
        pq = pq + t1;
    }
    pq = pq + vuMvuBits(0x3f490fdbu);
    return vuFlushDenorm(pq);
}

// EATAN: (x - 1) / (x + 1). EATANxy / EATANxz: (a - x) / (x + a), a = y or z.
static float vuMvuEatanRatio(float a, float x)
{
    volatile float num = a - x;
    volatile float den = x + a;
    return vuMvuEatanSeries(num / den);
}

static float vuCalculateESIN(float value)
{
    static const float sinconsts[5] = {1.0f, -0.166666567325592f, 0.008333025500178f,
                                       -0.000198074136279f, 0.000002601886990f};
    const float p = static_cast<float>(
        (sinconsts[0] * value) + (sinconsts[1] * std::pow(value, 3)) +
        (sinconsts[2] * std::pow(value, 5)) + (sinconsts[3] * std::pow(value, 7)) +
        (sinconsts[4] * std::pow(value, 9)));
    return vuFlushDenorm(p);
}

static float vuCalculateEEXP(float value)
{
    static const float consts[6] = {0.249998688697815f, 0.031257584691048f,
                                    0.002591371303424f, 0.000171562001924f,
                                    0.000005430199963f, 0.000000690600018f};
    float p = static_cast<float>(
        1.0f + (consts[0] * value) + (consts[1] * std::pow(value, 2)) +
        (consts[2] * std::pow(value, 3)) + (consts[3] * std::pow(value, 4)) +
        (consts[4] * std::pow(value, 5)) + (consts[5] * std::pow(value, 6)));
    p = static_cast<float>(std::pow(p, 4));
    p = vuFlushDenorm(p);
    return 1.0f / p;
}


// CLIP — factored out of execUpper()'s switch so the scalar reference path and
// the SIMD fast path share ONE implementation of the most correctness-sensitive
// op in this file (T-P7-CLIPFIX / T-P7-CLIPWHY).  Body unchanged.
void VU1Interpreter::clipOp(const float *vs, const float *vt)
{
    uint32_t flags = 0;
    if (const uint32_t diagLimit = vuClipDiagLimit(); diagLimit != 0u)
    {
        static uint32_t s_seen = 0u, s_total = 0u, s_diff = 0u;
        auto rb = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; };
        int32_t value = (int32_t)rb(vt[3]);
        value = (value & 0x7F800000) ? (value & 0x7FFFFFFF) : 0x007FFFFF;
        uint32_t fe = 0u;
        for (int c = 0; c < 3; c++)
        {
            const uint32_t b = rb(vs[c]);
            if ((int32_t)(b ^ 0x00000000u) > value) fe |= (uint32_t)(0x01 << (c * 2));
            if ((int32_t)(b ^ 0x80000000u) > value) fe |= (uint32_t)(0x02 << (c * 2));
        }
        const float wf = std::fabs(vt[3]);
        uint32_t ff = 0u;
        for (int c = 0; c < 3; c++)
        {
            if (vs[c] > +wf) ff |= (uint32_t)(0x01 << (c * 2));
            if (vs[c] < -wf) ff |= (uint32_t)(0x02 << (c * 2));
        }
        // The register the OTHER form would have built, so a reader
        // downstream can be shown both answers at once.
        m_clipShadow = ((m_clipShadow << 6) |
                        (vuClipExactEnabled() ? ff : fe)) & 0xFFFFFFu;
        ++s_total;
        if (fe != ff)
        {
            ++s_diff;
            m_clipWatchPc = m_state.pc;
            if (s_seen == 0u && m_codeImage && m_codeSize)
            {
                char path[256];
                std::snprintf(path, sizeof(path),
                              "/tmp/vu1_clip_%04X_entry%04X.code",
                              m_state.pc, m_entryPC);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(m_codeImage, 1, m_codeSize, f);
                    std::fclose(f);
                    std::fprintf(stderr,
                        "[clipdiag] dumped %u bytes of the microprogram "
                        "holding pc=%04X (entry %04X) -> %s\n",
                        m_codeSize, m_state.pc, m_entryPC, path);
                }
            }
            if (s_seen < diagLimit)
            {
                ++s_seen;
                std::fprintf(stderr,
                    "[clipdiag] pc=%04X exact=%02X float=%02X | "
                    "w=%08X(%g) x=%08X(%g) y=%08X(%g) z=%08X(%g)\n",
                    m_state.pc, fe, ff,
                    rb(vt[3]), (double)vt[3], rb(vs[0]), (double)vs[0],
                    rb(vs[1]), (double)vs[1], rb(vs[2]), (double)vs[2]);
            }
        }
        if ((s_total % 200000u) == 0u)
            std::fprintf(stderr, "[clipdiag] total=%u disagreements=%u\n",
                         s_total, s_diff);
    }
    if (vuClipExactEnabled())
    {
        // PCSX2 _vuCLIP (d5f75c9e4): compare sign-magnitude BIT
        // PATTERNS as s32, not floats, with a denormal fixup on |w| —
        // a denormal or ZERO w is replaced by 0x007FFFFF, the largest
        // denormal, so that only non-denormals compare higher. Also
        // masks the register to its real 24 bits.
        //
        // MEASURED (T-P7-CLIPWHY, RRV_VU_CLIP_DIAG): the two forms
        // disagree on 200,011 of 75.6 M CLIPs (0.26 %), ALL at VU
        // pc 0x1FB0, and every one is the same shape —
        //     w = 0x00000000, y = 0x0000008C (a denormal)
        //     exact = 0x11   float = 0x15   (bit 0x04, Y-positive)
        // The float form clips a denormal component against a zero w
        // because 1.96e-43 > 0.0f; hardware does not, because the
        // fixup puts the bound at the top of the denormal range. So
        // we were reporting SPURIOUS clip bits, not missing them.
        //
        // NOT the cause, both falsified by measurement: NaN reaching
        // CLIP (RRV_VU_MAC_EXACT=1 removes Inf/NaN from vf and the
        // defect survives), and the missing 24-bit mask (every reader
        // — FCEQ/FCAND/FCOR/FCGET — masks to 24 bits already).
        auto bits = [](float f) {
            uint32_t b;
            std::memcpy(&b, &f, sizeof(b));
            return b;
        };
        int32_t value = (int32_t)bits(vt[3]);
        value = (value & 0x7F800000) ? (value & 0x7FFFFFFF) : 0x007FFFFF;
        for (int c = 0; c < 3; c++)
        {
            const uint32_t b = bits(vs[c]);
            if ((int32_t)(b ^ 0x00000000u) > value)
                flags |= (uint32_t)(0x01 << (c * 2));
            if ((int32_t)(b ^ 0x80000000u) > value)
                flags |= (uint32_t)(0x02 << (c * 2));
        }
        m_state.clip = ((m_state.clip << 6) | flags) & 0xFFFFFFu;
    }
    else
    {
        float w = std::fabs(vt[3]);
        if (vs[0] > +w)
            flags |= 0x01;
        if (vs[0] < -w)
            flags |= 0x02;
        if (vs[1] > +w)
            flags |= 0x04;
        if (vs[1] < -w)
            flags |= 0x08;
        if (vs[2] > +w)
            flags |= 0x10;
        if (vs[2] < -w)
            flags |= 0x20;
        m_state.clip = (m_state.clip << 6) | flags;
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
void VU1Interpreter::execUpper(uint32_t instr)
{
    // Default: an FMAC-arithmetic op refreshes the MAC flags. Non-arithmetic ops
    // (MAX/MINI/ITOF/FTOI/CLIP/move) clear this before their applyDest.
    m_updateMac = true;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    const float *vs = m_state.vf[fs];
    const float *vt = m_state.vf[ft];
    const float *accIn = m_state.acc;
    float result[4];

    // PCSX2 applies vuDouble ONLY to the FMAC arithmetic ops (_vuOpADD/_vuOpMUL/
    // _vuOpMADD/...). MAX/MINI go through applyMinMax<fp_max/fp_min>, which
    // compares raw bit patterns, and ITOF/FTOI/ABS/CLIP have their own integer
    // semantics — none of those flush. Flushing them too is a SUPERSET of the
    // hardware rule and measurably wrong: it moved the first divergent vertex
    // from 35,189 to 1 (T-P7-VUSEM step 1, first attempt).
    const bool denormOperands = vuDenormFlushEnabled();
    const bool clampOperands = vuOpClampEnabled();
    bool flushOperands = denormOperands || clampOperands;
    if (flushOperands)
    {
        if (op >= 0x3C)
        {
            const uint8_t op2 = (uint8_t)((instr & 3) | ((instr >> 4) & 0x7C));
            if ((op2 >= 0x10 && op2 <= 0x17) || op2 == 0x1D || op2 == 0x1F ||
                op2 == 0x2E || op2 == 0x2F)
                flushOperands = false;          // ITOF/FTOI/ABS/CLIP/NOP
        }
        else if ((op >= 0x10 && op <= 0x17) || op == 0x1D || op == 0x1F ||
                 op == 0x2B || op == 0x2F)
        {
            flushOperands = false;              // MAX*/MINI*
        }
    }

    float vsBuf[4], vtBuf[4], accBuf[4];
    if (flushOperands)
    {
        for (int c = 0; c < 4; c++)
        {
            vsBuf[c] = vs[c];
            vtBuf[c] = vt[c];
            accBuf[c] = m_state.acc[c];
            if (denormOperands)
            {
                vsBuf[c] = vuFlushDenorm(vsBuf[c]);
                vtBuf[c] = vuFlushDenorm(vtBuf[c]);
                accBuf[c] = vuFlushDenorm(accBuf[c]);
            }
            if (clampOperands)
            {
                vsBuf[c] = vuClampOperand(vsBuf[c]);
                vtBuf[c] = vuClampOperand(vtBuf[c]);
                accBuf[c] = vuClampOperand(accBuf[c]);
            }
        }
        vs = vsBuf;
        vt = vtBuf;
        accIn = accBuf;
    }

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] + vs[c] * bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] - vs[c] * bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        m_updateMac = false; // MAX does not affect MAC flags
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        m_updateMac = false; // MINI does not affect MAC flags
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.q;
        applyDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > m_state.i) ? vs[c] : m_state.i;
        m_updateMac = false; // MAXi does not affect MAC flags
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * m_state.i;
        applyDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < m_state.i) ? vs[c] : m_state.i;
        m_updateMac = false; // MINIi does not affect MAC flags
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.q;
        applyDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] + vs[c] * m_state.q;
        applyDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + m_state.i;
        applyDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] + vs[c] * m_state.i;
        applyDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.q;
        applyDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] - vs[c] * m_state.q;
        applyDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - m_state.i;
        applyDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] - vs[c] * m_state.i;
        applyDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] + vs[c] * vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        m_updateMac = false; // MAX does not affect MAC flags
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = accIn[c] - vs[c] * vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = m_state.acc[0] - vs[1] * vt[2];
        result[1] = m_state.acc[1] - vs[2] * vt[0];
        result[2] = m_state.acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        m_updateMac = false; // MINI does not affect MAC flags
        applyDest(vd, result, dest);
        return;

    // Special1 group (0x3C..0x3F with secondary field)
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        // The upper-special opcode is op2 = (instr & 3) | ((instr >> 4) & 0x7C),
        // shared across funct 0x3C-0x3F (the funct low 2 bits ARE the broadcast
        // selector). Decoding via (instr>>6)&0x1F (as before) mis-mapped MADDAbc
        // -> ADDAbc, MULAbc -> SUBAbc, ITOF/FTOI/CLIP -> SUBAbc and dropped every
        // bc=y/z/w variant -> the VU matrix multiply produced garbage (w=0).
        // See DobieStation upper_special / the PS2 VU ISA.
        {
            uint8_t op2 = (uint8_t)((instr & 0x3) | ((instr >> 4) & 0x7C));
            uint8_t bcsel = (uint8_t)(instr & 0x3); // broadcast component for bc ops
            switch (op2)
            {
            case 0x00:
            case 0x01:
            case 0x02:
            case 0x03: // ADDAbc
            {
                float bc = broadcast(vt, bcsel);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x04:
            case 0x05:
            case 0x06:
            case 0x07: // SUBAbc
            {
                float bc = broadcast(vt, bcsel);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x08:
            case 0x09:
            case 0x0A:
            case 0x0B: // MADDAbc
            {
                float bc = broadcast(vt, bcsel);
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] + vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x0C:
            case 0x0D:
            case 0x0E:
            case 0x0F: // MSUBAbc
            {
                float bc = broadcast(vt, bcsel);
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] - vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x10: // ITOF0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv;
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x11: // ITOF4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 16.0f;
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x12: // ITOF12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 4096.0f;
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x13: // ITOF15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv;
                    std::memcpy(&iv, &vs[c], 4);
                    result[c] = (float)iv / 32768.0f;
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x14: // FTOI0
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)vs[c];
                    std::memcpy(&result[c], &iv, 4);
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x15: // FTOI4
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 16.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x16: // FTOI12
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 4096.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x17: // FTOI15
                for (int c = 0; c < 4; c++)
                {
                    int32_t iv = (int32_t)(vs[c] * 32768.0f);
                    std::memcpy(&result[c], &iv, 4);
                }
                m_updateMac = false; // ITOF/FTOI do not affect MAC flags
                // ITOF/FTOI write the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x18:
            case 0x19:
            case 0x1A:
            case 0x1B: // MULAbc
            {
                float bc = broadcast(vt, bcsel);
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * bc;
                applyDestAcc(result, dest);
                return;
            }
            case 0x1C: // MULAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x1D: // ABS
                for (int c = 0; c < 4; c++)
                    result[c] = std::fabs(vs[c]);
                m_updateMac = false; // ABS does not affect MAC flags
                // ABS writes the ft field (bits 20:16), NOT fd.
                applyDest(m_state.vf[ft], result, dest);
                return;
            case 0x1E: // MULAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x1F: // CLIP
                clipOp(vs, vt);
                return;
            case 0x20: // ADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x21: // MADDAq
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] + vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x22: // ADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x23: // MADDAi
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] + vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x24: // SUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x25: // MSUBAq
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] - vs[c] * m_state.q;
                applyDestAcc(result, dest);
                return;
            case 0x26: // SUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x27: // MSUBAi
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] - vs[c] * m_state.i;
                applyDestAcc(result, dest);
                return;
            case 0x28: // ADDA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] + vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x29: // MADDA
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] + vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x2A: // MULA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x2C: // SUBA
                for (int c = 0; c < 4; c++)
                    result[c] = vs[c] - vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x2D: // MSUBA
                for (int c = 0; c < 4; c++)
                    result[c] = accIn[c] - vs[c] * vt[c];
                applyDestAcc(result, dest);
                return;
            case 0x2E: // OPMULA
                result[0] = vs[1] * vt[2];
                result[1] = vs[2] * vt[0];
                result[2] = vs[0] * vt[1];
                result[3] = 0.0f;
                applyDestAcc(result, dest);
                return;
            default: // 0x2F/0x30 NOP and any unmapped
                return;
            }
        }
        return;
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33: // iadd-like upper? No, these are valid upper ops
    default:
        // NOP / unimplemented upper
        return;
    }
}

// ============================================================================
// Optimised upper pipe — pre-decoded IR + host-agnostic SIMD
// ============================================================================
//
// Semantics are those of execUpper() above, which stays the reference.  What
// changes is only HOW they are computed:
//
//   * four lanes at a time through rrv::simd (NEON on ARM64, SSE2 on x86-64,
//     scalar everywhere else) instead of a 4-iteration scalar loop;
//   * the write-back mask and the MAC update are branchless selects instead of
//     four conditional stores and four per-lane branch chains;
//   * the opcode, its destination file and its MAC/flush properties come from
//     the pre-decoded rrv::vu::SlotIR instead of being re-derived per
//     execution, which also removes the nested upper-special sub-switch.
//
// NOT FUSED: every multiply-add below is a separate mul() and add(), because
// the VU's FMAC rounds the product before adding (see rrv_simd.h rule 1 and
// the -ffp-contract=off in ps2xRuntime/CMakeLists.txt).  MAX/MINI use
// rrv::simd::max_c/min_c, which are C's `(a > b) ? a : b`, NOT the host's
// max/min instruction (rule 2).
//
// Verified equivalent instruction-by-instruction against execUpper() with
// RRV_VU_VERIFY=1 — docs/TESTING.md T-VU-FASTPATH.

namespace
{
    // The denormal flush of rule "PCSX2 vuDouble", four lanes at a time:
    // exponent 0 (denormal OR zero) collapses to the lane's signed zero.
    // Only reachable when RRV_VU_DENORM is on (default off).
    RRV_SIMD_INLINE rrv::simd::f32x4 flushDenormV(rrv::simd::f32x4 a)
    {
        using namespace rrv::simd;
        const u32x4 u = as_u(a);
        const u32x4 isDenOrZero = cmpeq_u(and_u(u, splat_u(0x7F800000u)), splat_u(0u));
        return as_f(select_u(isDenOrZero, and_u(u, splat_u(0x80000000u)), u));
    }

    // RRV_VU_OPCLAMP, four lanes at a time: exponent 255 -> sign|0x7F7FFFFF.
    RRV_SIMD_INLINE rrv::simd::f32x4 clampOperandV(rrv::simd::f32x4 a)
    {
        using namespace rrv::simd;
        const u32x4 u = as_u(a);
        const u32x4 isInf = cmpeq_u(and_u(u, splat_u(0x7F800000u)), splat_u(0x7F800000u));
        return as_f(select_u(isInf, or_u(and_u(u, splat_u(0x80000000u)), splat_u(0x7F7FFFFFu)), u));
    }
} // namespace

RRV_SIMD_INLINE rrv::simd::f32x4 VU1Interpreter::macUpdateV(rrv::simd::f32x4 result, uint8_t dest)
{
    return macUpdateVT<true>(result, dest);
}

template <bool KWORD>
RRV_SIMD_INLINE rrv::simd::f32x4 VU1Interpreter::macUpdateVT(rrv::simd::f32x4 result, uint8_t dest)
{
    using namespace rrv::simd;

    // Lane classification, exactly the order vuMacUpdateComponent() tests in:
    //   S  = sign bit set (independent of the zero/denormal/Inf tests)
    //   Z  = |v| == 0  OR  denormal (the denormal is flushed to zero, so it is
    //        reported as zero as well as underflow)
    //   U  = denormal (exponent 0, non-zero mantissa)
    //   O  = exponent 255 (Inf/NaN; the VU has neither, so it saturates)
    const u32x4 u = as_u(result);
    const u32x4 sign = and_u(u, splat_u(0x80000000u));
    const u32x4 absu = and_u(u, splat_u(0x7FFFFFFFu));
    const u32x4 expo = shr_u<23>(absu);

    const u32x4 isNeg = cmpgt_s(splat_u(0u), u); // (s32)v < 0  <=>  sign set
    const u32x4 isZero = cmpeq_u(absu, splat_u(0u));
    const u32x4 isDen = andnot_u(isZero, cmpeq_u(expo, splat_u(0u)));
    const u32x4 isInf = cmpeq_u(expo, splat_u(0xFFu));

    // The value the VU actually STORES: a denormal becomes its signed zero, an
    // exp==255 result is clamped to +/-0x7f7fffff, everything else is untouched
    // (a true zero keeps its sign because it is left alone).
    u32x4 val = select_u(isDen, sign, u);
    val = select_u(isInf, or_u(sign, splat_u(0x7F7FFFFFu)), val);

    // RRV_VU_AOT, kAotNoMacWord: the block's analysis proved that the next MAC
    // writer comes before any reader of this word. The stored value is the
    // same; only m_state.mac is left as it was.
    if (!KWORD)
        return as_f(val);

    // MAC word.  The VU puts lane x in the HIGH bit of every nibble (Z at 3-c,
    // S at 7-c, U at 11-c, O at 15-c), which is also the dest field's order, so
    // the dest mask applies directly once it is replicated into all four
    // nibbles.
    //
    // ONE reduction, not four.  The obvious form is four mask_bits_rev() calls
    // ORed together with shifts, but each of those is an `addv` plus a
    // vector-to-GPR move, and this runs on every FMAC.  Weighting each mask
    // with the bit positions it owns makes the four results disjoint, so ORing
    // the weighted vectors and reducing once gives exactly the same word.
    static const uint32_t kWz[4] = {0x0008u, 0x0004u, 0x0002u, 0x0001u};
    static const uint32_t kWs[4] = {0x0080u, 0x0040u, 0x0020u, 0x0010u};
    static const uint32_t kWu[4] = {0x0800u, 0x0400u, 0x0200u, 0x0100u};
    static const uint32_t kWo[4] = {0x8000u, 0x4000u, 0x2000u, 0x1000u};
    const u32x4 weighted =
        or_u(or_u(and_u(or_u(isZero, isDen), loadu_u(kWz)), and_u(isNeg, loadu_u(kWs))),
             or_u(and_u(isDen, loadu_u(kWu)), and_u(isInf, loadu_u(kWo))));
    const uint32_t d = dest & 0xFu;
    m_state.mac = reduce_add_u32(weighted) & (d | (d << 4) | (d << 8) | (d << 12));

    return as_f(val);
}

void VU1Interpreter::setMacFlagsV(rrv::simd::f32x4 result, uint8_t dest)
{
    using namespace rrv::simd;
    // Same single-reduction form as macUpdateV(): weight, OR, reduce once.
    const u32x4 u = as_u(result);
    static const uint32_t kWz[4] = {0x0008u, 0x0004u, 0x0002u, 0x0001u};
    static const uint32_t kWs[4] = {0x0080u, 0x0040u, 0x0020u, 0x0010u};
    const u32x4 weighted =
        or_u(and_u(cmpeq_u(and_u(u, splat_u(0x7FFFFFFFu)), splat_u(0u)), loadu_u(kWz)),
             and_u(cmpgt_s(splat_u(0u), u), loadu_u(kWs)));
    const uint32_t d = dest & 0xFu;
    m_state.mac = reduce_add_u32(weighted) & (d | (d << 4));
}

template <int KCFG, unsigned KOPT>
RRV_SIMD_INLINE void VU1Interpreter::applyDestVT(float *dst, rrv::simd::f32x4 result, uint8_t dest,
                                bool updatesMac)
{
    using namespace rrv::simd;
    if (updatesMac)
    {
        if (KCFG >= 0 ? true : m_cfgMacExact)
            result = macUpdateVT<(KOPT & kAotNoMacWord) == 0u>(result, dest);
        else
            setMacFlagsV(result, dest);
    }
    // Masked write-back with no branches: read the destination, blend in the
    // dest lanes, write all four back.  Rewriting the untouched lanes with
    // their own value is what makes this branchless, and the VU register file
    // is only ever touched from this thread.
    storeu(dst, select(dest_mask(dest), result, loadu(dst)));
}

void VU1Interpreter::applyDestV(float *dst, rrv::simd::f32x4 result, uint8_t dest, bool updatesMac)
{
    applyDestVT<-1>(dst, result, dest, updatesMac);
}

template <int KUOP, unsigned KOPT>
RRV_SIMD_INLINE void VU1Interpreter::execUpperFastT(const rrv::vu::SlotIR &ir)
{
    using namespace rrv::simd;
    using namespace rrv::vu;
    // RRV_VU_AOT: a constant KUOP selects one case at compile time; -1 is the
    // interpreter's run-time dispatch. Everything below is unchanged.
    const uint8_t uop = KUOP >= 0 ? static_cast<uint8_t>(KUOP) : ir.uop;
    constexpr bool kAotCfg = KUOP >= 0;

    if (uop == U_NOP)
        return;

    const float *vsP = m_state.vf[ir.fs];
    const float *vtP = m_state.vf[ir.ft];
    const float *accP = m_state.acc;

    // RRV_VU_DENORM (default off): PCSX2 routes FMAC operands through
    // vuDouble() on READ.  Spilling the flushed operands to a local buffer
    // keeps the runtime-indexed broadcast (`bc`) a plain scalar load, exactly
    // as the reference path reads vtBuf[bc].
    float fbuf[12];
    if (((kAotCfg ? false : m_cfgDenorm) || (kAotCfg ? true : vuOpClampEnabled())) && (ir.uflags & UF_FLUSH))
    {
        rrv::simd::f32x4 a = loadu(vsP), b = loadu(vtP), c = loadu(accP);
        if ((kAotCfg ? false : m_cfgDenorm))
        {
            a = flushDenormV(a);
            b = flushDenormV(b);
            c = flushDenormV(c);
        }
        if ((kAotCfg ? true : vuOpClampEnabled()))
        {
            // KOPT: the lanes this op reads from that operand cannot hold an
            // exponent of 255 (see AotOpt), so the clamp would return them
            // unchanged; the other lanes never reach the destination.
            if ((KOPT & kAotCleanVs) == 0u)
                a = clampOperandV(a);
            if ((KOPT & kAotCleanVt) == 0u)
                b = clampOperandV(b);
            if ((KOPT & kAotCleanAcc) == 0u)
                c = clampOperandV(c);
        }
        storeu(fbuf + 0, a);
        storeu(fbuf + 4, b);
        storeu(fbuf + 8, c);
        vsP = fbuf + 0;
        vtP = fbuf + 4;
        accP = fbuf + 8;
    }

    if (uop == U_CLIP)
    {
        clipOp(vsP, vtP);
        return;
    }

    const f32x4 vs = loadu(vsP);
    f32x4 r;

    switch (uop)
    {
    // ---- broadcast forms -------------------------------------------------
    case U_ADDbc:   r = add(vs, dup_lane(vtP, ir.bc)); break;
    case U_SUBbc:   r = sub(vs, dup_lane(vtP, ir.bc)); break;
    case U_MADDbc:  r = add(loadu(accP), mul(vs, dup_lane(vtP, ir.bc))); break;
    case U_MSUBbc:  r = sub(loadu(accP), mul(vs, dup_lane(vtP, ir.bc))); break;
    case U_MAXbc:   r = max_c(vs, dup_lane(vtP, ir.bc)); break;
    case U_MINIbc:  r = min_c(vs, dup_lane(vtP, ir.bc)); break;
    case U_MULbc:   r = mul(vs, dup_lane(vtP, ir.bc)); break;
    case U_ADDAbc:  r = add(vs, dup_lane(vtP, ir.bc)); break;
    case U_SUBAbc:  r = sub(vs, dup_lane(vtP, ir.bc)); break;
    case U_MADDAbc: r = add(loadu(accP), mul(vs, dup_lane(vtP, ir.bc))); break;
    case U_MSUBAbc: r = sub(loadu(accP), mul(vs, dup_lane(vtP, ir.bc))); break;
    case U_MULAbc:  r = mul(vs, dup_lane(vtP, ir.bc)); break;

    // ---- I / Q register forms --------------------------------------------
    case U_MULq:    case U_MULAq:  r = mul(vs, splat(m_state.q)); break;
    case U_MULi:    case U_MULAi:  r = mul(vs, splat(m_state.i)); break;
    case U_MAXi:    r = max_c(vs, splat(m_state.i)); break;
    case U_MINIi:   r = min_c(vs, splat(m_state.i)); break;
    case U_ADDq:    case U_ADDAq:  r = add(vs, splat(m_state.q)); break;
    case U_ADDi:    case U_ADDAi:  r = add(vs, splat(m_state.i)); break;
    case U_SUBq:    case U_SUBAq:  r = sub(vs, splat(m_state.q)); break;
    case U_SUBi:    case U_SUBAi:  r = sub(vs, splat(m_state.i)); break;
    case U_MADDq:   case U_MADDAq: r = add(loadu(accP), mul(vs, splat(m_state.q))); break;
    case U_MADDi:   case U_MADDAi: r = add(loadu(accP), mul(vs, splat(m_state.i))); break;
    case U_MSUBq:   case U_MSUBAq: r = sub(loadu(accP), mul(vs, splat(m_state.q))); break;
    case U_MSUBi:   case U_MSUBAi: r = sub(loadu(accP), mul(vs, splat(m_state.i))); break;

    // ---- full-vector forms -----------------------------------------------
    case U_ADD:     case U_ADDA:   r = add(vs, loadu(vtP)); break;
    case U_SUB:     case U_SUBA:   r = sub(vs, loadu(vtP)); break;
    case U_MUL:     case U_MULA:   r = mul(vs, loadu(vtP)); break;
    case U_MADD:    case U_MADDA:  r = add(loadu(accP), mul(vs, loadu(vtP))); break;
    case U_MSUB:    case U_MSUBA:  r = sub(loadu(accP), mul(vs, loadu(vtP))); break;
    case U_MAX:     r = max_c(vs, loadu(vtP)); break;
    case U_MINI:    r = min_c(vs, loadu(vtP)); break;

    // ---- outer-product forms ---------------------------------------------
    // Left scalar on purpose: these are lane permutations, and a portable
    // 4-lane shuffle is a bigger primitive than one op earns.  They keep the
    // reference path's exact expressions, including OPMSUB reading the RAW
    // accumulator rather than the (optionally denormal-flushed) copy.
    case U_OPMSUB:
    {
        float t[4];
        t[0] = m_state.acc[0] - vsP[1] * vtP[2];
        t[1] = m_state.acc[1] - vsP[2] * vtP[0];
        t[2] = m_state.acc[2] - vsP[0] * vtP[1];
        t[3] = 0.0f;
        r = loadu(t);
        break;
    }
    case U_OPMULA:
    {
        float t[4];
        t[0] = vsP[1] * vtP[2];
        t[1] = vsP[2] * vtP[0];
        t[2] = vsP[0] * vtP[1];
        t[3] = 0.0f;
        r = loadu(t);
        break;
    }

    // ---- integer <-> float -----------------------------------------------
    case U_ITOF0:   r = cvt_s32_to_f32(as_u(vs)); break;
    case U_ITOF4:   r = div(cvt_s32_to_f32(as_u(vs)), splat(16.0f)); break;
    case U_ITOF12:  r = div(cvt_s32_to_f32(as_u(vs)), splat(4096.0f)); break;
    case U_ITOF15:  r = div(cvt_s32_to_f32(as_u(vs)), splat(32768.0f)); break;
    case U_FTOI0:   r = as_f(cvt_f32_to_s32_trunc(vs)); break;
    case U_FTOI4:   r = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(16.0f)))); break;
    case U_FTOI12:  r = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(4096.0f)))); break;
    case U_FTOI15:  r = as_f(cvt_f32_to_s32_trunc(mul(vs, splat(32768.0f)))); break;
    case U_ABS:     r = abs_f(vs); break;

    default:
        return; // unmapped: the reference path returns without side effects
    }

    float *dst = (ir.udst == UD_ACC) ? m_state.acc
               : (ir.udst == UD_FT)  ? m_state.vf[ir.ft]
                                     : m_state.vf[ir.fd];
    applyDestVT<KUOP, KOPT>(dst, r, ir.dest, (ir.uflags & UF_MAC) != 0u);
}

void VU1Interpreter::execUpperFast(const rrv::vu::SlotIR &ir)
{
    execUpperFastT<-1>(ir);
}

void VU1Interpreter::execUpperSlot(const rrv::vu::SlotIR &ir, uint32_t upper)
{
    if (RRV_VU_UNLIKELY(m_cfgVerify))
    {
        // RRV_VU_VERIFY: run the scalar reference, keep what it produced,
        // rewind, run the fast path, and compare the architectural state.
        // m_updateMac is deliberately NOT compared: it is the scalar path's
        // internal carry from execUpper() to applyDest(), and the fast path
        // carries the same information in the IR's UF_MAC instead.  Nothing
        // outside execUpper() reads it — execLower() assigns it before use.
        const VU1State before = m_state;
        const uint32_t shadowBefore = m_clipShadow;
        execUpper(upper);
        const VU1State expected = m_state;
        m_state = before;
        m_clipShadow = shadowBefore;
        execUpperFast(ir);
        if (RRV_VU_UNLIKELY((++m_verifyChecked & 0xFFFFFFull) == 0ull))
            std::fprintf(stderr, "[vu-verify] %s: %llu M slots checked, %llu mismatches\n",
                         m_isVu0 ? "vu0" : "vu1",
                         (unsigned long long)(m_verifyChecked >> 20),
                         (unsigned long long)m_verifyMismatches);
        const bool same =
            m_state.mac == expected.mac && m_state.status == expected.status &&
            m_state.clip == expected.clip &&
            std::memcmp(m_state.vf, expected.vf, sizeof(m_state.vf)) == 0 &&
            std::memcmp(m_state.acc, expected.acc, sizeof(m_state.acc)) == 0;
        if (RRV_VU_UNLIKELY(!same))
            reportFastPathMismatch(ir, expected);
        return;
    }
    if (m_cfgFast)
        execUpperFast(ir);
    else
        execUpper(upper);
}

void VU1Interpreter::reportFastPathMismatch(const rrv::vu::SlotIR &ir,
                                            const VU1State &expected)
{
    static uint32_t s_reported = 0u;
    ++m_verifyMismatches;
    if (s_reported >= 32u)
        return;
    ++s_reported;
    std::fprintf(stderr,
                 "[vu-verify] MISMATCH #%llu pc=%04x upper=%08x lower=%08x uop=%u dest=%x "
                 "fs=%u ft=%u fd=%u bc=%u\n",
                 (unsigned long long)m_verifyMismatches, m_state.pc, ir.upper, ir.lower,
                 (unsigned)ir.uop, (unsigned)ir.dest, (unsigned)ir.fs, (unsigned)ir.ft,
                 (unsigned)ir.fd, (unsigned)ir.bc);
    for (uint32_t reg = 0; reg < 32u; ++reg)
    {
        if (std::memcmp(m_state.vf[reg], expected.vf[reg], 16) != 0)
        {
            const auto *g = reinterpret_cast<const uint32_t *>(m_state.vf[reg]);
            const auto *e = reinterpret_cast<const uint32_t *>(expected.vf[reg]);
            std::fprintf(stderr,
                         "[vu-verify]   vf%02u fast=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                         reg, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
        }
    }
    if (std::memcmp(m_state.acc, expected.acc, 16) != 0)
    {
        const auto *g = reinterpret_cast<const uint32_t *>(m_state.acc);
        const auto *e = reinterpret_cast<const uint32_t *>(expected.acc);
        std::fprintf(stderr,
                     "[vu-verify]   acc  fast=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x\n",
                     g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
    }
    if (m_state.mac != expected.mac || m_state.status != expected.status ||
        m_state.clip != expected.clip)
        std::fprintf(stderr,
                     "[vu-verify]   flags fast mac=%08x status=%08x clip=%06x | "
                     "ref mac=%08x status=%08x clip=%06x\n",
                     m_state.mac, m_state.status, m_state.clip,
                     expected.mac, expected.status, expected.clip);
}

// ============================================================================
// Lower instructions
// ============================================================================
// DIAG-TEMP (T-P7-CLIPWHO): a CENSUS of every CLIP reader, not just the first
// one after a disagreeing CLIP. The window-based version answered the wrong
// question -- it found FCAND@1FD0 imm24=0x30 (Z bits only), which masks off the
// Y bit the two forms disagree on, so it never diverges. The bits accumulate and
// shift up 6 per CLIP, so the reader that MATTERS can be far away and use a
// different mask. Keyed by (reader pc, kind, imm24); reports how often the two
// registers give that reader a different VI[1].
void VU1Interpreter::clipWho(const char *kind, uint32_t imm24, uint32_t sel,
                             uint32_t shadow, uint32_t (*fn)(uint32_t, uint32_t))
{
    struct Row { const char *kind; uint64_t total, diverge; uint32_t lastSel, lastOther; };
    static std::map<uint64_t, Row> s_census;
    static uint64_t s_calls = 0;

    const uint32_t a = fn(sel, imm24), b = fn(shadow, imm24);
    const uint64_t key = ((uint64_t)m_state.pc << 32) | imm24;
    Row &r = s_census[key];
    r.kind = kind;
    ++r.total;
    if (a != b)
    {
        ++r.diverge;
        r.lastSel = sel & 0xFFFFFFu;
        r.lastOther = shadow & 0xFFFFFFu;
        // Dump the program this READER lives in -- it is not necessarily the one
        // that ran the CLIP, because the 24-bit register survives across MSCALs.
        static bool s_dumped = false;
        if (!s_dumped && m_codeImage && m_codeSize)
        {
            s_dumped = true;
            char path[256];
            std::snprintf(path, sizeof(path), "/tmp/vu1_reader_%04X_entry%04X.code",
                          m_state.pc, m_entryPC);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(m_codeImage, 1, m_codeSize, f);
                std::fclose(f);
                std::fprintf(stderr,
                    "[clipwho] DIVERGENCE at %s@%04X imm24=%06X (entry %04X): "
                    "sel=%06X->VI1=%u other=%06X->VI1=%u; dumped -> %s\n",
                    kind, m_state.pc, imm24, m_entryPC, sel & 0xFFFFFFu, a,
                    shadow & 0xFFFFFFu, b, path);
            }
        }
    }

    if ((++s_calls % 2000000ull) == 0ull)
    {
        std::fprintf(stderr, "[clipcensus] ---- after %llu reads ----\n",
                     (unsigned long long)s_calls);
        for (const auto &kv : s_census)
        {
            const Row &row = kv.second;
            std::fprintf(stderr,
                         "[clipcensus] %s@%04X imm24=%06X total=%llu diverge=%llu (%.4f%%)%s\n",
                         row.kind, (uint32_t)(kv.first >> 32), (uint32_t)(kv.first & 0xFFFFFFu),
                         (unsigned long long)row.total, (unsigned long long)row.diverge,
                         row.total ? 100.0 * (double)row.diverge / (double)row.total : 0.0,
                         row.diverge ? "   <<< CONSUMES THE DIFFERENCE" : "");
            if (row.diverge)
                std::fprintf(stderr, "[clipcensus]     last divergent pair: sel=%06X other=%06X\n",
                             row.lastSel, row.lastOther);
        }
    }
}

template <int KOP, int KFUNCT, int KF2>
RRV_SIMD_INLINE void VU1Interpreter::execLowerT(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    (void)upperInstr;
    // The lower pipe never updates the MAC flags; clear the carry-over from
    // execUpper so MOVE/MFIR/LQ-result applyDest calls leave the flags intact.
    m_updateMac = false;
    if (instr == 0x00000000 || instr == 0x8000033C) // NOP
        return;

    // RRV_VU_AOT: constant template fields select one case at compile time;
    // -1 is the interpreter's run-time decode. Everything else is unchanged.
    constexpr bool kAotCfg = KOP >= 0;
    uint8_t opHi = KOP >= 0 ? static_cast<uint8_t>(KOP) : static_cast<uint8_t>((instr >> 25) & 0x7F);
    // One predicted-not-taken test stands for every per-instruction diagnostic
    // in this function, exactly as it does in run(): (kAotCfg ? false : m_cfgAnyDiag) is a superset
    // of s_p7TraceRun, resolved once per run() call.
    if (RRV_VU_UNLIKELY((kAotCfg ? false : m_cfgAnyDiag)) && s_p7TraceRun == 1 && m_state.pc <= 0x278u &&
        opHi >= 0x10u && opHi <= 0x1Cu)
        std::fprintf(stderr, "[p7-vu-flag-op] pc=%04x op=%02x vi1=%04x mac=%04x clip=%06x status=%03x\n",
                     m_state.pc, opHi, static_cast<uint32_t>(m_state.vi[1]) & 0xffffu,
                     m_state.mac & 0xffffu, m_state.clip & 0xffffffu, m_state.status & 0xfffu);

    // B-5: when the flag-visibility pipeline model is enabled, the flag-reader
    // ops below (FMAND/FMEQ/FMOR, FSAND/FSEQ/FSOR, FCAND/FCEQ/FCOR) consult
    // m_flagVisible (the snapshot from 4 slots ago, set in run()) instead of
    // the live m_state.mac/status/clip. Flag WRITERS (FCSET/FSSET, and CLIP /
    // FMAC ops in execUpper) are untouched — only reader visibility timing
    // changes, not flag computation.
    //
    // Read from the member resolved once per run() call rather than re-entering
    // the getenv-cached static: this function is called for roughly two thirds
    // of all instruction slots, and only a handful of its cases use the value.
    const bool pipelineOn = (kAotCfg ? true : m_cfgPipeline);

    // The lower instruction encoding uses bits 31:25 for the primary opcode
    switch (opHi)
    {
    case 0x00: // LQ (Load Quadword from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is & 0xF] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDestNoMac(m_state.vf[it], tmp, dest);
        }
        return;
    }
    case 0x01: // SQ (Store Quadword to VU data memory)
    {
        uint8_t is = LIS(instr);
        uint8_t it = LIT(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[it & 0xF] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            if (dest & 0x8)
                tmp[0] = m_state.vf[is][0];
            if (dest & 0x4)
                tmp[1] = m_state.vf[is][1];
            if (dest & 0x2)
                tmp[2] = m_state.vf[is][2];
            if (dest & 0x1)
                tmp[3] = m_state.vf[is][3];
            std::memcpy(vuData + addr, tmp, 16);
        }
        return;
    }
    case 0x04: // ILW (Integer Load Word from VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is & 0xF] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            int comp = 0;
            if (dest & 0x8)
                comp = 0;
            else if (dest & 0x4)
                comp = 1;
            else if (dest & 0x2)
                comp = 2;
            else
                comp = 3;
            uint32_t v;
            std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (it != 0)
                m_state.vi[it & 0xF] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return;
    }
    case 0x05: // ISW (Integer Store Word to VU data memory)
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is & 0xF] + imm)) * 16u;
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t val = (uint32_t)(uint16_t)(m_state.vi[it & 0xF] & 0xFFFF);
            if (dest & 0x8)
                std::memcpy(vuData + addr + 0, &val, 4);
            if (dest & 0x4)
                std::memcpy(vuData + addr + 4, &val, 4);
            if (dest & 0x2)
                std::memcpy(vuData + addr + 8, &val, 4);
            if (dest & 0x1)
                std::memcpy(vuData + addr + 12, &val, 4);
        }
        return;
    }
    case 0x08: // IADDIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it & 0xF] = (int16_t)(m_state.vi[is & 0xF] + imm);
        return;
    }
    case 0x09: // ISUBIU
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it & 0xF] = (int16_t)(m_state.vi[is & 0xF] - imm);
        return;
    }
    case 0x10: // FCEQ (clip reader)
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        uint32_t clipVal = pipelineOn ? m_flagVisible.clip : m_state.clip;
        if (1 != 0)
            m_state.vi[1] = ((clipVal & 0xFFFFFF) == imm24) ? 1 : 0;
        // T-P7-CLIPWHO census — a std::map lookup per clip read, so it is
        // armed with the diagnostic that feeds it (RRV_VU_CLIP_DIAG) instead
        // of running in every shipping frame. Its `shadow` input is only
        // maintained under that same flag, so ungated it would also be
        // counting a register nobody built.
        if (RRV_VU_UNLIKELY((kAotCfg ? false : m_cfgClipDiag)))
            clipWho("FCEQ", imm24, clipVal,
                       pipelineOn ? m_clipShadowVisible : m_clipShadow,
                       [](uint32_t c, uint32_t k) { return ((c & 0xFFFFFF) == k) ? 1u : 0u; });
        return;
    }
    case 0x11: // FCSET (writer — instant, unaffected)
    {
        m_state.clip = instr & 0xFFFFFF;
        m_clipShadow = m_state.clip;   // DIAG (T-P7-CLIPWHO): keep the shadow in
                                       // step, or every group above the newest
                                       // one drifts and the census reports
                                       // divergences the hardware never sees.
        return;
    }
    case 0x12: // FCAND (clip reader)
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        uint32_t clipVal = pipelineOn ? m_flagVisible.clip : m_state.clip;
        if (1 != 0)
            m_state.vi[1] = ((clipVal & imm24) != 0) ? 1 : 0;
        // T-P7-CLIPWHO census — a std::map lookup per clip read, so it is
        // armed with the diagnostic that feeds it (RRV_VU_CLIP_DIAG) instead
        // of running in every shipping frame. Its `shadow` input is only
        // maintained under that same flag, so ungated it would also be
        // counting a register nobody built.
        if (RRV_VU_UNLIKELY((kAotCfg ? false : m_cfgClipDiag)))
            clipWho("FCAND", imm24, clipVal,
                       pipelineOn ? m_clipShadowVisible : m_clipShadow,
                       [](uint32_t c, uint32_t k) { return ((c & k) != 0) ? 1u : 0u; });
        return;
    }
    case 0x13: // FCOR (clip reader)
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        uint32_t clipVal = pipelineOn ? m_flagVisible.clip : m_state.clip;
        if (1 != 0)
            m_state.vi[1] = ((clipVal | imm24) == 0xFFFFFF) ? 1 : 0;
        // T-P7-CLIPWHO census — a std::map lookup per clip read, so it is
        // armed with the diagnostic that feeds it (RRV_VU_CLIP_DIAG) instead
        // of running in every shipping frame. Its `shadow` input is only
        // maintained under that same flag, so ungated it would also be
        // counting a register nobody built.
        if (RRV_VU_UNLIKELY((kAotCfg ? false : m_cfgClipDiag)))
            clipWho("FCOR", imm24, clipVal,
                       pipelineOn ? m_clipShadowVisible : m_clipShadow,
                       [](uint32_t c, uint32_t k) { return (((c & 0xFFFFFF) | k) == 0xFFFFFF) ? 1u : 0u; });
        return;
    }
    case 0x14: // FSEQ (status reader)
    {
        uint16_t imm12 = instr & 0xFFF;
        uint32_t statusVal = pipelineOn ? m_flagVisible.status : m_state.status;
        if (1 != 0)
            m_state.vi[1] = ((statusVal & 0xFFF) == imm12) ? 1 : 0;
        return;
    }
    case 0x15: // FSSET (writer — instant, unaffected)
    {
        m_state.status = (instr >> 6) & 0xFC0;
        return;
    }
    case 0x16: // FSAND (status reader)
    {
        uint16_t imm12 = instr & 0xFFF;
        uint32_t statusVal = pipelineOn ? m_flagVisible.status : m_state.status;
        if (1 != 0)
            m_state.vi[1] = (int32_t)(statusVal & imm12);
        return;
    }
    case 0x17: // FSOR (status reader)
    {
        uint16_t imm12 = instr & 0xFFF;
        uint32_t statusVal = pipelineOn ? m_flagVisible.status : m_state.status;
        if (1 != 0)
            m_state.vi[1] = ((statusVal | imm12) == 0xFFF) ? 1 : 0;
        return;
    }
    case 0x18: // FMEQ (mac reader) — B-5 decode fix: this row was previously
               // (wrongly) executing FMAND's AND semantics; the VU lower
               // opcode table (bits 31:25) has 0x18=FMEQ, 0x1A=FMAND,
               // 0x1B=FMOR, 0x1C=FCGET — our decode had FMAND/FMEQ swapped
               // and was missing FMOR/FCGET entirely (aliased FMOR onto 0x1C
               // and had no case for the real 0x1C at all). See
               // docs/HANDOFF_BOOT_NAMCO_WHITE.md.
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint32_t macVal = pipelineOn ? m_flagVisible.mac : m_state.mac;
        if (it != 0)
            m_state.vi[it & 0xF] = ((macVal & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is & 0xF]) ? 1 : 0;
        return;
    }
    case 0x1A: // FMAND (mac reader) — the boot VU0 white-payload branch gate
               // is here: byte PC 0x2D0, VU0 instance only, `FMAND vi10,vi10`.
               // RRV_VU0_FMAND_DIAG (B-5, default off): logs instant vs.
               // 4-slots-ago MAC and both would-be AND results, so we can
               // confirm the branch direction flips under the delayed-flags
               // model with the NOW-correct AND semantics (the previous probe
               // here tested EQ semantics at this decode slot, before the
               // FMAND/FMEQ swap was found — the opcode at 0x2D0 was always
               // really FMAND; our table had it filed under 0x1A).
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        if (m_isVu0 && m_state.pc == 0x2D0u && (kAotCfg ? false : vu0FmandDiagEnabled()))
        {
            static const auto s_fmandStart = std::chrono::steady_clock::now();
            static std::atomic<uint32_t> s_fmandTotal{0};
            static std::atomic<uint32_t> s_fmandTotalWhite{0}; // entryPC==0x20 subset
            static std::atomic<uint32_t> s_fmandInstantNz{0};
            static std::atomic<uint32_t> s_fmandDelayedNz{0};
            static std::atomic<uint32_t> s_fmandFlips{0};
            static std::atomic<uint32_t> s_fmandFlipsWhite{0};
            static std::atomic<uint32_t> s_fmandFlipLines{0};
            static std::atomic<uint32_t> s_fmandLastSummarySec{0xFFFFFFFFu};
            uint16_t mask = (uint16_t)m_state.vi[is & 0xF];
            uint32_t instantMac = m_state.mac;
            uint32_t delayedMac = m_flagVisible.mac;
            uint32_t instantResult = instantMac & (uint32_t)mask;
            uint32_t delayedResult = delayedMac & (uint32_t)mask;
            uint32_t total = s_fmandTotal.fetch_add(1, std::memory_order_relaxed) + 1;
            const double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_fmandStart).count();
            const bool isWhiteEntry = (m_entryPC == 0x20u);
            if (isWhiteEntry)
                s_fmandTotalWhite.fetch_add(1, std::memory_order_relaxed);
            if (instantResult != 0u)
                s_fmandInstantNz.fetch_add(1, std::memory_order_relaxed);
            if (delayedResult != 0u)
                s_fmandDelayedNz.fetch_add(1, std::memory_order_relaxed);
            if ((instantResult != 0u) != (delayedResult != 0u))
            {
                s_fmandFlips.fetch_add(1, std::memory_order_relaxed);
                if (isWhiteEntry)
                    s_fmandFlipsWhite.fetch_add(1, std::memory_order_relaxed);
                if (s_fmandFlipLines.fetch_add(1, std::memory_order_relaxed) < 40u)
                {
                    std::fprintf(stderr,
                                 "[vu0fmand] FLIP t=%.2fs total=%u entryPC=0x%02x pc=0x%03x instant_mac=0x%02x delayed_mac(4slot)=0x%02x "
                                 "vi[%u]_mask=0x%04x instant_result=0x%02x delayed_result=0x%02x\n",
                                 elapsedSec, total, m_entryPC, m_state.pc, instantMac, delayedMac, (unsigned)(is & 0xF), mask,
                                 instantResult, delayedResult);
                }
            }
            // One summary line per elapsed second (finer than count-based, so we
            // can correlate flip onset against wall-clock / captured frame index).
            uint32_t sec = (uint32_t)elapsedSec;
            uint32_t prevSec = s_fmandLastSummarySec.exchange(sec, std::memory_order_relaxed);
            if (sec != prevSec)
            {
                std::fprintf(stderr,
                             "[vu0fmand] SUMMARY t=%.2fs total=%u total_white(entryPC=0x20)=%u instant_nz=%u delayed_nz=%u "
                             "flips=%u flips_white=%u last_entryPC=0x%02x last_instant_mac=0x%02x last_delayed_mac=0x%02x\n",
                             elapsedSec, total, s_fmandTotalWhite.load(std::memory_order_relaxed),
                             s_fmandInstantNz.load(std::memory_order_relaxed),
                             s_fmandDelayedNz.load(std::memory_order_relaxed),
                             s_fmandFlips.load(std::memory_order_relaxed),
                             s_fmandFlipsWhite.load(std::memory_order_relaxed),
                             m_entryPC, instantMac, delayedMac);
            }
        }
        uint32_t macVal = pipelineOn ? m_flagVisible.mac : m_state.mac;
        if (it != 0)
            m_state.vi[it & 0xF] = (int32_t)(macVal & (uint32_t)(uint16_t)m_state.vi[is & 0xF]);
        return;
    }
    case 0x1B: // FMOR (mac reader) — was missing entirely (previously aliased
               // onto the real 0x1C slot below).
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint32_t macVal = pipelineOn ? m_flagVisible.mac : m_state.mac;
        if (it != 0)
            m_state.vi[it & 0xF] = (int32_t)(macVal | (uint32_t)(uint16_t)m_state.vi[is & 0xF]);
        return;
    }
    case 0x1C: // FCGET (clip reader) — was missing entirely (this slot
               // previously ran FMOR's semantics by mistake).
    {
        uint8_t it = LIT(instr);
        uint32_t clipVal = pipelineOn ? m_flagVisible.clip : m_state.clip;
        if (it != 0)
            m_state.vi[it & 0xF] = (int32_t)(clipVal & 0xFFFu);
        return;
    }
    case 0x20: // B (unconditional branch)
    {
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        // Simplified branch delay: set PC so next iteration lands on target
        m_state.branchPending = true;
        m_state.branchTarget = target;
        return;
    }
    case 0x21: // BAL (Branch and link)
    {
        uint8_t it = LIT(instr);
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
        if (it != 0)
            m_state.vi[it & 0xF] = (int32_t)((m_state.pc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        return;
    }
    case 0x24: // JR
    {
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is & 0xF] * 8u) & 0x3FFF;
        m_state.branchPending = true;
        m_state.branchTarget = target;
        return;
    }
    case 0x25: // JALR
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint32_t target = ((uint32_t)(uint16_t)m_state.vi[is & 0xF] * 8u) & 0x3FFF;
        if (it != 0)
            m_state.vi[it & 0xF] = (int32_t)((m_state.pc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        return;
    }
    case 0x28: // IBEQ
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] == (int16_t)m_state.vi[it & 0xF])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }
    case 0x29: // IBNE
    {
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] != (int16_t)m_state.vi[it & 0xF])
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }
    case 0x2C: // IBLTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] < 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }
    case 0x2D: // IBGTZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] > 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }
    case 0x2E: // IBLEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] <= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }
    case 0x2F: // IBGEZ
    {
        uint8_t is = LIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)m_state.vi[is & 0xF] >= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & 0x3FFF;
            m_state.branchPending = true;
        m_state.branchTarget = target;
        }
        return;
    }

    case 0x40: // Lower special (opcode in bits 5:0)
    {
        uint8_t funct = KFUNCT >= 0 ? static_cast<uint8_t>(KFUNCT) : static_cast<uint8_t>(instr & 0x3F);
        uint8_t it = LIT(instr);
        uint8_t is = LIS(instr);
        uint8_t id = LID(instr);
        uint8_t dest = (instr >> 21) & 0xF;

        switch (funct)
        {
        case 0x30: // IADD
            if (id != 0)
                m_state.vi[id & 0xF] = (int16_t)(m_state.vi[is & 0xF] + m_state.vi[it & 0xF]);
            return;
        case 0x31: // ISUB
            if (id != 0)
                m_state.vi[id & 0xF] = (int16_t)(m_state.vi[is & 0xF] - m_state.vi[it & 0xF]);
            return;
        case 0x32: // IADDI
        {
            int16_t imm5 = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27);
            if (it != 0)
                m_state.vi[it & 0xF] = (int16_t)(m_state.vi[is & 0xF] + imm5);
            return;
        }
        case 0x34: // IAND
            if (id != 0)
                m_state.vi[id & 0xF] = m_state.vi[is & 0xF] & m_state.vi[it & 0xF];
            return;
        case 0x35: // IOR
            if (id != 0)
                m_state.vi[id & 0xF] = m_state.vi[is & 0xF] | m_state.vi[it & 0xF];
            return;

        case 0x3C: // Lower special2 group (0x3C-0x3F: op selected by broadcast bits)
        case 0x3D:
        case 0x3E:
        case 0x3F:
        {
            // The real VU lower-special opcode is bits[1:0] | (bits[10:6] << 2),
            // i.e. (instr & 3) | ((instr >> 4) & 0x7C). The funct (0x3C..0x3F) only
            // carries the low two opcode bits; decoding it as a separate op (as we
            // did before) silently dropped DIV/MTIR/XTOP/XGKICK and mis-ran others.
            // See DobieStation lower1_special / the PS2 VU ISA.
            uint8_t funct2 = KF2 >= 0 ? static_cast<uint8_t>(KF2) : (uint8_t)((instr & 0x3) | ((instr >> 4) & 0x7C));
            switch (funct2)
            {
            case 0x30: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[is], 16);
                applyDestNoMac(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x31: // MR32 (rotate right by 32 bits = shift xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[is][1], m_state.vf[is][2], m_state.vf[is][3], m_state.vf[is][0]};
                applyDestNoMac(m_state.vf[it], tmp, dest);
                return;
            }
            case 0x3D: // MFIR (Move From Integer Register)
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[is & 0xF] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDestNoMac(m_state.vf[it], result, dest);
                return;
            }
            case 0x3C: // MTIR (Move To Integer Register)
            {
                // MTIR selects ONE source component via fsf = bits[22:21] (0=x..3=w).
                const int comp = (instr >> 21) & 0x3;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[is][comp], 4);
                if (it != 0)
                    m_state.vi[it & 0xF] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x3E: // ILWR (Integer Load Word, register-indirect)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                    uint32_t v;
                    std::memcpy(&v, vuData + addr + comp * 4, 4);
                    if (it != 0)
                        m_state.vi[it & 0xF] = (int32_t)(int16_t)(v & 0xFFFF);
                }
                return;
            }
            case 0x3F: // ISWR (Integer Store Word, register-indirect)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t val = (uint32_t)(uint16_t)(m_state.vi[it & 0xF] & 0xFFFF);
                    if (dest & 0x8) std::memcpy(vuData + addr + 0, &val, 4);
                    if (dest & 0x4) std::memcpy(vuData + addr + 4, &val, 4);
                    if (dest & 0x2) std::memcpy(vuData + addr + 8, &val, 4);
                    if (dest & 0x1) std::memcpy(vuData + addr + 12, &val, 4);
                }
                return;
            }
            case 0x40: // RNEXT
                return;
            case 0x41: // RGET
                return;
            case 0x42: // RINIT
                return;
            case 0x43: // RXOR
                return;
            case 0x34: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDestNoMac(m_state.vf[it], tmp, dest);
                }
                if (is != 0)
                    m_state.vi[is & 0xF] = (int16_t)(m_state.vi[is & 0xF] + 1);
                return;
            }
            case 0x35: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                if (it != 0)
                    m_state.vi[it & 0xF] = (int16_t)(m_state.vi[it & 0xF] + 1);
                return;
            }
            case 0x36: // LQD (Load Quadword, pre-decrement)
            {
                if (is != 0)
                    m_state.vi[is & 0xF] = (int16_t)(m_state.vi[is & 0xF] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDestNoMac(m_state.vf[it], tmp, dest);
                }
                return;
            }
            case 0x37: // SQD (Store Quadword, pre-decrement)
            {
                if (it != 0)
                    m_state.vi[it & 0xF] = (int16_t)(m_state.vi[it & 0xF] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[it & 0xF]) * 16u;
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    if (dest & 0x8)
                        tmp[0] = m_state.vf[is][0];
                    if (dest & 0x4)
                        tmp[1] = m_state.vf[is][1];
                    if (dest & 0x2)
                        tmp[2] = m_state.vf[is][2];
                    if (dest & 0x1)
                        tmp[3] = m_state.vf[is][3];
                    std::memcpy(vuData + addr, tmp, 16);
                }
                return;
            }
            case 0x38: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = m_state.vf[it][ftf];
                if ((kAotCfg ? false : vuDivExactEnabled()))
                {
                    // PCSX2 _vuDIV (d5f75c9e4). Two differences from the branch
                    // below, both on the divide-by-zero path, and Q feeds the
                    // perspective divide — i.e. screen X/Y, not colour and not a
                    // constant Z:
                    //   * the sign of the Fmax result is the XOR of BOTH operand
                    //     SIGN BITS, not the numerator's value. Ours also treats
                    //     a -0.0 numerator as positive, since -0.0 >= 0.0f.
                    //   * STATUS D (0x20, divide by zero) and I (0x10, 0/0) are
                    //     set; ours never touches them.
                    auto bits = [](float f) {
                        uint32_t b;
                        std::memcpy(&b, &f, sizeof(b));
                        return b;
                    };
                    const uint32_t nb = bits(num), db = bits(den);
                    const float nv = vuFlushDenorm(num), dv = vuFlushDenorm(den);
                    m_state.status &= ~0x30u;
                    if (dv == 0.0f)
                    {
                        m_state.status |= (nv == 0.0f) ? 0x10u : 0x20u;
                        const uint32_t q = ((db ^ nb) & 0x80000000u) ? 0xFF7FFFFFu
                                                                     : 0x7F7FFFFFu;
                        float qf;
                        std::memcpy(&qf, &q, sizeof(qf));
                        qWrite(qf, 7u);
                    }
                    else
                    {
                        qWrite(vuFlushDenorm(nv / dv), 7u);
                    }
                    return;
                }
                qWrite(den != 0.0f
                           ? num / den
                           : ((num >= 0.0f) ? std::numeric_limits<float>::max()
                                            : -std::numeric_limits<float>::max()),
                       7u);
                return;
            }
            case 0x39: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                float val = m_state.vf[it][ftf];
                qWrite(std::sqrt(std::fabs(val)), 7u);
                return;
            }
            case 0x3A: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                float num = m_state.vf[is][fsf];
                float den = std::sqrt(std::fabs(m_state.vf[it][ftf]));
                qWrite(den != 0.0f ? num / den
                                   : std::numeric_limits<float>::max(),
                       13u);
                return;
            }
            case 0x3B: // WAITQ — stall until the FDIV pipe drains
                qFlush();
                return;
            case 0x70: // ESADD — P = Fs_x² + Fs_y² + Fs_z²
            {
                const float x = m_state.vf[is][0];
                const float y = m_state.vf[is][1];
                const float z = m_state.vf[is][2];
                m_state.p = x * x + y * y + z * z;
                return;
            }
            case 0x71: // ERSADD — P = 1 / (Fs_x² + Fs_y² + Fs_z²), 0 passes through
            {
                const float x = m_state.vf[is][0];
                const float y = m_state.vf[is][1];
                const float z = m_state.vf[is][2];
                const float s = x * x + y * y + z * z;
                m_state.p = (s != 0.0f) ? (1.0f / s) : std::numeric_limits<float>::max();
                return;
            }
            case 0x72: // ELENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                m_state.p = std::sqrt(s);
                return;
            }
            case 0x7A: // ERCPR
            {
                int fsf = (instr >> 21) & 0x3;
                float val = m_state.vf[is][fsf];
                m_state.p = (val != 0.0f) ? (1.0f / val) : std::numeric_limits<float>::max();
                return;
            }
            case 0x73: // ERLENG
            {
                float s = m_state.vf[is][0] * m_state.vf[is][0] + m_state.vf[is][1] * m_state.vf[is][1] + m_state.vf[is][2] * m_state.vf[is][2];
                float len = std::sqrt(s);
                m_state.p = (len != 0.0f) ? (1.0f / len) : std::numeric_limits<float>::max();
                return;
            }
            case 0x7B: // WAITP
                return;
            case 0x74: // EATANxy — P = EATAN(Fs_y / Fs_x), 0 when Fs_x == 0
            {
                const float x = vuFlushDenorm(m_state.vf[is][0]);
                if ((kAotCfg ? true : vuEatanMicroVu()))
                    m_state.p = vuMvuEatanRatio(vuFlushDenorm(m_state.vf[is][1]), x);
                else
                    m_state.p = (x != 0.0f)
                                    ? vuCalculateEATAN(vuFlushDenorm(m_state.vf[is][1]) / x)
                                    : 0.0f;
                return;
            }
            case 0x75: // EATANxz — P = EATAN(Fs_z / Fs_x), 0 when Fs_x == 0
            {
                const float x = vuFlushDenorm(m_state.vf[is][0]);
                if ((kAotCfg ? true : vuEatanMicroVu()))
                    m_state.p = vuMvuEatanRatio(vuFlushDenorm(m_state.vf[is][2]), x);
                else
                    m_state.p = (x != 0.0f)
                                    ? vuCalculateEATAN(vuFlushDenorm(m_state.vf[is][2]) / x)
                                    : 0.0f;
                return;
            }
            case 0x76: // ESUM — P = Fs_x + Fs_y + Fs_z + Fs_w
                m_state.p = m_state.vf[is][0] + m_state.vf[is][1] + m_state.vf[is][2] + m_state.vf[is][3];
                return;
            case 0x78: // ESQRT — P = sqrt(Fs[fsf]); a NEGATIVE input is passed
            {                //        through unchanged, it is not rectified
                const int fsf = (instr >> 21) & 0x3;
                const float v = vuFlushDenorm(m_state.vf[is][fsf]);
                m_state.p = (v >= 0.0f) ? std::sqrt(v) : v;
                return;
            }
            case 0x79: // ERSQRT — P = 1 / sqrt(Fs[fsf]); negative passes through,
            {          //          and sqrt(0) yields 0, not FLT_MAX
                const int fsf = (instr >> 21) & 0x3;
                float v = vuFlushDenorm(m_state.vf[is][fsf]);
                if (v >= 0.0f)
                {
                    v = std::sqrt(v);
                    if (v != 0.0f)
                        v = 1.0f / v;
                }
                m_state.p = v;
                return;
            }
            case 0x7C: // ESIN — P = the EFU's sine series (NOT std::sin)
            {
                const int fsf = (instr >> 21) & 0x3;
                m_state.p = vuCalculateESIN(vuFlushDenorm(m_state.vf[is][fsf]));
                return;
            }
            case 0x7D: // EATAN — P = the EFU's arctangent series (NOT std::atan)
            {
                const int fsf = (instr >> 21) & 0x3;
                const float v = vuFlushDenorm(m_state.vf[is][fsf]);
                m_state.p = (kAotCfg ? true : vuEatanMicroVu()) ? vuMvuEatanRatio(v, 1.0f) : vuCalculateEATAN(v);
                return;
            }
            case 0x7E: // EEXP — P = the EFU's exp(-x) series (NOT std::exp)
            {
                const int fsf = (instr >> 21) & 0x3;
                m_state.p = vuCalculateEEXP(vuFlushDenorm(m_state.vf[is][fsf]));
                return;
            }
            case 0x64: // MFP (Move From P register)
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDestNoMac(m_state.vf[it], result, dest);
                return;
            }
            case 0x68: // XTOP — returns VIF1 TOP (double-buffer output base)
                if (it != 0)
                    m_state.vi[it & 0xF] = (int32_t)m_state.top;
                return;
            case 0x69: // XITOP — returns ITOP
                if (it != 0)
                    m_state.vi[it & 0xF] = (int32_t)m_state.itop;
                return;
            case 0x6C: // XGKICK - send GIF packet from VU1 data memory
            {
            // The instruction PC is still current here; run() increments it
            // only after execLower returns. Preserve it with this packet rather
            // than observing m_state.pc after the kick has been submitted.
            // RRV_VU1_JIT_VERIFY: the recompiler's second pass must not submit
            // a second copy of every GIF packet. XGKICK touches no VU
            // architectural state, so skipping it leaves the comparison intact.
            // RRV_VU_PROG_VERIFY: both passes record where and from what they kick.
            if (RRV_VU_UNLIKELY(m_kickLog != nullptr) && vuData)
            {
                uint64_t h = 0xcbf29ce484222325ull;
                for (uint32_t n = 0; n < dataSize; ++n)
                {
                    h ^= vuData[n];
                    h *= 0x100000001b3ull;
                }
                m_kickLog->push_back((static_cast<uint64_t>(m_state.pc) << 32) |
                                     static_cast<uint16_t>(m_state.vi[is & 0xF]));
                m_kickLog->push_back(h);
            }
            if (RRV_VU_UNLIKELY(m_suppressSideEffects))
                return;
            const uint32_t xgkickPc = m_state.pc;
            if (s_carDmaVu1Scope.active)
                ++s_carDmaVu1Scope.result.attempted;
            if (!vuData || dataSize < 16u)
            {
                if (s_carDmaVu1Scope.active)
                    ++s_carDmaVu1Scope.result.skippedInvalid;
                return;
            }

            auto wrapOffset = [&](uint32_t off) -> uint32_t
            {
                return off % dataSize;
            };

            auto read64Wrap = [&](uint32_t off) -> uint64_t
            {
                uint8_t bytes[8];
                for (uint32_t i = 0; i < 8u; ++i)
                {
                    bytes[i] = vuData[wrapOffset(off + i)];
                }
                uint64_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                return value;
            };

            uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[is & 0xF]) * 16u;
            addr = wrapOffset(addr);
            uint32_t pktOff = addr;
            uint32_t totalBytes = 0u;
            bool done = false;

            for (int safety = 0; safety < 256 && !done; ++safety)
            {
                uint64_t tagLo = read64Wrap(pktOff);
                uint32_t nloop = (uint32_t)(tagLo & 0x7FFFu);
                uint8_t flg = (uint8_t)((tagLo >> 58) & 0x3u);
                uint32_t nreg = (uint32_t)((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                bool eop = ((tagLo >> 15) & 0x1ull) != 0ull;

                uint32_t pktSize = 16u;
                if (flg == 0u)
                {
                    pktSize += nloop * nreg * 16u;
                }
                else if (flg == 1u)
                {
                    uint32_t regs = nloop * nreg;
                    pktSize += regs * 8u;
                    if ((regs & 1u) != 0u)
                        pktSize += 8u;
                }
                else
                {
                    // FLG=2 (IMAGE) and FLG=3 (IMAGE2 / "disabled") both carry
                    // nloop quadwords of payload. FLG=3 is spec-reserved, but the
                    // GS consumes it exactly like IMAGE -- pinned PCSX2
                    // `GSState::Transfer` falls `case GIF_FLG_IMAGE2:` straight
                    // through into `case GIF_FLG_IMAGE:` and does
                    // `path.nloop -= len; mem += len * 16`.
                    //
                    // Counting FLG=3 as a bare 16-byte tag (the previous
                    // behaviour) makes this walk disagree with every real GIF
                    // consumer about the packet's length, so we hand the GS a
                    // packet that is short by nloop*16 bytes. The GS then keeps
                    // `nloop > 0` in its PERSISTENT per-path decode state and
                    // consumes the FOLLOWING packets as continuation data -- a
                    // desync that survives arbitrarily long and eventually walks
                    // the decoder off the end of a valid buffer (G1: EXC_BAD_ACCESS
                    // in GSState::Transfer<3> on both Software and Metal, plus the
                    // downstream `gd.sel.lcm` assertion).
                    //
                    // With the length counted correctly, a garbage FLG=3 tag makes
                    // pktSize exceed the 16 KiB VU1 data memory and the existing
                    // overflow guard below skips the whole kick, which is the
                    // established behaviour for "the kick base register held
                    // garbage rather than a real GIFtag".
                    pktSize += nloop * 16u;
                }

                if (pktSize == 0u)
                    break;

                // A single GIFtag's payload cannot exceed the 16 KiB VU1 data
                // memory it is sourced from. If even the first tag overflows, the
                // kick base register held garbage (not a real GIFtag) -- skip the
                // whole kick rather than clamp-and-flood the GS with ~1000 bogus
                // primitives walked out of vertex-float data.
                if (pktSize > dataSize)
                {
                    if (s_carDmaVu1Scope.active)
                        ++s_carDmaVu1Scope.result.skippedOverflow;
                    if ((kAotCfg ? false : g_vu1Diag.enabled))
                    {
                        std::lock_guard<std::mutex> lock(g_vu1Diag.mtx);
                        ++g_vu1Diag.xgkickSkipOverflow;
                    }
                    return;
                }

                totalBytes += pktSize;
                pktOff = wrapOffset(pktOff + pktSize);
                if (eop)
                    done = true;
            }

            if (totalBytes == 0u)
            {
                if (s_carDmaVu1Scope.active)
                    ++s_carDmaVu1Scope.result.skippedEmpty;
                if ((kAotCfg ? false : g_vu1Diag.enabled))
                {
                    std::lock_guard<std::mutex> lock(g_vu1Diag.mtx);
                    ++g_vu1Diag.xgkickSkipEmpty;
                }
                return;
            }

            if (s_carDmaVu1Scope.active)
                ++s_carDmaVu1Scope.result.submitted;

            if ((kAotCfg ? false : g_vu1Diag.enabled))
            {
                s_curRunXgkicks.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(g_vu1Diag.mtx);
                ++g_vu1Diag.xgkickByPc[xgkickPc];
                if (s_curRunCarLive)
                    ++g_vu1Diag.xgkickByPcCar[xgkickPc];
            }

            // A single XGKICK packet is sourced from the 16 KiB VU1 data memory and
            // cannot legitimately exceed it. A stale/garbage GIFtag (high nloop bits)
            // would otherwise inflate totalBytes to multiple MiB, triggering a huge
            // per-kick heap allocation below and flooding the GS with junk primitives.
            // Clamp to dataSize so a malformed tag walks its own buffer at most once.
            if (totalBytes > dataSize)
                totalBytes = dataSize;

            if (memory)
            {
                static const bool p7Trace = [] {
                    const char *v = std::getenv("RRV_P7_VU_TRACE");
                    return v && v[0] && v[0] != '0';
                }();
                static uint32_t p7Seq = 0;
                // DIAG-TEMP: the phase gate was `read32(0x334E94) == 7`, which is
                // never true in this context — a live run that triggered the
                // recorder at the phase-7 anchor and captured 11.5 MB emitted zero
                // [p7-vu] lines (T-P7-VERTEX). Gate on the env var alone and print
                // the phase word that is actually visible here, so the reason is
                // observable instead of guessed.
                if (p7Trace && p7Seq < 200000u)
                {
                    const uint64_t firstLo = read64Wrap(addr);
                    const uint32_t firstNreg =
                        static_cast<uint32_t>((firstLo >> 60) & 0xFu);
                    std::fprintf(stderr,
                                 "[p7-vu] seq=%u phase=%u entry=%04x pc=%04x addr=%04x "
                                 "bytes=%u nloop=%u flg=%u nreg=%u prim=%03x\n",
                                 p7Seq++, memory->read32(0x334E94u), m_entryPC,
                                 xgkickPc, addr, totalBytes,
                                 static_cast<uint32_t>(firstLo & 0x7FFFu),
                                 static_cast<uint32_t>((firstLo >> 58) & 3u),
                                 firstNreg ? firstNreg : 16u,
                                 static_cast<uint32_t>((firstLo >> 47) & 0x7FFu));
                }
            }

            // DIAG-TEMP (T-P7-VUIN) — the output half of the input trace: this is
            // the kick that identifies which microprogram invocation to write out.
            {
                auto &t = rrvVuInTrace();
                if (t.enabled && t.written < t.max &&
                    (t.phase < 0 || (memory && memory->read32(0x334E94u) == (uint32_t)t.phase)))
                {
                    const uint64_t lo = read64Wrap(addr);
                    const uint32_t kickNloop = static_cast<uint32_t>(lo & 0x7FFFu);
                    const uint32_t kickPrim = static_cast<uint32_t>((lo >> 47) & 0x7FFu);
                    if (t.log)
                        std::fprintf(t.log,
                                     "[rrv-vu1] xgk seq=%u entry=%04x addr=%04x nloop=%u eop=%u "
                                     "pre=%u prim=%03x flg=%u nreg=%u pending=%d\n",
                                     t.seq, m_entryPC, addr, kickNloop,
                                     static_cast<uint32_t>((lo >> 15) & 1u),
                                     static_cast<uint32_t>((lo >> 46) & 1u), kickPrim,
                                     static_cast<uint32_t>((lo >> 58) & 3u),
                                     static_cast<uint32_t>((lo >> 60) & 0xFu), t.pending ? 1 : 0);
                    const uint32_t kickNreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                    if (t.pending && kickNloop == t.nloop && kickPrim == t.prim &&
                        (t.nreg == 0u || kickNreg == t.nreg))
                    {
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s/rrv_in_%04x_%u.bin",
                                      t.dir.c_str(), t.entry, t.written);
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fwrite(t.vf, 1, sizeof(t.vf), f);
                            std::fwrite(t.vi, 1, sizeof(t.vi), f);
                            std::fwrite(&t.q, 1, sizeof(t.q), f);
                            std::fwrite(&t.i, 1, sizeof(t.i), f);
                            std::fwrite(&t.mac, 1, sizeof(t.mac), f);
                            std::fwrite(&t.status, 1, sizeof(t.status), f);
                            std::fwrite(&t.clip, 1, sizeof(t.clip), f);
                            std::fwrite(t.mem.data(), 1, t.mem.size(), f);
                            const uint32_t trailer[5] = {0x31565252u, t.entry, t.pendTop,
                                                         t.pendItop, (uint32_t)t.mem.size()};
                            std::fwrite(trailer, 1, sizeof(trailer), f);
                            std::fclose(f);
                            // The microprogram itself, so the two engines' code
                            // can be compared rather than assumed equal.
                            std::snprintf(path, sizeof(path), "%s/rrv_micro_%04x_%u.bin",
                                          t.dir.c_str(), t.entry, t.written);
                            if (FILE *m = std::fopen(path, "wb"))
                            {
                                std::fwrite(t.micro.data(), 1, t.micro.size(), m);
                                std::fclose(m);
                            }
                            // The GIF packet this invocation produced, as the VU
                            // wrote it (same length rule as the PCSX2 writer).
                            std::snprintf(path, sizeof(path), "%s/rrv_out_%04x_%u.bin",
                                          t.dir.c_str(), t.entry, t.written);
                            if (FILE *o = std::fopen(path, "wb"))
                            {
                                // The WHOLE kicked packet, not the first tag.
                                // A program whose first GIFtag is a bare A+D
                                // register write (RR5's foliage program, entry
                                // 0x00c0: every kick is nloop=1 nreg=1) carries
                                // its geometry in LATER tags of the same packet,
                                // so a first-tag-derived length dumps 32 bytes
                                // and hides the draw. docs/TESTING.md
                                // T-FLY-TREES.
                                for (uint32_t n = 0; n < totalBytes; ++n)
                                    std::fputc(vuData[wrapOffset(addr + n)], o);
                                std::fclose(o);
                            }
                            if (t.log)
                                std::fprintf(t.log,
                                             "[rrv-vu1] DUMP %s (mscal seq=%u, xgkick addr=%04x "
                                             "nloop=%u prim=%03x)\n",
                                             path, t.pendingSeq, addr, kickNloop, kickPrim);
                        }
                        ++t.written;
                        t.pending = false;
                    }
                }
            }

            const uint32_t debugIndex = s_debugVu1XgkickCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[vu1:xgkick] idx=" << debugIndex
                                                << " addr=0x" << std::hex << addr
                                                << " totalBytes=0x" << totalBytes
                                                << std::dec
                                                << " wrap=" << static_cast<uint32_t>((addr + totalBytes > dataSize) ? 1u : 0u)
                                                << std::endl);
            }

            // RRV_TEX_ENTRY=<tbp0> — which microprogram ENTRY draws a texture?
            // The packet is tagged with the XGKICK pc, not the entry, so the
            // ps2_memory.cpp RRV_TEX_PC probe names the kick site while
            // RRV_VU_INTRACE selects on the entry. This bins m_entryPC, which
            // is what RRV_VU_INTRACE actually wants. docs/TESTING.md
            // T-FLY-TREES.
            {
                static const uint32_t s_texEntry = [] {
                    const char *e = std::getenv("RRV_TEX_ENTRY");
                    return (e && e[0]) ? (uint32_t)std::strtoul(e, nullptr, 0) : 0u;
                }();
                if (s_texEntry != 0u && addr + totalBytes <= dataSize)
                {
                    static std::map<uint32_t, uint64_t> byEntry;
                    static uint64_t seen = 0u;
                    const uint8_t *p = vuData + addr;
                    bool match = false;
                    for (uint32_t o = 0; o + 16u <= totalBytes && !match; o += 16u)
                    {
                        uint64_t qlo = 0u, qhi = 0u;
                        std::memcpy(&qlo, p + o, sizeof(qlo));
                        std::memcpy(&qhi, p + o + 8, sizeof(qhi));
                        if ((qhi & 0xFFull) != 0x06ull && (qhi & 0xFFull) != 0x07ull)
                            continue;
                        if ((uint32_t)(qlo & 0x3FFFull) == s_texEntry)
                            match = true;
                    }
                    if (match)
                    {
                        byEntry[m_entryPC] += 1u;
                        if ((++seen % 20000ull) == 0ull)
                        {
                            std::fprintf(stderr, "[tex-entry] tbp0=%u entries:", s_texEntry);
                            for (const auto &kv : byEntry)
                                std::fprintf(stderr, " %04x:%llu", kv.first,
                                             (unsigned long long)kv.second);
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }

            if (addr + totalBytes <= dataSize)
            {
                if (memory)
                    memory->submitGifPacket(GifPathId::Path1, vuData + addr, totalBytes,
                                            true, false, xgkickPc);
                else
                    gs.processGIFPacket(GifPathId::Path1, vuData + addr, totalBytes, xgkickPc);
            }
            else
            {
                std::vector<uint8_t> wrappedPacket(totalBytes);
                for (uint32_t i = 0; i < totalBytes; ++i)
                {
                    wrappedPacket[i] = vuData[wrapOffset(addr + i)];
                }

                if (memory)
                    memory->submitGifPacket(GifPathId::Path1, wrappedPacket.data(), totalBytes,
                                            true, false, xgkickPc);
                else
                    gs.processGIFPacket(GifPathId::Path1, wrappedPacket.data(), totalBytes, xgkickPc);
            }
            return;
            } // end case 0x6C: XGKICK

            default:
                return;
            } // end lower-special2 (instr & 3 | (instr >> 4) & 0x7C) switch
        } // end case 0x3C..0x3F group

        default:
            return;
        } // end lower-special (0x40) funct switch
    }
    default:
        break;
    }
}

void VU1Interpreter::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    execLowerT<-1, -1, -1>(instr, vuData, dataSize, gs, memory, upperInstr);
}

#include "rrv_vu_aot_engine.inc"
