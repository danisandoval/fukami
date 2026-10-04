// Gate-4 S0 (docs/evidence/GATE4_VU1_GS_WORKER_DESIGN_2026-09-25.md §6): the
// owner-work timeline. RRV_GATE4_OWNER_TIMELINE=<path> records, on the one
// producer thread, a steady-clock ns stamp at:
//
//   * the begin and end of each OUTERMOST unit of work the VU1+GS owner would
//     take over (VIF1 queue/data, GIF arbiter drain, GS packet, HLE register
//     submission, field vsync). Nested units belong to the outer one, so each
//     begin/end pair is one would-be command, with its payload bytes;
//   * each point the EE would have to wait for the owner (GS CSR/SIGLBLID MMIO
//     read, a CSR publish while IMR unmasks a bridge-owned bit, a local-to-host
//     read), when it happens outside owner work;
//   * each EE write into VU1 memory (an ordered command, not a wait);
//   * each effective VBlank start (for the contract's windows).
//
// Records go to a preallocated array and are written once, at the temporal
// cut or at exit. Host-only; nothing here reads or writes guest state beyond
// what the call sites already hold, and it is off by default.
// scripts/gate4_owner_projection.py replays the file as a two-thread schedule.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rrv::gate4
{
enum class OwnerEvent : uint8_t { Begin = 1, End = 2, Barrier = 3, VuWrite = 4, Start = 5 };
enum class OwnerKind : uint8_t { Vif1Queue = 1, Vif1Data = 2, GifDrain = 3, GsPacket = 4, HleRegs = 5, FieldVsync = 6 };
enum class BarrierKind : uint8_t { CsrRead = 1, SiglblidRead = 2, CsrPublishUnmasked = 3, LocalRead = 4 };

struct OwnerRecord
{
    uint64_t ns;
    uint32_t arg; // bytes (Begin, VuWrite), start ordinal (Start), 0 otherwise
    uint8_t event;
    uint8_t kind;
    uint16_t reserved;
};
static_assert(sizeof(OwnerRecord) == 16, "owner timeline record layout");

struct OwnerTimeline
{
    bool enabled = false;
    bool written = false;
    unsigned depth = 0;
    std::string path;
    std::vector<OwnerRecord> records;
    uint64_t publishEvaluations = 0; // CSR publishes outside owner work
    uint64_t dropped = 0;

    static uint64_t Now()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    }
    void Push(OwnerEvent event, uint8_t kind, uint32_t arg)
    {
        if (records.size() == records.capacity() && records.size() >= (size_t(1) << 28))
        {
            ++dropped;
            return;
        }
        records.push_back(OwnerRecord{Now(), arg, uint8_t(event), kind, 0});
    }
    // File: "RRVOWNT1", u64 record count, u64 publish evaluations, u64 dropped,
    // then the records as written above (little-endian host layout).
    void Write()
    {
        if (!enabled || written)
            return;
        written = true;
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            std::fprintf(stderr, "[gate4-owner] cannot open %s\n", path.c_str());
            return;
        }
        const uint64_t header[3] = {uint64_t(records.size()), publishEvaluations, dropped};
        std::fwrite("RRVOWNT1", 1, 8, f);
        std::fwrite(header, sizeof(header), 1, f);
        if (!records.empty())
            std::fwrite(records.data(), sizeof(OwnerRecord), records.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "[gate4-owner] records=%llu publish_evaluations=%llu dropped=%llu path=%s\n",
                     (unsigned long long)records.size(), (unsigned long long)publishEvaluations,
                     (unsigned long long)dropped, path.c_str());
    }
};

inline OwnerTimeline &ownerTimeline()
{
    static OwnerTimeline *timeline = [] {
        auto *t = new OwnerTimeline();
        if (const char *v = std::getenv("RRV_GATE4_OWNER_TIMELINE"); v && *v)
        {
            t->enabled = true;
            t->path = v;
            t->records.reserve(size_t(1) << 22);
            std::atexit([] { ownerTimeline().Write(); });
        }
        return t;
    }();
    return *timeline;
}

// One would-be owner command: only the outermost scope records.
class OwnerScope
{
public:
    OwnerScope(OwnerKind kind, uint64_t bytes)
    {
        OwnerTimeline &t = ownerTimeline();
        if (!t.enabled)
            return;
        m_on = true;
        if (t.depth++ == 0)
            t.Push(OwnerEvent::Begin, uint8_t(kind), bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(bytes));
    }
    ~OwnerScope()
    {
        if (!m_on)
            return;
        OwnerTimeline &t = ownerTimeline();
        if (--t.depth == 0)
            t.Push(OwnerEvent::End, 0, 0);
    }
    OwnerScope(const OwnerScope &) = delete;
    OwnerScope &operator=(const OwnerScope &) = delete;

private:
    bool m_on = false;
};

inline void ownerBarrier(BarrierKind kind)
{
    OwnerTimeline &t = ownerTimeline();
    if (t.enabled && t.depth == 0)
        t.Push(OwnerEvent::Barrier, uint8_t(kind), 0);
}

// A CSR publish is a wait only when IMR unmasks a bridge-owned CSR bit
// (SIGNAL, FINISH, HSINT, EDWINT; bit 3 VSINT is EE-owned): design §4.2 item 2.
inline void ownerCsrPublish(uint64_t imr)
{
    OwnerTimeline &t = ownerTimeline();
    if (!t.enabled || t.depth != 0)
        return;
    ++t.publishEvaluations;
    if ((~(imr >> 8) & 0x17u) != 0u)
        t.Push(OwnerEvent::Barrier, uint8_t(BarrierKind::CsrPublishUnmasked), 0);
}

inline void ownerVuWrite(uint32_t physAddr, uint32_t bytes)
{
    OwnerTimeline &t = ownerTimeline();
    if (t.enabled && t.depth == 0 && physAddr >= 0x11008000u && physAddr < 0x11010000u)
        t.Push(OwnerEvent::VuWrite, 0, bytes);
}

inline void ownerStart(uint64_t ordinal)
{
    OwnerTimeline &t = ownerTimeline();
    if (t.enabled)
        t.Push(OwnerEvent::Start, 0, ordinal > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(ordinal));
}
} // namespace rrv::gate4
