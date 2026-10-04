#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/diag_counters.h" // RRV_RUNTIME_LOG: raster perf counters
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "runtime/ps2_gs_memory.h"
// NOTE: ps2_gs_psm{ct32,t4,t8}.h are reference-only oracles (see their
// header banners) and are deliberately NOT included here -- this file
// addresses VRAM through GSMem (runtime/ps2_gs_memory.h) only.
#if !defined(PS2X_RRV_FIELD_ONLY)
#include "rrv_ir_hooks.h" // milestone C1: IR-consuming rasterization path (RRV_IR_RASTER)
#endif
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace GSInternal;

#if defined(_DEBUG)
// RRV_RUNTIME_LOG: per-thread covered-pixel counter, flushed to g_pixelCount per
// chunk/primitive so the metric costs one atomic per chunk, not one per pixel
// (which bounced a cache line across all raster threads and dwarfed the real work).
static thread_local uint64_t t_rasterPixels = 0;
#endif

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0)  & 0xFF) >> 3;
        uint32_t g = ((c >> 8)  & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0)  & 0x1F) << 3;
        u32 g = ((c >> 5)  & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};
    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool preserveDestinationAlpha;
        bool writeDepth;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, false, true};

        // TEST.AFAIL controls what happens when the alpha comparison fails:
        // KEEP = neither buffer, FB_ONLY = color only (Z preserved),
        // ZB_ONLY = depth only, RGB_ONLY = color minus alpha (Z preserved).
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    // CLUT cache: see GSRasterizer::latchClut / buildClutTable below and
    // GSTex0Reg::clutCache in ps2_gs_gpu.h (RRV_GS_CLUT_CACHE, default off).
    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        switch (sourcePsm)
        {
        case GS_PSM_T4:
        case GS_PSM_T4HH:
        case GS_PSM_T4HL:
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);

            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            if (csm == 0)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
            break;
        default:
            break;
        }

        return clutIndex;
    }

    // RRV_GS_CLUT_CACHE (default OFF; unset/"0" = old always-live-VRAM-read
    // behaviour). Read once, cached, mirroring g_tileEnabled's pattern below.
    const bool g_clutCacheEnabled = [] {
        const char *v = std::getenv("RRV_GS_CLUT_CACHE");
        return v && v[0] && v[0] != '0';
    }();

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

// ── Tile-binning (RRV_RASTER_TILE) ────────────────────────────────────────
// A full snapshot of one primitive's draw-state — everything the raster inner
// path reads off `gs` (enumerated: m_prim, m_ctx, m_texa, m_texclut, m_pabe,
// m_vtxQueue[0..2]). VRAM is shared (a pointer), not snapshotted, which is
// sound because RRV performs ~0 mid-frame VRAM uploads (measured, cont.44) and
// flushTiles() is called before any upload/transfer/read-back.
//
// Defined at global scope (not the anonymous namespace) so the header can
// forward-declare it and flushTileCmds() can take the command list by reference
// — the async-raster path double-buffers two of these vectors.
struct GSTileCmd
{
    GSPrimReg    prim;
    GSContext    ctx[2];
    GSTexaReg    texa;
    GSTexClutReg texclut;
    bool         pabe;
    GSVertex     v[3];
};

namespace
{
    // Scoped by tile-hazard fallback while replaying captured commands. This
    // must disable the older per-primitive RowPool even if RRV_RASTER_MT=1.
    thread_local bool t_tileHazardSerialReplay = false;

    std::vector<GSTileCmd> g_tileCmds;
    // Default ON: the ~50fps attract needs both the 60fps-mode fix (patches.cpp)
    // AND parallel tiling to afford the raster within the frame budget — each alone
    // is bottlenecked (pacing vs synchronous raster). Disable hatch: RRV_NO_TILE.
    const bool g_tileEnabled = (std::getenv("RRV_NO_TILE") == nullptr);
    // Diagnostic hatch: retain tile capture/replay but force the whole frame into
    // the caller's single strip. This isolates StripPool partitioning from the
    // tile path itself; default behaviour remains parallel.
    const bool g_tileSingleWorker = (std::getenv("RRV_TILE_WORKERS") != nullptr &&
                                     std::strcmp(std::getenv("RRV_TILE_WORKERS"), "1") == 0);
    // Diagnostic-only M2 hatches. The bypass drains captured commands at the
    // normal flush fences but intentionally skips pixel work, isolating raster
    // throughput from guest simulation cadence. Both are read once at startup
    // and leave the default path unchanged.
    const bool g_tileFlushDiag = [] {
        const char *v = std::getenv("RRV_TILE_FLUSH_DIAG");
        return v && v[0] && v[0] != '0';
    }();
    const bool g_tileRasterBypass = [] {
        const char *v = std::getenv("RRV_TILE_RASTER_BYPASS");
        return v && v[0] && v[0] != '0';
    }();
    // NOTE (2026-08-02, measured then REMOVED): a hatch that skipped
    // tileFlushHasReadWriteHazard() entirely and forced every flush down the
    // parallel strip path priced the parallel ceiling at 60.0 ms/hardware frame
    // against a 113.8 ms default on gt_A2_girl_clean_2026-08-02.gsr — 1.90x
    // overall, 2.54x on the flush alone, not the ~6x previously assumed. It was
    // deleted rather than kept because it deliberately races on genuine
    // framebuffer feedback and produces wrong pixels; the number it established
    // is recorded here and in the raster-parallelism handoff instead.

    // Read-only strip-occupancy probe (default off). Accumulates per-strip busy
    // nanoseconds so the parallel path's load balance can be measured: equal row
    // bands over a scene whose covered rows are not uniformly distributed leave
    // most workers idle, which caps speed-up regardless of hazard precision.
    const bool g_tileStripDiag = [] {
        const char *v = std::getenv("RRV_TILE_STRIP_DIAG");
        return v && v[0] && v[0] != '0';
    }();
    std::array<std::atomic<uint64_t>, 16> g_stripBusyNs{};
    std::array<std::atomic<uint64_t>, 16> g_stripRuns{};
    std::atomic<uint64_t> g_stripHSum{0};
    std::atomic<uint64_t> g_stripHCount{0};

    void noteTileStrip(int wi, int parts, int H, uint64_t ns)
    {
        if (wi >= 0 && wi < 16)
        {
            g_stripBusyNs[static_cast<size_t>(wi)].fetch_add(ns, std::memory_order_relaxed);
            g_stripRuns[static_cast<size_t>(wi)].fetch_add(1u, std::memory_order_relaxed);
        }
        if (wi == 0)
        {
            g_stripHSum.fetch_add(static_cast<uint64_t>(H), std::memory_order_relaxed);
            g_stripHCount.fetch_add(1u, std::memory_order_relaxed);
            (void)parts;
        }
    }

    struct TileStripDiagReport
    {
        ~TileStripDiagReport()
        {
            if (!g_tileStripDiag)
                return;
            const uint64_t n = g_stripHCount.load();
            std::fprintf(stderr, "[tile:strip] parallel_flushes=%llu mean_H=%.1f\n",
                         static_cast<unsigned long long>(n),
                         n ? static_cast<double>(g_stripHSum.load()) / static_cast<double>(n) : 0.0);
            uint64_t total = 0, peak = 0;
            for (size_t i = 0; i < 16; ++i)
            {
                total += g_stripBusyNs[i].load();
                peak = std::max(peak, g_stripBusyNs[i].load());
            }
            for (size_t i = 0; i < 16; ++i)
            {
                const uint64_t b = g_stripBusyNs[i].load();
                if (!g_stripRuns[i].load())
                    continue;
                std::fprintf(stderr,
                             "[tile:strip]   strip %2zu: runs=%llu busy=%8.1f ms  (%5.1f%% of peak)\n",
                             i, static_cast<unsigned long long>(g_stripRuns[i].load()),
                             static_cast<double>(b) / 1.0e6,
                             peak ? 100.0 * static_cast<double>(b) / static_cast<double>(peak) : 0.0);
            }
            std::fprintf(stderr,
                         "[tile:strip] sum_busy=%.1f ms  peak_busy=%.1f ms  "
                         "balance=%.2fx (sum/peak; equals parts when perfectly balanced)\n",
                         static_cast<double>(total) / 1.0e6, static_cast<double>(peak) / 1.0e6,
                         peak ? static_cast<double>(total) / static_cast<double>(peak) : 0.0);
        }
    };
    TileStripDiagReport g_tileStripDiagReport;

    std::atomic<uint64_t> g_tileHazardFallbackCount{0};

    void noteTileFlush(size_t commandCount, bool serialFallback, bool bypass,
                       int parts, uint64_t wallNs)
    {
        if (!g_tileFlushDiag)
            return;

        struct Window
        {
            std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
            uint64_t calls = 0;
            uint64_t commands = 0;
            uint64_t maxCommands = 0;
            uint64_t serial = 0;
            uint64_t parallel = 0;
            uint64_t bypassed = 0;
            uint64_t wallNs = 0;
            uint64_t maxWallNs = 0;
            int maxParts = 0;
        };
        static Window w;

        ++w.calls;
        w.commands += commandCount;
        w.maxCommands = std::max<uint64_t>(w.maxCommands, commandCount);
        w.serial += serialFallback ? 1u : 0u;
        w.parallel += (!serialFallback && !bypass) ? 1u : 0u;
        w.bypassed += bypass ? 1u : 0u;
        w.wallNs += wallNs;
        w.maxWallNs = std::max(w.maxWallNs, wallNs);
        w.maxParts = std::max(w.maxParts, parts);

        const auto now = std::chrono::steady_clock::now();
        if (now - w.start < std::chrono::seconds(1))
            return;

        const double totalMs = static_cast<double>(w.wallNs) / 1.0e6;
        const double maxMs = static_cast<double>(w.maxWallNs) / 1.0e6;
        const double avgCommands = w.calls ? static_cast<double>(w.commands) / w.calls : 0.0;
        std::fprintf(stderr,
                     "[tile:flush] calls=%llu cmds=%llu avg_cmds=%.1f max_cmds=%llu "
                     "parallel=%llu serial=%llu bypass=%llu max_parts=%d total_ms=%.3f max_ms=%.3f\n",
                     static_cast<unsigned long long>(w.calls),
                     static_cast<unsigned long long>(w.commands), avgCommands,
                     static_cast<unsigned long long>(w.maxCommands),
                     static_cast<unsigned long long>(w.parallel),
                     static_cast<unsigned long long>(w.serial),
                     static_cast<unsigned long long>(w.bypassed), w.maxParts,
                     totalMs, maxMs);
        w = Window{};
    }

    // GS FRAME/ZBUF bases are 8 KiB pages, whereas TEX0/CLUT bases are 256-byte
    // blocks. The strip renderer is only independent when no sampled texture or
    // palette page can be written by this whole flush. Use a deliberately broad
    // page footprint: a false positive merely selects ordered replay; a false
    // negative would reintroduce the cross-strip read/write race.
    constexpr uint32_t kGsVramPages = 512u;
    constexpr uint32_t kGsPageBytes = 8192u;
    constexpr uint32_t kGsBlocksPerPage = 32u;

    struct TileVramPages
    {
        std::array<bool, kGsVramPages> bits{};

        void mark(uint32_t firstPage, uint64_t byteCount)
        {
            const uint32_t count = std::max<uint32_t>(1u, static_cast<uint32_t>(
                (byteCount + kGsPageBytes - 1u) / kGsPageBytes));
            for (uint32_t i = 0; i < count; ++i)
                bits[(firstPage + i) % kGsVramPages] = true;
        }

        void markAll()
        {
            bits.fill(true);
        }

        bool intersects(const TileVramPages &other) const
        {
            for (uint32_t i = 0; i < kGsVramPages; ++i)
                if (bits[i] && other.bits[i])
                    return true;
            return false;
        }
    };

