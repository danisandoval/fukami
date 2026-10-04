// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Contains logic adapted from PCSX2 (GPL-3.0+); the upstream revision, file and function of each adaptation
// are cited at the adapted code and in third_party/ps2recomp/RRV_CHANGES.md. Rest: PS2Recomp (GPL-3.0).
#include "runtime/ps2_memory.h"

#ifndef RRV_GATE4_QUIET_GENERATION_V1
#define RRV_GATE4_QUIET_GENERATION_V1
#include <atomic>
#include <cstdint>
namespace rrv::guest_time {
// Gate-4 lazy checkpoint: bumped by every change to a checkpoint quiet input.
inline std::atomic<uint64_t> quiet_generation{1};
inline void QuietBump() { quiet_generation.fetch_add(1, std::memory_order_relaxed); }
// The same bump without the bus lock, for code that only ever runs in the
// serialized producer domain (the guest-time owner, the INTC, the hardware
// commit): there it is `lock inc` five times per commit, about 0.4 ms per
// VBlank start on a Steam Deck (profile 2026-10-02). Every reader of the
// generation is in that domain too, so a reader never runs between this load
// and this store. Other threads keep bumping with QuietBump(); if one lands in
// between, the two bumps become one, and that is enough: a reader only asks
// whether the value differs from the one it armed with, the value it armed
// with is at most the value loaded here, and what is stored is one more.
inline void QuietBumpProducer() {
    quiet_generation.store(quiet_generation.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
}
#endif
#include "runtime/diag_counters.h"
#include "rrv_gate4_owner_timeline.h" // Gate-4 S0 owner timeline (RRV_GATE4_OWNER_TIMELINE)
#include "ps2_log.h"
#include <atomic>
#include <optional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <memory>
#include <algorithm>
#include "runtime/rrv_ring_copy.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

// RRV_FLAG_A_M1_DIAG — metadata-only rate probe for the FLAG_A pin audit.
// The present loop calls the two public helpers below once per host present and
// once per existing one-second perf report.  GIF ingress increments the guest
// frame counter only for the game's PATH3 320-byte/NLOOP=19 clear signature.
// Default off; no guest payload bytes are logged or persisted.
namespace
{
    constexpr uint32_t kFlagAM1PhaseCount = 64u;
    std::atomic<uint64_t> s_flagAM1Presents[kFlagAM1PhaseCount]{};
    std::atomic<uint64_t> s_flagAM1Clear320[kFlagAM1PhaseCount]{};
    std::atomic<uint64_t> s_flagAM1PresentWallNs[kFlagAM1PhaseCount]{};

    bool flagAM1DiagEnabledCached()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_FLAG_A_M1_DIAG");
            return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }
}

bool rrvFlagAM1DiagEnabled()
{
    return flagAM1DiagEnabledCached();
}

void rrvNoteFlagAM1Present(unsigned phase, uint64_t nowNs)
{
    if (!flagAM1DiagEnabledCached() || phase >= kFlagAM1PhaseCount)
        return;

    // Called only by the host present thread. Attribute the interval since the
    // previous present to the phase observed at that previous sample.
    static uint64_t lastNs = 0u;
    static uint32_t lastPhase = UINT32_MAX;
    if (lastNs != 0u && lastPhase < kFlagAM1PhaseCount && nowNs >= lastNs)
        s_flagAM1PresentWallNs[lastPhase].fetch_add(nowNs - lastNs,
                                                   std::memory_order_relaxed);
    lastNs = nowNs;
    lastPhase = phase;
    s_flagAM1Presents[phase].fetch_add(1u, std::memory_order_relaxed);
}

void rrvReportFlagAM1Rates(double seconds)
{
    if (!flagAM1DiagEnabledCached() || seconds <= 0.0)
        return;

    static uint64_t presentBase[kFlagAM1PhaseCount]{};
    static uint64_t clearBase[kFlagAM1PhaseCount]{};
    static uint64_t wallBase[kFlagAM1PhaseCount]{};
    uint64_t totalPresents = 0u;
    uint64_t totalClears = 0u;
    for (uint32_t phase = 0u; phase < kFlagAM1PhaseCount; ++phase)
    {
        const uint64_t presents = s_flagAM1Presents[phase].load(std::memory_order_relaxed);
        const uint64_t clears = s_flagAM1Clear320[phase].load(std::memory_order_relaxed);
        const uint64_t wallNs = s_flagAM1PresentWallNs[phase].load(std::memory_order_relaxed);
        const uint64_t dPresents = presents - presentBase[phase];
        const uint64_t dClears = clears - clearBase[phase];
        const uint64_t dWallNs = wallNs - wallBase[phase];
        presentBase[phase] = presents;
        clearBase[phase] = clears;
        wallBase[phase] = wallNs;
        totalPresents += dPresents;
        totalClears += dClears;
        if (dPresents != 0u || dClears != 0u)
        {
            const double phaseSeconds = static_cast<double>(dWallNs) / 1.0e9;
            std::fprintf(stderr,
                         "[flag-a-m1:rate] phase=%u wall=%.6f presents=%llu "
                         "present_fps=%.3f clear320=%llu build_hz=%.3f\n",
                         phase, phaseSeconds,
                         static_cast<unsigned long long>(dPresents),
                         phaseSeconds > 0.0 ? static_cast<double>(dPresents) / phaseSeconds : 0.0,
                         static_cast<unsigned long long>(dClears),
                         phaseSeconds > 0.0 ? static_cast<double>(dClears) / phaseSeconds : 0.0);
        }
    }
    std::fprintf(stderr,
                 "[flag-a-m1:total] wall=%.3f presents=%llu present_fps=%.3f "
                 "clear320=%llu build_hz=%.3f\n",
                 seconds, static_cast<unsigned long long>(totalPresents),
                 static_cast<double>(totalPresents) / seconds,
                 static_cast<unsigned long long>(totalClears),
                 static_cast<double>(totalClears) / seconds);
}

// ---------------------------------------------------------------------------
// RRV_DMA_ORDER_DIAG — B-3 cross-channel GIF ordering probe.
//
// The girl cinematic's defect (docs/HANDOFF_GIRL_B3.md, top section) is that the
// frame's big PATH3 chain reaches the GS before the same frame's VIF1 setup+city,
// where hardware delivers it after. The recording proves the SUBMIT order; this
// probe records the EE's KICK order plus every MSKPATH3 transition, which is what
// distinguishes "the game masks PATH3 and we mishandle the mask" from "the game
// relies on the transfer taking time / on PATH1 arbitration".
//
// Bounded by construction: a fixed 256-entry ring, dumped ONCE when the girl
// frame's 1,845,152 B PATH3 chain is kicked, followed by a fixed tail of later
// events. Metadata only — never payload bytes. Default off.
// ---------------------------------------------------------------------------
namespace
{
    constexpr size_t kDmaOrderRing = 256;
    constexpr size_t kDmaOrderTail = 160;
    // Threshold, not an exact size: the girl frame's chain is 1,845,152 B in the
    // captured run, but any PATH3 chain this large is a frame display list and is
    // the interesting case. An exact match silently never fires if the size moves.
    constexpr uint32_t kBigPath3ChainBytes = 400000u;
}

// Diagnostic env lookups on the GIF submit path, resolved ONCE.
//
// std::getenv takes a lock inside libc and walks environ linearly. Three of the
// probes below sat directly in submitGifPacket() and in the PATH3 chain walk --
// i.e. ~900 times per guest display list, ~27,000 times a second -- to compare
// against variables that are unset in every shipping run. A profile of the
// release build found getenv/__findenv_locked reachable from submitGifPacket
// among the top self-time symbols on the guest thread (docs/TESTING.md
// T-EE-PATHWATCH). The probes are unchanged; only the lookup is hoisted.
static const char *rrvCachedEnv(const char *name)
{
    // Small, fixed, append-only: one entry per probe below. Built on first use
    // per name, never mutated afterwards, and only read from the guest thread's
    // submit path plus the DMA walk, both of which run under the guest lock.
    static std::map<std::string, const char *> cache;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.emplace(name, std::getenv(name)).first;
    return it->second;
}

bool rrvDmaOrderDiagEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("RRV_DMA_ORDER_DIAG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// RRV_DMA_ORDER_DIAG=<bytes> triggers the dump on a PATH3 chain kick of exactly
// that size; =1 uses the girl frame's chain. Configurable because the size that
// exposes the defect is scene-specific.
static uint32_t rrvDmaOrderTriggerBytes()
{
    static const uint32_t bytes = []() -> uint32_t
    {
        const char *value = std::getenv("RRV_DMA_ORDER_DIAG");
        if (!value || !value[0])
            return kBigPath3ChainBytes;
        const unsigned long parsed = std::strtoul(value, nullptr, 10);
        return (parsed > 1ul) ? static_cast<uint32_t>(parsed) : kBigPath3ChainBytes;
    }();
    return bytes;
}

void rrvNoteDmaOrderEvent(const char *what, unsigned long long a, unsigned long long b)
{
    if (!rrvDmaOrderDiagEnabled())
        return;

    static std::vector<std::string> ring(kDmaOrderRing);
    static size_t head = 0;
    static size_t filled = 0;
    static bool dumped = false;
    static size_t tailLeft = 0;
    static unsigned long long seq = 0;

    if (seq == 0)
    {
        // One-line wiring check: proves the hooks fire even if the trigger never
        // matches (a silent probe is indistinguishable from a dead one).
        std::fprintf(stderr, "[dma-order] armed, trigger>=%u B on a PATH3 chain kick\n",
                     rrvDmaOrderTriggerBytes());
    }

    char line[128];
    std::snprintf(line, sizeof(line), "%6llu %-10s %llu %llu", seq++, what, a, b);

    if (tailLeft > 0)
    {
        std::fprintf(stderr, "[dma-order] %s\n", line);
        --tailLeft;
        return;
    }

    ring[head] = line;
    head = (head + 1) % kDmaOrderRing;
    if (filled < kDmaOrderRing)
        ++filled;

    // The girl frame's PATH3 chain is the one moment the defect is observable.
    const bool trigger = !dumped && std::strcmp(what, "kick-gif") == 0 &&
                         a >= rrvDmaOrderTriggerBytes();
    if (!trigger)
        return;

    dumped = true;
    std::fprintf(stderr,
                 "[dma-order] ==== big PATH3 chain (%llu B) kicked; preceding %zu events ====\n",
                 a, filled);
    const size_t start = (head + kDmaOrderRing - filled) % kDmaOrderRing;
    for (size_t i = 0; i < filled; ++i)
        std::fprintf(stderr, "[dma-order] %s\n", ring[(start + i) % kDmaOrderRing].c_str());
    std::fprintf(stderr, "[dma-order] ==== following %zu events ====\n", kDmaOrderTail);
    tailLeft = kDmaOrderTail;
}

// RRV_GIF_PATH_LATENCY — the B-3 cross-channel ordering fix (default ON;
// explicit =0 is the rollback hatch).
//
// Measured mechanism (docs/HANDOFF_GIRL_B3.md): the EE kicks the frame's VIF1
// geometry chain and THEN a 320-byte PATH3 clear. On hardware the clear reaches
// the GS first, because the VIF1 chain still has to be unpacked into VU1 and run
// a microprogram before any XGKICK exists, while 320 bytes of PATH3 move in
// microseconds. We expand a VIF1 chain — VU1 and every XGKICK — synchronously at
// kick time, so the clear can never overtake it and lands on top of the finished
// city instead of ahead of it.
//
// The hatch restores the race: hold a kicked VIF1 chain, let small PATH3 packets
// past it, and expand it once a PATH3 transfer too large to overtake it arrives
// (or the EE starts the next frame's chain). Guest-visible DMA completion is
// unchanged — only the expansion moves.
//
// RRV_GIF_LATENCY_DIAG — does a held VIF1 chain ever survive a frame boundary?
//
// The held queue is flushed only by a large PATH3 kick or the next VIF1 kick, so
// in principle a scene that ends without either strands a frame of geometry until
// the next kick. This measures whether that actually happens: it records the
// vsync tick when the queue goes empty->non-empty and reports the delta at flush,
// plus anything still held at teardown. Metadata only, default off.
namespace ps2_syscalls
{
    uint64_t GetCurrentVSyncTick();
}

namespace
{
    bool gifLatencyDiagEnabled()
    {
        static const bool enabled = []
        {
            const char *v = std::getenv("RRV_GIF_LATENCY_DIAG");
            return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }();
        return enabled;
    }

    uint64_t g_latencyHoldTick = 0u;   // vsync tick when the queue became non-empty
    uint64_t g_latencyHolds = 0u;      // times a chain was held
    uint64_t g_latencyFlushes = 0u;    // times the held queue was drained
    uint64_t g_latencyMaxTicks = 0u;   // worst hold, in vsync ticks
    uint64_t g_latencyCrossings = 0u;  // holds that survived >=1 vblank

    // Per-scene-phase accounting. "How are holds released in THIS scene" is the
    // promotion question: a high frame-boundary rate is expected where the scene
    // has no large PATH3 packet to flush against, and suspicious where it does.
    constexpr uint32_t kLatencyPhases = 64u;
    uint32_t g_latencyPhase = 0u;                   // phase at the current hold
    uint64_t g_latencyByPhaseHolds[kLatencyPhases]{};
    uint64_t g_latencyByPhaseBig[kLatencyPhases]{};      // flushed by a large PATH3 kick
    uint64_t g_latencyByPhaseNextVif[kLatencyPhases]{};  // flushed by the next VIF1 chain
    uint64_t g_latencyByPhaseBackstop[kLatencyPhases]{}; // flushed at the frame boundary
    uint64_t g_latencyByPhaseMaxTicks[kLatencyPhases]{};
}

void rrvReportGifLatencyByPhase()
{
    if (!gifLatencyDiagEnabled())
        return;
    std::fprintf(stderr, "[gif-latency:phase] phase holds big next-vif1 backstop maxTicks\n");
    for (uint32_t p = 0u; p < kLatencyPhases; ++p)
    {
        if (g_latencyByPhaseHolds[p] == 0u)
            continue;
        std::fprintf(stderr, "[gif-latency:phase] %5u %6llu %4llu %10llu %9llu %8llu\n", p,
                     static_cast<unsigned long long>(g_latencyByPhaseHolds[p]),
                     static_cast<unsigned long long>(g_latencyByPhaseBig[p]),
                     static_cast<unsigned long long>(g_latencyByPhaseNextVif[p]),
                     static_cast<unsigned long long>(g_latencyByPhaseBackstop[p]),
                     static_cast<unsigned long long>(g_latencyByPhaseMaxTicks[p]));
    }
}

void rrvNoteGifLatencyHold(uint32_t scenePhase)
{
    if (!gifLatencyDiagEnabled())
        return;
    g_latencyHoldTick = ps2_syscalls::GetCurrentVSyncTick();
    g_latencyPhase = (scenePhase < kLatencyPhases) ? scenePhase : 0u;
    ++g_latencyByPhaseHolds[g_latencyPhase];
    ++g_latencyHolds;
    // Periodic denominator: crossings are only meaningful against the total.
    if ((g_latencyHolds % 256u) == 0u)
    {
        std::fprintf(stderr,
                     "[gif-latency] holds=%llu flushes=%llu crossings=%llu maxHeldTicks=%llu\n",
                     static_cast<unsigned long long>(g_latencyHolds),
                     static_cast<unsigned long long>(g_latencyFlushes),
                     static_cast<unsigned long long>(g_latencyCrossings),
                     static_cast<unsigned long long>(g_latencyMaxTicks));
        rrvReportGifLatencyByPhase();
    }
}

void rrvNoteGifLatencyFlush(const char *reason)
{
    if (!gifLatencyDiagEnabled())
        return;
    const uint64_t now = ps2_syscalls::GetCurrentVSyncTick();
    const uint64_t held = (now >= g_latencyHoldTick) ? (now - g_latencyHoldTick) : 0u;
    ++g_latencyFlushes;
    if (held > g_latencyMaxTicks)
        g_latencyMaxTicks = held;
    if (held > g_latencyByPhaseMaxTicks[g_latencyPhase])
        g_latencyByPhaseMaxTicks[g_latencyPhase] = held;
    if (std::strcmp(reason, "big-path3") == 0)        ++g_latencyByPhaseBig[g_latencyPhase];
    else if (std::strcmp(reason, "next-vif1") == 0)   ++g_latencyByPhaseNextVif[g_latencyPhase];
    else                                              ++g_latencyByPhaseBackstop[g_latencyPhase];
    if (held > 0u)
    {
        ++g_latencyCrossings;
        if (false) std::fprintf(stderr,
                     "[gif-latency] held %llu vsync tick(s) before flush (%s) "
                     "holds=%llu flushes=%llu crossings=%llu max=%llu\n",
                     static_cast<unsigned long long>(held), reason,
                     static_cast<unsigned long long>(g_latencyHolds),
                     static_cast<unsigned long long>(g_latencyFlushes),
                     static_cast<unsigned long long>(g_latencyCrossings),
                     static_cast<unsigned long long>(g_latencyMaxTicks));
    }
}

void rrvReportGifLatencyStranded(size_t stillHeld)
{
    if (!gifLatencyDiagEnabled())
        return;
    std::fprintf(stderr,
                 "[gif-latency] teardown: stillHeld=%zu holds=%llu flushes=%llu "
                 "crossings=%llu maxHeldTicks=%llu\n",
                 stillHeld,
                 static_cast<unsigned long long>(g_latencyHolds),
                 static_cast<unsigned long long>(g_latencyFlushes),
                 static_cast<unsigned long long>(g_latencyCrossings),
                 static_cast<unsigned long long>(g_latencyMaxTicks));
}

// RRV_GIF_PATH_LATENCY=1 uses the default threshold; =<bytes> overrides it.
// Unset, empty and =1 use the default threshold; =0 disables the hold; a
// numeric value >1 overrides the threshold.
const RrvGifPathLatencyConfig &gifPathLatencyConfig()
{
    static const RrvGifPathLatencyConfig config =
        rrvParseGifPathLatencyConfig(std::getenv("RRV_GIF_PATH_LATENCY"));
    return config;
}

bool gifPathLatencyEnabled()
{
    return gifPathLatencyConfig().enabled;
}

uint64_t gifPathLatencySmallPacketBytes()
{
    return gifPathLatencyConfig().smallPacketBytes;
}

// RRV_GIF_PATH3_INTERLEAVE — cross-channel DMA slicing. Unset/empty/=1 use the
// default equal-bandwidth ratio; =0 disables slicing (PATH3 is then submitted
// whole, after the VIF1 chain, which is the pre-2026-08-15 behaviour).
const RrvGifPath3InterleaveConfig &gifPath3InterleaveConfig()
{
    static const RrvGifPath3InterleaveConfig config =
        rrvParseGifPath3InterleaveConfig(std::getenv("RRV_GIF_PATH3_INTERLEAVE"));
    return config;
}

bool gifPath3InterleaveEnabled()
{
    return gifPath3InterleaveConfig().enabled;
}

// RRV_GIF_PATH3_HAZARD — see RrvGsBufferShadow in ps2_memory.h. Default on;
// `=0` restores the unconditional interleave that buries the flyover overlay.
bool gifPath3HazardEnabled()
{
    static const bool enabled = []
    {
        const char *v = std::getenv("RRV_GIF_PATH3_HAZARD");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return enabled;
}


namespace
{
    bool carDmaProvenanceDiagEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_CAR_DMA_PROVENANCE_DIAG");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // Metadata-only probe for the early boot GIF chains.  Keep this cached and
    // default-off: this path is hot, and the probe deliberately never prints
    // game-derived payload bytes.
    bool bootGifDmaDiagEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_BOOT_DMA_DIAG");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    bool shadowDmaDiagEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_SHADOW_DMA_DIAG");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    bool sprDmaDiagEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_SPR_DMA_DIAG");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    bool girlGifFlattenedDumpEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_GIRL_GIF_FLATTENED_DUMP");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // B-3: provenance of the native-only upload batch.  The 991,552 B "reduced"
    // PATH3 chain is 93.8% one PSMT8 texture load into dbp=6784 dbw=8 that PCSX2
    // never performs (handoff §0.0b/§0.0c).  This records, per source-chain
    // block, the EE RAM address it was fetched from, then reports only the
    // blocks that actually carry a BITBLTBUF selecting that destination — which
    // names the asset group the EE picked.  Default off; costs nothing when off.
    bool girlUploadProvenanceEnabled()
    {
        static const bool enabled = []
        {
            const char *value = rrvCachedEnv("RRV_GIRL_UPLOAD_PROV");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    bool girlGsVramDumpEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("RRV_GIRL_GS_VRAM_DUMP");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    uint64_t fnv1a64(const uint8_t *data, size_t size)
    {
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    const char *dmaTagName(uint32_t id)
    {
        static constexpr const char *names[] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
        return id < 8u ? names[id] : "INVALID";
    }

    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return addr >= PS2_GS_PRIV_REG_BASE && addr < PS2_GS_PRIV_REG_BASE + PS2_GS_PRIV_REG_SIZE;
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        case 0x1000:
            return &gs.csr;
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

void rrvNoteGifLatencyHold(uint32_t scenePhase);
void rrvNoteGifLatencyFlush(const char *reason);
void rrvReportGifLatencyStranded(size_t stillHeld);

PS2Memory::~PS2Memory()
{
    rrvReportGifLatencyStranded(m_heldVif1Transfers.size());
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    m_pendingDmacCompletionCauses.clear();
    m_dispatchingDmacCompletions = false;

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        rrv::guest_time::QuietBump();
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        rrvVuCodeChanged(0);
        rrvVuCodeChanged(1);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (virtualAddress >= 0x20000000u && virtualAddress < 0x40000000u)
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (virtualAddress >= 0x80000000 && virtualAddress < 0xC0000000)
    {
        return virtualAddress & 0x1FFFFFFF;
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (virtualAddress >= 0xC0000000)
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    uint64_t gate3Value = 0;
    if (m_gate3MmioRead && m_gate3MmioRead(address, 1u, gate3Value))
        return static_cast<uint8_t>(gate3Value);
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr < PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        return m_vu0Code[physAddr - PS2_VU0_CODE_BASE];
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr < PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        return m_vu0Data[physAddr - PS2_VU0_DATA_BASE];
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    uint64_t gate3Value = 0;
    if (m_gate3MmioRead && m_gate3MmioRead(address, 2u, gate3Value))
        return static_cast<uint16_t>(gate3Value);
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(uint16_t) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        return loadScalar<uint16_t>(m_vu0Code, physAddr - PS2_VU0_CODE_BASE,
                                    PS2_VU0_CODE_SIZE, "read16 vu0 code", address);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(uint16_t) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        return loadScalar<uint16_t>(m_vu0Data, physAddr - PS2_VU0_DATA_BASE,
                                    PS2_VU0_DATA_SIZE, "read16 vu0 data", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    uint64_t gate3Value = 0;
    if (m_gate3MmioRead && m_gate3MmioRead(address, 4u, gate3Value))
        return static_cast<uint32_t>(gate3Value);
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint32_t off = address & 7;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(uint32_t) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        return loadScalar<uint32_t>(m_vu0Code, physAddr - PS2_VU0_CODE_BASE,
                                    PS2_VU0_CODE_SIZE, "read32 vu0 code", address);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(uint32_t) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        return loadScalar<uint32_t>(m_vu0Data, physAddr - PS2_VU0_DATA_BASE,
                                    PS2_VU0_DATA_SIZE, "read32 vu0 data", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    uint64_t gate3Value = 0;
    if (m_gate3MmioRead && m_gate3MmioRead(address, 8u, gate3Value))
        return static_cast<uint64_t>(gate3Value);
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (address >= PS2_IO_BASE && address < (PS2_IO_BASE + PS2_IO_SIZE))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    uint64_t gate3Lo = 0;
    if (m_gate3MmioRead && m_gate3MmioRead(address, 8u, gate3Lo))
    {
        uint64_t gate3Hi = 0;
        if (!m_gate3MmioRead(address + 8u, 8u, gate3Hi))
            throw std::logic_error("Gate-3 MMIO read128 second lane unhandled");
        alignas(16) uint64_t lanes[2]{gate3Lo, gate3Hi};
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes));
    }
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(__m128i) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_vu0Code[physAddr - PS2_VU0_CODE_BASE]));
    }
    if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(__m128i) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_vu0Data[physAddr - PS2_VU0_DATA_BASE]));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    if (m_gate3MmioWrite && m_gate3MmioWrite(address, 1u, value)) return;
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr < PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        m_vu0Code[physAddr - PS2_VU0_CODE_BASE] = value;
        rrvVuCodeChanged(0);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr < PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        m_vu0Data[physAddr - PS2_VU0_DATA_BASE] = value;
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (m_gate3MmioWrite && m_gate3MmioWrite(address, 2u, value)) return;
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(uint16_t) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        storeScalar<uint16_t>(m_vu0Code, physAddr - PS2_VU0_CODE_BASE,
                              PS2_VU0_CODE_SIZE, value, "write16 vu0 code", address);
        rrvVuCodeChanged(0);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(uint16_t) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        storeScalar<uint16_t>(m_vu0Data, physAddr - PS2_VU0_DATA_BASE,
                              PS2_VU0_DATA_SIZE, value, "write16 vu0 data", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}


// ── EE stores into VU0/VU1 code and data memory ─────────────────────────────
// KNOWN_ISSUES #37. The EE can address VU memory directly:
//   VU0 micro 0x11000000  VU0 data 0x11004000
//   VU1 micro 0x11008000  VU1 data 0x1100C000
// write32/write64/write128 handled only the VU0 pair (and write64 handled
// neither), so every EE store into VU1 memory was silently DROPPED. That is a
// real hole regardless of this bug: it is a channel into VU1 that no VIF dump
// can see, which is exactly why the phase-3 investigation could measure
// "identical VIF input" and "identical VU1 memory at entry" and still get
// different output.
//
// RRV_VU_MEM_DIAG=1 reports what the guest actually does here (default off).
// RRV_EE_VU_WRITES=0 restores the old drop-everything behaviour as a rollback.
namespace
{
    bool eeVuWritesEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("RRV_EE_VU_WRITES");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return on;
    }

    void rrvNoteVuMemWrite(uint32_t physAddr, uint32_t width, uint32_t phase)
    {
        rrv::gate4::ownerVuWrite(physAddr, width / 8u);
        static const bool on = [] {
            const char *v = std::getenv("RRV_VU_MEM_DIAG");
            return v && v[0] && v[0] != '0';
        }();
        if (!on)
            return;
        const char *what = (physAddr >= 0x1100C000u) ? "VU1data"
                           : (physAddr >= 0x11008000u) ? "VU1code"
                           : (physAddr >= 0x11004000u) ? "VU0data"
                                                       : "VU0code";
        static std::atomic<uint64_t> s_total{0};
        const uint64_t n = s_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 24u || (n % 100000u) == 0u)
        {
            std::fprintf(stderr, "[vu-mem] n=%llu %s addr=%08x w=%u phase=%u\n",
                         (unsigned long long)n, what, physAddr, width, phase);
        }
    }
} // namespace

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (m_gate3MmioWrite && m_gate3MmioWrite(address, 4u, value)) return;
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (reg)
        {
            uint32_t off = address & 7;
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u && off == 0u)
            {
                // CSR low dword: bits 0..1 are write-one-to-clear status bits.
                constexpr uint32_t kW1cMask = 0x3u;
                uint64_t current = *reg;
                uint32_t oldLow = static_cast<uint32_t>(current & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                current = (current & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                current &= ~static_cast<uint64_t>(value & kW1cMask);
                *reg = current;
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFULL << (off * 8);
                uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
                *reg = newVal;
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(uint32_t) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        storeScalar<uint32_t>(m_vu0Code, physAddr - PS2_VU0_CODE_BASE,
                              PS2_VU0_CODE_SIZE, value, "write32 vu0 code", address);
        rrvVuCodeChanged(0);
    }
    else if (physAddr >= PS2_VU1_CODE_BASE && physAddr + sizeof(uint32_t) <= PS2_VU1_CODE_BASE + PS2_VU1_CODE_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 32u, read32(0x334E94u));
        if (eeVuWritesEnabled())
        {
            storeScalar<uint32_t>(m_vu1Code, physAddr - PS2_VU1_CODE_BASE,
                                  PS2_VU1_CODE_SIZE, value, "write32 vu1 code", address);
            rrvVuCodeChanged(1);
        }
    }
    else if (physAddr >= PS2_VU1_DATA_BASE && physAddr + sizeof(uint32_t) <= PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 32u, read32(0x334E94u));
        if (eeVuWritesEnabled())
            storeScalar<uint32_t>(m_vu1Data, physAddr - PS2_VU1_DATA_BASE,
                                  PS2_VU1_DATA_SIZE, value, "write32 vu1 data", address);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(uint32_t) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 32u, read32(0x334E94u));
        storeScalar<uint32_t>(m_vu0Data, physAddr - PS2_VU0_DATA_BASE,
                              PS2_VU0_DATA_SIZE, value, "write32 vu0 data", address);
    }
    else if (physAddr >= PS2_IO_BASE && physAddr < PS2_IO_BASE + PS2_IO_SIZE)
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (m_gate3MmioWrite && m_gate3MmioWrite(address, 8u, value)) return;
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (reg)
        {
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u)
            {
                // CSR: bits 0..1 are write-one-to-clear status bits.
                constexpr uint64_t kW1cMask = 0x3ull;
                uint64_t next = (*reg & kW1cMask) | (value & ~kW1cMask);
                next &= ~(value & kW1cMask);
                *reg = next;
            }
            else
            {
                *reg = value;
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (m_gate3MmioWrite)
    {
        alignas(16) uint64_t lanes[2];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(lanes), value);
        if (m_gate3MmioWrite(address, 8u, lanes[0]))
        {
            if (!m_gate3MmioWrite(address + 8u, 8u, lanes[1]))
                throw std::logic_error("Gate-3 MMIO write128 second lane unhandled");
            return;
        }
    }
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else if (physAddr >= PS2_VU0_CODE_BASE && physAddr + sizeof(__m128i) <= PS2_VU0_CODE_BASE + PS2_VU0_CODE_SIZE)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_vu0Code[physAddr - PS2_VU0_CODE_BASE]), value);
        rrvVuCodeChanged(0);
    }
    else if (physAddr >= PS2_VU1_CODE_BASE && physAddr + sizeof(__m128i) <= PS2_VU1_CODE_BASE + PS2_VU1_CODE_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 128u, read32(0x334E94u));
        if (eeVuWritesEnabled() && m_vu1Code)
        {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_vu1Code[physAddr - PS2_VU1_CODE_BASE]), value);
            rrvVuCodeChanged(1);
        }
    }
    else if (physAddr >= PS2_VU1_DATA_BASE && physAddr + sizeof(__m128i) <= PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 128u, read32(0x334E94u));
        if (eeVuWritesEnabled() && m_vu1Data)
            _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_vu1Data[physAddr - PS2_VU1_DATA_BASE]), value);
    }
    else if (physAddr >= PS2_VU0_DATA_BASE && physAddr + sizeof(__m128i) <= PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
    {
        ownerFenceForVuWriteV1(physAddr);
        rrvNoteVuMemWrite(physAddr, 128u, read32(0x334E94u));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_vu0Data[physAddr - PS2_VU0_DATA_BASE]), value);
    }
    else
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        m_ioRegisters[address] = value;
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint32_t off = address & 7u;
            const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
            if (regOff == 0x1000u && off == 0u)
            {
                constexpr uint32_t kW1cMask = 0x3u;
                uint64_t current = *reg;
                uint32_t oldLow = static_cast<uint32_t>(current & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                current = (current & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                current &= ~static_cast<uint64_t>(value & kW1cMask);
                *reg = current;
            }
            else
            {
                const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        ownerFenceV1(); // VIF1 registers are owner state
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            // Scratchpad (SPR) DMA channels: ch8 fromSPR (0x1000D000) and ch9 toSPR
            // (0x1000D400). These copy between main RAM (MADR) and the 16KB scratchpad
            // at the channel's SADR (0x..80) offset. RRV's display-list builder
            // sub_00222EB8 kicks a toSPR burst to stage the GIF command list into
            // scratchpad 0x70000100 before walking it; without this the scratchpad
            // stays empty and the walker never finds its terminator. The transfer is
            // synchronous in our HLE, so run the copy now and clear CHCR.STR (the game
            // busy-waits on it) plus drain QWC.
            if (channelBase == 0x1000D000u || channelBase == 0x1000D400u)
            {
                const bool toScratchpad = (channelBase == 0x1000D400u);
                const uint32_t sadr = m_ioRegisters[channelBase + 0x80];
                const uint32_t mode = (value >> 2) & 0x3u;
                const uint64_t bytes = static_cast<uint64_t>(qwc) * 16ull;
                uint32_t finalSadr = sadr;
                uint32_t finalMadr = madr;
                uint32_t finalChcr = value;
                if (m_scratchpad && !toScratchpad && mode == 1u)
                {
                    // fromSPR chain mode uses an inline chain in scratchpad. RRV's
                    // sub_00222EB8 emits CNT tags whose ADDR field is the RDRAM
                    // destination and whose payload immediately follows the tag,
                    // terminated by END. This is the path that publishes the white
                    // Namco animation's EE/VU0-built GIF primitives.
                    uint32_t tagOff = sadr & (PS2_SCRATCHPAD_SIZE - 1u);
                    const bool sprDiag = sprDmaDiagEnabled();
                    if (sprDiag)
                    {
                        std::fprintf(stderr,
                                     "[spr-from-chain] START chcr=%08x madr=%08x qwc=%08x sadr=%04x\n",
                                     value, madr, qwc, tagOff);
                    }
                    // Ordering/termination follows PCSX2 2.8.2 pcsx2/SPR.cpp dmaSPR0 and
                    // _dmaSPR0 (PCSX2 Dev Team, 2002-2026, GPL-3.0+).
                    // Pending QWC describes payload at SADR, not a new tag. Only an
                    // incoming END tag finishes this resumed burst; dmaSPR0 does
                    // not use incoming IRQ/TIE to terminate pending fromSPR data.
                    bool chainDone = qwc > 0u && ((value >> 28) & 0x7u) == 7u;
                    if (qwc > 0u)
                    {
                        uint32_t ramOff = translateAddress(madr) & PS2_RAM_MASK;
                        rrv::ringCopy(m_rdram, ramOff, PS2_RAM_MASK,
                                    m_scratchpad, tagOff, PS2_SCRATCHPAD_SIZE - 1u, bytes);
                        finalSadr = tagOff;
                        finalMadr = static_cast<uint32_t>(madr + bytes);
                    }
                    for (uint32_t tagCount = 0u; !chainDone && tagCount < 1024u; ++tagCount)
                    {
                        uint64_t tagLo = 0u;
                        for (uint32_t i = 0u; i < sizeof(tagLo); ++i)
                        {
                            reinterpret_cast<uint8_t *>(&tagLo)[i] =
                                m_scratchpad[(tagOff + i) & (PS2_SCRATCHPAD_SIZE - 1u)];
                        }
                        // DMACh::unsafeTransfer preserves the low CHCR controls and
                        // replaces TAG with the fetched tag's upper 16 bits.
                        finalChcr = (finalChcr & 0xFFFFu) |
                                    (static_cast<uint32_t>(tagLo) & 0xFFFF0000u);
                        const uint32_t tagQwc = static_cast<uint32_t>(tagLo & 0xFFFFu);
                        const uint32_t tagId = static_cast<uint32_t>((tagLo >> 28) & 0x7u);
                        const uint32_t dstAddr = static_cast<uint32_t>(tagLo >> 32) & 0x7FFFFFFFu;
                        const uint32_t payloadOff =
                            (tagOff + 16u) & (PS2_SCRATCHPAD_SIZE - 1u);
                        const uint32_t payloadBytes = tagQwc * 16u;
                        if (sprDiag)
                        {
                            uint64_t payloadHash = 14695981039346656037ull;
                            uint32_t nonzero = 0u;
                            for (uint32_t i = 0u; i < payloadBytes; ++i)
                            {
                                const uint8_t byte = m_scratchpad[
                                    (payloadOff + i) & (PS2_SCRATCHPAD_SIZE - 1u)];
                                payloadHash ^= byte;
                                payloadHash *= 1099511628211ull;
                                nonzero += byte != 0u ? 1u : 0u;
                            }
                            std::fprintf(stderr,
                                         "[spr-from-chain] TAG n=%u off=%04x id=%u qwc=%u dst=%08x "
                                         "payload=%04x bytes=%u nonzero=%u hash=%016llx\n",
                                         tagCount, tagOff, tagId, tagQwc, dstAddr, payloadOff,
                                         payloadBytes, nonzero,
                                         static_cast<unsigned long long>(payloadHash));
                        }
                        uint32_t ramOff = translateAddress(dstAddr) & PS2_RAM_MASK;
                        uint32_t sprOff = payloadOff;
                        rrv::ringCopy(m_rdram, ramOff, PS2_RAM_MASK,
                                    m_scratchpad, sprOff, PS2_SCRATCHPAD_SIZE - 1u, payloadBytes);
                        tagOff = sprOff;
                        finalSadr = tagOff;
                        finalMadr = dstAddr + payloadBytes;
                        chainDone = tagId == 7u || // END
                                    ((finalChcr & 0x80u) != 0u && (tagLo & 0x80000000ull) != 0u);
                    }
                }
                else if (m_scratchpad && qwc > 0u)
                {
                    uint32_t ramOff = translateAddress(madr) & PS2_RAM_MASK;
                    uint32_t sprOff = sadr & (PS2_SCRATCHPAD_SIZE - 1u);
                    if (toScratchpad)
                        rrv::ringCopy(m_scratchpad, sprOff, PS2_SCRATCHPAD_SIZE - 1u,
                                    m_rdram, ramOff, PS2_RAM_MASK, bytes);
                    else
                        rrv::ringCopy(m_rdram, ramOff, PS2_RAM_MASK,
                                    m_scratchpad, sprOff, PS2_SCRATCHPAD_SIZE - 1u, bytes);
                    finalMadr = static_cast<uint32_t>(madr + bytes);
                    finalSadr = (sadr + static_cast<uint32_t>(bytes)) &
                                (PS2_SCRATCHPAD_SIZE - 1u);
                }
                // Completion: STR clear, QWC drained, MADR/SADR advanced past the burst.
                m_ioRegisters[channelBase + 0x00] = finalChcr & ~0x100u;
                m_ioRegisters[channelBase + 0x10] = finalMadr;
                m_ioRegisters[channelBase + 0x20] = 0u;
                m_ioRegisters[channelBase + 0x80] = finalSadr;
                return true;
            }

            if ((channelBase == 0x1000A000 || channelBase == 0x10009000 ||
                 channelBase == 0x10008000) && m_gsVRAM)
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000)
                    {
                        rrvNoteDmaOrderEvent("kick-gif", qwCount * 16ull, 0ull);
                        m_pendingGifTransfers.push_back(pt);
                    }
                    else if (channelBase == 0x10008000)
                    {
                        rrvNoteDmaOrderEvent("kick-vif0", qwCount * 16ull, 0ull);
                        m_pendingVif0Transfers.push_back(pt);
                    }
                    else if (channelBase == 0x10009000)
                    {
                        rrvNoteDmaOrderEvent("kick-vif1", qwCount * 16ull, 0ull);
                        m_pendingVif1Transfers.push_back(pt);
                    }
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    const int kMaxChainTags = 4096;
                    std::vector<uint8_t> chainBuf;

                    const bool bootGifDmaDiag =
                        channelBase == 0x1000A000u && bootGifDmaDiagEnabled();
                    uint64_t bootGifDmaSequence = 0u;
                    const uint32_t bootGifInitialTadr = tagAddr;
                    const char *bootGifTermination = nullptr;
                    uint32_t bootGifTerminalTag = tagAddr;
                    if (bootGifDmaDiag)
                    {
                        static std::atomic<uint64_t> nextBootGifDmaSequence{1u};
                        bootGifDmaSequence =
                            nextBootGifDmaSequence.fetch_add(1u, std::memory_order_relaxed);
                        std::fprintf(stderr,
                                     "[boot-gif-dma:%llu] START CHCR=%08x TIE=%u TTE=%u "
                                     "MADR=%08x QWC=%08x TADR=%08x path3-masked=%u queued=%zu\n",
                                     static_cast<unsigned long long>(bootGifDmaSequence), chcr,
                                     tieEnabled ? 1u : 0u, (chcr >> 6) & 1u, madr, qwc,
                                     bootGifInitialTadr, m_path3Masked ? 1u : 0u,
                                     m_path3MaskedFifo.size());
                    }

                    // Match the immutable on-chain shape emitted by the car packet builder:
                    // QWC=0 CALL to the phase-7 car target 0x41f560, an immediately following
                    // QWC=0 NEXT return tag, and a called chain beginning with CNT/QWC=0x5f4.
                    const bool carDmaNearMissDiag = carDmaProvenanceDiagEnabled();
                    uint64_t carDiagChain = 0u;
                    const uint32_t carDiagInitialTadr = tagAddr;
                    if (carDmaNearMissDiag)
                    {
                        static std::atomic<uint64_t> nextCarDiagChain{1u};
                        carDiagChain = nextCarDiagChain.fetch_add(1u, std::memory_order_relaxed);
                    }

                    bool carDiagMatched = false;
                    bool inCarCall = false;
                    bool logPostCarReturn = false;
                    uint32_t carCallDepth = 0u;
                    uint32_t carReturnAddr = 0u;
                    size_t carAppendBegin = 0u;
                    size_t carAppendEnd = 0u;
                    uint32_t carMatchedCallTag = 0u;
                    uint32_t carMatchedCallTarget = 0u;

                    auto terminateCarDiag = [&](bool returned, const char *reason, uint32_t terminalTagAddr)
                    {
                        if (!inCarCall)
                            return;
                        carAppendEnd = chainBuf.size();
                        std::fprintf(stderr,
                                     "[car-dma:%llu] TERMINAL returned=%u reason=%s tag=%08x "
                                     "expected-return=%08x chain contribution=[%zu,%zu) bytes=%zu\n",
                                     static_cast<unsigned long long>(carDiagChain), returned ? 1u : 0u,
                                     reason, terminalTagAddr, carReturnAddr, carAppendBegin,
                                     carAppendEnd, carAppendEnd - carAppendBegin);
                        inCarCall = false;
                        carCallDepth = 0u;
                    };

                    // (offset in chainBuf, guest source address, bytes) per block.
                    struct ProvBlock { size_t off; uint32_t src; uint32_t bytes; uint32_t tag; uint32_t id; };
                    std::vector<ProvBlock> provBlocks;
                    uint32_t provCurrentTag = 0u;
                    uint32_t provCurrentId = 0u;
                    const bool matrixTrace = [] {
                        const char *v = std::getenv("RRV_VU_MATRIX_TRACE");
                        return v && v[0] && v[0] != '0';
                    }();

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        if (girlUploadProvenanceEnabled() || matrixTrace)
                            provBlocks.push_back({chainBuf.size(), srcAddr, qwCount * 16u,
                                                  provCurrentTag, provCurrentId});
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0; 
                        src = translateAddress(srcAddr); 
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }
                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendDmaTagUpper = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr); 
                        tagPhys = translateAddress(localTagAddr);
                        
                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // With CHCR.TTE, VIF0/VIF1 receive the DMAtag's upper
                        // 64 bits before that tag's payload, for every tag ID.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    auto tryLoadDmaTag = [&](uint32_t guestAddr, uint64_t &tagValue)
                    {
                        try
                        {
                            const bool scratch = isScratchpad(guestAddr);
                            const uint32_t phys = translateAddress(guestAddr);
                            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
                            const uint32_t maxSize = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                            if (phys + 16u > maxSize)
                                return false;
                            tagValue = loadScalar<uint64_t>(base, phys, maxSize,
                                                            "car dma signature tag", guestAddr);
                            return true;
                        }
                        catch (...)
                        {
                            return false;
                        }
                    };

                    int tagsProcessed = 0;

                    // RRV_VIF0_CHAIN_DIAG=1 — one line per VIF0 chain kick
                    // (default off). The libdma path is HLE'd (ps2_stubs::
                    // sceDmaSend), so RRV_DMA_KICK_PC cannot see these kicks at
                    // all: it probes Store32 and the HLE writes the channel
                    // registers directly. This is the only way to see whether a
                    // scratchpad-rooted VIF0 chain actually walks.
                    static const bool s_vif0ChainDiag = [] {
                        const char *v = std::getenv("RRV_VIF0_CHAIN_DIAG");
                        return v && v[0] && v[0] != '0';
                    }();
                    const bool vif0Diag = s_vif0ChainDiag && channelBase == 0x10008000u;
                    const uint32_t vif0RootTadr = tagAddr;
                    uint32_t vif0FirstId = 0xFFFFFFFFu;
                    uint32_t vif0FirstQwc = 0u;
                    uint32_t vif0FirstAddr = 0u;
                    uint64_t vif0DataQw = 0u;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            if (bootGifDmaDiag)
                            {
                                bootGifTermination = "tag-translate";
                                bootGifTerminalTag = currentTagAddr;
                            }
                            terminateCarDiag(false, "tag-translate", currentTagAddr);
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                        {
                            if (bootGifDmaDiag)
                            {
                                bootGifTermination = "tag-oob";
                                bootGifTerminalTag = currentTagAddr;
                            }
                            terminateCarDiag(false, "tag-oob", currentTagAddr);
                            break;
                        }

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        ++tagsProcessed;

                        if (vif0Diag && vif0FirstId == 0xFFFFFFFFu)
                        {
                            vif0FirstId = id;
                            vif0FirstQwc = tagQwc;
                            vif0FirstAddr = addr;
                        }
                        if (vif0Diag)
                            vif0DataQw += tagQwc;

                        if (bootGifDmaDiag && currentTagAddr == 0x01573d20u)
                        {
                            static std::atomic<bool> dumpedBootLogoRegion{false};
                            if (!dumpedBootLogoRegion.exchange(true, std::memory_order_relaxed))
                            {
                                constexpr uint32_t kDumpStart = 0x01573d20u;
                                constexpr size_t kDumpSize = 0x12000u;
                                if (FILE *dump = std::fopen("/private/tmp/native_logo_region.bin", "wb"))
                                {
                                    std::fwrite(m_rdram + kDumpStart, 1, kDumpSize, dump);
                                    std::fclose(dump);
                                    std::fprintf(stderr,
                                                 "[boot-gif-dma] dumped native logo region "
                                                 "%08x+%zu\n",
                                                 kDumpStart, kDumpSize);
                                }
                                if (FILE *dump = std::fopen("/private/tmp/native_logo_rdram.bin", "wb"))
                                {
                                    std::fwrite(m_rdram, 1, PS2_RAM_SIZE, dump);
                                    std::fclose(dump);
                                }
                            }
                        }

                        const uint32_t aspBefore = asp;
                        bool matchingCarCall = false;
                        uint64_t callUpper = 0u;
                        uint64_t followingTag = 0u;
                        uint64_t targetTag = 0u;
                        bool callTagsReadable = false;
                        if (carDmaNearMissDiag && id == 5u && tagQwc == 0u)
                        {
                            std::memcpy(&callUpper, tp + 8u, sizeof(callUpper));
                            callTagsReadable = tryLoadDmaTag(currentTagAddr + 16u, followingTag) &&
                                               tryLoadDmaTag(addr, targetTag);
                            if (callTagsReadable)
                            {
                                const uint16_t followingQwc = static_cast<uint16_t>(followingTag & 0xffffu);
                                const uint32_t followingId = static_cast<uint32_t>((followingTag >> 28) & 0x7u);
                                const uint16_t targetQwc = static_cast<uint16_t>(targetTag & 0xffffu);
                                const uint32_t targetId = static_cast<uint32_t>((targetTag >> 28) & 0x7u);
                                matchingCarCall = !carDiagMatched &&
                                                  addr == 0x0041f560u &&
                                                  followingId == 2u && followingQwc == 0u &&
                                                  targetId == 1u && targetQwc == 0x05f4u;

                                static std::atomic<uint32_t> exactTargetNearMissCount{0u};
                                static std::atomic<uint32_t> vifShapeCandidateCount{0u};
                                const bool exactTarget = addr == 0x0041f560u;
                                const bool vifShapeCandidate = channelBase == 0x10009000u &&
                                                               targetId == 1u && targetQwc == 0x05f4u;
                                const uint32_t exactIndex = exactTarget
                                                                ? exactTargetNearMissCount.fetch_add(
                                                                      1u, std::memory_order_relaxed)
                                                                : 32u;
                                const uint32_t candidateIndex = vifShapeCandidate
                                                                    ? vifShapeCandidateCount.fetch_add(
                                                                          1u, std::memory_order_relaxed)
                                                                    : 16u;
                                if ((exactTarget && exactIndex < 32u) ||
                                    (vifShapeCandidate && candidateIndex < 16u))
                                {
                                    const uint32_t followingAddr =
                                        static_cast<uint32_t>((followingTag >> 32) & 0x7fffffffu);
                                    std::fprintf(stderr,
                                                 "[car-dma-near] channel=%08x CHCR=%08x TADR=%08x "
                                                 "current=%08x tag-space=%s CALL-target=%08x "
                                                 "following=%08x/%08x id=%s qwc=%04x addr=%08x "
                                                 "target-tag=%08x/%08x id=%s qwc=%04x target-space=%s "
                                                 "exact-index=%u shape-index=%u\n",
                                                 channelBase, chcr, carDiagInitialTadr, currentTagAddr,
                                                 tagInSPR ? "scratch" : "rdram", addr,
                                                 static_cast<uint32_t>(followingTag),
                                                 static_cast<uint32_t>(followingTag >> 32),
                                                 dmaTagName(followingId), followingQwc, followingAddr,
                                                 static_cast<uint32_t>(targetTag),
                                                 static_cast<uint32_t>(targetTag >> 32),
                                                 dmaTagName(targetId), targetQwc,
                                                 isScratchpad(addr) ? "scratch" : "rdram",
                                                 exactIndex, candidateIndex);
                                }
                            }
                        }
                        const bool wasInCarCall = inCarCall;
                        const bool logThisTag = matchingCarCall || wasInCarCall || logPostCarReturn;
                        const size_t appendBegin = chainBuf.size();

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;
                        bool carAspOverflow = false;
                        const char *chainEndReason = nullptr;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            chainEndReason = "refe";
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (matchingCarCall)
                                {
                                    carDiagMatched = true;
                                    inCarCall = true;
                                    carCallDepth = 1u;
                                    carReturnAddr = retAddr;
                                    carAppendBegin = appendBegin;
                                    carMatchedCallTag = currentTagAddr;
                                    carMatchedCallTarget = addr;
                                    std::fprintf(stderr,
                                                 "[car-dma:%llu] CHCR=%08x TADR=%08x TTE=%u\n",
                                                 static_cast<unsigned long long>(carDiagChain), chcr,
                                                 carDiagInitialTadr,
                                                 (chcr >> 6) & 1u);
                                    std::fprintf(stderr,
                                                 "[car-dma:%llu] MATCH CALL tag=%08x target=%08x return=%08x "
                                                 "qwc=%04x upper=%016llx following=%08x/%08x following-id=%s "
                                                 "following-qwc=%04x asp=%u\n",
                                                 static_cast<unsigned long long>(carDiagChain), currentTagAddr,
                                                 addr, retAddr, tagQwc,
                                                 static_cast<unsigned long long>(callUpper),
                                                 static_cast<uint32_t>(followingTag),
                                                 static_cast<uint32_t>(followingTag >> 32),
                                                 dmaTagName(static_cast<uint32_t>((followingTag >> 28) & 0x7u)),
                                                 static_cast<uint16_t>(followingTag & 0xffffu), aspBefore);
                                    if (asp >= 2u)
                                        carAspOverflow = true;
                                }
                                else if (wasInCarCall)
                                {
                                    if (asp < 2u)
                                        ++carCallDepth;
                                    else
                                        carAspOverflow = true;
                                }
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            if (girlUploadProvenanceEnabled())
                                provBlocks.push_back({chainBuf.size(), addr, 0u,
                                                      currentTagAddr, 5u});
                            tagAddr = addr;
                            break;
                        case 6:
                            if (girlUploadProvenanceEnabled())
                                provBlocks.push_back({chainBuf.size(), tagAddr, 0u,
                                                      currentTagAddr, 6u});
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                                chainEndReason = "ret-empty";
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            chainEndReason = "end";
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            chainEndReason = "invalid-tag";
                            break;
                        }

                        const bool transferVifTag =
                            (channelBase == 0x10008000u || channelBase == 0x10009000u) &&
                            (chcr & 0x40u) != 0u;
                        if (transferVifTag)
                            appendDmaTagUpper(currentTagAddr);

                        if (hasPayload)
                        {
                            provCurrentTag = currentTagAddr;
                            provCurrentId = id;
                            appendData(dataAddr, tagQwc);
                        }

                        if (logThisTag)
                        {
                            const bool compactVif1LocalPayload =
                                hasPayload && (id == 1u || id == 2u || id == 5u || id == 6u || id == 7u);
                            uint32_t payloadPhys = 0u;
                            uint32_t tagUpperPhys = 0u;
                            const char *sourceRegion = "none";
                            if (hasPayload)
                            {
                                payloadPhys = translateAddress(dataAddr);
                                tagUpperPhys = compactVif1LocalPayload
                                                   ? translateAddress(currentTagAddr) + 8u
                                                   : 0u;
                                sourceRegion = isScratchpad(dataAddr) ? "scratch" : "rdram";
                            }
                            std::fprintf(stderr,
                                         "[car-dma:%llu] %s tag=%08x phys=%08x qwc=%04x addr=%08x "
                                         "payload=%s:%08x+%u tag-upper8=%08x "
                                         "append=[%zu,%zu) next=%08x asp=%u->%u%s\n",
                                         static_cast<unsigned long long>(carDiagChain), dmaTagName(id),
                                         currentTagAddr, physTag, tagQwc, addr, sourceRegion, payloadPhys,
                                         static_cast<unsigned int>(tagQwc) * 16u, tagUpperPhys, appendBegin,
                                         chainBuf.size(), tagAddr, aspBefore, asp,
                                         logPostCarReturn ? " post-car-return" : "");
                        }

                        if (bootGifDmaDiag)
                        {
                            uint32_t payloadPhys = 0u;
                            const char *payloadSpace = "none";
                            bool payloadResolved = false;
                            if (hasPayload)
                            {
                                try
                                {
                                    payloadPhys = translateAddress(dataAddr);
                                    payloadSpace = isScratchpad(dataAddr) ? "scratch" : "rdram";
                                    payloadResolved = true;
                                }
                                catch (...)
                                {
                                    payloadSpace = "unresolved";
                                }
                            }
                            if (hasPayload)
                            {
                                std::fprintf(stderr,
                                             "[boot-gif-dma:%llu] TAG=%08x space=%s id=%s qwc=%04x "
                                             "irq=%u addr=%08x payload=%s:guest=%08x%s%08x+%u "
                                             "append=[%zu,%zu) next=%08x asp=%u->%u\n",
                                             static_cast<unsigned long long>(bootGifDmaSequence),
                                             currentTagAddr, tagInSPR ? "scratch" : "rdram",
                                             dmaTagName(id), tagQwc, irq ? 1u : 0u, addr,
                                             payloadSpace, dataAddr,
                                             payloadResolved ? ":phys=" : ":phys-unresolved=",
                                             payloadResolved ? payloadPhys : 0u,
                                             static_cast<unsigned int>(tagQwc) * 16u, appendBegin,
                                             chainBuf.size(), tagAddr, aspBefore, asp);
                            }
                            else
                            {
                                std::fprintf(stderr,
                                             "[boot-gif-dma:%llu] TAG=%08x space=%s id=%s qwc=%04x "
                                             "irq=%u addr=%08x payload=none append=[%zu,%zu) "
                                             "next=%08x asp=%u->%u\n",
                                             static_cast<unsigned long long>(bootGifDmaSequence),
                                             currentTagAddr, tagInSPR ? "scratch" : "rdram",
                                             dmaTagName(id), tagQwc, irq ? 1u : 0u, addr,
                                             appendBegin, chainBuf.size(), tagAddr, aspBefore, asp);
                            }
                        }

                        if (carAspOverflow)
                        {
                            terminateCarDiag(false, "asp-overflow", currentTagAddr);
                        }
                        else if (wasInCarCall && id == 6u)
                        {
                            if (carCallDepth > 1u)
                            {
                                --carCallDepth;
                            }
                            else
                            {
                                const bool returnedToExpected =
                                    (tagAddr & 0x0fffffffu) == (carReturnAddr & 0x0fffffffu);
                                if (returnedToExpected)
                                {
                                    terminateCarDiag(true, "ret", currentTagAddr);
                                    logPostCarReturn = true;
                                }
                                else
                                {
                                    terminateCarDiag(false, "ret-address-mismatch", currentTagAddr);
                                }
                            }
                        }
                        else if (logPostCarReturn && !wasInCarCall)
                        {
                            std::fprintf(stderr,
                                         "[car-dma:%llu] POST-RETURN %s qwc=%04x actual-following-next=%u\n",
                                         static_cast<unsigned long long>(carDiagChain), dmaTagName(id),
                                         tagQwc, id == 2u ? 1u : 0u);
                            logPostCarReturn = false;
                        }
                        if (irq && tieEnabled)
                        {
                            terminateCarDiag(false, "irq-tie", currentTagAddr);
                            endChain = true;
                            chainEndReason = "irq-tie";
                        }
                        if (inCarCall && endChain)
                        {
                            const char *reason = id == 0u ? "refe" : (id == 7u ? "end" : "invalid-tag");
                            terminateCarDiag(false, reason, currentTagAddr);
                        }
                        if (endChain)
                        {
                            if (bootGifDmaDiag)
                            {
                                bootGifTermination = chainEndReason ? chainEndReason : "end-chain";
                                bootGifTerminalTag = currentTagAddr;
                            }
                            break;
                        }
                    }

                    if (vif0Diag)
                    {
                        std::fprintf(stderr,
                                     "[vif0-chain] tadr=%08x spr=%u tags=%d firstId=%u "
                                     "firstQwc=%u firstAddr=%08x dataQW=%llu\n",
                                     vif0RootTadr, isScratchpad(vif0RootTadr) ? 1u : 0u,
                                     tagsProcessed,
                                     vif0FirstId == 0xFFFFFFFFu ? 99u : vif0FirstId,
                                     vif0FirstQwc, vif0FirstAddr,
                                     (unsigned long long)vif0DataQw);
                    }

                    if (inCarCall && tagsProcessed >= kMaxChainTags)
                        terminateCarDiag(false, "tag-cap", tagAddr);

                    if (bootGifDmaDiag)
                    {
                        if (!bootGifTermination)
                        {
                            bootGifTermination = tagsProcessed >= kMaxChainTags ? "tag-cap" : "loop-exit";
                            bootGifTerminalTag = tagAddr;
                        }
                        std::fprintf(stderr,
                                     "[boot-gif-dma:%llu] TERMINAL reason=%s tag=%08x tags=%d "
                                     "bytes=%zu fnv1a64=%016llx\n",
                                     static_cast<unsigned long long>(bootGifDmaSequence),
                                     bootGifTermination, bootGifTerminalTag, tagsProcessed,
                                     chainBuf.size(), static_cast<unsigned long long>(
                                                          fnv1a64(chainBuf.data(), chainBuf.size())));
                    }

                    if (girlGifFlattenedDumpEnabled() && channelBase == 0x1000A000u &&
                        (bootGifInitialTadr == 0x003472a0u || bootGifInitialTadr == 0x00347180u) &&
                        tagsProcessed >= 100 && chainBuf.size() >= 1000000u)
                    {
                        static std::atomic<bool> dumpedGirlVifStream{false};
                        if (!dumpedGirlVifStream.exchange(true, std::memory_order_relaxed))
                        {
                            const char *path = "/private/tmp/girl_gif_flattened.bin";
                            bool wrote = false;
                            if (FILE *dump = std::fopen(path, "wb"))
                            {
                                wrote = std::fwrite(chainBuf.data(), 1, chainBuf.size(), dump) ==
                                        chainBuf.size();
                                std::fclose(dump);
                            }
                            std::fprintf(stderr,
                                         "[girl-gif-flat] root=%08x tags=%d bytes=%zu "
                                         "fnv1a64=%016llx wrote=%u path=%s\n",
                                         bootGifInitialTadr, tagsProcessed, chainBuf.size(),
                                         static_cast<unsigned long long>(
                                             fnv1a64(chainBuf.data(), chainBuf.size())),
                                         wrote ? 1u : 0u, path);
                        }
                    }

                    if (girlUploadProvenanceEnabled() && !provBlocks.empty() &&
                        chainBuf.size() >= 100000u)
                    {
                        // Locate A+D writes of BITBLTBUF (reg 0x50) selecting the
                        // spurious destination, then attribute each to the source
                        // block it arrived in.
                        static std::atomic<uint32_t> reported{0};
                        if (reported.load(std::memory_order_relaxed) < 6u)
                        {
                            std::vector<size_t> hits;
                            for (size_t off = 0; off + 16u <= chainBuf.size(); off += 16u)
                            {
                                uint64_t lo, hi;
                                std::memcpy(&lo, chainBuf.data() + off, 8);
                                std::memcpy(&hi, chainBuf.data() + off + 8, 8);
                                if ((hi & 0xFFu) != 0x50u)
                                    continue;
                                const uint32_t dbp = static_cast<uint32_t>((lo >> 32) & 0x3FFFu);
                                const uint32_t dbw = static_cast<uint32_t>((lo >> 48) & 0x3Fu);
                                if (dbp == 6784u && dbw == 8u)
                                    hits.push_back(off);
                            }
                            // Level 3 also reports chains with no spurious
                            // BITBLTBUF, so the 503,408 B lattice chain (whose
                            // defect is a GIFtag PRIM, not an upload) can be
                            // traced to its source blocks the same way.
                            const char *provLvl = rrvCachedEnv("RRV_GIRL_UPLOAD_PROV");
                            // Level 3 targets the lattice chain specifically:
                            // RRV_GIRL_UPLOAD_PROV=3:<bytes> reports only chains
                            // of exactly that flattened size, so the bounded
                            // report budget is not spent on earlier chains.
                            size_t wantBytes = 0;
                            if (provLvl && provLvl[0] == '3' && provLvl[1] == ':')
                                wantBytes = std::strtoull(provLvl + 2, nullptr, 10);
                            // When a size filter is given it is EXCLUSIVE: the
                            // bounded report budget must not be spent on other
                            // chains before the wanted one appears (that is what
                            // produced the false "no chain flattens to 503,408 B").
                            const bool sizeMatch =
                                wantBytes != 0 && chainBuf.size() == wantBytes;
                            if (wantBytes != 0 ? sizeMatch : !hits.empty())
                            {
                                reported.fetch_add(1, std::memory_order_relaxed);
                                std::fprintf(stderr,
                                             "[girl-prov] root=%08x tags=%d bytes=%zu blocks=%zu "
                                             "spurious-bitbltbuf=%zu\n",
                                             bootGifInitialTadr, tagsProcessed, chainBuf.size(),
                                             provBlocks.size(), hits.size());
                                size_t bi = 0;
                                int printed = 0;
                                for (size_t h : hits)
                                {
                                    while (bi + 1 < provBlocks.size() &&
                                           provBlocks[bi + 1].off <= h)
                                        ++bi;
                                    if (printed++ < 24)
                                        std::fprintf(stderr,
                                                     "[girl-prov]   hit@%08zx  block[%zu] "
                                                     "src=%08x bytes=%u off=%08zx tag=%08x\n",
                                                     h, bi, provBlocks[bi].src,
                                                     provBlocks[bi].bytes, provBlocks[bi].off,
                                                     provBlocks[bi].tag);
                                }
                                // Source-address span of the whole chain, so the
                                // asset group's RAM residence is visible at a glance.
                                uint32_t lo32 = 0xFFFFFFFFu, hi32 = 0u;
                                for (const ProvBlock &b : provBlocks)
                                {
                                    lo32 = std::min(lo32, b.src);
                                    hi32 = std::max(hi32, b.src + b.bytes);
                                }
                                std::fprintf(stderr,
                                             "[girl-prov]   chain source span %08x..%08x\n",
                                             lo32, hi32);
                                // RRV_GIRL_UPLOAD_PROV=2 lists every block, so the
                                // reduced and full chains can be diffed block by
                                // block to find where they stop agreeing.
                                if (const char *lvl = provLvl;
                                    lvl && (lvl[0] == '2' || lvl[0] == '3'))
                                {
                                    for (size_t i = 0; i < provBlocks.size(); ++i)
                                        std::fprintf(stderr,
                                                     "[girl-prov]   blk[%02zu] tag=%08x "
                                                     "src=%08x bytes=%u off=%08zx id=%u(%s)\n",
                                                     i, provBlocks[i].tag, provBlocks[i].src,
                                                     provBlocks[i].bytes, provBlocks[i].off,
                                                     provBlocks[i].id,
                                                     dmaTagName(provBlocks[i].id));
                                }
                            }
                        }
                    }

                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    // RRV_SHADOW_DMA_DIAG: the car drop-shadow strips are appended to
                    // the display list as DMAtag REF blocks (guest 0x21F9C0) and carry
                    // the GIFtag 0x11AA4000_00008000|nloop. They are present in RDRAM
                    // but never reach the GS, so report how many of them each walked
                    // chain actually picked up, per channel.
                    if (!chainBuf.empty() && shadowDmaDiagEnabled())
                    {
                        uint32_t shadowTags = 0u;
                        for (size_t off = 0; off + 8u <= chainBuf.size(); off += 4u)
                        {
                            uint32_t w;
                            std::memcpy(&w, chainBuf.data() + off + 4u, 4u);
                            if (w == 0x11AA4000u)
                                ++shadowTags;
                        }
                        std::fprintf(stderr,
                                     "[shadow-chain] chan=%08x tags=%d bytes=%zu shadowGifTags=%u\n",
                                     channelBase, tagsProcessed, chainBuf.size(), shadowTags);
                    }
                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (matrixTrace && channelBase == 0x10009000)
                        {
                            pt.traceChainId = ++m_vif1TraceNextChainId;
                            pt.sourceBlocks.reserve(provBlocks.size());
                            for (const ProvBlock &b : provBlocks)
                                pt.sourceBlocks.push_back({b.off, b.src, b.bytes});
                        }
                        if (carDiagMatched && carAppendEnd >= carAppendBegin)
                        {
                            pt.carDmaDiag = true;
                            pt.carDmaDiagChain = carDiagChain;
                            pt.carDmaDiagBegin = static_cast<uint32_t>(carAppendBegin);
                            pt.carDmaDiagEnd = static_cast<uint32_t>(carAppendEnd);
                            pt.carDmaDiagCallTag = carMatchedCallTag;
                            pt.carDmaDiagCallTarget = carMatchedCallTarget;
                        }
                        if (channelBase == 0x1000A000)
                        {
                            rrvNoteDmaOrderEvent("kick-gif", pt.chainData.size(),
                                                 m_path3Masked ? 1ull : 0ull);
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000)
                        {
                            rrvNoteDmaOrderEvent("kick-vif0", pt.chainData.size(), 0ull);
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000)
                        {
                            rrvNoteDmaOrderEvent("kick-vif1", pt.chainData.size(), 0ull);
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                    }
                    else
                    {
                        // A chain containing only zero-QWC tags still starts and
                        // completes the DMAC channel. Keep a payload-free marker
                        // in the channel queue so processPendingTransfers raises
                        // D_STAT and dispatches the completion handler. Dropping
                        // this completion loses successor chains (RRV phase 6's
                        // tag-only state 3 must advance to its state-4 GIF list).
                        PendingTransfer completionMarker;
                        if (channelBase == 0x1000A000u)
                            m_pendingGifTransfers.push_back(std::move(completionMarker));
                        else if (channelBase == 0x10008000u)
                            m_pendingVif0Transfers.push_back(std::move(completionMarker));
                        else if (channelBase == 0x10009000u)
                            m_pendingVif1Transfers.push_back(std::move(completionMarker));
                    }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (eeTimerDecode(address, nullptr) >= 0)
            throw std::logic_error("EE timer write escaped Gate-3 MMIO hook");
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

