#include "runtime/ps2_gif_arbiter.h"
#include "ps2_log.h"
#include "rrv_gate4_owner_timeline.h" // Gate-4 S0 owner timeline (RRV_GATE4_OWNER_TIMELINE)
#include <cstdlib>
#include <optional>
#include "rrv_gs_record_hooks.h" // milestone B4: write-only GS-stream recorder (RRV_GS_RECORD)
#include "rrv_snapshot_hooks.h"  // developer snapshots: display-list-signature trigger
#include "rrv_gif_guard.h"      // G1 packet-lifetime probe (RRV_GIF_GUARD); default off
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>

namespace
{
    std::atomic<uint32_t> s_debugGifArbiterSubmitCount{0};
    std::atomic<uint32_t> s_debugGifArbiterDrainCount{0};

    const char *pathName(GifPathId id)
    {
        switch (id)
        {
        case GifPathId::Path1:
            return "path1";
        case GifPathId::Path2:
            return "path2";
        case GifPathId::Path3:
            return "path3";
        default:
            return "path?";
        }
    }
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                        bool path2DirectHl, uint32_t vu1Pc,
                        const GifPacketDiagnostic *diagnostic)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    // --- Milestone B4: GS-stream recorder. This is the chosen recording
    // boundary — the raw packet in arrival order, AFTER PATH1/2/3 arbitration
    // has selected it (queued for drain()) but BEFORE any GS decode. Recording
    // here (rather than inside GS::processGIFPacket) means replay re-derives
    // the arbiter's own priority sort in drain() from the SAME recorded
    // arrival order, so the replay re-executes that logic too, not just the
    // GS decode. See src/gs-record/rrv_gs_record_format.h for the full
    // rationale. Guarded by a single relaxed load so recording-off cost is
    // one branch.
    if (rrv::gsrecord::rrv_gs_record_enabled())
    {
        rrv::gsrecord::hookGifPacket(static_cast<uint8_t>(pathId), data, sizeBytes);
    }

    // Developer snapshots (docs/SNAPSHOTS.md §5): lets a checkpoint be triggered
    // by a display-list signature ("the first PATH3 chain of >= N bytes"), which
    // for a scene with no convenient state word is the most reliable
    // deterministic marker available. Shape only — no packet data crosses the
    // boundary — and the capture still happens at the next dispatch safepoint,
    // never here. No-op unless such a trigger was armed.
    if (rrv::snapshot::enabled())
    {
        rrv::snapshot::hookGifPacket(static_cast<uint8_t>(pathId), sizeBytes);
    }

    const uint32_t debugIndex = s_debugGifArbiterSubmitCount.fetch_add(1, std::memory_order_relaxed);
    if (debugIndex < 96u)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        RUNTIME_LOG("[gif:submit] idx=" << debugIndex
                                        << " path=" << pathName(pathId)
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " directhl=" << static_cast<uint32_t>(path2DirectHl ? 1u : 0u)
                                        << std::endl);
    }

    GifArbiterPacket pkt;
    if (!m_pool.empty())
    {
        // Reuse a recycled packet (retains its data buffer's capacity).
        pkt = std::move(m_pool.back());
        m_pool.pop_back();
    }
    pkt.pathId = pathId;
    pkt.vu1Pc = (pathId == GifPathId::Path1) ? vu1Pc : 0u;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.diagnostic = diagnostic ? *diagnostic : GifPacketDiagnostic{};
    // assign() reuses existing capacity and (for trivially-copyable bytes) copies
    // without the redundant zero-fill that resize() would do before the memcpy.
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;
    uint64_t gate4OwnerBytes = 0u;
    if (rrv::gate4::ownerTimeline().enabled)
        for (const auto &pkt : m_queue)
            gate4OwnerBytes += pkt.data.size();
    std::optional<rrv::gate4::OwnerScope> gate4Owner;
    if (!m_queue.empty())
        gate4Owner.emplace(rrv::gate4::OwnerKind::GifDrain, gate4OwnerBytes);

    // RRV_GIF_NO_SORT=1 -- deliver packets in ARRIVAL order instead of the
    // priority sort below. This exists because the GS-stream recorder hooks
    // submit() (arrival) while the GS is fed from drain() (sorted), so a .gsr
    // and the live GS can see two different orders, and every offline .gs
    // comparison validates the arrival order rather than the delivered one.
    // Causality probe for the A1 texture corruption; default off.
    static const bool noSort = [] {
        const char *v = std::getenv("RRV_GIF_NO_SORT");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (!noSort)
    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            const uint32_t debugIndex = s_debugGifArbiterDrainCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 96u)
            {
                uint64_t tagLo = 0;
                std::memcpy(&tagLo, pkt.data.data(), sizeof(tagLo));
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                RUNTIME_LOG("[gif:drain] idx=" << debugIndex
                                               << " path=" << pathName(pkt.pathId)
                                               << " size=" << pkt.data.size()
                                               << " nloop=" << nloop
                                               << " flg=" << static_cast<uint32_t>(flg)
                                               << " nreg=" << nreg
                                               << " directhl=" << static_cast<uint32_t>(pkt.path2DirectHl ? 1u : 0u)
                                               << " path3image=" << static_cast<uint32_t>(pkt.path3Image ? 1u : 0u)
                                               << std::endl);
            }
            // G1: is the arbiter's OWN copy still mapped at the instant we
            // hand it to the sink? Observe only -- never skip the packet, or
            // the probe would mask the defect it exists to measure.
            rrv::gifguard::checkSpan("drain", static_cast<uint32_t>(pkt.pathId),
                                     pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            m_processFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()), pkt.vu1Pc,
                        pkt.diagnostic.carDma ? &pkt.diagnostic : nullptr);
        }
    }
    // Recycle the drained packets instead of freeing their buffers. Cap the pool so
    // an occasional burst of packets cannot pin a large amount of memory forever.
    constexpr size_t kMaxPooledPackets = 256;
    for (auto &pkt : m_queue)
    {
        if (m_pool.size() < kMaxPooledPackets)
        {
            m_pool.push_back(std::move(pkt));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