    // Index-only fork-join pool. run(job) executes job(i) for i in [1..N] on N
    // worker threads while the caller runs job(0), then blocks. Exactly ONE
    // dispatch per flush (per frame) — this is what fixes the dispatch-bound
    // per-primitive prototype (cont.43): the CV-signal cost is amortised across
    // the whole ~6000-primitive command buffer instead of paid per primitive.
    class StripPool
    {
    public:
        void ensureInit()
        {
            if (m_init.load(std::memory_order_acquire))
                return;
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_init.load(std::memory_order_relaxed))
                return;
            int hc = static_cast<int>(std::thread::hardware_concurrency());
            m_workers = g_tileSingleWorker ? 0 : std::max(0, std::min(hc - 2, 7)); // leave cores for sim/present/vblank/audio
            for (int i = 0; i < m_workers; ++i)
                m_threads.emplace_back([this, i] { workerLoop(i + 1); });
            m_init.store(true, std::memory_order_release);
        }

        int parts() const { return m_workers + 1; }

        void run(const std::function<void(int)> &job)
        {
            if (m_workers == 0)
            {
                job(0);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_job = &job;
                m_done.store(0, std::memory_order_relaxed);
                ++m_gen;
            }
            m_cvGo.notify_all();
            job(0); // caller renders strip 0
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [this] { return m_done.load(std::memory_order_relaxed) == m_workers; });
        }

        ~StripPool()
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_stop = true;
                ++m_gen;
            }
            m_cvGo.notify_all();
            for (auto &t : m_threads)
                if (t.joinable())
                    t.join();
        }

    private:
        void workerLoop(int id)
        {
            int myGen = 0;
            for (;;)
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvGo.wait(lk, [this, &myGen] { return m_stop || m_gen != myGen; });
                if (m_stop)
                    return;
                myGen = m_gen;
                const std::function<void(int)> *job = m_job;
                lk.unlock();

                (*job)(id);

                if (m_done.fetch_add(1, std::memory_order_acq_rel) + 1 == m_workers)
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_cvDone.notify_one();
                }
            }
        }

        std::atomic<bool> m_init{false};
        int m_workers = 0;
        std::vector<std::thread> m_threads;
        std::mutex m_mtx;
        std::condition_variable m_cvGo, m_cvDone;
        int m_gen = 0;
        bool m_stop = false;
        const std::function<void(int)> *m_job = nullptr;
        std::atomic<int> m_done{0};
    };

    StripPool g_stripPool;
    // Persistent per-strip shadow draw-state containers (index 0 = caller). Each
    // is a full GS used ONLY as a state bag sharing the real m_vram; its own
    // m_rasterizer is never used (the real GSRasterizer drives them). GS is
    // non-copyable (holds mutexes / MB vectors), so we construct a small fixed
    // pool once and reload only the ~5 draw-state fields per primitive.
    std::vector<std::unique_ptr<GS>> g_tileShadows;

    // ── Async raster (RRV_ASYNC_RASTER, default OFF) ──────────────────────────
    // Move the end-of-frame raster flush off the gameThread onto ONE render
    // worker. The gameThread keeps accumulating the next frame's command list
    // (captureTile writes no VRAM) while the worker drains the swapped-out
    // previous frame. This overlaps the ~half-frame of VU1 the gameThread spends
    // building frame N+1 with the ~half-frame of raster for frame N, roughly
    // doubling the content-frame throughput (target 33%->~61% at the anchor).
    //
    // Correctness rests on the same fences the synchronous path uses: every VRAM
    // upload/transfer/read-back and the present latch already call flushTiles(),
    // which in async mode first waits for the worker to finish. The gameThread
    // performs no VRAM writes between dispatch and the next fence, so the worker
    // owns VRAM exclusively while it runs.
    const bool g_asyncRasterEnabled = [] {
        const char *v = std::getenv("RRV_ASYNC_RASTER");
        return v && v[0] && v[0] != '0';
    }();

    class RasterWorker
    {
    public:
        // Hand the swapped-out command list to the worker and return immediately.
        // Caller must have already waited (so `consumer` is idle) before swapping
        // into it. `self`/`gs` drive the same flushTileCmds() the sync path uses.
        void submit(GSRasterizer *self, GS *gs)
        {
            ensureThread();
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_self = self;
                m_gs = gs;
                m_busy = true;
                ++m_gen;
            }
            m_cvGo.notify_one();
        }

        // Block until the worker has finished draining the consumer buffer. Safe
        // to call when no job is in flight (returns immediately).
        void wait()
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [this] { return !m_busy; });
        }

        std::vector<GSTileCmd> &consumer() { return m_consumer; }

        ~RasterWorker()
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_stop = true;
                ++m_gen;
            }
            m_cvGo.notify_one();
            if (m_thread.joinable())
                m_thread.join();
        }

    private:
        void ensureThread()
        {
            if (m_started)
                return;
            m_thread = std::thread([this] { loop(); });
            m_started = true;
        }

        void loop()
        {
            int myGen = 0;
            for (;;)
            {
                GSRasterizer *self;
                GS *gs;
                {
                    std::unique_lock<std::mutex> lk(m_mtx);
                    m_cvGo.wait(lk, [this, &myGen] { return m_stop || m_gen != myGen; });
                    if (m_stop)
                        return;
                    myGen = m_gen;
                    self = m_self;
                    gs = m_gs;
                }

                self->flushTileCmds(gs, m_consumer); // drains m_consumer

                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_busy = false;
                }
                m_cvDone.notify_all();
            }
        }

        std::vector<GSTileCmd> m_consumer;
        std::thread m_thread;
        std::mutex m_mtx;
        std::condition_variable m_cvGo, m_cvDone;
        int m_gen = 0;
        bool m_busy = false;
        bool m_stop = false;
        bool m_started = false;
        GSRasterizer *m_self = nullptr;
        GS *m_gs = nullptr;
    };

    RasterWorker g_rasterWorker;

    uint64_t tileTargetBytes(const GSFrameReg &frame)
    {
        // Four bytes/pixel is intentionally a safe upper bound for every GS
        // target PSM. Scanout/raster targets are bounded to 512 rows here.
        return static_cast<uint64_t>(std::max<uint32_t>(1u, frame.fbw) * 64u) * 512u * 4u;
    }

    uint64_t tileTextureBytes(const GSTex0Reg &tex)
    {
        const uint32_t width = 1u << std::min<uint32_t>(tex.tw, 10u);
        const uint32_t height = 1u << std::min<uint32_t>(tex.th, 10u);
        const uint32_t stride = std::max<uint32_t>(width, std::max<uint32_t>(1u, tex.tbw) * 64u);
        // As above, four bytes/pixel is conservative for indexed formats too.
        return static_cast<uint64_t>(stride) * height * 4u;
    }

    bool tileFlushHasReadWriteHazard(const std::vector<GSTileCmd> &cmds)
    {
        TileVramPages writes;
        TileVramPages reads;
        for (const GSTileCmd &c : cmds)
        {
            const GSContext &ctx = c.ctx[c.prim.ctxt ? 1 : 0];
            writes.mark(ctx.frame.fbp, tileTargetBytes(ctx.frame));
            if (!ctx.zbuf.zmask)
                writes.mark(ctx.zbuf.zbp, tileTargetBytes(ctx.frame));

            if (c.prim.tme)
            {
                reads.mark(ctx.tex0.tbp0 / kGsBlocksPerPage, tileTextureBytes(ctx.tex0));
                switch (ctx.tex0.psm)
                {
                case GS_PSM_T8:
                case GS_PSM_T8H:
                case GS_PSM_T4:
                case GS_PSM_T4HL:
                case GS_PSM_T4HH:
                    // lookupCLUT also uses TEXCLUT's CBW/COU/COV mapping. Until
                    // that non-linear footprint is represented exactly, mark the
                    // complete VRAM as sampled: conservative, but no CLUT false
                    // negative can leak a cross-strip read/write dependency.
                    reads.markAll();
                    break;
                default:
                    break;
                }
            }
        }
        return writes.intersects(reads);
    }

    // ── RRV_TILE_FOOTPRINT_DIAG (read-only, default OFF) ───────────────────
    // Measurement-only probe. Answers: of the flushes that take the serial
    // hazard fallback, how many do so because tileTargetBytes()/
    // tileTextureBytes()/the CLUT reads.markAll() over-mark, versus how many
    // contain a genuine framebuffer-feedback hazard no amount of precision
    // removes? Computes a SEPARATE "precise" verdict alongside the existing
    // coarse one and only ever reports it -- see docs/CODEX_TASK_RASTER_PARALLELISM.md
    // and docs/HANDOFF_RASTER_PARALLELISM.md for the investigation this
    // measures. NEVER used to choose a replay path; tileFlushHasReadWriteHazard()
    // above is untouched and remains the sole thing that decides behaviour.
    const bool g_tileFootprintDiag = [] {
        const char *v = std::getenv("RRV_TILE_FOOTPRINT_DIAG");
        return v && v[0] && v[0] != '0';
    }();

    // Bits per pixel for the VRAM-page footprint a PSM occupies. The "H"
    // (high-bits-of-a-32-bit-word) indexed formats T8H/T4HL/T4HH physically
    // overlay a 32bpp buffer -- a WxH rectangle of them spans the SAME page
    // range as a 32bpp buffer of that size even though only 4-8 bits of each
    // 32-bit word are meaningful, so 32 is correct there, not an over-mark.
    // Formats outside this known set fall back to 32 (over-mark, never
    // under-mark).
    uint32_t gsPsmBitsPerPixel(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_Z32:
        case GS_PSM_Z24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return 32u;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 16u;
        case GS_PSM_T8:
            return 8u;
        case GS_PSM_T4:
            return 4u;
        default:
            return 32u; // unknown PSM: over-mark, never under-mark
        }
    }

    uint64_t bytesForRectBits(uint32_t width, uint32_t height, uint32_t bpp)
    {
        return (static_cast<uint64_t>(width) * height * bpp + 7ull) / 8ull;
    }

    // Precise target footprint: real bpp (not a flat 4), bounded by the
    // SCISSORED height of this one command rather than a flat 512 rows. Rows
    // [0, y0) are still counted (the base page is not shifted by a sub-page
    // byte offset, only the row count is shrunk) -- a deliberate over-mark,
    // never an under-mark.
    uint64_t tileTargetBytesPrecise(const GSFrameReg &frame, const GSScissorReg &scissor)
    {
        const uint32_t width = std::max<uint32_t>(1u, frame.fbw) * 64u;
        const uint32_t height = std::max<uint32_t>(1u, static_cast<uint32_t>(scissor.y1) + 1u);
        return bytesForRectBits(width, height, gsPsmBitsPerPixel(frame.psm));
    }

    // Precise texture footprint: the addressing stride comes from TBW (the
    // buffer's real row pitch) rather than from 1<<TW widened via max() —
    // TW/TH describe the logical (possibly sub-rectangle) extent being
    // sampled, TBW is the actual physical stride. TBW==0 is reserved on real
    // hardware (nothing to derive a stride from); fall back to 1<<TW and flag
    // the over-mark.
    uint64_t tileTextureBytesPrecise(const GSTex0Reg &tex, bool *overMarked)
    {
        const uint32_t height = 1u << std::min<uint32_t>(tex.th, 10u);
        uint32_t width;
        if (tex.tbw != 0u)
        {
            width = static_cast<uint32_t>(tex.tbw) * 64u;
        }
        else
        {
            width = 1u << std::min<uint32_t>(tex.tw, 10u);
            if (overMarked)
                *overMarked = true;
        }
        return bytesForRectBits(width, height, gsPsmBitsPerPixel(tex.psm));
    }

    bool texPsmIsIndexed(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return true;
        default:
            return false;
        }
    }

    // Precise CLUT footprint, bounded to its own page(s) instead of
    // reads.markAll(). CSM==0 (CSM1: packed table addressed by CBP/CSA) is
    // handled exactly. CSM==1 (CSM2: table streamed like a small texture,
    // addressed via TEXCLUT's CBW/COU/COV) is bounded to a generous
    // rectangle around the offset rather than modeled exactly -- flagged as
    // an over-mark; RRV is not known to use CSM2, so this path is untested
    // against real content and is a documented gap, not a claimed exact model.
    void markClutPrecise(TileVramPages &reads, const GSTex0Reg &tex, const GSTexClutReg &texclut,
                         bool *overMarked)
    {
        const uint32_t entries = (tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL ||
                                  tex.psm == GS_PSM_T4HH)
                                     ? 16u
                                     : 256u;
        const uint32_t bytesPerEntry = (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24) ? 4u : 2u;
        const uint32_t basePage = tex.cbp / kGsBlocksPerPage;

        if (tex.csm == 0u)
        {
            // CSM1: packed table. CSA selects a 16-entry-aligned sub-block
            // within CLUT storage; over-mark by extending the byte count to
            // cover [0, CSA*16*bytesPerEntry + entries*bytesPerEntry) rather
            // than shifting the base page by a sub-page byte offset.
            const uint64_t csaOffset = static_cast<uint64_t>(tex.csa) * 16ull * bytesPerEntry;
            const uint64_t totalBytes = csaOffset + static_cast<uint64_t>(entries) * bytesPerEntry;
            reads.mark(basePage, totalBytes);
        }
        else
        {
            const uint32_t stride = std::max<uint32_t>(1u, static_cast<uint32_t>(texclut.cbw)) * 64u;
            const uint32_t rows = (entries + stride - 1u) / stride + 1u; // +1 row of COV slop
            const uint64_t offsetBytes =
                (static_cast<uint64_t>(texclut.cov) * stride + texclut.cou) * bytesPerEntry;
            const uint64_t totalBytes =
                offsetBytes + bytesForRectBits(stride, rows, bytesPerEntry * 8u);
            reads.mark(basePage, totalBytes);
            if (overMarked)
                *overMarked = true;
        }
    }

    // Per-command precise footprint, mirroring tileFlushHasReadWriteHazard's
    // write/read categorisation exactly (writes = FRAME always + ZBUF unless
    // zmask; reads = TEX0 only if TME, plus CLUT for indexed formats) so the
    // only thing that changes versus the coarse predicate is the byte math.
    struct TileCmdFootprintPrecise
    {
        TileVramPages writes;
        TileVramPages reads;
        uint32_t writeFbp = 0;      // representative FRAME.fbp, for pair attribution
        uint32_t readTbp0Page = 0;  // representative TEX0.tbp0 page, for pair attribution
        bool hasRead = false;
    };

    TileCmdFootprintPrecise computeCmdFootprintPrecise(const GSTileCmd &c, bool *anyOverMark)
    {
        TileCmdFootprintPrecise fp;
        const GSContext &ctx = c.ctx[c.prim.ctxt ? 1 : 0];
        fp.writeFbp = ctx.frame.fbp;
        fp.writes.mark(ctx.frame.fbp, tileTargetBytesPrecise(ctx.frame, ctx.scissor));
        if (!ctx.zbuf.zmask)
            fp.writes.mark(ctx.zbuf.zbp, tileTargetBytesPrecise(ctx.frame, ctx.scissor));

        if (c.prim.tme)
        {
            fp.hasRead = true;
            fp.readTbp0Page = ctx.tex0.tbp0 / kGsBlocksPerPage;
            bool over = false;
            fp.reads.mark(fp.readTbp0Page, tileTextureBytesPrecise(ctx.tex0, &over));
            if (texPsmIsIndexed(ctx.tex0.psm))
                markClutPrecise(fp.reads, ctx.tex0, c.texclut, &over);
            if (over && anyOverMark)
                *anyOverMark = true;
        }
        return fp;
    }

    struct FootprintDiagAccum
    {
        uint64_t flushes = 0;
        uint64_t coarseHazardous = 0;
        uint64_t preciseHazardous = 0;
        uint64_t overMarkFlushes = 0;

        // cmd-weighted versions of the three counters above. Flush sizes
        // span ~3 to ~12700 commands in this benchmark, so a flush-COUNT
        // percentage can be a poor proxy for wall-clock impact; this is the
        // number that answers "what fraction of the raster work is still
        // serial", not "what fraction of dispatches are still serial".
        uint64_t totalCmds = 0;
        uint64_t coarseHazardousCmds = 0;
        uint64_t preciseHazardousCmds = 0;

        // Run-split stats over EVERY flush, directly comparable to the prior
        // handoff's avg_run_cmds=1.0 (measured over all flushes with the
        // coarse footprint; see docs/HANDOFF_RASTER_PARALLELISM.md §4).
        uint64_t allFlushCmds = 0;
        uint64_t allFlushRuns = 0;

        // Run-split stats restricted to flushes the PRECISE whole-flush
        // predicate still calls hazardous -- this is the number that answers
        // whether precision alone unlocks meaningful parallelism.
        uint64_t hazardousFlushCmds = 0;
        uint64_t hazardousFlushRuns = 0;

        std::map<std::pair<uint32_t, uint32_t>, uint64_t> hazardPairCounts;

        struct Sample
        {
            size_t cmdCount = 0;
            std::vector<std::string> lines;
        };
        std::vector<Sample> falseNegSamples;
    };
    FootprintDiagAccum g_footprintDiag;
    std::mutex g_footprintDiagMutex;

    // Maximal-run partition with no cross-command write/read page overlap in
    // either direction (same rule as CODEX_TASK_RASTER_PARALLELISM.md M2),
    // using the PRECISE per-command footprints. Runs execute in submission
    // order; this only measures the partition, it never changes replay.
    void simulateRunsPrecise(const std::vector<TileCmdFootprintPrecise> &fps,
                             uint64_t &outRuns, uint64_t &outCmds)
    {
        TileVramPages runWrites, runReads;
        bool runOpen = false;
        for (const auto &fp : fps)
        {
            const bool hazardsWithRun =
                runOpen && (fp.reads.intersects(runWrites) || fp.writes.intersects(runReads));
            if (!runOpen || hazardsWithRun)
            {
                ++outRuns;
                runWrites = TileVramPages{};
                runReads = TileVramPages{};
                runOpen = true;
            }
            for (uint32_t p = 0; p < kGsVramPages; ++p)
            {
                if (fp.writes.bits[p])
                    runWrites.bits[p] = true;
                if (fp.reads.bits[p])
                    runReads.bits[p] = true;
            }
            ++outCmds;
        }
    }

    // TD REVIEW PROBE (default off) — drive the flush's serial/parallel decision
    // from the precise footprint instead of the coarse one, so the projected win
    // from footprint precision can be MEASURED rather than modelled. Same
    // accumulate-then-intersect shape as tileFlushHasReadWriteHazard(), so it is
    // a like-for-like swap of the byte math only. Not a promotion: a hazard
    // predicate needs far more than one stream's hashes before it can be a
    // default, and a false negative here races silently.
    const bool g_tilePreciseHazard = [] {
        const char *v = std::getenv("RRV_TILE_PRECISE_HAZARD");
        return v && v[0] && v[0] != '0';
    }();

    // Companion counters: how many commands hazard with THEMSELVES (own writes
    // intersect own reads). A command that samples its own render target is
    // genuine feedback that no footprint precision and no run-splitting can
    // remove; one that does not is only hazardous against its neighbours.
    std::atomic<uint64_t> g_selfHazardCmds{0};
    std::atomic<uint64_t> g_preciseHazardFlushCmds{0};

    bool tileFlushHasReadWriteHazardPrecise(const std::vector<GSTileCmd> &cmds)
    {
        TileVramPages writes;
        TileVramPages reads;
        uint64_t selfHazard = 0;
        for (const GSTileCmd &c : cmds)
        {
            bool overMark = false;
            const TileCmdFootprintPrecise fp = computeCmdFootprintPrecise(c, &overMark);
            if (g_tileFootprintDiag && fp.writes.intersects(fp.reads))
                ++selfHazard;
            for (uint32_t p = 0; p < kGsVramPages; ++p)
            {
                if (fp.writes.bits[p])
                    writes.bits[p] = true;
                if (fp.reads.bits[p])
                    reads.bits[p] = true;
            }
        }
        const bool hazard = writes.intersects(reads);
        if (g_tileFootprintDiag && hazard)
        {
            g_selfHazardCmds.fetch_add(selfHazard, std::memory_order_relaxed);
            g_preciseHazardFlushCmds.fetch_add(cmds.size(), std::memory_order_relaxed);
        }
        return hazard;
    }

    struct SelfHazardReport
    {
        ~SelfHazardReport()
        {
            if (!g_tileFootprintDiag || !g_tilePreciseHazard)
                return;
            const uint64_t self = g_selfHazardCmds.load();
            const uint64_t total = g_preciseHazardFlushCmds.load();
            std::fprintf(stderr,
                         "[tile:selfhazard] cmds_in_precise_hazardous_flushes=%llu "
                         "self_hazarding_cmds=%llu (%.1f%%)\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(self),
                         total ? 100.0 * static_cast<double>(self) / static_cast<double>(total) : 0.0);
        }
    };
    SelfHazardReport g_selfHazardReport;

    void noteTileFootprintDiag(const std::vector<GSTileCmd> &cmds, bool coarseHazard)
    {
        if (!g_tileFootprintDiag || cmds.empty())
            return;

        bool anyOverMark = false;
        std::vector<TileCmdFootprintPrecise> fps;
        fps.reserve(cmds.size());
        for (const GSTileCmd &c : cmds)
            fps.push_back(computeCmdFootprintPrecise(c, &anyOverMark));

        // Whole-flush precise verdict, mirroring tileFlushHasReadWriteHazard's
        // accumulate-then-intersect structure (union of all cmd writes, union
        // of all cmd reads, one intersects test) so this is a like-for-like
        // comparison against the coarse boolean.
        TileVramPages allWrites, allReads;
        for (const auto &fp : fps)
            for (uint32_t p = 0; p < kGsVramPages; ++p)
            {
                if (fp.writes.bits[p])
                    allWrites.bits[p] = true;
                if (fp.reads.bits[p])
                    allReads.bits[p] = true;
            }
        const bool preciseHazard = allWrites.intersects(allReads);

        uint64_t allRuns = 0, allCmds = 0;
        simulateRunsPrecise(fps, allRuns, allCmds);

        std::lock_guard<std::mutex> lk(g_footprintDiagMutex);
        ++g_footprintDiag.flushes;
        g_footprintDiag.coarseHazardous += coarseHazard ? 1u : 0u;
        g_footprintDiag.preciseHazardous += preciseHazard ? 1u : 0u;
        g_footprintDiag.overMarkFlushes += anyOverMark ? 1u : 0u;
        g_footprintDiag.totalCmds += cmds.size();
        g_footprintDiag.coarseHazardousCmds += coarseHazard ? cmds.size() : 0u;
        g_footprintDiag.preciseHazardousCmds += preciseHazard ? cmds.size() : 0u;
        g_footprintDiag.allFlushRuns += allRuns;
        g_footprintDiag.allFlushCmds += allCmds;

        if (preciseHazard)
        {
            g_footprintDiag.hazardousFlushRuns += allRuns;
            g_footprintDiag.hazardousFlushCmds += allCmds;

            // Pair attribution: which write source (FRAME.fbp page) overlaps
            // which read source (TEX0.tbp0 page) inside this flush. A page
            // can have more than one writer across a flush; the LAST writer
            // to touch a page is used as that page's representative owner
            // (a flush-level approximation, not a per-pixel one -- adequate
            // for "which pair explains the residue", not a full trace).
            // Pairs are counted once per flush they appear in, so the tally
            // answers "how many flushes does this pair explain" rather than
            // being skewed by how many pages one pair happens to span.
            std::array<uint32_t, kGsVramPages> writeOwner{};
            std::array<bool, kGsVramPages> writeOwnerSet{};
            for (const auto &fp : fps)
                for (uint32_t p = 0; p < kGsVramPages; ++p)
                    if (fp.writes.bits[p])
                    {
                        writeOwner[p] = fp.writeFbp;
                        writeOwnerSet[p] = true;
                    }

            std::set<std::pair<uint32_t, uint32_t>> seenPairs;
            for (const auto &fp : fps)
            {
                if (!fp.hasRead)
                    continue;
                for (uint32_t p = 0; p < kGsVramPages; ++p)
                    if (fp.reads.bits[p] && writeOwnerSet[p])
                        seenPairs.insert({writeOwner[p], fp.readTbp0Page});
            }
            for (const auto &pr : seenPairs)
                g_footprintDiag.hazardPairCounts[pr]++;
        }

        // False-negative sanity check: coarse says hazard, precise says
        // none. Dump raw register state for a handful of samples so the
        // report can be verified by eye that the pages really are disjoint.
        if (coarseHazard && !preciseHazard && g_footprintDiag.falseNegSamples.size() < 8)
        {
            FootprintDiagAccum::Sample s;
            s.cmdCount = cmds.size();
            for (size_t i = 0; i < cmds.size() && s.lines.size() < 12; ++i)
            {
                const GSTileCmd &c = cmds[i];
                const GSContext &ctx = c.ctx[c.prim.ctxt ? 1 : 0];
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                             "cmd[%zu] tme=%d FRAME.fbp=%u.psm=%u ZBUF.zbp=%u.zmask=%d "
                             "TEX0.tbp0page=%u.psm=%u.tbw=%u.tw=%u.th=%u "
                             "CLUT.cbp=%u.cpsm=%u.csm=%u.csa=%u scissor.y=[%u,%u]",
                             i, c.prim.tme ? 1 : 0, ctx.frame.fbp, ctx.frame.psm, ctx.zbuf.zbp,
                             ctx.zbuf.zmask ? 1 : 0, ctx.tex0.tbp0 / kGsBlocksPerPage, ctx.tex0.psm,
                             ctx.tex0.tbw, ctx.tex0.tw, ctx.tex0.th, ctx.tex0.cbp, ctx.tex0.cpsm,
                             ctx.tex0.csm, ctx.tex0.csa, ctx.scissor.y0, ctx.scissor.y1);
                s.lines.emplace_back(buf);
            }
            g_footprintDiag.falseNegSamples.push_back(std::move(s));
        }
    }

    struct FootprintDiagReport
    {
        ~FootprintDiagReport()
        {
            if (!g_tileFootprintDiag)
                return;
            std::lock_guard<std::mutex> lk(g_footprintDiagMutex);
            const auto &d = g_footprintDiag;
            std::fprintf(stderr,
                         "[tile:footprint] flushes=%llu coarse_hazardous=%llu precise_hazardous=%llu "
                         "over_mark_flushes=%llu\n",
                         static_cast<unsigned long long>(d.flushes),
                         static_cast<unsigned long long>(d.coarseHazardous),
                         static_cast<unsigned long long>(d.preciseHazardous),
                         static_cast<unsigned long long>(d.overMarkFlushes));
            std::fprintf(stderr,
                         "[tile:footprint] cmd-weighted: total_cmds=%llu coarse_hazardous_cmds=%llu (%.1f%%) "
                         "precise_hazardous_cmds=%llu (%.1f%%)\n",
                         static_cast<unsigned long long>(d.totalCmds),
                         static_cast<unsigned long long>(d.coarseHazardousCmds),
                         d.totalCmds ? 100.0 * static_cast<double>(d.coarseHazardousCmds) /
                                           static_cast<double>(d.totalCmds)
                                     : 0.0,
                         static_cast<unsigned long long>(d.preciseHazardousCmds),
                         d.totalCmds ? 100.0 * static_cast<double>(d.preciseHazardousCmds) /
                                           static_cast<double>(d.totalCmds)
                                     : 0.0);
            std::fprintf(stderr,
                         "[tile:footprint] ALL flushes (precise footprint): runs=%llu cmds=%llu avg_run_cmds=%.2f\n",
                         static_cast<unsigned long long>(d.allFlushRuns),
                         static_cast<unsigned long long>(d.allFlushCmds),
                         d.allFlushRuns ? static_cast<double>(d.allFlushCmds) / static_cast<double>(d.allFlushRuns)
                                        : 0.0);
            std::fprintf(stderr,
                         "[tile:footprint] PRECISE-HAZARDOUS-only flushes: runs=%llu cmds=%llu avg_run_cmds=%.2f\n",
                         static_cast<unsigned long long>(d.hazardousFlushRuns),
                         static_cast<unsigned long long>(d.hazardousFlushCmds),
                         d.hazardousFlushRuns
                             ? static_cast<double>(d.hazardousFlushCmds) / static_cast<double>(d.hazardousFlushRuns)
                             : 0.0);

            std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint64_t>> pairs(
                d.hazardPairCounts.begin(), d.hazardPairCounts.end());
            std::sort(pairs.begin(), pairs.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
            std::fprintf(stderr,
                         "[tile:footprint] residual hazard pairs (write_fbp_page, read_tbp0_page) -> flush_count:\n");
            for (size_t i = 0; i < pairs.size() && i < 20; ++i)
                std::fprintf(stderr, "[tile:footprint]   (%u,%u) -> %llu\n", pairs[i].first.first,
                             pairs[i].first.second, static_cast<unsigned long long>(pairs[i].second));

            std::fprintf(stderr,
                         "[tile:footprint] false-negative sanity samples (coarse=hazard, precise=clear): %zu\n",
                         d.falseNegSamples.size());
            for (size_t i = 0; i < d.falseNegSamples.size(); ++i)
            {
                std::fprintf(stderr, "[tile:footprint]   sample %zu (cmds=%zu):\n", i,
                             d.falseNegSamples[i].cmdCount);
                for (const auto &line : d.falseNegSamples[i].lines)
                    std::fprintf(stderr, "[tile:footprint]     %s\n", line.c_str());
            }
        }
    };
    FootprintDiagReport g_footprintDiagReport;
}