// RRV_GIF_PATH3_INTERLEAVE — see the config struct in ps2_memory.h and
// docs/TESTING.md T-P7-CHANNEL-ATOMIC.
namespace
{
    // Length of the GIFtag-delimited block starting at `data`, or 0 if the tag
    // does not fit. `eop` reports the tag's EOP bit, which is the ONLY legal
    // splice point: a GIFtag with PRE=0 inherits the latched PRIM, and 1,424 of
    // the 2,240 tags in phase 7's PATH3 chain are PRE=0. Interleaving a PATH1
    // packet — which sets its own PRIM — between two of those reprograms the
    // primitive out from under the continuation and shears the whole scene
    // (measured). EOP is where the real GIF re-arbitrates, and it is where the
    // guest has finished with the shared drawing state.
    uint32_t gifPacketLength(const uint8_t *data, uint32_t available, bool *eop)
    {
        if (available < 16u)
            return 0u;
        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        if (eop)
            *eop = ((tagLo >> 15) & 1u) != 0u;

        uint64_t payloadQw = 0u;
        switch (flg)
        {
        case 0u: // PACKED
            payloadQw = static_cast<uint64_t>(nloop) * nreg;
            break;
        case 1u: // REGLIST — two registers per qword, padded up
            payloadQw = (static_cast<uint64_t>(nloop) * nreg + 1u) / 2u;
            break;
        default: // IMAGE (2) and the disabled encoding (3), which falls through
            payloadQw = nloop;
            break;
        }

        const uint64_t total = 16ull + payloadQw * 16ull;
        if (total > available)
            return 0u;
        return static_cast<uint32_t>(total);
    }

