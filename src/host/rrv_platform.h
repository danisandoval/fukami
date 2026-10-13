#pragma once
// The few operating-system calls shared by several RRV modules, behind one spelling per platform. Anything used by one
// module only (module enumeration for the bridge overlay, the resource package's file handling) stays in that module.

#include <cstdint>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace rrv::platform
{

// A monotonic clock in nanoseconds that keeps counting through system sleep where the OS allows it
// (CLOCK_MONOTONIC on macOS and Linux; QueryPerformanceCounter on Windows). Unrelated to the wall clock.
inline uint64_t MonotonicNs()
{
#if defined(_WIN32)
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    // Split the division so a long uptime cannot overflow 64 bits before the scale to nanoseconds.
    return (ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

// Flush a file descriptor's data to stable storage. Returns 0 on success, -1 on failure (errno set).
inline int FsyncFd(int fd)
{
#if defined(_WIN32)
    return _commit(fd);
#else
    return ::fsync(fd);
#endif
}

} // namespace rrv::platform
