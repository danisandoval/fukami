#include "rrv_thread_cpu_log.h"

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Owner-thread split (Gate 5 Deck speed work). The GS backend and the VU1+GS
// owner stream report through these extern "C" hooks; they reference them
// weakly and only on Linux, so other platforms and test binaries are unchanged.
namespace
{
std::atomic<unsigned long long> g_ownerNs{0}, g_gsNs{0}, g_ownerCommands{0}, g_ownerDenormalCommands{0};
} // namespace

extern "C" void rrv_cpu_log_add_owner(unsigned long long ns, int denormal)
{
    g_ownerNs.fetch_add(ns, std::memory_order_relaxed);
    g_ownerCommands.fetch_add(1, std::memory_order_relaxed);
    if (denormal)
        g_ownerDenormalCommands.fetch_add(1, std::memory_order_relaxed);
}

// Split (RRV_VU1GS_SPLIT): owner time is rrv-vu1 alone; GS is not nested in it.
namespace { std::atomic<bool> g_split{false}; }
extern "C" void rrv_cpu_log_set_split(int split)
{
    g_split.store(split != 0, std::memory_order_relaxed);
}

extern "C" void rrv_cpu_log_add_gs(unsigned long long ns)
{
    g_gsNs.fetch_add(ns, std::memory_order_relaxed);
}

namespace rrv::host
{
namespace
{
struct Sample
{
    std::string name;
    unsigned long long ticks = 0; // utime + stime, in clock ticks
};

std::map<int, Sample> readThreads()
{
    std::map<int, Sample> out;
    DIR *dir = opendir("/proc/self/task");
    if (!dir)
        return out;
    while (dirent *entry = readdir(dir))
    {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0)
            continue;
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE *file = std::fopen(path, "r");
        if (!file)
            continue;
        char line[1024];
        const bool ok = std::fgets(line, sizeof line, file) != nullptr;
        std::fclose(file);
        if (!ok)
            continue;
        // "tid (comm) state ..." - comm may contain spaces, so split at the
        // last ')'. utime and stime are fields 14 and 15.
        const char *open = std::strchr(line, '(');
        const char *close = std::strrchr(line, ')');
        if (!open || !close || close < open)
            continue;
        Sample sample;
        sample.name.assign(open + 1, close);
        unsigned long long utime = 0, stime = 0;
        if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2)
            continue;
        sample.ticks = utime + stime;
        out[tid] = sample;
    }
    closedir(dir);
    return out;
}

// Clocks (Steam Deck: CPU and GPU share one power budget, so heavy GPU work
// at a high internal scale can lower the CPU clock under the GS owner). Reads
// sysfs; a missing file reads as 0 and its column is left out.
long readLong(const char *path)
{
    FILE *file = std::fopen(path, "r");
    if (!file)
        return 0;
    long value = 0;
    if (std::fscanf(file, "%ld", &value) != 1)
        value = 0;
    std::fclose(file);
    return value;
}

// Highest current clock over all CPUs, in MHz (scaling_cur_freq is in kHz).
long cpuMhz()
{
    long best = 0;
    for (int cpu = 0; cpu < 256; ++cpu)
    {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        const long khz = readLong(path);
        if (khz <= 0 && cpu > 0)
            break;
        best = std::max(best, khz / 1000);
    }
    return best;
}

// The first amdgpu card's current shader clock ("1: 1600Mhz *" in pp_dpm_sclk)
// and busy percentage.
void gpuState(long &mhz, long &busy)
{
    mhz = busy = -1;
    for (int card = 0; card < 4; ++card)
    {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/class/drm/card%d/device/pp_dpm_sclk", card);
        FILE *file = std::fopen(path, "r");
        if (!file)
            continue;
        char line[128];
        while (std::fgets(line, sizeof line, file))
        {
            if (!std::strchr(line, '*'))
                continue;
            const char *colon = std::strchr(line, ':');
            mhz = colon ? std::strtol(colon + 1, nullptr, 10) : -1;
            break;
        }
        std::fclose(file);
        std::snprintf(path, sizeof path, "/sys/class/drm/card%d/device/gpu_busy_percent", card);
        FILE *busyFile = std::fopen(path, "r");
        if (busyFile)
        {
            if (std::fscanf(busyFile, "%ld", &busy) != 1)
                busy = -1;
            std::fclose(busyFile);
        }
        return;
    }
}