    // A+D register addresses the buffer-hazard walk cares about.
    constexpr uint8_t kGsRegTest1 = 0x47u;
    constexpr uint8_t kGsRegZbuf1 = 0x4Eu;
    constexpr uint8_t kRegAd = 0xEu;

    inline bool regIsDraw(uint8_t reg)
    {
        // XYZF2 / XYZ2 / XYZF3 / XYZ3 — the registers that kick a vertex.
        return reg == 0x04u || reg == 0x05u || reg == 0x0Cu || reg == 0x0Du;
    }

    inline void applyPrim(RrvGsBufferShadow &sh, uint64_t prim)
    {
        sh.ctx = static_cast<uint8_t>((prim >> 9) & 1u);
    }

    inline void applyAd(RrvGsBufferShadow &sh, uint8_t addr, uint64_t val)
    {
        if (addr == 0x00u)
        {
            applyPrim(sh, val);
        }
        else if (addr == kGsRegZbuf1 || addr == kGsRegZbuf1 + 1u)
        {
            const uint32_t c = addr - kGsRegZbuf1;
            sh.zbp[c] = static_cast<uint16_t>(val & 0x1FFu);
            sh.zmsk[c] = static_cast<uint8_t>((val >> 32) & 1u);
        }
        else if (addr == kGsRegTest1 || addr == kGsRegTest1 + 1u)
        {
            const uint32_t c = addr - kGsRegTest1;
            sh.zte[c] = static_cast<uint8_t>((val >> 16) & 1u);
            sh.ztst[c] = static_cast<uint8_t>((val >> 17) & 3u);
        }
    }