void GSRasterizer::drawPrimitive(GS *gs)
{
#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: tile-binning batch-size feasibility stats ([perf:batch]).
    ps2_diag::noteDraw();
#endif

    // Tile-binning: defer the primitive into the per-frame command buffer instead
    // of rasterizing now. The preferred-display-source bookkeeping (normally a
    // side effect of the immediate sprite/prim path) is a per-frame present-source
    // decision, not a pixel op, so it must still run now on the REAL gs.
    if (g_tileEnabled)
    {
        captureTile(gs);
        return;
    }

#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: time the whole primitive (all exit paths) into g_rasterNs, and
    // bin pixels-per-primitive to reveal the threading design (few big vs many small).
    struct PerfRT { std::chrono::steady_clock::time_point s{std::chrono::steady_clock::now()};
        uint64_t p0{ps2_diag::g_pixelCount.load(std::memory_order_relaxed)};
        ~PerfRT() { ps2_diag::g_rasterNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - s).count(), std::memory_order_relaxed);
            if (t_rasterPixels) { ps2_diag::g_pixelCount.fetch_add(t_rasterPixels, std::memory_order_relaxed); t_rasterPixels = 0; }
            const uint64_t d = ps2_diag::g_pixelCount.load(std::memory_order_relaxed) - p0;
            const int b = d == 0 ? 0 : d < 64 ? 1 : d < 256 ? 2 : d < 1024 ? 3 : d < 4096 ? 4 : d < 16384 ? 5 : 6;
            ps2_diag::g_primPixBucket[b].fetch_add(d, std::memory_order_relaxed); } } _perfRt;
