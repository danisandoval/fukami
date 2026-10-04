#include "rrv_m2p_pad_observer.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rrv::m2ppad
{
namespace
{
constexpr uint64_t kCapacity = 4096u;

struct HostRecord
{
    uint64_t sampleId = 0;
    uint64_t eventOrdinal = 0;
    uint32_t eventType = 0;
    uint32_t keyboardButtons = 0;
    uint32_t controllerButtons = 0;
    uint32_t mergedButtons = 0;
    uint8_t selected = 0;
    uint8_t connected = 0;
};

struct PacketRecord
{
    uint64_t sampleId = 0;
    uint64_t pollOrdinal = 0;
    uint16_t activeLowButtons = 0xffffu;
    uint8_t byte2 = 0xffu;
    uint8_t byte3 = 0xffu;
    uint8_t overrideActive = 0;
};

std::array<HostRecord, kCapacity> g_host{};
std::array<PacketRecord, kCapacity> g_packet{};
std::atomic<uint64_t> g_hostWrites{0};
std::atomic<uint64_t> g_packetWrites{0};
const char *g_outputPath = nullptr;
bool g_enabled = false;
bool g_initialized = false;
bool g_finalized = false;

bool enabledValue(const char *value)
{
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void exportAfterRuntime()
{
    if (!g_enabled)
        return;
    // The SDL pad owner invokes this after PS2Runtime joins. Measured host and
    // guest paths write only fixed storage plus relaxed counters.
    FILE *stream = g_outputPath ? std::fopen(g_outputPath, "w") : stderr;
    if (!stream)
        return;
    const uint64_t hostWrites = g_hostWrites.load(std::memory_order_acquire);
    const uint64_t packetWrites = g_packetWrites.load(std::memory_order_acquire);
    std::fprintf(stream,
                 "{\"kind\":\"m2p_pad_observer\",\"schema\":1,\"host_writes\":%llu,"
                 "\"packet_writes\":%llu,\"capacity\":%llu}\n",
                 static_cast<unsigned long long>(hostWrites),
                 static_cast<unsigned long long>(packetWrites),
                 static_cast<unsigned long long>(kCapacity));
    const uint64_t hostFirst = hostWrites > kCapacity ? hostWrites - kCapacity : 0u;
    for (uint64_t ordinal = hostFirst; ordinal < hostWrites; ++ordinal)
    {
        const HostRecord &record = g_host[ordinal % kCapacity];
        std::fprintf(stream,
                     "{\"kind\":\"host\",\"sample_id\":%llu,\"event_ordinal\":%llu,"
                     "\"event_type\":%u,\"keyboard\":%u,\"controller\":%u,"
                     "\"selected\":%u,\"merged\":%u,\"connected\":%u}\n",
                     static_cast<unsigned long long>(record.sampleId),
                     static_cast<unsigned long long>(record.eventOrdinal), record.eventType,
                     record.keyboardButtons, record.controllerButtons, record.selected,
                     record.mergedButtons, record.connected);
    }
    const uint64_t packetFirst = packetWrites > kCapacity ? packetWrites - kCapacity : 0u;
    for (uint64_t ordinal = packetFirst; ordinal < packetWrites; ++ordinal)
    {
        const PacketRecord &record = g_packet[ordinal % kCapacity];
        std::fprintf(stream,
                     "{\"kind\":\"packet\",\"sample_id\":%llu,\"poll_ordinal\":%llu,"
                     "\"active_low\":%u,\"byte2\":%u,\"byte3\":%u,\"override\":%u}\n",
                     static_cast<unsigned long long>(record.sampleId),
                     static_cast<unsigned long long>(record.pollOrdinal), record.activeLowButtons,
                     record.byte2, record.byte3, record.overrideActive);
    }
    if (stream != stderr)
        std::fclose(stream);
}
} // namespace

void initializeFromEnvironment()
{
    if (g_initialized)
        return;
    g_initialized = true;
    g_enabled = enabledValue(std::getenv("RRV_M2P_PAD_OBSERVER"));
    g_outputPath = std::getenv("RRV_M2P_PAD_OBSERVER_PATH");
}

void finalize()
{
    if (!g_enabled || g_finalized)
        return;
    g_finalized = true;
    exportAfterRuntime();
}

void hostSample(uint64_t sampleId, uint64_t eventOrdinal, uint32_t eventType,
                uint32_t keyboardButtons, uint32_t controllerButtons,
                SelectedSource selected, uint32_t mergedButtons, bool connected)
{
    if (!g_enabled)
        return;
    const uint64_t ordinal = g_hostWrites.fetch_add(1u, std::memory_order_relaxed);
    g_host[ordinal % kCapacity] = {sampleId, eventOrdinal, eventType, keyboardButtons,
                                   controllerButtons, mergedButtons,
                                   static_cast<uint8_t>(selected), static_cast<uint8_t>(connected)};
}

void guestPacket(uint64_t sampleId, uint64_t pollOrdinal, uint16_t activeLowButtons,
                 uint8_t byte2, uint8_t byte3, bool overrideActive)
{
    if (!g_enabled)
        return;
    const uint64_t ordinal = g_packetWrites.fetch_add(1u, std::memory_order_relaxed);
    g_packet[ordinal % kCapacity] = {sampleId, pollOrdinal, activeLowButtons, byte2, byte3,
                                     static_cast<uint8_t>(overrideActive)};
}
} // namespace rrv::m2ppad