    // One pass over a GIF span. Latches buffer state; optionally reports the Z
    // pages the span reads (depth test enabled and not ALWAYS/NEVER) and writes
    // (ZMSK=0), each capped at a handful of distinct pages — RR5 never uses
    // more than one per span, and the cap only has to keep the walk bounded.
    struct ZPageSet
    {
        static constexpr uint32_t kCap = 4u;
        uint16_t pages[kCap] = {0u, 0u, 0u, 0u};
        uint32_t count = 0u;

        void add(uint16_t page)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (pages[i] == page)
                    return;
            if (count < kCap)
                pages[count++] = page;
        }
        bool has(uint16_t page) const
        {
            for (uint32_t i = 0; i < count; ++i)
                if (pages[i] == page)
                    return true;
            return false;
        }
    };

    void walkGifBuffers(RrvGsBufferShadow &sh, const uint8_t *data, uint32_t size,
                        ZPageSet *zReads, ZPageSet *zWrites, bool *sawDraw)
    {
        uint32_t off = 0u;
        while (off + 16u <= size)
        {
            uint64_t tagLo = 0u;
            uint64_t regs64 = 0u;
            std::memcpy(&tagLo, data + off, sizeof(tagLo));
            std::memcpy(&regs64, data + off + 8, sizeof(regs64));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            bool eop = false;
            const uint32_t len = gifPacketLength(data + off, size - off, &eop);
            if (len == 0u)
                return;

            if (((tagLo >> 46) & 1u) != 0u) // PRE: the tag carries its own PRIM
                applyPrim(sh, (tagLo >> 47) & 0x7FFu);

            if (flg == 0u || flg == 1u)
            {
                // Which register slots matter is a property of the tag, so the
                // per-vertex loop is only entered for tags that actually carry
                // A+D writes. Everything else is a cheap slot test.
                bool hasAd = false;
                bool hasDraw = false;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint8_t reg = static_cast<uint8_t>((regs64 >> (4u * r)) & 0xFu);
                    if (reg == kRegAd && flg == 0u)
                        hasAd = true;
                    else if (regIsDraw(reg))
                        hasDraw = true;
                }

                if (hasAd || hasDraw)
                {
                    const uint8_t *payload = data + off + 16u;
                    for (uint32_t i = 0; i < nloop; ++i)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            const uint8_t reg =
                                static_cast<uint8_t>((regs64 >> (4u * r)) & 0xFu);
                            if (flg == 0u && reg == kRegAd)
                            {
                                uint64_t val = 0u;
                                uint64_t addr = 0u;
                                const uint8_t *qw = payload + (i * nreg + r) * 16u;
                                std::memcpy(&val, qw, sizeof(val));
                                std::memcpy(&addr, qw + 8, sizeof(addr));
                                applyAd(sh, static_cast<uint8_t>(addr & 0xFFu), val);
                            }
                            else if (regIsDraw(reg))
                            {
                                if (sawDraw)
                                    *sawDraw = true;
                                const uint32_t c = sh.ctx & 1u;
                                if (zWrites && sh.zmsk[c] == 0u)
                                    zWrites->add(sh.zbp[c]);
                                if (zReads && sh.zte[c] != 0u && sh.ztst[c] > 1u)
                                    zReads->add(sh.zbp[c]);
                            }
                        }
                        if (!hasAd)
                            break; // no state changes inside the loop; one pass is enough
                    }
                }
            }

            off += len;
        }
    }
}