#endif
    const auto &ctx = gs->activeContext();
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << gs->m_vtxQueue[0].x << "," << gs->m_vtxQueue[0].y << ")"
                      << " uv0=(" << (gs->m_vtxQueue[0].u >> 4) << "," << (gs->m_vtxQueue[0].v >> 4) << ")"
                      << " stq0=(" << gs->m_vtxQueue[0].s << "," << gs->m_vtxQueue[0].t << "," << gs->m_vtxQueue[0].q << ")"
                      << " v1=(" << gs->m_vtxQueue[1].x << "," << gs->m_vtxQueue[1].y << ")"
                      << " uv1=(" << (gs->m_vtxQueue[1].u >> 4) << "," << (gs->m_vtxQueue[1].v >> 4) << ")"
                      << " stq1=(" << gs->m_vtxQueue[1].s << "," << gs->m_vtxQueue[1].t << "," << gs->m_vtxQueue[1].q << ")"
                      << " v2=(" << gs->m_vtxQueue[2].x << "," << gs->m_vtxQueue[2].y << ")"
                      << " uv2=(" << (gs->m_vtxQueue[2].u >> 4) << "," << (gs->m_vtxQueue[2].v >> 4) << ")"
                      << " stq2=(" << gs->m_vtxQueue[2].s << "," << gs->m_vtxQueue[2].t << "," << gs->m_vtxQueue[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(gs->m_vtxQueue[0].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(gs->m_vtxQueue[1].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(gs->m_vtxQueue[2].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((gs->m_prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(gs->m_prim.type)
                      << " tme=" << static_cast<uint32_t>(gs->m_prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_texclut.cou)
                      << " cov=" << gs->m_texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
    {
        gs->m_hasPreferredDisplaySource = false;
    }

    switch (gs->m_prim.type)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixel(gs, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }
}

bool GSRasterizer::tileEnabled() { return g_tileEnabled; }

// Snapshot the current primitive into the tile command buffer, and replicate the
// two present-source side effects the immediate path performs on the real gs (so
// deferring rasterization to flush time doesn't change which buffer is displayed).
void GSRasterizer::captureTile(GS *gs)
{
    const GSContext &ctx = gs->activeContext();

    // Mirror drawPrimitive's line: a draw INTO the current preferred-display dest
    // invalidates the cached preferred source.
    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
        gs->m_hasPreferredDisplaySource = false;

    // Mirror drawSprite's looksLikeDisplayCopy detection (a full-screen textured
    // blit designates the VRAM region the present path should show).
    if (gs->m_prim.type == GS_PRIM_SPRITE)
    {
        const GSVertex &v0 = gs->m_vtxQueue[0];
        const GSVertex &v1 = gs->m_vtxQueue[1];
        int ofx = ctx.xyoffset.ofx >> 4;
        int ofy = ctx.xyoffset.ofy >> 4;
        int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
        int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        const int ux1 = x0 + std::max(1, x1 - x0) - 1;
        const int uy1 = y0 + std::max(1, y1 - y0) - 1;
        const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
        const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
        const bool looksLikeDisplayCopy =
            gs->m_prim.tme && gs->m_prim.abe && gs->m_prim.fst && gs->m_prim.ctxt &&
            ctx.frame.fbp != ctx.tex0.tbp0 &&
            alphaMode == 0x64u && (alphaFix == 0x60u || alphaFix == 0x80u) &&
            x0 <= 0 && y0 <= 0 && ux1 >= 639 && uy1 >= 447;
        if (looksLikeDisplayCopy)
        {
            gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
            gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
            gs->m_hasPreferredDisplaySource = true;
        }
    }

    GSTileCmd c;
    c.prim = gs->m_prim;
    c.ctx[0] = gs->m_ctx[0];
    c.ctx[1] = gs->m_ctx[1];
    c.texa = gs->m_texa;
    c.texclut = gs->m_texclut;
    c.pabe = gs->m_pabe;
    c.v[0] = gs->m_vtxQueue[0];
    c.v[1] = gs->m_vtxQueue[1];
    c.v[2] = gs->m_vtxQueue[2];
    g_tileCmds.push_back(c);
}

// Rasterize the whole deferred command buffer across horizontal screen strips in
// a single parallel dispatch, then clear it. Output-preserving: strip S renders
// exactly the immediate-mode output restricted to its rows (scissor = original ∩
// strip); disjoint strips write disjoint VRAM pixels; within a strip primitives
// replay in submission order (correct overdraw/blend/Z). Must be called before
// any VRAM read-back/upload/transfer so ordering matches immediate mode.
//
// Thin wrapper: rasterize the process-global producer buffer. The synchronous
// (default) path fills and drains g_tileCmds on the same thread; the async
// path hands a swapped-out buffer to flushTileCmds() on a render worker.
void GSRasterizer::flushTiles(GS *gs)
{
    // VRAM fence: in async mode the worker owns the consumer buffer, so wait for
    // it to finish before this thread drains the producer and the caller then
    // reads/mutates VRAM. Then drain anything accumulated since the last dispatch.
    if (g_asyncRasterEnabled)
        g_rasterWorker.wait();
    flushTileCmds(gs, g_tileCmds);
}

bool GSRasterizer::asyncRasterEnabled() { return g_asyncRasterEnabled; }

void GSRasterizer::waitForRasterWorker()
{
    if (g_asyncRasterEnabled)
        g_rasterWorker.wait();
}

// Frame boundary: hand the just-completed frame to the render worker and return
// so the gameThread starts the next frame immediately. Wait for the previous
// frame first (single consumer buffer), swap producer->consumer, then submit.
void GSRasterizer::dispatchAsyncFrame(GS *gs)
{
    g_rasterWorker.wait();
    std::swap(g_tileCmds, g_rasterWorker.consumer());
    if (std::getenv("RRV_ASYNC_RASTER_DIAG"))
        std::fprintf(stderr, "[async] dispatch cmds=%zu\n", g_rasterWorker.consumer().size());
    g_rasterWorker.submit(this, gs);
}

// Rasterize an explicit command list. Body of the former flushTiles(); operates
// only on `cmds` (never the global) so it can drain a double-buffered list the
// gameThread has already stopped writing to.
void GSRasterizer::flushTileCmds(GS *gs, std::vector<GSTileCmd> &cmds)
{
    if (cmds.empty())
        return;

    const size_t commandCount = cmds.size();
    const auto diagStart = g_tileFlushDiag ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    auto finishDiag = [&](bool serialFallback, bool bypass, int parts)
    {
        if (!g_tileFlushDiag)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - diagStart)
                .count());
        noteTileFlush(commandCount, serialFallback, bypass, parts, ns);
    };

    // Causality probe only: captureTile() already performed the real-GS
    // presentation bookkeeping when each command was queued. Draining here
    // preserves all existing flush fences while deliberately leaving pixels
    // stale/blank so guest cadence can be measured without synchronous raster.
    if (g_tileRasterBypass)
    {
        cmds.clear();
        finishDiag(false, true, 0);
        return;
    }

#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: fold the flush wall-time into g_rasterNs so [perf:t1]
    // ns/pix measures the tiled path (parallel wall-ns per covered pixel).
    const auto tStart = std::chrono::steady_clock::now();
#endif

    // Load one captured command into a private state bag, then replay it. The
    // real GS may already contain later GIF state by flush time, so fallback
    // must never mutate it. `clipToStrip=false` preserves the original scissor
    // and gives exact submission-order immediate semantics.
    auto replayCmd = [this](GS *sh, const GSTileCmd &c, int sy, int ey, bool clipToStrip)
    {
        sh->m_prim = c.prim;
        sh->m_ctx[0] = c.ctx[0];
        sh->m_ctx[1] = c.ctx[1];
        sh->m_texa = c.texa;
        sh->m_texclut = c.texclut;
        sh->m_pabe = c.pabe;
        sh->m_vtxQueue[0] = c.v[0];
        sh->m_vtxQueue[1] = c.v[1];
        sh->m_vtxQueue[2] = c.v[2];

        GSContext &actx = sh->activeContext();
        if (clipToStrip)
        {
            const int csy = std::max<int>(actx.scissor.y0, sy);
            const int cey = std::min<int>(actx.scissor.y1, ey - 1);
            if (csy > cey)
                return;
            actx.scissor.y0 = static_cast<uint16_t>(csy);
            actx.scissor.y1 = static_cast<uint16_t>(cey);
        }

        switch (c.prim.type)
        {
        case GS_PRIM_SPRITE:
            drawSprite(sh);
            break;
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN:
            drawTriangle(sh);
            break;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
            drawLine(sh);
            break;
        case GS_PRIM_POINT:
        {
            const GSVertex &v = sh->m_vtxQueue[0];
            const int px = static_cast<int>(v.x) - (actx.xyoffset.ofx >> 4);
            const int py = static_cast<int>(v.y) - (actx.xyoffset.ofy >> 4);
            writePixel(sh, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a);
            break;
        }
        default:
            break;
        }
    };

    // RRV_TILE_FOOTPRINT_DIAG reads coarseHazard alongside the existing
    // predicate but NEVER influences serialFallback -- it is computed from
    // the exact same tileFlushHasReadWriteHazard() call the live path always
    // made, just given a name so the diag probe can also see it.
    const bool coarseHazard = g_tilePreciseHazard
                                  ? tileFlushHasReadWriteHazardPrecise(cmds)
                                  : tileFlushHasReadWriteHazard(cmds);
    const bool serialFallback = coarseHazard;
    if (g_tileFootprintDiag)
        noteTileFootprintDiag(cmds, coarseHazard);
    if (serialFallback)
    {
        if (g_tileShadows.empty() || g_tileShadows[0]->m_vram != gs->m_vram)
        {
            g_tileShadows.clear();
            auto sh = std::make_unique<GS>();
            sh->init(gs->m_vram, gs->m_vramSize, gs->m_privRegs);
            g_tileShadows.push_back(std::move(sh));
        }

        struct SerialReplayScope
        {
            SerialReplayScope() { t_tileHazardSerialReplay = true; }
            ~SerialReplayScope() { t_tileHazardSerialReplay = false; }
        } serialReplayScope;

        GS *const sh = g_tileShadows[0].get();
        for (const GSTileCmd &c : cmds)
            replayCmd(sh, c, 0, 0, false);

        const uint64_t fallbackIndex = g_tileHazardFallbackCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        RUNTIME_LOG("[tile:hazard] serial fallback=" << fallbackIndex
                    << " commands=" << cmds.size() << std::endl);
        cmds.clear();
#if defined(_DEBUG)
        if (t_rasterPixels)
        {
            ps2_diag::g_pixelCount.fetch_add(t_rasterPixels, std::memory_order_relaxed);
            t_rasterPixels = 0;
        }
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tStart)
                .count());
        ps2_diag::g_rasterNs.fetch_add(ns, std::memory_order_relaxed);
#endif
        finishDiag(true, false, 1);
        return;
    }

    g_stripPool.ensureInit();
    const int parts = g_stripPool.parts();

    // Reuse the shadow pool; rebuild only if the worker count or VRAM moved.
    if (static_cast<int>(g_tileShadows.size()) != parts ||
        (parts > 0 && g_tileShadows[0]->m_vram != gs->m_vram))
    {
        g_tileShadows.clear();
        for (int i = 0; i < parts; ++i)
        {
            auto sh = std::make_unique<GS>();
            sh->init(gs->m_vram, gs->m_vramSize, gs->m_privRegs);
            g_tileShadows.push_back(std::move(sh));
        }
    }

    // Strip domain = the tallest scissored row actually used this frame, capped at
    // the framebuffer height. Rows above that are never written, so we skip them.
    int maxY1 = 0;
    for (const GSTileCmd &c : cmds)
    {
        const GSContext &ac = c.ctx[c.prim.ctxt ? 1 : 0];
        if (ac.scissor.y1 > maxY1)
            maxY1 = ac.scissor.y1;
    }
    const int H = std::min(maxY1 + 1, 512);
    auto job = [&](int wi)
    {
        GS *sh = g_tileShadows[wi].get();
        const int sy = H * wi / parts;
        const int ey = H * (wi + 1) / parts; // exclusive
        if (sy >= ey)
            return;

        const auto stripStart = g_tileStripDiag
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        for (const GSTileCmd &c : cmds)
            replayCmd(sh, c, sy, ey, true);
        if (g_tileStripDiag)
            noteTileStrip(wi, parts, H, static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - stripStart)
                    .count()));
#if defined(_DEBUG)
        // RRV_RUNTIME_LOG: flush this worker's covered-pixel tally.
        if (t_rasterPixels)
        {
            ps2_diag::g_pixelCount.fetch_add(t_rasterPixels, std::memory_order_relaxed);
            t_rasterPixels = 0;
        }
#endif
    };

    g_stripPool.run(job);
    cmds.clear();
    finishDiag(false, false, parts);

