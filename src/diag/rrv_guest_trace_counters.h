#ifndef RRV_GUEST_TRACE_COUNTERS_H
#define RRV_GUEST_TRACE_COUNTERS_H
#include <atomic>
#include <cstdint>
namespace rrv::guesttrace
{
inline std::atomic<uint64_t> g_gsSubmits{0};
inline std::atomic<uint64_t> g_gsBytes{0};
inline std::atomic<uint64_t> g_vsyncs{0};
inline void noteGsSubmit(uint32_t bytes)
{
    g_gsSubmits.fetch_add(1, std::memory_order_relaxed);
    g_gsBytes.fetch_add(bytes, std::memory_order_relaxed);
}
inline void noteVsync() { g_vsyncs.fetch_add(1, std::memory_order_relaxed); }
}
#endif