void PS2Memory::trackGifBufferState(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes)
{
    if (!gifPath3HazardEnabled() || !data || sizeBytes < 16u)
        return;

    ZPageSet writes;
    bool sawDraw = false;
    walkGifBuffers(m_gsShadow, data, sizeBytes, nullptr,
                   pathId == GifPathId::Path1 ? &writes : nullptr,
                   pathId == GifPathId::Path1 ? &sawDraw : nullptr);
    if (pathId != GifPathId::Path1 || !sawDraw)
        return;

    // Keep the two most recent distinct Z pages: one is the answer in every
    // scene measured, and two absorbs a buffer flip without ever growing into a
    // union that would call the whole composite chain hazardous.
    for (uint32_t i = 0; i < writes.count; ++i)
    {
        const uint16_t page = writes.pages[i];
        if (m_path1ZPageCount > 0u && m_path1ZPages[0] == page)
            continue;
        if (m_path1ZPageCount > 1u && m_path1ZPages[1] == page)
        {
            m_path1ZPages[1] = m_path1ZPages[0];
            m_path1ZPages[0] = page;
            continue;
        }
        m_path1ZPages[1] = m_path1ZPages[0];
        m_path1ZPages[0] = page;
        if (m_path1ZPageCount < 2u)
            ++m_path1ZPageCount;
    }
}

