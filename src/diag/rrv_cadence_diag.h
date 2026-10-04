// rrv_cadence_diag.h — P0-cadence (C0) frame-budget probe.
//
// WHY THIS EXISTS
// ---------------
// After G0 the A2 girl scene completes ~1 display list per 2 emulated fields
// where hardware completes ~1 per field (docs/TESTING.md T-G0-A2-ABI3-
// DEFINITIVE: 30 lists / 59 field intervals = 1.97). C0 must decide whether the
// residual is (A) work-bound host execution, (B) a synchronisation/timing
// semantics defect, or (C) both — BEFORE any throughput work.
//
// The decisive fact that shapes this probe: RRV has **no EE cycle model**.
// `R5900Context::insn_count` is declared but never incremented and no generated
// function accounts cycles, so guest execution is free-running at host speed.
// The only clock in the system is the wall-clock vblank worker
// (`kVblankPeriod = 16667 us`, Kernel/Syscalls/Interrupt.cpp) and `sceGsSyncV`
// blocks on it. Therefore:
//
//     fields per display list == ceil(host wall time per guest frame / 16.667 ms)
//
// so the classification reduces to a wall-clock budget of ONE guest frame,
// split into the components that can each be attributed to a subsystem:
//
//   wall  = wait + run
//   wait  = blocked inside WaitForNextVSyncTick (the guest's own frame barrier)
//   run   = gs + shadowGs + vu1 + lock + ee
//     gs    time inside the GS backend seam on the guest thread (PCSX2 GS decode
//           + rasterisation, which is synchronous for G0)
//     shadowGs FullFrame-only local decode/normalisation and IR/provenance
//           production for the same post-arbitration GIF packets
//     vu1   time inside the VU1 interpreter on the guest thread
//     lock  time the guest thread spent BLOCKED acquiring the recursive guest-
//           execution mutex (i.e. stolen by the vblank worker's INTC dispatch /
//           GSvsync, or by the frontend's snapshot)
//     ee    residual: recompiled EE code, DMAC walk, VIF interpreters, HLE
//
// and two cross-thread series that explain `lock`:
//   fieldGs   ns inside Backend::vsync (vblank worker thread)
//   snapGs    ns inside Backend::copyFrame (frontend/present thread)
//
// Reading the result:
//   run >= 1 field and wait ~ 0            -> (A) work-bound
//   run << 1 field and wait ~ 1+ fields    -> (B) the guest is idling; find what
//                                             it waits on and why it misses
//   run in (1,2) fields, wait = remainder  -> (C) work slightly over budget,
//                                             then QUANTISED to 2 by the frame
//                                             barrier — the interesting mixed case
//
// DISCIPLINE
// ----------
// Diagnostic only, default off, zero behaviour change. Everything is gated on
// one cached bool; when unset each site costs a single relaxed load. Header-only
// (C++17 inline variables) so it needs no new library and no link changes — the
// vendored runtime just includes it, exactly like rrv_gs_record_hooks.h.
//
// Enable with RRV_CADENCE_DIAG=1. RRV_CADENCE_DIAG_EVERY=N prints one line per
// N guest frames (default 1); a rolling summary prints once a second.
#ifndef RRV_CADENCE_DIAG_H
#define RRV_CADENCE_DIAG_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rrv::cadence
{
    inline bool enabled()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("RRV_CADENCE_DIAG");
            return v && v[0] && v[0] != '0';
        }();
        return s_on;
    }

    inline uint64_t nowNs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Guest-thread accumulators. Thread-local because the guest frame budget is
    // by definition what happened on the thread that runs the game loop; the
    // frame boundary (sceGsSyncV) runs on that same thread and reads its own
    // TLS, so no cross-thread synchronisation is involved and no other thread's
    // GS traffic can contaminate the budget.
    inline thread_local uint64_t t_gsNs = 0;
    // FullFrame-only local semantic shadow. This is deliberately separate from
    // `gs`: the same packet has already traversed the PCSX2 bridge when this
    // scope runs, and live Field never enters it.
    inline thread_local uint64_t t_shadowGsNs = 0;
    // Subset of t_gsNs: time the submitting thread spent BLOCKED on the backend
    // mutex rather than inside the bridge. The frontend's presentation snapshot
    // takes the same mutex, so this separates "GS is slow" from "the present
    // path is holding the seam" — the two are indistinguishable in t_gsNs alone
    // and they have completely different fixes.
    inline thread_local uint64_t t_gsLockNs = 0;
    inline thread_local uint64_t t_vu1Ns = 0;
    inline thread_local uint64_t t_lockNs = 0;
    // Every guest-thread HLE block goes through GuestExecutionReleaseScope
    // (Thread.cpp sleep/wait, Sync.cpp semaphore wait, Interrupt.cpp vsync
    // wait), so timing that one scope captures ALL deliberate guest idling.
    // The vsync-barrier portion is reported separately, and `blk` below is the
    // remainder — i.e. guest idling that is NOT the frame barrier.
    inline thread_local uint64_t t_blockNs = 0;
    inline thread_local uint64_t t_gifBytes = 0;
    inline thread_local uint64_t t_gifPackets = 0;
    inline thread_local uint64_t t_dispatches = 0;

    // Cross-thread series, sampled as deltas at each frame boundary. These are
    // the two known ways another thread holds the guest-execution lock.
    inline std::atomic<uint64_t> g_fieldGsNs{0};   // Backend::vsync (vblank worker)
    inline std::atomic<uint64_t> g_snapGsNs{0};    // Backend::copyFrame (frontend)
    inline std::atomic<uint64_t> g_fieldGsCalls{0};
    inline std::atomic<uint64_t> g_snapGsCalls{0};

    // Live guest PC, published by the dispatch loop. A sampler can histogram
    // this to attribute the `ee` residual to a guest function without adding any
    // per-instruction cost. Function-granular: it is the PC the dispatch loop
    // last entered, so a long-running guest loop shows as its enclosing function.
    inline std::atomic<uint32_t> g_guestPc{0};

    // RAII accumulator for a guest-thread bucket. The bucket is passed by
    // reference rather than as a template argument because the address of a
    // thread_local is not a constant expression.
    class Scope
    {
    public:
        explicit Scope(uint64_t &bucket)
            : m_bucket(bucket), m_start(enabled() ? nowNs() : 0u) {}
        ~Scope()
        {
            if (m_start)
            {
                m_bucket += nowNs() - m_start;
            }
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        uint64_t &m_bucket;
        uint64_t m_start;
    };

    inline void noteDispatch(uint32_t pc)
    {
        if (!enabled())
            return;
        ++t_dispatches;
        g_guestPc.store(pc, std::memory_order_relaxed);
    }

    inline void noteGifPacket(uint32_t bytes)
    {
        if (!enabled())
            return;
        ++t_gifPackets;
        t_gifBytes += bytes;
    }

    // RRV_PC_SAMPLE=1 — attribute the `ee` residual and, more usefully, any
    // `blk` idling to a guest function, without adding a single instruction to
    // the dispatch path. A detached thread samples the PC the dispatch loop last
    // entered at 1 kHz and prints the top entries once a second.
    //
    // Why sampling and not instrumenting the blocking syscalls: while the guest
    // is blocked the dispatch loop is not dispatching, so `g_guestPc` holds the
    // function that made the call and stays there for the whole wait. A scene
    // that idles shows up as one PC with a huge share, which names the owner
    // directly. Instrumenting the three vendored `GuestExecutionReleaseScope`
    // sites would need a vendored edit and would name the syscall, not the
    // caller — the caller is the thing you can look up.
    inline void startPcSampler()
    {
        static const bool on = [] {
            const char *v = std::getenv("RRV_PC_SAMPLE");
            return v && v[0] && v[0] != '0';
        }();
        if (!on)
            return;
        static bool started = false;
        if (started)
            return;
        started = true;
        std::thread([] {
            std::unordered_map<uint32_t, uint64_t> hist;
            uint64_t total = 0;
            uint64_t last = nowNs();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++hist[g_guestPc.load(std::memory_order_relaxed)];
                ++total;
                const uint64_t now = nowNs();
                if (now - last < 1000000000ull)
                    continue;
                last = now;
                std::vector<std::pair<uint32_t, uint64_t>> top(hist.begin(), hist.end());
                std::sort(top.begin(), top.end(),
                          [](const auto &a, const auto &b) { return a.second > b.second; });
                std::string line = "[pc-sample]";
                for (size_t i = 0; i < top.size() && i < 8u; ++i)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), " %08X=%.0f%%", top[i].first,
                                  100.0 * static_cast<double>(top[i].second) /
                                      static_cast<double>(total ? total : 1));
                    line += buf;
                }
                std::fprintf(stderr, "%s (n=%llu)\n", line.c_str(),
                             static_cast<unsigned long long>(total));
                hist.clear();
                total = 0;
            }
        }).detach();
    }

    inline void addCrossThread(std::atomic<uint64_t> &ns, std::atomic<uint64_t> &calls,
                               uint64_t startNs)
    {
        if (!startNs)
            return;
        ns.fetch_add(nowNs() - startNs, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Called from sceGsSyncV once per guest frame, on the guest thread, AFTER
    // the frame barrier has returned. `waitNs` is the time that call spent
    // blocked; `tick` is the emulated field index it observed.
    inline void frameBoundary(uint64_t waitNs, uint64_t tick)
    {
        if (!enabled())
            return;
        startPcSampler();

        static uint64_t s_prevExitNs = 0;
        static uint64_t s_prevTick = 0;
        static uint64_t s_frames = 0;
        static uint64_t s_prevFieldGsNs = 0;
        static uint64_t s_prevSnapGsNs = 0;
        static uint64_t s_reportNs = 0;
        // Rolling one-second sums, for a summary that is not hostage to a single
        // frame's jitter.
        static uint64_t a_wall = 0, a_wait = 0, a_gs = 0, a_shadowGs = 0, a_gsLock = 0, a_vu1 = 0, a_lock = 0, a_blk = 0;
        static uint64_t a_fieldGs = 0, a_snapGs = 0, a_disp = 0, a_gifB = 0;
        static uint64_t a_frames = 0, a_ticks = 0;
        static uint64_t a_deltaHist[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        const uint64_t exitNs = nowNs();
        static const uint64_t every = [] {
            const char *v = std::getenv("RRV_CADENCE_DIAG_EVERY");
            const long n = v && v[0] ? std::strtol(v, nullptr, 10) : 1;
            return n > 0 ? static_cast<uint64_t>(n) : 1u;
        }();

        const uint64_t fieldGsNs = g_fieldGsNs.load(std::memory_order_relaxed);
        const uint64_t snapGsNs = g_snapGsNs.load(std::memory_order_relaxed);

        if (s_prevExitNs != 0u)
        {
            const uint64_t wall = exitNs - s_prevExitNs;
            const uint64_t wait = waitNs < wall ? waitNs : wall;
            const uint64_t run = wall - wait;
            const uint64_t gs = t_gsNs;
            const uint64_t shadowGs = t_shadowGsNs;
            const uint64_t gsLock = t_gsLockNs < gs ? t_gsLockNs : gs;
            const uint64_t vu1 = t_vu1Ns;
            const uint64_t lock = t_lockNs;
            // t_blockNs includes this frame's own vsync barrier wait, which is
            // already reported as `wait`; `blk` is the rest of the guest's
            // deliberate idling (semaphores, thread sleeps, other HLE waits).
            const uint64_t blk = t_blockNs > waitNs ? t_blockNs - waitNs : 0u;
            const uint64_t accounted = gs + shadowGs + vu1 + lock + blk;
            const uint64_t ee = run > accounted ? run - accounted : 0u;
            const uint64_t dTick = tick - s_prevTick;
            const uint64_t fieldGs = fieldGsNs - s_prevFieldGsNs;
            const uint64_t snapGs = snapGsNs - s_prevSnapGsNs;

            ++s_frames;
            a_wall += wall;
            a_wait += wait;
            a_gs += gs;
            a_shadowGs += shadowGs;
            a_gsLock += gsLock;
            a_vu1 += vu1;
            a_lock += lock;
            a_blk += blk;
            a_fieldGs += fieldGs;
            a_snapGs += snapGs;
            a_disp += t_dispatches;
            a_gifB += t_gifBytes;
            a_ticks += dTick;
            ++a_frames;
            a_deltaHist[dTick < 8u ? dTick : 7u]++;

            if ((s_frames % every) == 0u)
            {
                std::fprintf(stderr,
                             "[cadence:frame] n=%llu tick=%llu dTick=%llu wall=%.2fms "
                             "wait=%.2f run=%.2f | gs=%.2f(lk %.2f) shadowGs=%.2f vu1=%.2f lock=%.2f blk=%.2f "
                             "ee=%.2f | fieldGs=%.2f snapGs=%.2f disp=%llu gifKB=%llu\n",
                             (unsigned long long)s_frames, (unsigned long long)tick,
                             (unsigned long long)dTick, wall / 1e6, wait / 1e6, run / 1e6,
                             gs / 1e6, gsLock / 1e6, shadowGs / 1e6, vu1 / 1e6, lock / 1e6, blk / 1e6, ee / 1e6,
                             fieldGs / 1e6, snapGs / 1e6,
                             (unsigned long long)t_dispatches,
                             (unsigned long long)(t_gifBytes / 1024u));
            }

            if (s_reportNs == 0u)
            {
                s_reportNs = exitNs;
            }
            else if (exitNs - s_reportNs >= 1000000000ull && a_frames != 0u)
            {
                const double f = static_cast<double>(a_frames);
                const uint64_t accum = a_gs + a_shadowGs + a_vu1 + a_lock + a_blk;
                const uint64_t aee = a_wall > a_wait + accum ? a_wall - a_wait - accum : 0u;
                std::fprintf(stderr,
                             "[cadence:1s] frames=%llu fields/list=%.2f wall=%.2fms "
                             "wait=%.2f run=%.2f | gs=%.2f(lk %.2f) shadowGs=%.2f vu1=%.2f lock=%.2f blk=%.2f "
                             "ee=%.2f | fieldGs=%.2f snapGs=%.2f disp/f=%.0f gifKB/f=%.0f "
                             "dTick[0..7]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
                             (unsigned long long)a_frames,
                             static_cast<double>(a_ticks) / f,
                             a_wall / 1e6 / f, a_wait / 1e6 / f,
                             (a_wall - a_wait) / 1e6 / f, a_gs / 1e6 / f, a_gsLock / 1e6 / f,
                             a_shadowGs / 1e6 / f, a_vu1 / 1e6 / f,
                             a_lock / 1e6 / f, a_blk / 1e6 / f, aee / 1e6 / f, a_fieldGs / 1e6 / f,
                             a_snapGs / 1e6 / f, static_cast<double>(a_disp) / f,
                             static_cast<double>(a_gifB) / 1024.0 / f,
                             (unsigned long long)a_deltaHist[0], (unsigned long long)a_deltaHist[1],
                             (unsigned long long)a_deltaHist[2], (unsigned long long)a_deltaHist[3],
                             (unsigned long long)a_deltaHist[4], (unsigned long long)a_deltaHist[5],
                             (unsigned long long)a_deltaHist[6], (unsigned long long)a_deltaHist[7]);
                s_reportNs = exitNs;
                a_wall = a_wait = a_gs = a_shadowGs = a_gsLock = a_vu1 = a_lock = a_blk = 0;
                a_fieldGs = a_snapGs = a_disp = a_gifB = 0;
                a_frames = a_ticks = 0;
                for (uint64_t &h : a_deltaHist)
                    h = 0;
            }
        }

        s_prevExitNs = exitNs;
        s_prevTick = tick;
        s_prevFieldGsNs = fieldGsNs;
        s_prevSnapGsNs = snapGsNs;
        t_gsNs = t_shadowGsNs = t_gsLockNs = t_vu1Ns = t_lockNs = t_blockNs = 0;
        t_gifBytes = t_gifPackets = t_dispatches = 0;
    }

    // Like Scope, but subtracts whatever the two nested buckets accrued while
    // it was open, so the buckets stay mutually exclusive. VU1 needs this:
    // XGKICK submits first to the PCSX2 bridge and, in FullFrame, then to the
    // local semantic shadow from inside VU1Interpreter::run. Both must be
    // removed from the VU1-exclusive total.
    class ExclusiveScope
    {
    public:
        ExclusiveScope(uint64_t &bucket, const uint64_t &nestedA, const uint64_t &nestedB)
            : m_bucket(bucket), m_nestedA(nestedA), m_nestedB(nestedB),
              m_nestedAAtEntry(nestedA), m_nestedBAtEntry(nestedB),
              m_start(enabled() ? nowNs() : 0u) {}
        ~ExclusiveScope()
        {
            if (m_start)
            {
                const uint64_t elapsed = nowNs() - m_start;
                const uint64_t nested = (m_nestedA - m_nestedAAtEntry) +
                                        (m_nestedB - m_nestedBAtEntry);
                m_bucket += elapsed > nested ? elapsed - nested : 0u;
            }
        }
        ExclusiveScope(const ExclusiveScope &) = delete;
        ExclusiveScope &operator=(const ExclusiveScope &) = delete;

    private:
        uint64_t &m_bucket;
        const uint64_t &m_nestedA;
        const uint64_t &m_nestedB;
        uint64_t m_nestedAAtEntry;
        uint64_t m_nestedBAtEntry;
        uint64_t m_start;
    };

    // Convenience factories; `auto s = rrv::cadence::gsScope();` reads better at
    // the call sites than naming the bucket variable twice.
    inline Scope gsScope() { return Scope(t_gsNs); }
    inline Scope shadowGsScope() { return Scope(t_shadowGsNs); }
    inline Scope gsLockScope() { return Scope(t_gsLockNs); }
    inline Scope lockScope() { return Scope(t_lockNs); }
    inline Scope blockScope() { return Scope(t_blockNs); }
    inline ExclusiveScope vu1Scope()
    {
        return ExclusiveScope(t_vu1Ns, t_gsNs, t_shadowGsNs);
    }
}

#endif // RRV_CADENCE_DIAG_H