#if defined(_DEBUG)
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tStart)
            .count());
    ps2_diag::g_rasterNs.fetch_add(ns, std::memory_order_relaxed);
#endif
}

// ── Milestone C1: IR-consuming rasterization (RRV_IR_RASTER) ─────────────────
// Replicate ONLY the present-source bookkeeping side effects the immediate/tile
// draw path performs on the REAL gs. The IR-raster path skips drawPrimitive/
// captureTile, so without this the preferred-display-source decision (which the
// present latch reads) would diverge. Pixels themselves come from flushIR().
// Mirrors captureTile()'s side-effect block exactly; runs at vertexKick time when
// gs->activeContext()/m_vtxQueue still hold the just-assembled primitive.
#if !defined(PS2X_RRV_FIELD_ONLY)
void GSRasterizer::captureIRSideEffects(GS *gs)
{
    const GSContext &ctx = gs->activeContext();

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
        gs->m_hasPreferredDisplaySource = false;

    if (gs->m_prim.type == GS_PRIM_SPRITE)
    {
        const GSVertex &v0 = gs->m_vtxQueue[0];
        const GSVertex &v1 = gs->m_vtxQueue[1];
        int ofx = ctx.xyoffset.ofx >> 4;
        int ofy = ctx.xyoffset.ofy >> 4;
        int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
        int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        const int ux1 = x0 + std::max(1, x1 - x0) - 1;
        const int uy1 = y0 + std::max(1, y1 - y0) - 1;
        const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
        const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
        const bool looksLikeDisplayCopy =
            gs->m_prim.tme && gs->m_prim.abe && gs->m_prim.fst && gs->m_prim.ctxt &&
            ctx.frame.fbp != ctx.tex0.tbp0 &&
            alphaMode == 0x64u && (alphaFix == 0x60u || alphaFix == 0x80u) &&
            x0 <= 0 && y0 <= 0 && ux1 >= 639 && uy1 >= 447;
        if (looksLikeDisplayCopy)
        {
            gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
            gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
            gs->m_hasPreferredDisplaySource = true;
        }
    }
}