bool PS2Memory::path3PieceHazardous(RrvGsBufferShadow &shadow, const uint8_t *data,
                                    uint32_t begin, uint32_t stop) const
{
    ZPageSet reads;
    ZPageSet writes;
    walkGifBuffers(shadow, data + begin, stop - begin, &reads, &writes, nullptr);
    for (uint32_t i = 0; i < reads.count; ++i)
    {
        const uint16_t page = reads.pages[i];
        if (writes.has(page))
            continue; // read-modify-write of its own target: ordinary co-rendering
        for (uint32_t p = 0; p < m_path1ZPageCount; ++p)
            if (m_path1ZPages[p] == page)
                return true;
    }
    return false;
}

bool PS2Memory::armPath3Pacer(std::vector<PendingTransfer> &gif)
{
    m_path3Paced.clear();
    m_path3PacedChain = 0u;
    m_path3PacedTotalBytes = 0u;
    m_path3PacedSentBytes = 0u;
    m_path3PacedActive = false;

    // Only the payload-bearing PREFIX is taken, so whatever stays behind is
    // still submitted in its original position by the caller's GIF loop.
    size_t taken = 0u;
    for (auto &p : gif)
    {
        if (p.chainData.empty())
            break;

        Path3PacedChain chain;
        chain.data = std::move(p.chainData);
        chain.hasDiagnostic = p.carDmaDiag;
        if (p.carDmaDiag)
        {
            chain.diagnostic.carDma = true;
            chain.diagnostic.chainId = p.carDmaDiagChain;
            chain.diagnostic.begin = p.carDmaDiagBegin;
            chain.diagnostic.end = p.carDmaDiagEnd;
            chain.diagnostic.callTag = p.carDmaDiagCallTag;
            chain.diagnostic.callTarget = p.carDmaDiagCallTarget;
        }

        // Pre-split at EOP boundaries: [tagOffsets[i], tagOffsets[i+1]) is one
        // complete, self-contained GIF packet. A malformed tag stops the walk
        // and the remainder becomes one final piece, so a bad chain degrades to
        // the old whole-chain submit rather than being dropped.
        const uint32_t size = static_cast<uint32_t>(chain.data.size());
        uint32_t off = 0u;
        bool startPacket = true;
        while (off < size)
        {
            bool eop = false;
            const uint32_t len = gifPacketLength(chain.data.data() + off, size - off, &eop);
            if (len == 0u)
                break;
            if (startPacket)
                chain.tagOffsets.push_back(off);
            startPacket = eop;
            off += len;
        }
        if (off < size)
            chain.tagOffsets.push_back(off);
        chain.tagOffsets.push_back(size);

        // RRV_GIF_PATH3_HAZARD: find the first piece that consumes a Z buffer
        // PATH1 is still producing. Everything from there on waits for the
        // flush, which runs once the VIF1 expansion is complete.
        if (gifPath3HazardEnabled() && m_path1ZPageCount > 0u)
        {
            RrvGsBufferShadow shadow = m_gsShadow;
            for (size_t i = 0; i + 1u < chain.tagOffsets.size(); ++i)
            {
                if (path3PieceHazardous(shadow, chain.data.data(), chain.tagOffsets[i],
                                        chain.tagOffsets[i + 1u]))
                {
                    chain.hazardTag = i;
                    break;
                }
            }
            if (const char *diag = rrvCachedEnv("RRV_GIF_PATH3_HAZARD_DIAG");
                diag && diag[0] != '\0' && diag[0] != '0')
            {
                // One line per distinct chain shape: the same chain re-arms
                // every field, and the interesting shapes appear phases apart.
                static std::atomic<uint64_t> lastShape{~0ull};
                const uint64_t shape = (static_cast<uint64_t>(size) << 24) ^
                                       (static_cast<uint64_t>(chain.tagOffsets.size()) << 8) ^
                                       static_cast<uint64_t>(chain.hazardTag & 0xFFu);
                if (lastShape.exchange(shape, std::memory_order_relaxed) != shape)
                {
                    const size_t pieces = chain.tagOffsets.size() - 1u;
                    uint64_t early = 0u;
                    if (chain.hazardTag != SIZE_MAX && chain.hazardTag < pieces)
                        early = chain.tagOffsets[chain.hazardTag];
                    else
                        early = size;
                    std::fprintf(stderr,
                                 "[p3-hazard] pieces=%zu barrier=%zd early=%llu/%u "
                                 "z1=0x%03x z2=0x%03x n=%u\n",
                                 pieces,
                                 chain.hazardTag == SIZE_MAX
                                     ? static_cast<ptrdiff_t>(-1)
                                     : static_cast<ptrdiff_t>(chain.hazardTag),
                                 static_cast<unsigned long long>(early), size,
                                 m_path1ZPages[0], m_path1ZPages[1], m_path1ZPageCount);
                }
            }
        }

        m_path3PacedTotalBytes += size;
        m_path3Paced.push_back(std::move(chain));
        ++taken;
    }

    if (taken == 0u)
        return false;

    gif.erase(gif.begin(),
                                gif.begin() + static_cast<long>(taken));
    m_path3PacedActive = true;
    return true;
}

void PS2Memory::pacePath3()
{
    if (!m_path3PacedActive || m_path3PacingNow)
        return;
    m_path3PacingNow = true;

    // What PATH3 competes with, in bytes:
    //   * VIF1 chain bytes consumed — channel 1 holding the DMA bus. This term
    //     is what lets PATH3 move during the VU1 upload and microprogram, before
    //     any XGKICK exists, which is where hardware front-loads its uploads.
    //   * PATH1/PATH2 bytes emitted — the GIF itself busy. VU1 expands the VIF1
    //     chain about 1.7x, so pacing against the chain alone released PATH3 at
    //     well under half the right rate (measured: median upload at 22.7 % of
    //     the list against hardware's 7.4 %).
    // Equal bandwidth over the sum is the physical default; ratio256 scales it.
    const uint64_t others = m_path3PacedVif1Cursor + m_path3PacedPath1Bytes;
    uint64_t target = (others * gifPath3InterleaveConfig().ratio256) / 256u;
    if (target > m_path3PacedTotalBytes)
        target = m_path3PacedTotalBytes;

    while (m_path3PacedSentBytes < target && m_path3PacedChain < m_path3Paced.size())
    {
        Path3PacedChain &chain = m_path3Paced[m_path3PacedChain];
        if (chain.nextTag + 1u >= chain.tagOffsets.size())
        {
            ++m_path3PacedChain;
            continue;
        }

        // RRV_GIF_PATH3_HAZARD: a piece that reads a buffer PATH1 has not
        // finished writing stops the pacer dead. Order is preserved, so nothing
        // behind it may pass either — hence break, not continue.
        const size_t barrier = m_path3PacedFlushing ? SIZE_MAX : chain.hazardTag;
        if (chain.nextTag >= barrier)
            break;

        // Coalesce consecutive whole packets into one submit, so a chain of tiny
        // tags does not become thousands of one-tag submits.
        const uint32_t begin = chain.tagOffsets[chain.nextTag];
        size_t end = chain.nextTag + 1u;
        while (end + 1u < chain.tagOffsets.size() && end < barrier &&
               m_path3PacedSentBytes + (chain.tagOffsets[end] - begin) < target)
        {
            ++end;
        }
        const uint32_t stop = chain.tagOffsets[end];
        const uint32_t bytes = stop - begin;

        submitGifPacket(GifPathId::Path3, chain.data.data() + begin, bytes, true, false, 0u,
                        chain.hasDiagnostic ? &chain.diagnostic : nullptr);

        chain.nextTag = end;
        m_path3PacedSentBytes += bytes;
    }
    m_path3PacingNow = false;
}

void PS2Memory::flushPath3Pacer()
{
    if (!m_path3PacedActive)
        return;

    // Force the remainder out: a PATH3 chain must never outlive the kick that
    // started it, whatever the other channels did.
    m_path3PacedPath1Bytes = m_path3PacedTotalBytes;
    m_path3PacedVif1Cursor = m_path3PacedTotalBytes;
    m_path3PacedFlushing = true;
    pacePath3();
    m_path3PacedFlushing = false;

    if (const char *diag = rrvCachedEnv("RRV_GIF_PATH3_HAZARD_DIAG");
        diag && diag[0] != '\0' && diag[0] != '0')
    {
        static std::atomic<uint64_t> lastShape{~0ull};
        const uint64_t shape = (m_path3PacedSentBytes << 8) ^ m_path3PacedTotalBytes;
        if (lastShape.exchange(shape, std::memory_order_relaxed) != shape)
            std::fprintf(stderr, "[p3-hazard] flush sent=%llu/%llu\n",
                         static_cast<unsigned long long>(m_path3PacedSentBytes),
                         static_cast<unsigned long long>(m_path3PacedTotalBytes));
    }

    m_path3PacedActive = false;
    m_path3Paced.clear();
    m_path3PacedChain = 0u;
    m_path3PacedTotalBytes = 0u;
    m_path3PacedSentBytes = 0u;
    m_path3PacedVif1Base = 0u;
    m_path3PacedVif1Cursor = 0u;
    m_path3PacedPath1Bytes = 0u;
}

