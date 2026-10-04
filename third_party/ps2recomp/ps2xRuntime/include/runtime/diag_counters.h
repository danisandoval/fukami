// RRV_RUNTIME_LOG performance counters (Phase-3 sim-rate / tile-binning / VU1
// profiling). Zero-cost when built without -DRRV_RUNTIME_LOG=ON: every
// increment site is gated behind #if defined(_DEBUG) (see ps2_log.h /
// HANDOFF.md sec.4). Reported periodically as [perf:t1]/[perf:cam]/[perf:vu1]/
// [perf:batch]/[perf:hist] in PS2Runtime::run() (ps2_runtime.cpp).
// Cross-thread: the main/present loop, the vblank worker, and the GS rasterizer
// each run on different threads, so these are atomics. Definitions live in
// ps2_runtime.cpp (no new translation unit / CMake change needed).
#pragma once

#include <atomic>
#include <cstdint>

namespace ps2_diag
{
    extern std::atomic<uint64_t> g_ticksCredited;    // EE vblank ticks raised
    extern std::atomic<uint64_t> g_framesPresented;  // host frames presented
    extern std::atomic<uint64_t> g_triCount;         // triangles rasterized
    extern std::atomic<uint64_t> g_rasterNs;         // wall-ns spent in drawPrimitive
    extern std::atomic<uint64_t> g_pixelCount;       // covered pixels (writePixel entries)
    extern std::atomic<uint64_t> g_primPixBucket[7]; // pixels binned by per-primitive size
    extern std::atomic<uint64_t> g_parPixels;        // pixels rendered via the parallel path
    extern std::atomic<uint64_t> g_vu1Ns;            // wall-ns spent in VU1Interpreter::run
    extern std::atomic<uint64_t> g_vu1Instrs;        // VU1 instruction-pairs executed
    extern std::atomic<uint64_t> g_vu1Mscals;        // VU1 program invocations (execute/resume)
    extern std::atomic<uint64_t> g_vu1MpgUploads;    // VU1 microcode (MPG) uploads = cache invalidations

    // Tile-binning feasibility. All GS draws + VRAM uploads run on the
    // single GS thread, so we can exactly measure the run-length of consecutive
    // draws uninterrupted by a VRAM mutation (image upload / local-to-local xfer).
    // That run-length is the achievable tile-binning batch size: an upload whose
    // destination could alias a later draw's texture source forces a flush, so the
    // draws BETWEEN uploads are what batch. Big runs => tile-binning amortizes the
    // per-frame dispatch; runs of ~1 => it degrades to flush-per-upload (no win).
    extern std::atomic<uint64_t> g_primCount;        // all primitives via drawPrimitive
    extern std::atomic<uint64_t> g_uploadEvents;     // VRAM-mutating events (uploads/xfers)
    extern std::atomic<uint64_t> g_batchBuckets[7];  // count of draw-runs, binned by run length

    inline thread_local uint64_t t_primsSinceUpload = 0;

    inline void noteDraw()
    {
        ++t_primsSinceUpload;
        g_primCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Call at each VRAM-mutating event. Consecutive uploads with no draws between
    // them collapse (n==0 => no-op), so chunked DMA of one texture counts once.
    inline void noteUpload()
    {
        g_uploadEvents.fetch_add(1, std::memory_order_relaxed);
        const uint64_t n = t_primsSinceUpload;
        if (n)
        {
            const int b = n < 2 ? 0 : n < 8 ? 1 : n < 32 ? 2 : n < 128 ? 3 : n < 512 ? 4 : n < 2048 ? 5 : 6;
            g_batchBuckets[b].fetch_add(1, std::memory_order_relaxed);
            t_primsSinceUpload = 0;
        }
    }

    // B-1 car-geometry probe (RRV_CAR_GIF_DIAG / RRV_VU1_DIAG car split).
    // The demo car registers in EE slot 0x334E98 at attract phase 7 and stays
    // live (0xFFFFFFFF = empty slot). The GS/GIF and VU1 decode paths run on
    // threads with no direct EE-memory handle, so the value is latched here from
    // the upstream sites that DO have PS2Memory (submitGifPacket, VU1 mscal
    // entry). It changes slowly (a scene-level latch), so a coarse mirror is
    // sufficient to gate "what reaches GIF/GS while the car is live".
    constexpr uint32_t kCarSlotAddr = 0x334E98u;
    constexpr uint32_t kDemoPhaseAddr = 0x334E94u; // adjacent demo phase word
    inline std::atomic<uint32_t> g_carSlot{0xFFFFFFFFu};
    inline std::atomic<uint32_t> g_demoPhase{0xFFFFFFFFu};
    inline void noteCarSlot(uint32_t v) { g_carSlot.store(v, std::memory_order_relaxed); }
    inline void noteDemoPhase(uint32_t v) { g_demoPhase.store(v, std::memory_order_relaxed); }
    inline bool carLive() { return g_carSlot.load(std::memory_order_relaxed) != 0xFFFFFFFFu; }
    inline uint32_t demoPhase() { return g_demoPhase.load(std::memory_order_relaxed); }
}