// Rasterize the pending IR draw stream by reconstructing a shadow GS from the
// interned IR (rrv::ir::rrv_ir_raster_get) and driving the SAME inner draw
// functions the live path uses. Single-threaded, submission order — bit-identical
// to the immediate path (which tile-binning also matches, C0). No-op when the
// RRV_IR_RASTER hatch is off. Placed at the same flush points as flushTiles().
void GSRasterizer::flushIR(GS *gs)
{
    if (!rrv::ir::rrv_ir_raster_enabled())
        return;
    const int n = rrv::ir::rrv_ir_raster_pending();
    if (n <= 0)
        return;

    // Persistent shadow GS: a state bag sharing the REAL vram (like g_tileShadows).
    // Rebuilt only if vram moved. Process-global (RENDERER_IR.md C0-findings
    // landmine #5) — safe with today's single-real-GS runtime and with the
    // gs-replay tool (one real GS per process); revisit if multiple real GS
    // instances ever share a process.
    static std::unique_ptr<GS> s_irShadow;
    if (!s_irShadow || s_irShadow->m_vram != gs->m_vram)
    {
        s_irShadow = std::make_unique<GS>();
        s_irShadow->init(gs->m_vram, gs->m_vramSize, gs->m_privRegs);
    }
    GS *sh = s_irShadow.get();

    for (int i = 0; i < n; ++i)
    {
        rrv::ir::IrRasterDraw d;
        if (!rrv::ir::rrv_ir_raster_get(i, &d))
            break;
        if (!d.drawing || d.vtxCount <= 0)
            continue;

        sh->m_prim.type = static_cast<GSPrimType>(d.topology);
        sh->m_prim.iip = d.iip != 0; sh->m_prim.tme = d.tme != 0;
        sh->m_prim.fge = d.fge != 0; sh->m_prim.abe = d.abe != 0;
        sh->m_prim.fst = d.fst != 0; sh->m_prim.ctxt = d.ctxt != 0;
        sh->m_prim.aa1 = false; sh->m_prim.fix = false;
        sh->m_pabe = d.pabe != 0;

        sh->m_texa.ta0 = d.ta0; sh->m_texa.aem = d.aem != 0; sh->m_texa.ta1 = d.ta1;
        sh->m_texclut.cbw = d.cbw; sh->m_texclut.cou = d.cou; sh->m_texclut.cov = d.cov;

        // Only activeContext() is read by the inner path; populate that one.
        GSContext &cx = sh->m_ctx[d.ctxt ? 1 : 0];
        cx.frame.fbp = d.fbp; cx.frame.fbw = d.fbw; cx.frame.psm = d.fpsm; cx.frame.fbmsk = d.fbmsk;
        cx.zbuf.zbp = d.zbp; cx.zbuf.psm = d.zpsm; cx.zbuf.zmask = d.zmask != 0;
        cx.scissor.x0 = d.sx0; cx.scissor.x1 = d.sx1; cx.scissor.y0 = d.sy0; cx.scissor.y1 = d.sy1;
        cx.xyoffset.ofx = d.ofx; cx.xyoffset.ofy = d.ofy;
        cx.tex0.tbp0 = d.tbp0; cx.tex0.tbw = d.tbw; cx.tex0.psm = d.psm;
        cx.tex0.tw = d.tw; cx.tex0.th = d.th; cx.tex0.tcc = d.tcc; cx.tex0.tfx = d.tfx;
        cx.tex0.cbp = d.cbp; cx.tex0.cpsm = d.cpsm; cx.tex0.csm = d.csm; cx.tex0.csa = d.csa;
        cx.tex0.cld = 0;
        cx.tex1 = d.tex1; cx.clamp = 0; cx.alpha = d.alpha; cx.test = d.test;
        cx.fba = static_cast<uint64_t>(d.fba);

        const int vc = (d.vtxCount < GS::kMaxVerts) ? d.vtxCount : GS::kMaxVerts;
        for (int k = 0; k < vc; ++k)
        {
            const rrv::ir::IrRasterVertex &sv = d.verts[k];
            GSVertex &dv = sh->m_vtxQueue[k];
            dv.x = sv.x; dv.y = sv.y; dv.z = sv.z;
            dv.r = sv.r; dv.g = sv.g; dv.b = sv.b; dv.a = sv.a;
            dv.q = sv.q; dv.s = sv.s; dv.t = sv.t; dv.u = sv.u; dv.v = sv.v; dv.fog = sv.fog;
        }

        switch (sh->m_prim.type)
        {
        case GS_PRIM_SPRITE:
            drawSprite(sh);
            break;
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN:
            drawTriangle(sh);
            break;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
            drawLine(sh);
            break;
        case GS_PRIM_POINT:
        {
            const GSVertex &v = sh->m_vtxQueue[0];
            int px = static_cast<int>(v.x) - (cx.xyoffset.ofx >> 4);
            int py = static_cast<int>(v.y) - (cx.xyoffset.ofy >> 4);
            writePixel(sh, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a);
            break;
        }
        default:
            break;
        }
    }

    rrv::ir::rrv_ir_raster_advance();
}
#endif

void GSRasterizer::writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
#if defined(_DEBUG)
    ++t_rasterPixels; // RRV_RUNTIME_LOG: thread-local (no cross-thread atomic contention)
#endif
    const auto &ctx = gs->activeContext();

    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 ||
        y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);

    if (!alphaTest.writeFramebuffer && !alphaTest.writeDepth)
        return;

    u8* vram = gs->m_vram;

    const u32 fbp  = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw  = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 fmsk = ctx.frame.fbmsk;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const bool alphaBlendEnabled = gs->m_prim.abe;
    const bool destinationAlpha  = alphaTest.preserveDestinationAlpha;

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = (ctx.frame.fbmsk != 0) || alphaBlendEnabled || destinationAlpha;

    u32 fbrgba = 0;
    if (frmw)
    {
        fbrgba = gs->ReadVram(fpsm, fbp, fbw, x, y);

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
    }

    uint ztest_method = (ctx.test >> 17) & 3;


    bool zpass = false;
    switch (ztest_method)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        zpass = z >= gs->ReadVram(zpsm, zbp, fbw, x, y);
        break;
    case 3:
        zpass = z > gs->ReadVram(zpsm, zbp, fbw, x, y);
        break;
    }

    // RRV_STENCIL_PROBE=1 — census of a destination-alpha stencil pass
    // (FBMSK=0x00FFFFFF: RGB fully masked, so the draw exists only to set the
    // Ad that a later feedback composite blends with — docs/TESTING.md
    // T-FLY-OVERLAY). Reports, per source alpha, how many fragments the DEPTH
    // test admits, because in RR5 the stencil is a depth-graded ramp: one
    // unconditional sweep at a=0 followed by ZTST=GREATER sweeps at descending
    // Z. If the ramp writes nothing, the suspect is the Z BUFFER CONTENT, not
    // the alpha test. RRV_STENCIL_PROBE_AT="x,y" additionally logs every stencil
    // fragment that lands on one pixel, in order, with the Z it was tested
    // against — that is what identifies which sweep should have won.
    {
        static const int s_sp = [] {
            const char *e = std::getenv("RRV_STENCIL_PROBE");
            return (e && e[0] && e[0] != '0') ? 1 : 0;
        }();
        static int s_px = -1, s_py = -1;
        static const int s_at = [] {
            const char *e = std::getenv("RRV_STENCIL_PROBE_AT");
            int a = 0, b = 0;
            if (e && e[0] && std::sscanf(e, "%d,%d", &a, &b) == 2) { s_px = a; s_py = b; return 1; }
            return 0;
        }();
        if (s_sp && ctx.frame.fbmsk == 0x00FFFFFFu)
        {
            static std::atomic<uint64_t> seen[256]{}, pass[256]{};
            static std::atomic<uint64_t> total{0};
            seen[a].fetch_add(1, std::memory_order_relaxed);
            if (zpass)
                pass[a].fetch_add(1, std::memory_order_relaxed);
            if (s_at && x == s_px && y == s_py)
                std::fprintf(stderr,
                    "[stencil@%d,%d] fbp=%u a=%u z=%#x zbuf=%#x zbp=%u ztst=%u "
                    "zpass=%d writeFb=%d dest=%#x\n",
                    x, y, (unsigned)ctx.frame.fbp, (unsigned)a, (unsigned)z,
                    (unsigned)gs->ReadVram(zpsm, zbp, fbw, x, y), (unsigned)zbp,
                    (unsigned)ztest_method, (int)zpass,
                    (int)alphaTest.writeFramebuffer,
                    (unsigned)gs->ReadVram(fpsm, fbp, fbw, x, y));
            if ((total.fetch_add(1, std::memory_order_relaxed) % 4000000ull) == 0ull)
            {
                std::fprintf(stderr, "[stencil] zpsm=%#x zbp=%u | admitted/seen:",
                             (unsigned)zpsm, (unsigned)zbp);
                for (int i = 0; i < 256; ++i)
                    if (seen[i].load(std::memory_order_relaxed))
                        std::fprintf(stderr, " a=%d %llu/%llu", i,
                                     (unsigned long long)pass[i].load(std::memory_order_relaxed),
                                     (unsigned long long)seen[i].load(std::memory_order_relaxed));
                std::fprintf(stderr, "\n");
            }
        }
    }

    if (!zpass)
    {
        return;
    }

    const u8 srcR = r;
    const u8 srcG = g;
    const u8 srcB = b;

    if (gs->m_prim.abe)
    {
        uint8_t dr = fbrgba & 0xFF;
        uint8_t dg = (fbrgba >> 8) & 0xFF;
        uint8_t db = (fbrgba >> 16) & 0xFF;
        uint8_t da = (fbrgba >> 24) & 0xFF;

        // PABE disables alpha blending when the source alpha MSB is clear.
        if (!(gs->m_pabe && (a & 0x80u) == 0u))
        {
            uint64_t alphaReg = ctx.alpha;
            uint8_t asel = alphaReg & 3;
            uint8_t bsel = (alphaReg >> 2) & 3;
            uint8_t csel = (alphaReg >> 4) & 3;
            uint8_t dsel = (alphaReg >> 6) & 3;
            uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                       : fix;

            r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
            g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
            b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
        }
        else
        {
            r = srcR;
            g = srcG;
            b = srcB;
        }
    }

    // RRV_BLEND_PROBE: for girl draws (tbp0==6784), accumulate texel(src) vs
    // destination(fb) vs blended result, windowed. Isolates "wrong Cd going in"
    // from "wrong blend math". Compare ours vs GT stream.
    {
        static const int s_bp = [] {
            const char *e = std::getenv("RRV_BLEND_PROBE");
            return (e && e[0] && e[0] != '0') ? 1 : 0;
        }();
        // tbp0 6784 is REUSED across scenes (other assets pair it with cbp 6976);
        // the attract girl is specifically tbp0=6784 + cbp=8596. Filter on both, or
        // the average is dominated by unrelated draws.
        if (s_bp && ctx.tex0.tbp0 == 6784u && ctx.tex0.cbp == 8596u && gs->m_prim.abe &&
            ((ctx.alpha >> 32) & 0xFFu) >= 120u)   // fully-faded pixels only (match GT regime)
        {
            static std::atomic<uint64_t> n{0}, sSR{0}, sSG{0}, sSB{0},
                sDR{0}, sDG{0}, sDB{0}, sRR{0}, sRG{0}, sRB{0}, aMode{0}, aFix{0}, fbpMark{0};
            const uint64_t k = n.fetch_add(1, std::memory_order_relaxed) + 1;
            sSR.fetch_add(srcR, std::memory_order_relaxed); sSG.fetch_add(srcG, std::memory_order_relaxed); sSB.fetch_add(srcB, std::memory_order_relaxed);
            sDR.fetch_add(fbrgba & 0xFF, std::memory_order_relaxed); sDG.fetch_add((fbrgba>>8)&0xFF, std::memory_order_relaxed); sDB.fetch_add((fbrgba>>16)&0xFF, std::memory_order_relaxed);
            sRR.fetch_add(r, std::memory_order_relaxed); sRG.fetch_add(g, std::memory_order_relaxed); sRB.fetch_add(b, std::memory_order_relaxed);
            aMode.store(ctx.alpha & 0xFFu, std::memory_order_relaxed); aFix.store((ctx.alpha>>32)&0xFFu, std::memory_order_relaxed);
            fbpMark.store(ctx.frame.fbp, std::memory_order_relaxed);
            if ((k % 20000ull) == 0ull)
            {
                std::fprintf(stderr,
                    "[blend6784] fbp=%llu src=[%.0f,%.0f,%.0f] dest=[%.0f,%.0f,%.0f] result=[%.0f,%.0f,%.0f] aMode=%#llx fix=%llu\n",
                    (unsigned long long)fbpMark.load(), (double)sSR/k,(double)sSG/k,(double)sSB/k,
                    (double)sDR/k,(double)sDG/k,(double)sDB/k, (double)sRR/k,(double)sRG/k,(double)sRB/k,
                    (unsigned long long)aMode.load(), (unsigned long long)aFix.load());
                n.store(0); sSR.store(0);sSG.store(0);sSB.store(0); sDR.store(0);sDG.store(0);sDB.store(0); sRR.store(0);sRG.store(0);sRB.store(0);
            }
        }
    }

    // RRV_PIXEL_TRACE="x,y" — log EVERY write that lands on one screen pixel of the
    // main display buffers (fbp 0/70), in order, with who wrote it. Used to find what
    // overwrites the girl after she is correctly composited (she writes correct skin
    // tones, then vanishes by present time).
    {
        static int s_tx = -1, s_ty = -1;
        static const int s_pt = [] {
            const char *e = std::getenv("RRV_PIXEL_TRACE");
            if (!e || !e[0]) return 0;
            int a = 0, b = 0;
            if (std::sscanf(e, "%d,%d", &a, &b) == 2) { s_tx = a; s_ty = b; return 1; }
            return 0;
        }();
        if (s_pt && x == s_tx && y == s_ty &&
            (ctx.frame.fbp == 0u || ctx.frame.fbp == 70u))
        {
            std::fprintf(stderr,
                "[px] path=%d fbp=%u tbp0=%u cbp=%u abe=%d aMode=%#x fix=%u fbmsk=%#x "
                "src=[%u,%u,%u] dest=[%u,%u,%u] -> [%u,%u,%u]\n",
                (int)gs->m_currentGifPacketPath,
                ctx.frame.fbp, ctx.tex0.tbp0, ctx.tex0.cbp, (int)gs->m_prim.abe,
                (unsigned)(ctx.alpha & 0xFFu), (unsigned)((ctx.alpha >> 32) & 0xFFu),
                (unsigned)ctx.frame.fbmsk,
                (unsigned)srcR, (unsigned)srcG, (unsigned)srcB,
                (unsigned)(fbrgba & 0xFF), (unsigned)((fbrgba >> 8) & 0xFF), (unsigned)((fbrgba >> 16) & 0xFF),
                (unsigned)r, (unsigned)g, (unsigned)b);
        }
    }

    u32 fbmask = ctx.frame.fbmsk;
    bool zmask = ctx.zbuf.zmask;

    if (!alphaTest.preserveDestinationAlpha &&
        (ctx.fba & 0x1ull) != 0ull &&
        ctx.frame.psm != GS_PSM_CT24)
    {
        a = static_cast<uint8_t>(a | 0x80u);
    }

    u32 pixel = pack32(r, g, b, a);

    if (fbmask != 0)
    {
        pixel = (pixel & ~fbmask) | (fbrgba & fbmask);
    }

    if (alphaTest.preserveDestinationAlpha)
    {
        pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
    }
    
    // format conversion
    if (bitsPerPixel(fpsm) == 16)
    {
        pixel = Rgba8888ToRgba5551(pixel);
    }

    if (alphaTest.writeFramebuffer)
    {
        gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
    }

    if (!zmask && alphaTest.writeDepth)
    {
        gs->WriteVram(zpsm, zbp, fbw, x, y, z);
    }
}

