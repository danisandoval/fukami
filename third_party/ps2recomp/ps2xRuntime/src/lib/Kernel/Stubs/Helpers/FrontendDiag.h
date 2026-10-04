#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <array>
#include <vector>
#include <algorithm>

namespace rrv_fediag
{
    // RRV_FRONTEND_DIAG: env-gated front-end/IO instrumentation.
    // Once per second, logs counts of scePad*, sceCd*, and sceSif RPC calls,
    // plus the first 8 sceCdRead tuples and EE RAM word watchers.
    // Zero cost when RRV_FRONTEND_DIAG is unset.
    
    // CD read tuple for the ring buffer
    struct CdReadTuple
    {
        uint32_t lbn = 0;
        uint32_t sectors = 0;
        uint32_t buf = 0;
    };

    // Per-second watcher for EE RAM words
    struct AddressWatcher
    {
        uint32_t addr = 0;
        uint32_t lastValue = 0;
        bool hasLastValue = false;
    };

    struct FrontendDiag
    {
        bool enabled = false;
        std::mutex mtx;
        
        // Pad counters
        std::atomic<uint64_t> scePadRead{0};
        std::atomic<uint64_t> scePadGetState{0};
        std::atomic<uint64_t> scePadPortOpen{0};
        std::atomic<uint64_t> scePadInit{0};
        std::atomic<uint64_t> scePadOther{0};  // All other scePad* functions
        
        // CD counters
        std::atomic<uint64_t> sceCdRead{0};
        std::atomic<uint64_t> sceCdReadChain{0};
        std::atomic<uint64_t> sceCdCallback{0};
        std::atomic<uint64_t> cbDeliver{0};  // Count of callback deliveries
        std::atomic<uint64_t> sceCdOther{0};  // All other sceCd* functions
        
        // SIF RPC counters (key = function name hash)
        std::unordered_map<uint32_t, std::pair<std::string, uint64_t>> sifCounts;
        
        // CD read ring (first 8 of each second)
        std::array<CdReadTuple, 8> cdReadRing{};
        uint32_t cdReadRingIdx = 0;
        
        // EE address watchers (up to 8)
        std::array<AddressWatcher, 8> watchers{};
        uint32_t watcherCount = 0;
        
        std::chrono::steady_clock::time_point lastDump;
        uint32_t vblanksSinceDump = 0;
    };

    inline FrontendDiag g_frontendDiag;

    inline void frontendDiagInit()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            const char *v = std::getenv("RRV_FRONTEND_DIAG");
            g_frontendDiag.enabled = (v && v[0] && v[0] != '0');
            g_frontendDiag.lastDump = std::chrono::steady_clock::now();
            
