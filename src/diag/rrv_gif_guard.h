// rrv_gif_guard.h — G1 GIF packet-lifetime probe.
//
// WHY THIS EXISTS
// ---------------
// The phase-6->7 cold-boot crash (KNOWN_ISSUES #16, docs/TESTING.md
// T-C0-COLDBOOT-CRASH) faults inside the PCSX2 bridge while decoding a GIF
// packet, at a PAGE-ALIGNED heap address, on both Software and Metal. That
// signature is a freed large allocation, not a GS defect: `GifArbiter` copies
// every packet into its own `std::vector<uint8_t>`, so the pointer handed
// across the seam is supposed to be arbiter-owned storage that is alive for the
// whole call.
//
// This probe answers exactly one question without crashing: **is the buffer
// still mapped at the moment the arbiter hands it to the sink?**
//   - if it is already dead in drain(), the defect is upstream of the seam and
//     the bridge is only the first consumer to touch the corpse;
//   - if it is alive in drain() but the bridge still faults, the defect is at
//     the seam (pointer/size transformation) or in PCSX2's decode.
//
// `mincore()` is the test: it reports residency for MAPPED pages and fails with
// ENOMEM for unmapped ones, so it distinguishes "freed and unmapped" from
// "valid" without dereferencing anything. macOS frees large (>= ~128 KB)
// allocations back to the kernel with munmap, which is why this class of
// use-after-free faults hard here instead of silently reading stale bytes --
// and RRV's expanded VIF1 chains reach ~1.8 MB.
//
// A small ring of recent submit/drain events is kept so the offending packet
// can be placed in its sequence rather than reported in isolation.
//
// DISCIPLINE
// ----------
// Diagnostic only, default off (RRV_GIF_GUARD=1), zero behaviour change when
// unset (one cached bool). When set it only observes and logs -- it does NOT
// skip, repair or reorder any packet, because a probe that silently drops the
// bad packet would mask the very defect it is measuring.
#ifndef RRV_GIF_GUARD_H
#define RRV_GIF_GUARD_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#if !defined(_WIN32)
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace rrv::gifguard
{
    inline bool enabled()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("RRV_GIF_GUARD");
            return v && v[0] && v[0] != '0';
        }();
        return s_on;
    }

    // true  = every page spanned by [p, p+bytes) is mapped
    // false = at least one page is not mapped (freed back to the kernel)
    inline bool mapped(const void *p, size_t bytes)
    {
#if defined(_WIN32)
        (void)p;
        (void)bytes;
        return true;
#else
        if (!p || bytes == 0)
            return false;
        static const size_t pageSize = static_cast<size_t>(::getpagesize());
        const uintptr_t first = reinterpret_cast<uintptr_t>(p) & ~(uintptr_t)(pageSize - 1);
        const uintptr_t last = (reinterpret_cast<uintptr_t>(p) + bytes - 1) & ~(uintptr_t)(pageSize - 1);
        // Probe only the first and last page: a vector's storage is one
        // contiguous mapping, so its ends decide the whole span, and probing
        // every page of a 1.8 MB chain on every packet would dominate the run.
        for (uintptr_t page : {first, last})
        {
            // Darwin and Linux expose different pointer types for this host
            // diagnostic; the residency byte and guest behavior are unchanged.
#if defined(__APPLE__)
            char vec = 0;
#else
            unsigned char vec = 0;
#endif
            if (::mincore(reinterpret_cast<void *>(page), pageSize, &vec) != 0)
            {
                if (errno == ENOMEM)
                    return false;
            }
        }
        return true;
#endif
    }

    struct Event
    {
        const void *ptr;
        uint32_t bytes;
        uint32_t pathId;
        uint32_t seq;
        char kind;
        // First GIFtag of the packet, plus whether the packet is SELF-CONTAINED:
        // i.e. whether walking its tag chain lands exactly on its last byte.
        // PCSX2 keeps GIFPath decode state across calls and RRV funnels all
        // three arbitrated paths onto one Transfer<3>, so a packet that ends
        // mid-tag makes PCSX2 resume the remainder using the NEXT packet's
        // bytes -- and its partial-consume loop decrements an unsigned `size`
        // that has already reached 0. That is the shape to look for.
        uint64_t tag0;
        int32_t walk; // +1 exact, 0 short (ends mid-tag), -1 overrun, -2 unwalkable
    };

    // Walk the packet's GIFtag chain and report how it lands relative to its
    // declared length. Read-only; bounded by the packet size.
    inline int32_t walkTags(const uint8_t *p, uint32_t bytes)
    {
        if (!p || bytes < 16u || (bytes & 15u) != 0u)
            return -2;
        uint32_t off = 0;
        for (uint32_t guard = 0; guard < (1u << 20); ++guard)
        {
            if (off == bytes)
                return 1; // landed exactly on the end
            if (off + 16u > bytes)
                return 0; // not even room for the next tag
            uint64_t tag = 0;
            std::memcpy(&tag, p + off, sizeof(tag));
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            off += 16u; // the tag itself
            uint64_t payload = 0;
            switch (flg)
            {
            case 0: payload = static_cast<uint64_t>(nloop) * nreg * 16ull; break;       // PACKED
            case 1: payload = static_cast<uint64_t>(nloop) * nreg * 8ull; break;        // REGLIST
            default: payload = static_cast<uint64_t>(nloop) * 16ull; break;             // IMAGE/disabled
            }
            if (flg == 1 && (static_cast<uint64_t>(nloop) * nreg) & 1ull)
                payload += 8ull; // REGLIST pads to a qword
            if (off + payload > bytes)
                return 0; // declares more data than the packet carries
            off += static_cast<uint32_t>(payload);
        }
        return -2;
    }

    inline constexpr uint32_t kRing = 64u;
    inline Event g_ring[kRing]{};
    inline std::atomic<uint32_t> g_ringPos{0};
    inline std::atomic<uint32_t> g_seq{0};

    inline void note(char kind, uint32_t pathId, const void *ptr, uint32_t bytes)
    {
        const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        const uint32_t slot = g_ringPos.fetch_add(1, std::memory_order_relaxed) % kRing;
        uint64_t tag0 = 0;
        if (ptr && bytes >= 8u)
            std::memcpy(&tag0, ptr, sizeof(tag0));
        g_ring[slot] = Event{ptr, bytes, pathId, seq, kind, tag0,
                             walkTags(static_cast<const uint8_t *>(ptr), bytes)};
    }

    // Print every GIFtag in a packet, so a non-self-contained packet can be
    // attributed to a specific tag rather than inferred from the total.
    inline void dumpTags(const uint8_t *p, uint32_t bytes)
    {
        if (!p || bytes < 16u)
            return;
        uint32_t off = 0;
        for (uint32_t i = 0; i < 64u && off + 16u <= bytes; ++i)
        {
            uint64_t tag = 0;
            std::memcpy(&tag, p + off, sizeof(tag));
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
            const uint32_t eop = static_cast<uint32_t>((tag >> 15) & 1u);
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t payload = 0;
            switch (flg)
            {
            case 0: payload = static_cast<uint64_t>(nloop) * nreg * 16ull; break;
            case 1:
                payload = static_cast<uint64_t>(nloop) * nreg * 8ull;
                if ((static_cast<uint64_t>(nloop) * nreg) & 1ull)
                    payload += 8ull;
                break;
            default: payload = static_cast<uint64_t>(nloop) * 16ull; break;
            }
            std::fprintf(stderr,
                         "[gif:guard]   tag[%u] off=%u nloop=%u nreg=%u flg=%u eop=%u "
                         "payload=%llu next=%llu limit=%u%s\n",
                         i, off, nloop, nreg, flg, eop, (unsigned long long)payload,
                         (unsigned long long)(off + 16ull + payload), bytes,
                         (off + 16ull + payload > bytes) ? "  <-- OVERRUNS PACKET" : "");
            off += 16u;
            if (off + payload > bytes)
                return;
            off += static_cast<uint32_t>(payload);
            if (eop)
                return;
        }
    }

    inline void dumpRing(const char *why)
    {
        std::fprintf(stderr, "[gif:guard] ---- ring dump (%s) ----\n", why);
        const uint32_t pos = g_ringPos.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < kRing; ++i)
        {
            const Event &e = g_ring[(pos + i) % kRing];
            if (!e.ptr && !e.bytes)
                continue;
            const char *land = e.walk == 1 ? "exact" : e.walk == 0 ? "SHORT" : "unwalkable";
            const uint32_t nloop = static_cast<uint32_t>(e.tag0 & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((e.tag0 >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((e.tag0 >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            std::fprintf(stderr,
                         "[gif:guard]   seq=%u %c path=%u ptr=%p bytes=%u mapped=%d "
                         "nloop=%u flg=%u nreg=%u land=%s\n",
                         e.seq, e.kind, e.pathId, e.ptr, e.bytes,
                         mapped(e.ptr, e.bytes) ? 1 : 0, nloop, flg, nreg, land);
        }
        std::fprintf(stderr, "[gif:guard] ---- end ----\n");
    }

    // Crash-time dump. The failure is intermittent and kills the process, so the
    // ring is only useful if it survives the fault; these are the three signals
    // observed for this defect (SIGSEGV/SIGBUS from the decode walking off the
    // end, SIGABRT from PCSX2's Devel-only assertion downstream of it).
    inline void onFatalSignal(int sig)
    {
        std::fprintf(stderr, "[gif:guard] fatal signal %d\n", sig);
        dumpRing("fatal-signal");
        std::fflush(stderr);
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

    inline void installCrashHandler()
    {
        if (!enabled())
            return;
        static bool s_installed = false;
        if (s_installed)
            return;
        s_installed = true;
        std::signal(SIGSEGV, onFatalSignal);
        std::signal(SIGBUS, onFatalSignal);
        std::signal(SIGABRT, onFatalSignal);
    }

    // Called from GifArbiter::drain() immediately before the sink, and from the
    // GS backend seam. Returns true when the span is intact.
    inline bool checkSpan(const char *site, uint32_t pathId, const void *ptr, uint32_t bytes)
    {
        if (!enabled())
            return true;
        installCrashHandler();
        note(site[0], pathId, ptr, bytes);
        // A packet that does not land exactly on its own end is the shape that
        // desynchronises PCSX2's persistent GIFPath state. Report it even when
        // the memory itself is fine -- that is the defect, not the symptom.
        const Event &last = g_ring[(g_ringPos.load(std::memory_order_relaxed) - 1u) % kRing];
        if (last.walk == 0)
        {
            static std::atomic<uint32_t> s_shortReported{0};
            if (s_shortReported.fetch_add(1, std::memory_order_relaxed) < 8u)
            {
                std::fprintf(stderr,
                             "[gif:guard] NON-SELF-CONTAINED packet at %s: path=%u ptr=%p bytes=%u\n",
                             site, pathId, ptr, bytes);
                dumpTags(static_cast<const uint8_t *>(ptr), bytes);
                dumpRing(site);
                std::fflush(stderr);
            }
        }
        if (mapped(ptr, bytes))
            return true;
        static std::atomic<uint32_t> s_reported{0};
        if (s_reported.fetch_add(1, std::memory_order_relaxed) < 4u)
        {
            std::fprintf(stderr,
                         "[gif:guard] UNMAPPED GIF PACKET at %s: path=%u ptr=%p bytes=%u\n"
                         "[gif:guard] the buffer handed across the seam was already freed\n",
                         site, pathId, ptr, bytes);
            dumpRing(site);
            std::fflush(stderr);
        }
        return false;
    }
}

#endif // RRV_GIF_GUARD_H