// Expand one queue of VIF1 chains (VU1 upload + microprogram + every XGKICK).
// Split out of processPendingTransfers so RRV_GIF_PATH_LATENCY can run it at a
// different point than the kick — see docs/HANDOFF_GIRL_B3.md.
void PS2Memory::processVif1Queue(std::vector<PendingTransfer> &queue)
{
    uint64_t gate4OwnerBytes = 0u;
    if (rrv::gate4::ownerTimeline().enabled)
        for (const auto &p : queue)
            gate4OwnerBytes += p.chainData.empty() ? uint64_t(p.qwc) * 16ull : uint64_t(p.chainData.size());
    std::optional<rrv::gate4::OwnerScope> gate4Owner;
    if (!queue.empty())
        gate4Owner.emplace(rrv::gate4::OwnerKind::Vif1Queue, gate4OwnerBytes);
    // RRV_GIF_PATH3_INTERLEAVE: the cursor runs across the whole queue, so the
    // PATH3 side advances smoothly through a multi-chain flush instead of
    // restarting at every chain.
    if (m_path3PacedActive)
        m_path3PacedVif1Base = 0u;

    for (auto &p : queue)
    {
        const uint64_t vif1ChainBytes = p.chainData.empty()
                                            ? static_cast<uint64_t>(p.qwc) * 16ull
                                            : static_cast<uint64_t>(p.chainData.size());
        if (!p.chainData.empty())
        {
            const bool oldCarDmaDiagActive = m_carDmaVifDiagActive;
            const uint64_t oldCarDmaDiagChain = m_carDmaVifDiagChain;
            const uint32_t oldCarDmaDiagBegin = m_carDmaVifDiagBegin;
            const uint32_t oldCarDmaDiagEnd = m_carDmaVifDiagEnd;
            m_carDmaVifDiagActive = p.carDmaDiag;
            m_carDmaVifDiagChain = p.carDmaDiagChain;
            m_carDmaVifDiagBegin = p.carDmaDiagBegin;
            m_carDmaVifDiagEnd = p.carDmaDiagEnd;

            const uint64_t oldTraceChainId = m_vif1TraceActiveChainId;
            const uint8_t *oldTraceDataBase = m_vif1TraceDataBase;
            const auto *oldTraceSourceBlocks = m_vif1TraceSourceBlocks;
            m_vif1TraceActiveChainId = p.traceChainId;
            m_vif1TraceDataBase = p.chainData.data();
            m_vif1TraceSourceBlocks = &p.sourceBlocks;

            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));

            m_vif1TraceActiveChainId = oldTraceChainId;
            m_vif1TraceDataBase = oldTraceDataBase;
            m_vif1TraceSourceBlocks = oldTraceSourceBlocks;

            m_carDmaVifDiagActive = oldCarDmaDiagActive;
            m_carDmaVifDiagChain = oldCarDmaDiagChain;
            m_carDmaVifDiagBegin = oldCarDmaDiagBegin;
            m_carDmaVifDiagEnd = oldCarDmaDiagEnd;
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
        m_path3PacedVif1Base += vif1ChainBytes;
    }
    queue.clear();
}

// RRV_GIF_PATH_LATENCY backstop — see the declaration in ps2_memory.h.
// Releases a held VIF1 chain once its own frame is over, so the hold can never
// become an unbounded queue. Same-tick holds are left alone: that is the race
// the feature exists to reproduce.
void PS2Memory::flushHeldVif1AtFrameBoundary()
{
    if (!rrvGifPathLatencyFrameBackstopDue(gifPathLatencyConfig(),
                                           ownerStreamV1() ? m_heldMirrorCountV1 != 0u
                                                           : !m_heldVif1Transfers.empty(),
                                           ps2_syscalls::GetCurrentVSyncTick(),
                                           m_heldVif1Tick))
        return;
    if (ownerStreamV1())
    {
        ownerFlushHeldV1("frame-boundary");
        return;
    }
    rrvNoteGifLatencyFlush("frame-boundary");
    processVif1Queue(m_heldVif1Transfers);
}

// See the declaration in ps2_memory.h. Called from sceGsSyncV, the guest's own
// frame barrier, BEFORE it waits for the tick — so a held chain expands inside
// the frame that kicked it and carries that frame's FRAME register.
void PS2Memory::flushHeldVif1AtGuestFrameEnd()
{
    if (ownerStreamV1())
    {
        if (gifPathLatencyEnabled() && m_heldMirrorCountV1 != 0u)
            ownerFlushHeldV1("guest-frame-end");
        return;
    }
    if (!gifPathLatencyEnabled() || m_heldVif1Transfers.empty())
        return;
    rrvNoteGifLatencyFlush("guest-frame-end");
    processVif1Queue(m_heldVif1Transfers);
}

void PS2Memory::processPendingTransfers()
{
    if (ownerStreamV1())
    {
        processPendingTransfersStreamV1();
        return;
    }
    // RRV_GIF_PATH_LATENCY: flush VIF1 chains held back by the previous call
    // BEFORE this call's PATH3 packets, once the race a held chain would have won
    // on hardware is over — i.e. once a PATH3 transfer too big to overtake it is
    // kicked, or the EE starts the next frame's chain.
    bool pacedGif = false;
    if (gifPathLatencyEnabled() && !m_heldVif1Transfers.empty())
    {
        uint64_t pendingGifBytes = 0;
        for (const auto &p : m_pendingGifTransfers)
            pendingGifBytes = std::max<uint64_t>(
                pendingGifBytes,
                p.chainData.empty() ? static_cast<uint64_t>(p.qwc) * 16ull
                                    : static_cast<uint64_t>(p.chainData.size()));
        const bool bigPath3 = pendingGifBytes >= gifPathLatencySmallPacketBytes();
        if (bigPath3 || !m_pendingVif1Transfers.empty())
        {
            rrvNoteGifLatencyFlush(bigPath3 ? "big-path3" : "next-vif1");
            // RRV_GIF_PATH3_INTERLEAVE: a LARGE PATH3 chain must not simply wait
            // for the whole VIF1 chain — on hardware the two channels share the
            // bus and interleave. Hand it to the pacer, which releases it in
            // GIFtag pieces from inside the VIF1 expansion below.
            if (bigPath3 && gifPath3InterleaveEnabled())
                pacedGif = armPath3Pacer(m_pendingGifTransfers);
            processVif1Queue(m_heldVif1Transfers);
            flushPath3Pacer();
        }
    }

    // A paced chain has left m_pendingGifTransfers, but it is still this call's
    // GIF traffic: `hadGif` drives D_STAT and the DMAC completion handler, and
    // dropping it loses the channel's completion (the phase-6 failure mode).
    const bool hadGif = !m_pendingGifTransfers.empty() || pacedGif;
    ownerGifLoopV1(m_pendingGifTransfers);

    const bool hadVif0 = processPendingVif0V1();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    if (gifPathLatencyEnabled())
    {
        // Hardware delivers a small PATH3 packet kicked right after a VIF1 chain
        // BEFORE that chain's PATH1 output, because VU1 has not reached an XGKICK
        // yet. Holding the chain here reproduces that; the held work is flushed
        // above, once a large PATH3 transfer or a new VIF1 chain arrives.
        if (m_heldVif1Transfers.empty() && !m_pendingVif1Transfers.empty())
        {
            m_heldVif1Tick = ps2_syscalls::GetCurrentVSyncTick();
            rrvNoteGifLatencyHold(read32(0x334E94u));
        }
        for (auto &p : m_pendingVif1Transfers)
            m_heldVif1Transfers.push_back(std::move(p));
        m_pendingVif1Transfers.clear();
    }
    else
    {
        processVif1Queue(m_pendingVif1Transfers);
    }

    if (m_gifArbiter)
        m_gifArbiter->drain();

    completePendingTransfersV1(hadGif, hadVif0, hadVif1);
}

// Owner work: the PATH3 GIF submissions of one processPendingTransfers().
void PS2Memory::ownerGifLoopV1(std::vector<PendingTransfer> &gif)
{
    for (size_t idx = 0; idx < gif.size(); ++idx)
    {
        auto &p = gif[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            GifPacketDiagnostic diagnostic;
            diagnostic.carDma = p.carDmaDiag;
            diagnostic.chainId = p.carDmaDiagChain;
            diagnostic.begin = p.carDmaDiagBegin;
            diagnostic.end = p.carDmaDiagEnd;
            diagnostic.callTag = p.carDmaDiagCallTag;
            diagnostic.callTarget = p.carDmaDiagCallTarget;
            if (girlGsVramDumpEnabled() && p.chainData.size() == 1845152u &&
                fnv1a64(p.chainData.data(), p.chainData.size()) == 0x30005bea209ef1eaull)
            {
                static std::atomic<bool> dumpedGirlGsState{false};
                if (!dumpedGirlGsState.exchange(true, std::memory_order_relaxed))
                {
                    bool vramWrote = false;
                    bool regsWrote = false;
                    if (FILE *dump = std::fopen("/private/tmp/girl_native_pre_gs_vram.bin", "wb"))
                    {
                        vramWrote = std::fwrite(m_gsVRAM, 1, PS2_GS_VRAM_SIZE, dump) ==
                                    PS2_GS_VRAM_SIZE;
                        std::fclose(dump);
                    }
                    if (FILE *dump = std::fopen("/private/tmp/girl_native_pre_gs_regs.bin", "wb"))
                    {
                        regsWrote = std::fwrite(&gs_regs, 1, sizeof(gs_regs), dump) ==
                                    sizeof(gs_regs);
                        std::fclose(dump);
                    }
                    std::fprintf(stderr,
                                 "[girl-gs-pre] bytes=%zu fnv1a64=%016llx vram=%u regs=%u\n",
                                 p.chainData.size(),
                                 static_cast<unsigned long long>(
                                     fnv1a64(p.chainData.data(), p.chainData.size())),
                                 vramWrote ? 1u : 0u, regsWrote ? 1u : 0u);
                }
            }
            submitGifPacket(GifPathId::Path3, p.chainData.data(),
                            static_cast<uint32_t>(p.chainData.size()), false, false, 0u,
                            diagnostic.carDma ? &diagnostic : nullptr);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            // B-3 green lattice: this non-chain PATH3 branch is where the
            // 503,408 B packet carrying ~3M REGLIST sprite batches arrives
            // (handoff §0.0h).  RRV_GIRL_LATTICE_PROV=<bytes> reports the guest
            // source address of transfers of exactly that size, which names the
            // EE buffer holding the GIFtags whose PRIM says sprite where
            // hardware says linestrip.  Default off, bounded.
            if (const char *want = rrvCachedEnv("RRV_GIRL_LATTICE_PROV");
                want && want[0] != '\0')
            {
                const uint32_t wantBytes =
                    static_cast<uint32_t>(std::strtoul(want, nullptr, 10));
                static std::atomic<uint32_t> latticeReports{0};
                if (sizeBytes == wantBytes &&
                    latticeReports.fetch_add(1, std::memory_order_relaxed) < 8u)
                {
                    std::fprintf(stderr,
                                 "[girl-lattice] PATH3 qwc-transfer src=%08x qwc=%u "
                                 "bytes=%u scratchpad=%u\n",
                                 p.srcAddr, p.qwc, sizeBytes,
                                 p.fromScratchpad ? 1u : 0u);
                }
            }
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }

        }
    }
    gif.clear();

}

// EE work: VIF0/VU0 transfers of one processPendingTransfers().
bool PS2Memory::processPendingVif0V1()
{
    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = bytes64 > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                         : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0u;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
            const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            while (sizeBytes > 0u)
            {
                if (srcPhys >= limit)
                    srcPhys = 0u;
                const uint32_t chunk = std::min<uint32_t>(sizeBytes, limit - srcPhys);
                if (chunk == 0u)
                    break;
                processVIF0Data(base + srcPhys, chunk);
                sizeBytes -= chunk;
                srcPhys += chunk;
            }

            // In normal mode the DMAC advances MADR by the completed transfer.
            // RRV's VIF0/VU0 builders program MADR once, then submit consecutive
            // QWC-sized blocks by rewriting only QWC and CHCR. Leaving MADR at
            // the first block replays that block on every subsequent kick.
            m_ioRegisters[0x10008010u] =
                p.srcAddr + static_cast<uint32_t>(bytes64);
        }
    }
    m_pendingVif0Transfers.clear();

    return hadVif0;
}