            // Parse RRV_WATCH_ADDRS if enabled
            if (g_frontendDiag.enabled)
            {
                const char *watchAddrs = std::getenv("RRV_WATCH_ADDRS");
                if (watchAddrs)
                {
                    std::string addrsStr(watchAddrs);
                    size_t pos = 0;
                    while (pos < addrsStr.length() && g_frontendDiag.watcherCount < 8)
                    {
                        size_t commaPos = addrsStr.find(',', pos);
                        if (commaPos == std::string::npos)
                        {
                            commaPos = addrsStr.length();
                        }
                        
                        std::string hexStr = addrsStr.substr(pos, commaPos - pos);
                        uint32_t addr = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
                        g_frontendDiag.watchers[g_frontendDiag.watcherCount].addr = addr;
                        g_frontendDiag.watchers[g_frontendDiag.watcherCount].hasLastValue = false;
                        g_frontendDiag.watcherCount++;
                        
                        pos = commaPos + 1;
                    }
                }
            }
        });
    }

    inline void frontendDiagCountPad(const char *funcName)
    {
        if (!g_frontendDiag.enabled) return;
        
        if (strcmp(funcName, "scePadRead") == 0)
            g_frontendDiag.scePadRead.fetch_add(1, std::memory_order_relaxed);
        else if (strcmp(funcName, "scePadGetState") == 0)
            g_frontendDiag.scePadGetState.fetch_add(1, std::memory_order_relaxed);
        else if (strcmp(funcName, "scePadPortOpen") == 0)
            g_frontendDiag.scePadPortOpen.fetch_add(1, std::memory_order_relaxed);
        else if (strcmp(funcName, "scePadInit") == 0)
            g_frontendDiag.scePadInit.fetch_add(1, std::memory_order_relaxed);
        else
            g_frontendDiag.scePadOther.fetch_add(1, std::memory_order_relaxed);
    }

    inline void frontendDiagCountCd(const char *funcName)
    {
        if (!g_frontendDiag.enabled) return;
        
        if (strcmp(funcName, "sceCdRead") == 0)
            g_frontendDiag.sceCdRead.fetch_add(1, std::memory_order_relaxed);
        else if (strcmp(funcName, "sceCdReadChain") == 0)
            g_frontendDiag.sceCdReadChain.fetch_add(1, std::memory_order_relaxed);
        else if (strcmp(funcName, "sceCdCallback") == 0)
            g_frontendDiag.sceCdCallback.fetch_add(1, std::memory_order_relaxed);
        else
            g_frontendDiag.sceCdOther.fetch_add(1, std::memory_order_relaxed);
    }

    inline void frontendDiagAddCdRead(uint32_t lbn, uint32_t sectors, uint32_t buf)
    {
        if (!g_frontendDiag.enabled) return;
        
        std::lock_guard<std::mutex> lock(g_frontendDiag.mtx);
        if (g_frontendDiag.cdReadRingIdx < 8)
        {
            g_frontendDiag.cdReadRing[g_frontendDiag.cdReadRingIdx].lbn = lbn;
            g_frontendDiag.cdReadRing[g_frontendDiag.cdReadRingIdx].sectors = sectors;
            g_frontendDiag.cdReadRing[g_frontendDiag.cdReadRingIdx].buf = buf;
            g_frontendDiag.cdReadRingIdx++;
        }
    }

    inline void frontendDiagCountCdCallback()
    {
        if (!g_frontendDiag.enabled) return;
        g_frontendDiag.cbDeliver.fetch_add(1, std::memory_order_relaxed);
    }

    inline void frontendDiagCountSif(const char *funcName)
    {
        if (!g_frontendDiag.enabled) return;
        
        // Simple hash of function name
        uint32_t hash = 5381;
        for (const char *p = funcName; *p; ++p)
        {
            hash = ((hash << 5) + hash) + *p;
        }
        
        std::lock_guard<std::mutex> lock(g_frontendDiag.mtx);
        auto &entry = g_frontendDiag.sifCounts[hash];
        if (entry.first.empty())
        {
            entry.first = funcName;
            entry.second = 1;
        }
        else
        {
            entry.second++;
        }
    }

    inline void frontendDiagDumpLocked(uint8_t *rdram)
    {
        if (!g_frontendDiag.enabled) return;
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_frontendDiag.lastDump);
        if (elapsed.count() < 1)
        {
            return;
        }

        g_frontendDiag.lastDump = now;
        uint64_t elapsed_s = elapsed.count();

        // Main counters line
        fprintf(stderr, "[fediag] t=%llus pad: scePadRead=%llu scePadGetState=%llu | cd: sceCdRead=%llu cbDeliver=%llu | sif:",
                (unsigned long long)elapsed_s,
                (unsigned long long)g_frontendDiag.scePadRead.load(std::memory_order_relaxed),
                (unsigned long long)g_frontendDiag.scePadGetState.load(std::memory_order_relaxed),
                (unsigned long long)g_frontendDiag.sceCdRead.load(std::memory_order_relaxed),
                (unsigned long long)g_frontendDiag.cbDeliver.load(std::memory_order_relaxed));
        
        // SIF counters
        for (auto &[hash, p] : g_frontendDiag.sifCounts)
        {
            fprintf(stderr, " %s=%llu", p.first.c_str(), (unsigned long long)p.second);
        }
        fprintf(stderr, "\n");

        // CD reads ring
        if (g_frontendDiag.cdReadRingIdx > 0)
        {
            fprintf(stderr, "[fediag]   cdreads:");
            for (uint32_t i = 0; i < g_frontendDiag.cdReadRingIdx; ++i)
            {
                const auto &t = g_frontendDiag.cdReadRing[i];
                fprintf(stderr, " (lbn=0x%x,n=%u,buf=0x%x)",
                        t.lbn, t.sectors, t.buf);
            }
            fprintf(stderr, "\n");
        }

        // Address watchers
        if (g_frontendDiag.watcherCount > 0 && rdram)
        {
            fprintf(stderr, "[fediag]   watch:");
            for (uint32_t i = 0; i < g_frontendDiag.watcherCount; ++i)
            {
                auto &w = g_frontendDiag.watchers[i];
                uint32_t offset = w.addr & PS2_RAM_MASK;
                if (offset + sizeof(uint32_t) <= PS2_RAM_SIZE)
                {
                    uint32_t val = *reinterpret_cast<const uint32_t*>(rdram + offset);
                    const char *marker = (w.hasLastValue && val != w.lastValue) ? "*" : "";
                    fprintf(stderr, " 0x%x=0x%08x%s", w.addr, val, marker);
                    w.lastValue = val;
                    w.hasLastValue = true;
                }
            }
            fprintf(stderr, "\n");
        }

        // Reset per-second counters
        g_frontendDiag.scePadRead.store(0, std::memory_order_relaxed);
        g_frontendDiag.scePadGetState.store(0, std::memory_order_relaxed);
        g_frontendDiag.scePadPortOpen.store(0, std::memory_order_relaxed);
        g_frontendDiag.scePadInit.store(0, std::memory_order_relaxed);
        g_frontendDiag.scePadOther.store(0, std::memory_order_relaxed);
        g_frontendDiag.sceCdRead.store(0, std::memory_order_relaxed);
        g_frontendDiag.sceCdReadChain.store(0, std::memory_order_relaxed);
        g_frontendDiag.sceCdCallback.store(0, std::memory_order_relaxed);
        g_frontendDiag.cbDeliver.store(0, std::memory_order_relaxed);
        g_frontendDiag.sceCdOther.store(0, std::memory_order_relaxed);
        g_frontendDiag.sifCounts.clear();
        g_frontendDiag.cdReadRingIdx = 0;
    }
}

using namespace rrv_fediag;