void samplerLoop()
{
    nameCurrentThread("rrv-cpu-log");
    const double ticksPerSecond = static_cast<double>(sysconf(_SC_CLK_TCK));
    constexpr auto kInterval = std::chrono::seconds(2);
    auto previous = readThreads();
    auto previousTime = std::chrono::steady_clock::now();
    const auto start = previousTime;
    for (;;)
    {
        std::this_thread::sleep_for(kInterval);
        auto current = readThreads();
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - previousTime).count();
        std::vector<std::pair<double, std::string>> busy;
        double total = 0.0;
        for (const auto &[tid, sample] : current)
        {
            const auto before = previous.find(tid);
            const unsigned long long base = before != previous.end() ? before->second.ticks : 0ull;
            const double percent = 100.0 * static_cast<double>(sample.ticks - std::min(base, sample.ticks)) /
                                   ticksPerSecond / seconds;
            total += percent;
            if (percent >= 5.0)
                busy.emplace_back(percent, sample.name);
        }
        std::sort(busy.begin(), busy.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::string line = "[cpu] t=" +
                           std::to_string(static_cast<long>(std::chrono::duration<double>(now - start).count())) + "s";
        for (const auto &[percent, name] : busy)
            line += " " + name + "=" + std::to_string(static_cast<int>(percent + 0.5)) + "%";
        line += " total=" + std::to_string(static_cast<int>(total + 0.5)) + "%";
        // Instantaneous clocks at the sample, not averages over the interval.
        if (const long mhz = cpuMhz(); mhz > 0)
            line += " cpu-mhz=" + std::to_string(mhz);
        long gpuMhz = -1, gpuBusy = -1;
        gpuState(gpuMhz, gpuBusy);
        if (gpuMhz >= 0)
            line += " gpu-mhz=" + std::to_string(gpuMhz);
        if (gpuBusy >= 0)
            line += " gpu-busy=" + std::to_string(gpuBusy) + "%";
        // Owner split: wall time inside owner commands, the GS part of it
        // (Backend::submit/vsync on any thread), and how many owner commands
        // raised the x86 denormal-operand flag (0 on other ISAs).
        const unsigned long long ownerNs = g_ownerNs.exchange(0), gsNs = g_gsNs.exchange(0);
        const unsigned long long commands = g_ownerCommands.exchange(0), denormal = g_ownerDenormalCommands.exchange(0);
        if (commands)
        {
            const auto pct = [&](unsigned long long ns) { return std::to_string(static_cast<int>(100.0 * ns / 1e9 / seconds + 0.5)); };
            line += " owner=" + pct(ownerNs) + "% gs=" + pct(gsNs) + "% vu1+vif=" + pct(g_split.load(std::memory_order_relaxed) ? ownerNs : ownerNs - std::min(ownerNs, gsNs)) +
                    "% denormal-cmds=" + std::to_string(static_cast<int>(100.0 * denormal / commands + 0.5)) + "%";
        }
        line += "\n";
        std::fputs(line.c_str(), stderr);
        previous = std::move(current);
        previousTime = now;
    }
}
} // namespace

void startThreadCpuLog()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;
    std::thread(samplerLoop).detach();
}

void nameCurrentThread(const char *name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
}
} // namespace rrv::host

#else

namespace rrv::host
{
void startThreadCpuLog() {}
void nameCurrentThread(const char *) {}
} // namespace rrv::host

#endif