bool GSRasterizer::clutCacheEnabled() { return g_clutCacheEnabled; }

// Decode the full 256-entry CLUT addressable by a paletted texture's raw
// (pre-swizzle) texel value, from the CURRENT vram contents at cbp, using
// exactly the same addressing math lookupCLUT() used to compute live
// (resolveClutIndex + TEXCLUT cbw/cou/cov + GSMem::ReadCTxx + applyTexa).
// Called once at TEX0/TEX2-write ("latch") time so the result is independent
// of whatever later primitives in the same frame do to the source VRAM
// region -- this is the actual bug fix (see B-3 girl-scene writeup: a later
// PSMT8 atlas upload + CT32 render target alias the same VRAM blocks as an
// already-latched palette). A GSRasterizer member (not a free function) so it
// gets GS-friend access to m_vram/m_texa/m_texclut.
//
// Simplification: TEXA (m_texa) is applied here, at latch time, using
// whatever TEXA is current when TEX0/TEX2 is written -- not at sample time.
// Real hardware technically expands 16-bit non-alpha CLUT entries via TEXA at
// read time from the physical on-chip buffer, so a mid-scene TEXA change
// between a CLD load and the draws that sample it would, in principle,
// observe the new TEXA on real hardware but the old one here. TEXA is not
// observed to change mid-scene in RRV's captured traces; if that ever becomes
// wrong, applyTexa would need to move into lookupCLUT's cached-read path and
// the cache would need to store pre-TEXA raw values.
std::shared_ptr<std::array<uint32_t, 256>> GSRasterizer::buildClutTable(GS *gs, const GSTex0Reg &tex)
{
    auto table = std::make_shared<std::array<uint32_t, 256>>();
    const uint32_t clutWidth = (gs->m_texclut.cbw != 0u) ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;

    for (uint32_t raw = 0; raw < 256u; ++raw)
    {
        const uint32_t clutIndex = resolveClutIndex(static_cast<uint8_t>(raw), tex.csm, tex.csa, tex.psm);
        const uint32_t clutX = static_cast<uint32_t>(gs->m_texclut.cou) + (clutIndex & 0x0Fu);
        const uint32_t clutY = static_cast<uint32_t>(gs->m_texclut.cov) + (clutIndex >> 4);

        uint32_t value = 0xFFFF00FFu;
        switch (tex.cpsm)
        {
        case GS_PSM_CT32:
            value = applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT32(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
            break;
        case GS_PSM_CT24:
            value = applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT24(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
            break;
        case GS_PSM_CT16:
            value = applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT16(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
            break;
        case GS_PSM_CT16S:
            value = applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT16S(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
            break;
        default:
            break;
        }
        (*table)[raw] = value;
    }
    return table;
}

// Latch (or skip, per CLD) this context's CLUT at TEX0/TEX2 write time. See
// GSTex0Reg::clutCache (ps2_gs_gpu.h) and buildClutTable() above for why this
// must happen at write time rather than at sample/draw time. No-op unless
// RRV_GS_CLUT_CACHE is set.
//
// CLD cases implemented faithfully (per GS Users Manual / ps2tek "GS TEX0/TEX2
// CLUT Cache Control"):
//   0 = keep current buffer (no reload)     -- exact, except see note below
//   1 = load unconditionally                -- exact
//   2 = load unconditionally, CBP -> CBP0   -- exact
//   3 = load unconditionally, CBP -> CBP1   -- exact
//   4 = load only if CBP != CBP0, then CBP -> CBP0 (only on the taken branch) -- exact
//   5 = load only if CBP != CBP1, then CBP -> CBP1 (only on the taken branch) -- exact
// Simplified:
//   - CLD=0 with no prior latch for this context (clutCache still null, e.g.
//     the very first paletted draw the engine ever sees) is treated as an
//     implicit load instead of leaving the buffer undefined, since we have no
//     "whatever was physically in the chip's CLUT buffer at boot" to fall
//     back to. Real hardware's CLD=0-with-nothing-loaded-yet result is
//     undefined/implementation garbage anyway, so this is a reasonable
//     interpretation of a state hardware never intentionally puts you in.
//   - CBP0/CBP1 are chip-global here (as on real hardware), but the DECODED
//     buffer (clutCache) is tracked per rendering context rather than as one
//     shared physical 1KB buffer. See the CBP0/CBP1 member comment in
//     ps2_gs_gpu.h for why (tile-binning snapshot correctness).
//   - Per PCSX2 prior art, CLUT loads/CBP updates are skipped entirely when
//     tex.psm is not a paletted format (T4/T4HL/T4HH/T8/T8H); TEX0's cbp/csa/
//     cld bits are meaningless noise for non-indexed textures and must not
//     perturb CBP0/CBP1 bookkeeping used by an unrelated later paletted draw.
void GSRasterizer::latchClut(GS *gs, int ctxIndex)
{
    if (!g_clutCacheEnabled)
        return;

    GSTex0Reg &tex = gs->m_ctx[ctxIndex].tex0;

    switch (tex.psm)
    {
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        break;
    default:
        return; // not a paletted texture format: TEX0's CLUT bits are inert.
    }

    bool doLoad = false;
    bool updateCbp0 = false;
    bool updateCbp1 = false;

    switch (tex.cld)
    {
    case 0:
        doLoad = (tex.clutCache == nullptr);
        break;
    case 1:
        doLoad = true;
        break;
    case 2:
        doLoad = true;
        updateCbp0 = true;
        break;
    case 3:
        doLoad = true;
        updateCbp1 = true;
        break;
    case 4:
        doLoad = (tex.cbp != gs->m_clutCbp0);
        updateCbp0 = doLoad;
        break;
    case 5:
        doLoad = (tex.cbp != gs->m_clutCbp1);
        updateCbp1 = doLoad;
        break;
    default:
        // CLD is a 3-bit field; 6/7 are reserved/unused by real hardware.
        // Treat conservatively as an unconditional load rather than silently
        // keeping a possibly-stale buffer.
        doLoad = true;
        break;
    }

    if (doLoad)
        tex.clutCache = buildClutTable(gs, tex);
    if (updateCbp0)
        gs->m_clutCbp0 = tex.cbp;
    if (updateCbp1)
        gs->m_clutCbp1 = tex.cbp;
}

uint32_t GSRasterizer::lookupCLUT(GS *gs, uint8_t index, const GSTex0Reg &tex)
{
    if (g_clutCacheEnabled && tex.clutCache)
    {
        return (*tex.clutCache)[index];
    }

    // Legacy / fallback path: live-sample straight from VRAM. Used when the
    // cache hatch is off (default), or when it's on but nothing has latched a
    // palette for this context yet (tex.clutCache still null).
    const uint32_t clutIndex = resolveClutIndex(index, tex.csm, tex.csa, tex.psm);
    const uint32_t clutWidth = (gs->m_texclut.cbw != 0u) ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(gs->m_texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(gs->m_texclut.cov) + (clutIndex >> 4);

    switch (tex.cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT32(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(gs->m_texa, tex.cpsm, GSMem::ReadCT24(gs->m_vram, tex.cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(gs->m_texa, tex.cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16(gs->m_vram, tex.cbp, clutWidth, clutX, clutY)));
    case GS_PSM_CT16S:
        return applyTexa(gs->m_texa, tex.cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16S(gs->m_vram, tex.cbp, clutWidth, clutX, clutY)));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

// GS texture wrap modes (CLAMP register WMS/WMT).  Until 2026-07-22 the sampler
// ignored this register entirely and always clamped to [0, size-1], which is
// only correct for WMS/WMT=CLAMP.  Ridge Racer V leans on REGION_REPEAT for the
// attract character: `u = (u & MINU) | MAXU` selects a sub-tile of a packed
// atlas, so ignoring it makes every sprite address the whole atlas and the
// screen fills with a regular repeating lattice (B-3, handoff 0.0).
// Opt out with RRV_NO_TEX_CLAMP=1 to A/B against the old behaviour.
namespace
{
    bool texClampModesEnabled()
    {
        static const bool disabled = []
        {
            const char *v = std::getenv("RRV_NO_TEX_CLAMP");
            return v != nullptr && v[0] != '\0' && v[0] != '0';
        }();
        return !disabled;
    }

    inline int applyTexWrap(int c, uint32_t mode, uint32_t lo, uint32_t hi, int size)
    {
        switch (mode)
        {
        case 0: // REPEAT — size is always a power of two (1 << TW/TH).
            return static_cast<int>(static_cast<uint32_t>(c) & static_cast<uint32_t>(size - 1));
        case 2: // REGION_CLAMP — MINU/MAXU are bounds, not a mask.
            return clampInt(c, static_cast<int>(lo), static_cast<int>(hi));
        case 3: // REGION_REPEAT — MINU is the mask, MAXU the OR-in fix.
            return static_cast<int>((static_cast<uint32_t>(c) & lo) | hi);
        case 1:
        default:
            return clampInt(c, 0, size - 1);
        }
    }
}

uint32_t GSRasterizer::sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = gs->activeContext();
    const auto &tex = ctx.tex0;

    int texW = 1 << tex.tw;
    int texH = 1 << tex.th;

    float texUf, texVf;
    if (gs->m_prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    const uint64_t clampReg = ctx.clamp;
    const uint32_t wms  = static_cast<uint32_t>(clampReg & 3u);
    const uint32_t wmt  = static_cast<uint32_t>((clampReg >> 2) & 3u);
    const uint32_t minu = static_cast<uint32_t>((clampReg >> 4) & 0x3FFu);
    const uint32_t maxu = static_cast<uint32_t>((clampReg >> 14) & 0x3FFu);
    const uint32_t minv = static_cast<uint32_t>((clampReg >> 24) & 0x3FFu);
    const uint32_t maxv = static_cast<uint32_t>((clampReg >> 34) & 0x3FFu);
    const bool useWrapModes = texClampModesEnabled();

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        if (useWrapModes)
        {
            sampleU = applyTexWrap(sampleU, wms, minu, maxu, texW);
            sampleV = applyTexWrap(sampleV, wmt, minv, maxv, texH);
            // REGION_* can address outside the declared TW/TH extent by design;
            // bound it only against the GS's own 1024-texel maximum so a bad
            // register value can never index outside VRAM.
            sampleU = clampInt(sampleU, 0, 1023);
            sampleV = clampInt(sampleV, 0, 1023);
        }
        else
        {
            sampleU = clampInt(sampleU, 0, texW - 1);
            sampleV = clampInt(sampleV, 0, texH - 1);
        }

        u32 out = gs->ReadVram(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(gs->m_texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(gs->m_texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint32_t col = lookupCLUT(gs, static_cast<u8>(out), tex);
            // RRV_ATLAS_PROBE: accumulate what the girl atlas (tbp0==6784) actually
            // returns AT DRAW TIME — raw index + post-CLUT color — isolating the
            // texture sample from any downstream blend. Compare ours vs GT stream.
            static const int s_ap = [] {
                const char *e = std::getenv("RRV_ATLAS_PROBE");
                return (e && e[0] && e[0] != '0') ? 1 : 0;
            }();
            if (s_ap && tex.tbp0 == 6784u)
            {
                // Windowed: reset every 500k samples so each printed line reflects
                // only the most recent samples (the girl scene is the tail of the run).
                static std::atomic<uint64_t> n{0}, sIdx{0}, sR{0}, sG{0}, sB{0}, sA{0}, nzIdx{0}, cbpMark{0};
                const uint64_t k = n.fetch_add(1, std::memory_order_relaxed) + 1;
                sIdx.fetch_add(static_cast<u8>(out), std::memory_order_relaxed);
                if (static_cast<u8>(out) != 0) nzIdx.fetch_add(1, std::memory_order_relaxed);
                sR.fetch_add(col & 0xFF, std::memory_order_relaxed);
                sG.fetch_add((col >> 8) & 0xFF, std::memory_order_relaxed);
                sB.fetch_add((col >> 16) & 0xFF, std::memory_order_relaxed);
                sA.fetch_add((col >> 24) & 0xFF, std::memory_order_relaxed);
                cbpMark.store(tex.cbp, std::memory_order_relaxed);
                if ((k % 200000ull) == 0ull)
                {
                    std::fprintf(stderr,
                        "[atlas6784] window200k avgIdx=%.1f nonzeroIdx=%.1f%% avgRGBA=[%.1f,%.1f,%.1f,%.1f] cbp=%llu\n",
                        (double)sIdx / k, 100.0 * nzIdx / k,
                        (double)sR / k, (double)sG / k, (double)sB / k, (double)sA / k,
                        (unsigned long long)cbpMark.load(std::memory_order_relaxed));
                    n.store(0); sIdx.store(0); nzIdx.store(0); sR.store(0); sG.store(0); sB.store(0); sA.store(0);
                }
            }
            return col;
        }
        }

        return 0xFFFF00FFu;
    };

    if (!tex1UsesLinearFilter(ctx.tex1))
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

namespace
{
    // Defined alongside the RowPool below; runs fn over rows [y0,y1) across the
    // worker pool + caller, returns false if threading is disabled/unavailable.
    bool rasterTryParallelRows(int y0, int y1, const std::function<void(int, int)> &fn);
}

void GSRasterizer::drawSprite(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
    {
        // maybe a log here idk ?
        return;
    }

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    const bool looksLikeDisplayCopy =
        gs->m_prim.tme &&
        gs->m_prim.abe &&
        gs->m_prim.fst &&
        gs->m_prim.ctxt &&
        ctx.frame.fbp != ctx.tex0.tbp0 &&
        alphaMode == 0x64u &&
        (alphaFix == 0x60u || alphaFix == 0x80u) &&
        unclippedX0 <= 0 &&
        unclippedY0 <= 0 &&
        unclippedX1 >= 639 &&
        unclippedY1 >= 447;
    if (looksLikeDisplayCopy)
    {
        gs->m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
        gs->m_hasPreferredDisplaySource = true;
    }

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    const auto &tex = ctx.tex0;
    int texW = 1, texH = 1;
    float u0f = 0.0f, v0f = 0.0f, u1f = 0.0f, v1f = 0.0f, spriteW = 1.0f, spriteH = 1.0f;
    if (gs->m_prim.tme)
    {
        texW = 1 << tex.tw;
        texH = 1 << tex.th;
        if (texW == 0)
            texW = 1;
        if (texH == 0)
            texH = 1;

        if (gs->m_prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        spriteW = static_cast<float>(spanX);
        spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;
    }

    auto renderRows = [&](int yA, int yB)
    {
      if (gs->m_prim.tme)
      {
        // GS sprites interpolate from the covered pixel coordinate, not its
        // centre.  Set this default-off hatch only to recover the former
        // centre-sampled behaviour while investigating a regression.
        static const bool s_legacySpriteCenter = []
        {
            const char *v = std::getenv("RRV_GS_LEGACY_SPRITE_CENTER");
            return v && v[0] && v[0] != '0';
        }();

        for (int y = yA; y < yB; ++y)
        {
            const float sampleOffset = s_legacySpriteCenter ? 0.5f : 0.0f;
            float ty = (static_cast<float>(y - unclippedY0) + sampleOffset) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + sampleOffset) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (gs->m_prim.fst)
                {
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texUf * 16.0f)), 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(static_cast<int>(std::lround(texVf * 16.0f)), 0, 0xFFFF));
                    texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = sampleTexture(gs,
                                          texUf / static_cast<float>(texW),
                                          texVf / static_cast<float>(texH),
                                          1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                writePixel(gs, x, y, z1, color.r, color.g, color.b, color.a);
            }
        }
      }
      else
      {
        for (int y = yA; y < yB; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                writePixel(gs, x, y, z1, r, g, b, a);
      }
    };

    const int spanRowsS = drawY1 - drawY0 + 1;
    const long long areaS = static_cast<long long>(drawX1 - drawX0 + 1) * static_cast<long long>(spanRowsS);
    if (spanRowsS >= 16 && areaS >= 4096 && rasterTryParallelRows(drawY0, drawY1 + 1, renderRows))
        return;
    renderRows(drawY0, drawY1 + 1);
}

namespace
{
    // Fork-join pool for scanline-parallel rasterization of large primitives.
    // Only the game thread rasterizes, so it is the sole caller of parallelRows;
    // workers render disjoint y-ranges of the SAME primitive (disjoint (x,y) ->
    // disjoint swizzled VRAM addresses => no data races), and the caller blocks
    // until all rows finish, so primitives still composite in submission order.
    class RowPool
    {
    public:
        void ensureInit()
        {
            if (m_init.load(std::memory_order_acquire))
                return;
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_init.load(std::memory_order_relaxed))
                return;
            int hc = static_cast<int>(std::thread::hardware_concurrency());
            // Leave cores for the game/present/vblank/audio threads.
            m_workers = std::max(0, std::min(hc - 2, 7));
            for (int i = 0; i < m_workers; ++i)
                m_threads.emplace_back([this, i] { workerLoop(i + 1); });
            m_init.store(true, std::memory_order_release);
        }

        bool enabled() const { return m_workers > 0; }

        // Render rows [y0,y1) via fn(a,b), split across workers + the caller.
        void parallelRows(int y0, int y1, const std::function<void(int, int)> &fn)
        {
            const int parts = m_workers + 1;
            const int rows = y1 - y0;
            const int chunk = (rows + parts - 1) / parts;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_y0 = y0; m_y1 = y1; m_chunk = chunk; m_fn = &fn;
                m_done.store(0, std::memory_order_relaxed);
                ++m_gen;
            }
            m_cvGo.notify_all();
            // Caller renders chunk 0 while workers handle 1..m_workers.
            const int cy1 = std::min(y1, y0 + chunk);
            if (y0 < cy1)
                fn(y0, cy1);
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [this] { return m_done.load(std::memory_order_relaxed) == m_workers; });
        }

        ~RowPool()
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_stop = true;
                ++m_gen;
            }
            m_cvGo.notify_all();
            for (auto &t : m_threads)
                if (t.joinable())
                    t.join();
        }

    private:
        void workerLoop(int id)
        {
            int myGen = 0;
            for (;;)
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvGo.wait(lk, [this, &myGen] { return m_stop || m_gen != myGen; });
                if (m_stop)
                    return;
                myGen = m_gen;
                const int y0 = m_y0, chunk = m_chunk, y1 = m_y1;
                const std::function<void(int, int)> *fn = m_fn;
                lk.unlock();

                const int ry0 = y0 + id * chunk;
                const int ry1 = std::min(y1, ry0 + chunk);
                if (ry0 < ry1)
                    (*fn)(ry0, ry1);
#if defined(_DEBUG)
                if (t_rasterPixels) { ps2_diag::g_pixelCount.fetch_add(t_rasterPixels, std::memory_order_relaxed); t_rasterPixels = 0; }
#endif

                if (m_done.fetch_add(1, std::memory_order_acq_rel) + 1 == m_workers)
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_cvDone.notify_one();
                }
            }
        }

        std::atomic<bool> m_init{false};
        int m_workers = 0;
        std::vector<std::thread> m_threads;
        std::mutex m_mtx;
        std::condition_variable m_cvGo, m_cvDone;
        int m_gen = 0;
        bool m_stop = false;
        int m_y0 = 0, m_y1 = 0, m_chunk = 0;
        const std::function<void(int, int)> *m_fn = nullptr;
        std::atomic<int> m_done{0};
    };

    RowPool g_rowPool;
    // Per-primitive scanline threading is DISPATCH-BOUND (thousands of prims/frame
    // => CV-signal overhead eats the parallel win, ~11% raster reduction, caps
    // ~35 game-fps). It is validated output-correct but OFF by default (opt-in via
    // RRV_RASTER_MT) pending the tile-binning rework that dispatches once per frame.
    const bool g_rasterSingleThread = (std::getenv("RRV_RASTER_MT") == nullptr);

    bool rasterTryParallelRows(int y0, int y1, const std::function<void(int, int)> &fn)
    {
        if (g_rasterSingleThread || t_tileHazardSerialReplay)
            return false;
        g_rowPool.ensureInit();
        if (!g_rowPool.enabled())
            return false;
#if defined(_DEBUG)
        const uint64_t pb = ps2_diag::g_pixelCount.load(std::memory_order_relaxed);
        g_rowPool.parallelRows(y0, y1, fn);
        ps2_diag::g_parPixels.fetch_add(ps2_diag::g_pixelCount.load(std::memory_order_relaxed) - pb, std::memory_order_relaxed);
#else
        g_rowPool.parallelRows(y0, y1, fn);
#endif
        return true;
    }

}

void GSRasterizer::drawTriangle(GS *gs)
{
#if defined(_DEBUG)
    ps2_diag::g_triCount.fetch_add(1, std::memory_order_relaxed); // RRV_RUNTIME_LOG: [perf:t1] tris/frame
#endif
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const GSVertex &v2 = gs->m_vtxQueue[2];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    auto renderRows = [&](int yA, int yB)
    {
      for (int y = yA; y < yB; ++y)
      {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (gs->m_prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (gs->m_prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (gs->m_prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    const float invQ0 = 1.0f / fabsQ(v0.q);
                    const float invQ1 = 1.0f / fabsQ(v1.q);
                    const float invQ2 = 1.0f / fabsQ(v2.q);
                    const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                    const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                    const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                    iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                    is = sOverQ * iq;
                    it = tOverQ * iq;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = sampleTexture(gs, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            writePixel(gs, x, y, static_cast<u32>(z + 0.5), r, g, b, a);
        }
      }
    };

    // Scanline-parallel for large triangles (they hold most of the fill work);
    // small ones render inline to avoid dispatch overhead. Output-preserving:
    // disjoint y-ranges write disjoint pixels and the caller blocks until done.
    const int spanRows = maxY - minY + 1;
    const long long area = static_cast<long long>(maxX - minX + 1) * static_cast<long long>(spanRows);
    if (spanRows >= 16 && area >= 4096 && rasterTryParallelRows(minY, maxY + 1, renderRows))
        return;
    renderRows(minY, maxY + 1);
}

void GSRasterizer::drawLine(GS *gs)
{
    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (gs->m_prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);

        writePixel(gs, x0, y0, static_cast<u32>(z), r, g, b, a);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}