// EE work: channel completion, D_STAT and DMAC handlers.
void PS2Memory::completePendingTransfersV1(bool hadGif, bool hadVif0, bool hadVif1)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }

    // DMAC completions are synchronous in this HLE, but they still have to run
    // the guest handlers registered for each channel. Kernel handler enablement
    // is checked by dispatchDmacHandlersForCause; do not gate notification on
    // D_STAT's hardware mask bits because the HLE EnableDmacHandler path owns
    // that state and does not mirror it into the emulated register.
    if (m_dmacCompletionCallback && hadGif)
        m_pendingDmacCompletionCauses.push_back(2u);
    if (m_dmacCompletionCallback && hadVif0)
        m_pendingDmacCompletionCauses.push_back(0u);
    if (m_dmacCompletionCallback && hadVif1)
        m_pendingDmacCompletionCauses.push_back(1u);

    // A guest DMAC handler may start its successor VIF1/GIF transfer. Queue the
    // resulting completion until the current handler returns so interrupt work
    // is FIFO and never recursively nested while the handler has interrupts off.
    if (m_dmacCompletionCallback && !m_dispatchingDmacCompletions)
    {
        m_dispatchingDmacCompletions = true;
        try
        {
            while (!m_pendingDmacCompletionCauses.empty())
            {
                const uint32_t cause = m_pendingDmacCompletionCauses.front();
                m_pendingDmacCompletionCauses.pop_front();
                m_dmacCompletionCallback(cause);
            }
        }
        catch (...)
        {
            // Do not deliver successor events from a failed handler during a
            // later transfer or after PS2Memory is reinitialized.
            m_pendingDmacCompletionCauses.clear();
            m_dispatchingDmacCompletions = false;
            throw;
        }
        m_dispatchingDmacCompletions = false;
    }
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    rrvNoteDmaOrderEvent("p3-flush", m_path3MaskedFifo.size(), 0ull);

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize,
                    const GifPacketDiagnostic *diagnostic)
    {
        if (bootGifDmaDiagEnabled())
        {
            std::fprintf(stderr,
                         "[boot-gif-dma] PATH3 queued-flush size=%u fnv1a64=%016llx sink=%s\n",
                         packetSize, static_cast<unsigned long long>(fnv1a64(packetData, packetSize)),
                         m_gifArbiter ? "arbiter" : (m_gifPacketCallback ? "callback" : "none"));
        }
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false, 0u, diagnostic);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.data.size() >= 16u)
            emit(packet.data.data(), static_cast<uint32_t>(packet.data.size()),
                 packet.diagnostic.carDma ? &packet.diagnostic : nullptr);
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                                bool drainImmediately, bool path2DirectHl, uint32_t vu1Pc,
                                const GifPacketDiagnostic *diagnostic)
{
    if (!data || sizeBytes < 16)
        return;

    // RRV_GIF_PATH3_HAZARD: the GS drawing context latches across packets and
    // across paths, so the shadow has to see every submit in submission order.
    trackGifBufferState(pathId, data, sizeBytes);

    if (flagAM1DiagEnabledCached() && pathId == GifPathId::Path3 && sizeBytes == 320u)
    {
        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t phase = read32(0x334E94u);
        if (nloop == 19u && phase < kFlagAM1PhaseCount)
            s_flagAM1Clear320[phase].fetch_add(1u, std::memory_order_relaxed);
    }

    // B-3 green lattice: report every submit of a given packet size, whatever
    // the call site, and resolve the source pointer back to a guest address
    // when it lies inside EE RAM or the scratchpad.  This is what finally names
    // the EE buffer holding the REGLIST GIFtags whose PRIM says sprite where
    // hardware says linestrip (handoff §0.0h).  Default off, bounded.
    if (const char *want = rrvCachedEnv("RRV_GIRL_LATTICE_PROV");
        want && want[0] != '\0')
    {
        const uint32_t wantBytes = static_cast<uint32_t>(std::strtoul(want, nullptr, 10));
        static std::atomic<uint32_t> reports{0};
        if (sizeBytes == wantBytes && reports.fetch_add(1, std::memory_order_relaxed) < 8u)
        {
            const char *region = "external";
            uint64_t guest = 0u;
            if (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE)
            {
                region = "rdram";
                guest = static_cast<uint64_t>(data - m_rdram);
            }
            else if (data >= m_scratchpad && data < m_scratchpad + PS2_SCRATCHPAD_SIZE)
            {
                region = "scratchpad";
                guest = static_cast<uint64_t>(data - m_scratchpad);
            }
            std::fprintf(stderr,
                         "[girl-lattice] submitGifPacket path=%d size=%u region=%s "
                         "guest=%08llx vu1pc=%u\n",
                         static_cast<int>(pathId), sizeBytes, region,
                         static_cast<unsigned long long>(guest), vu1Pc);
        }
    }

    // RRV_TEX_PC=<tbp0> — which VU1 microprogram draws a given texture?
    // Scans the packet for a TEX0_1/TEX0_2 A+D write naming that TBP0 and bins
    // the submitting VU1 entry PC (bytes). Names the microprogram behind a pass
    // that was identified in a .gsr by texture identity, which is the step
    // between "this pass is wrong" and pointing RRV_VU_INTRACE at it.
    // docs/TESTING.md T-FLY-TREES.
    {
        static const uint32_t s_texPc = [] {
            const char *e = std::getenv("RRV_TEX_PC");
            return (e && e[0]) ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
        }();
        if (s_texPc != 0u && sizeBytes >= 16u)
        {
            static std::mutex mtx;
            static std::map<uint32_t, uint64_t> hits;
            static uint64_t seen = 0u;
            bool match = false;
            for (uint32_t o = 0; o + 16u <= sizeBytes && !match; o += 16u)
            {
                uint64_t lo = 0u, hi = 0u;
                std::memcpy(&lo, data + o, sizeof(lo));
                std::memcpy(&hi, data + o + 8, sizeof(hi));
                // A+D entries carry the register address in the high qword.
                if ((hi & 0xFFull) != 0x06ull && (hi & 0xFFull) != 0x07ull)
                    continue;
                if (static_cast<uint32_t>(lo & 0x3FFFull) == s_texPc)
                    match = true;
            }
            if (match)
            {
                std::lock_guard<std::mutex> lk(mtx);
                hits[vu1Pc] += 1u;
                if ((++seen % 20000ull) == 0ull)
                {
                    std::fprintf(stderr, "[tex-pc] tbp0=%u submitters:", s_texPc);
                    for (const auto &kv : hits)
                        std::fprintf(stderr, " pc=0x%04x:%llu", kv.first,
                                     static_cast<unsigned long long>(kv.second));
                    std::fprintf(stderr, "\n");
                }
            }
        }
    }

    // B-1: refresh the car-live latch from EE memory (we have read32 here; the
    // GS/GIF decode thread does not). Gated so it costs nothing unless a car
    // probe is armed. See diag_counters.h / RRV_CAR_GIF_DIAG.
    {
        static const bool carProbe = []
        {
            const char *g = std::getenv("RRV_CAR_GIF_DIAG");
            const char *v = std::getenv("RRV_VU1_DIAG");
            auto on = [](const char *e) { return e && e[0] && e[0] != '0'; };
            return on(g) || on(v);
        }();
        if (carProbe)
        {
            ps2_diag::noteCarSlot(read32(ps2_diag::kCarSlotAddr));
            ps2_diag::noteDemoPhase(read32(ps2_diag::kDemoPhaseAddr));
        }
    }

    if (shadowDmaDiagEnabled())
    {
        uint32_t shadowTags = 0u;
        for (uint32_t off = 0; off + 8u <= sizeBytes; off += 4u)
        {
            uint32_t w;
            std::memcpy(&w, data + off + 4u, 4u);
            if (w == 0x11AA4000u)
                ++shadowTags;
        }
        if (shadowTags)
        {
            std::fprintf(stderr,
                         "[shadow-submit-gif] path=%d size=%u shadowGifTags=%u masked=%u "
                         "arbiter=%u callback=%u\n",
                         static_cast<int>(pathId), sizeBytes, shadowTags,
                         m_path3Masked ? 1u : 0u, m_gifArbiter ? 1u : 0u,
                         m_gifPacketCallback ? 1u : 0u);
        }
    }

    if (pathId == GifPathId::Path3)
    {
        if (bootGifDmaDiagEnabled())
        {
            const char *state = m_path3Masked ? "masked-queued" :
                                (m_gifArbiter ? "forwarded-arbiter" :
                                 (m_gifPacketCallback ? "forwarded-callback" : "no-sink"));
            std::fprintf(stderr,
                         "[boot-gif-dma] PATH3 %s size=%u fnv1a64=%016llx masked=%u queued=%zu\n",
                         state, sizeBytes, static_cast<unsigned long long>(fnv1a64(data, sizeBytes)),
                         m_path3Masked ? 1u : 0u, m_path3MaskedFifo.size());
        }
        if (m_path3Masked)
        {
            rrvNoteDmaOrderEvent("p3-queued", sizeBytes, 0ull);
            MaskedPath3Packet packet;
            packet.data.assign(data, data + sizeBytes);
            packet.diagnostic = diagnostic ? *diagnostic : GifPacketDiagnostic{};
            m_path3MaskedFifo.push_back(std::move(packet));
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl, vu1Pc, diagnostic);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();

    // RRV_GIF_PATH3_INTERLEAVE: account the higher-priority traffic, but do NOT
    // release PATH3 here. PATH1 has priority over PATH3, and a draw run is a
    // burst of XGKICKs that hardware never interrupts: splicing PATH3 draws
    // between two XGKICKs of the same run reorders writes to the same
    // framebuffer and shears the composite (measured — hardware keeps
    // `fb70c0:716` intact and only puts PATH3 between runs). The release point
    // is the VIFcode boundary in processVIF1Data, which is between MSCALs and
    // therefore between geometry batches. Only the RATE comes from here: VU1
    // expands the VIF1 chain about 1.7x, so PATH1 output, not chain bytes, is
    // what PATH3 actually competes with for the GIF.
    if (m_path3PacedActive && !m_path3PacingNow &&
        (pathId == GifPathId::Path1 || pathId == GifPathId::Path2))
    {
        m_path3PacedPath1Bytes += sizeBytes;
    }
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;
            
        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);
        
        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (isGsPrivReg(address))
    {
        rrv::guest_time::QuietBump();
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint32_t off = address & 7u;
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }
    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (eeTimerDecode(address, nullptr) >= 0)
            throw std::logic_error("EE timer read escaped Gate-3 MMIO hook");

        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}

// ── EE timers T0-T3 ─────────────────────────────────────────────────────────
// Returns the timer index for an EE timer register address, or -1. `regIndex`
// receives 0=COUNT, 1=MODE, 2=COMP, 3=HOLD.
int PS2Memory::eeTimerDecode(uint32_t address, uint32_t *regIndex)
{
    if (address < 0x10000000u || address >= 0x10002000u)
        return -1;
    const uint32_t offset = address - 0x10000000u;
    const uint32_t index = offset >> 11;       // 0x800 apart
    const uint32_t within = offset & 0x7FFu;
    if (index > 3u || (within & 0xFu) != 0u || within > 0x30u)
        return -1;
    if (regIndex)
        *regIndex = within >> 4;
    return static_cast<int>(index);
}


// ---------------------------------------------------------------------------
// Gate-4 VU1+GS owner stream: PS2Memory side
// (docs/evidence/GATE4_VU1_GS_WORKER_DESIGN_2026-09-25.md §4, S1-S3).
//
// With an owner stream installed (RRV_VU1GS_EXECUTION != inline), the EE keeps
// every guest-visible decision -- channel completion, D_STAT, handlers, the
// RRV_GIF_PATH_LATENCY hold/flush decision and its tick -- and hands the work
// behind it (VIF1 expansion, VU1, the PATH3 pacer and mask FIFO, the GIF
// arbiter and GS submission) to the owner as one command, in the inline order.
// The owner runs the same functions the inline path runs. The EE mirrors only
// what it needs to decide: how many VIF1 chains are held, and whether one of
// them still reads guest RAM when it is expanded (a normal-mode transfer). A
// command that reads guest RAM runs while the EE waits, so it reads the bytes
// the inline path would have read.
// ---------------------------------------------------------------------------

void PS2Memory::ownerPostV1(std::function<void()> command, size_t bytes, size_t fields, bool sync)
{
    ++m_ownerCommandsV1;
    if (sync)
        ++m_ownerSyncCommandsV1;
    // The worker's byte cap bounds queued payload; a single larger command is
    // still legal (it is copied already) and is charged at the cap.
    constexpr size_t kCap = 16u * 1024u * 1024u;
    m_ownerStreamV1.post(std::move(command), bytes > kCap ? kCap : bytes, fields, sync);
}

void PS2Memory::ownerFenceForVuWriteV1(uint32_t physAddr)
{
    if (ownerStreamV1() && physAddr >= PS2_VU1_CODE_BASE && physAddr < PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
        ownerFenceV1();
}

// processPendingTransfers() with an owner stream. Same decisions and the same
// order of owner work as the inline body; VIF0 stays on the EE (it touches no
// owner state), so running it before the one owner command is equivalent.
void PS2Memory::processPendingTransfersStreamV1()
{
    // Payload bytes of a queue; `normal` is set if a transfer still reads guest
    // RAM when processed (chainData empty), which makes its command synchronous.
    const auto transferBytes = [](const std::vector<PendingTransfer> &queue, bool &normal)
    {
        size_t bytes = 0u;
        for (const auto &p : queue)
        {
            if (p.chainData.empty())
            {
                if (p.qwc > 0u)
                    normal = true;
                bytes += static_cast<size_t>(p.qwc) * 16u;
            }
            else
                bytes += p.chainData.size();
        }
        return bytes;
    };
    const bool latency = gifPathLatencyEnabled();
    bool flushHeld = false;
    bool bigPath3 = false;
    if (latency && m_heldMirrorCountV1 != 0u)
    {
        uint64_t pendingGifBytes = 0;
        for (const auto &p : m_pendingGifTransfers)
            pendingGifBytes = std::max<uint64_t>(
                pendingGifBytes,
                p.chainData.empty() ? static_cast<uint64_t>(p.qwc) * 16ull
                                    : static_cast<uint64_t>(p.chainData.size()));
        bigPath3 = pendingGifBytes >= gifPathLatencySmallPacketBytes();
        if (bigPath3 || !m_pendingVif1Transfers.empty())
        {
            rrvNoteGifLatencyFlush(bigPath3 ? "big-path3" : "next-vif1");
            flushHeld = true;
        }
    }
    // armPath3Pacer() either takes a non-empty prefix (and reports it) or
    // takes nothing, so the inline `hadGif` is exactly "GIF work was pending".
    const bool hadGif = !m_pendingGifTransfers.empty();
    std::vector<PendingTransfer> gif = std::move(m_pendingGifTransfers);
    m_pendingGifTransfers.clear();

    const bool hadVif0 = processPendingVif0V1();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    bool normal = flushHeld && m_heldMirrorNormalV1;
    size_t bytes = transferBytes(gif, normal);
    size_t heldCount = flushHeld ? 0u : m_heldMirrorCountV1;
    bool heldNormal = flushHeld ? false : m_heldMirrorNormalV1;
    if (latency)
    {
        if (heldCount == 0u && !m_pendingVif1Transfers.empty())
        {
            m_heldVif1Tick = ps2_syscalls::GetCurrentVSyncTick();
            rrvNoteGifLatencyHold(read32(0x334E94u));
        }
        bool vif1Normal = false;
        bytes += transferBytes(m_pendingVif1Transfers, vif1Normal);
        heldNormal = heldNormal || vif1Normal; // read later, at the flush
        heldCount += m_pendingVif1Transfers.size();
    }
    else
        bytes += transferBytes(m_pendingVif1Transfers, normal); // read now
    m_heldMirrorCountV1 = heldCount;
    m_heldMirrorNormalV1 = heldNormal;
    std::vector<PendingTransfer> vif1 = std::move(m_pendingVif1Transfers);
    m_pendingVif1Transfers.clear();

    // The inline path always drains the arbiter here. That is a no-op unless a
    // held-chain flush (which does not drain) left packets queued, which the EE
    // tracks, so an empty call posts nothing.
    if (flushHeld || !gif.empty() || !vif1.empty() || m_arbiterDirtyV1)
    {
        m_arbiterDirtyV1 = false;
        auto work = std::make_shared<std::pair<std::vector<PendingTransfer>, std::vector<PendingTransfer>>>(
            std::move(gif), std::move(vif1));
        ownerPostV1([this, work, flushHeld, bigPath3, latency]
        {
            auto &[ownerGif, ownerVif1] = *work;
            if (flushHeld)
            {
                if (bigPath3 && gifPath3InterleaveEnabled())
                    armPath3Pacer(ownerGif);
                processVif1Queue(m_heldVif1Transfers);
                flushPath3Pacer();
            }
            ownerGifLoopV1(ownerGif);
            if (latency)
            {
                for (auto &p : ownerVif1)
                    m_heldVif1Transfers.push_back(std::move(p));
                ownerVif1.clear();
            }
            else
                processVif1Queue(ownerVif1);
            if (m_gifArbiter)
                m_gifArbiter->drain();
        }, bytes, 0u, normal);
    }

    completePendingTransfersV1(hadGif, hadVif0, hadVif1);
}

void PS2Memory::ownerFlushHeldV1(const char *reason)
{
    rrvNoteGifLatencyFlush(reason);
    const bool normal = m_heldMirrorNormalV1;
    m_heldMirrorCountV1 = 0u;
    m_heldMirrorNormalV1 = false;
    m_arbiterDirtyV1 = true; // processVif1Queue may leave packets for the next drain
    ownerPostV1([this] { processVif1Queue(m_heldVif1Transfers); }, 0u, 0u, normal);
}
