#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#define TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
struct FakeBridge
{
    uint64_t csr = 0x1234u;
    uint64_t siglblid = 0x5678u;
    uint64_t sequence = 0u;
    uint64_t requestedCaptures = 0u;
    uint64_t completedCaptures = 0u;
    uint64_t readbacks = 0u;
    uint64_t directPresents = 0u;
    uint64_t unexpectedReadbacks = 0u;
    uint64_t cpuWaits = 0u;
    bool directMode = false;
    bool drawableAvailable = true;
    bool captureInProgress = false;
    bool blankStartup = false;
    bool receiptArmAfterVsyncPending = false;
    bool receiptArmed = false;
    uint64_t receiptConsumerAckOrdinal = 0u;
    struct ReceiptBoundaryOrdering
    {
        uint32_t structSize = 0u;
        uint32_t armScheduled = 0u;
        uint32_t armedAfterSuccessfulVsync = 0u;
        uint32_t firstPostArmConsumerEventType = 0u;
        uint64_t mixedVsyncConsumerAckOrdinal = 0u;
        uint64_t armConsumerAckOrdinal = 0u;
        uint64_t bridgeVsyncReturnConsumerAckOrdinal = 0u;
        uint64_t firstPostArmConsumerAckOrdinal = 0u;
        uint64_t firstPostArmTransferAckOrdinal = 0u;
    } receiptBoundaryOrdering{56u};
    std::vector<uint8_t> localMemory = std::vector<uint8_t>(4u * 1024u * 1024u, 0x3cu);
};

struct Surface
{
    uint32_t structSize;
    uint32_t kind;
    uint32_t flags;
    uint32_t reserved;
    void *view;
    void *layer;
    uint32_t width;
    uint32_t height;
    float scale;
    uint32_t reserved2;
};

struct Config
{
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    uint32_t threads;
    uint32_t renderMode;
    uint32_t presentationMode;
    uint32_t rendererKind;
    uint32_t reserved;
    Surface surface;
};

uint64_t g_configDigest = 0u;
uint64_t g_eventDigest = 0u;
uint32_t g_lastRenderMode = 0xffffffffu;
uint64_t g_lastReceiptMixedVsyncAck = 0u;
uint64_t g_lastReceiptArmAck = 0u;
uint64_t g_lastReceiptReturnAck = 0u;
uint64_t g_lastReceiptFirstPostEventAck = 0u;
uint64_t g_lastReceiptFirstPostTransferAck = 0u;
bool g_ownerEnabled = false;
std::thread::id g_mainThread, g_ownerThread;
std::atomic<uint64_t> g_ownerViolations{0}, g_ownerOperations{0}, g_mainPumps{0};
std::atomic<bool> g_lifecyclePending{false};

void noteOwner(unsigned operation)
{
    if (!g_ownerEnabled) return;
    g_ownerOperations.fetch_or(uint64_t{1} << operation);
    if (std::this_thread::get_id() != g_ownerThread || g_ownerThread == g_mainThread)
        ++g_ownerViolations;
}

void mainLifecycleCallback()
{
    if (!g_ownerEnabled) return;
    g_lifecyclePending = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_lifecyclePending)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        { ++g_ownerViolations; break; }
        std::this_thread::yield();
    }
}

uint64_t hashBytes(uint64_t hash, const void *bytes, size_t size)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

void resetDigests(const Config &config)
{
    g_configDigest = hashBytes(1469598103934665603ull, &config, sizeof(config));
    g_eventDigest = 1469598103934665603ull;
}

void noteEvent(uint8_t tag, const void *bytes, size_t size)
{
    g_eventDigest = hashBytes(g_eventDigest, &tag, sizeof(tag));
    g_eventDigest = hashBytes(g_eventDigest, bytes, size);
}

struct Capabilities
{
    uint32_t structSize;
    uint32_t supportedRenderModes;
    uint32_t rendererKind;
    uint32_t capabilityFlags;
    const char *rendererName;
};

struct Frame
{
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const uint8_t *rgba;
    uint64_t sequence;
};

struct CaptureRequest
{
    uint32_t structSize;
    uint32_t flags;
    uint64_t guestTick;
    uint64_t fieldIndex;
    uint64_t presentIndex;
};

struct CaptureResult
{
    uint32_t structSize;
    uint32_t flags;
    uint64_t guestTick;
    uint64_t fieldIndex;
    uint64_t presentIndex;
    Frame frame;
};

struct Stats
{
    uint32_t structSize;
    uint32_t reserved;
    uint64_t presentedFrames;
    uint64_t directGpuPresents;
    uint64_t requestedCaptures;
    uint64_t completedCaptures;
    uint64_t synchronousCpuReadbacks;
    uint64_t unexpectedReadbacks;
    uint64_t cpuWaits;
};

uint8_t pixels[24] = {
    1, 2, 3, 4, 5, 6, 7, 8, 0xee, 0xee, 0xee, 0xee,
    9, 10, 11, 12, 13, 14, 15, 16, 0xdd, 0xdd, 0xdd, 0xdd,
};
uint8_t blankPixels[16] = {};

void errorText(char *error, uint32_t capacity, const char *text)
{
    if (!error || capacity == 0u)
        return;
    std::strncpy(error, text, capacity - 1u);
    error[capacity - 1u] = '\0';
}
} // namespace

TEST_EXPORT uint32_t rrv_pcsx2_gs_bridge_version()
{
    return 5u;
}

// Optional dedicated-owner sideband. The fake checks the same split as Metal:
// preparation/pumping on the surface thread, every GS operation on one owner.
TEST_EXPORT int rrv_pcsx2_gs_bridge_prepare_owner_surface(Surface* surface, char*, uint32_t)
{
    if (!surface || (surface->flags & 3u) != 3u) return 0;
    g_mainThread = std::this_thread::get_id();
    g_ownerThread = {};
    g_ownerEnabled = true;
    g_ownerViolations = g_ownerOperations = g_mainPumps = 0;
    g_lifecyclePending = false;
    surface->flags |= 4u;
    return 1;
}

TEST_EXPORT void rrv_pcsx2_gs_bridge_owner_thread_init()
{
    g_ownerThread = std::this_thread::get_id();
    noteOwner(0);
}

TEST_EXPORT void rrv_pcsx2_gs_bridge_owner_command(void (*command)(void*), void* context)
{
    command(context);
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_pump_main_thread()
{
    if (std::this_thread::get_id() != g_mainThread) { ++g_ownerViolations; return 0; }
    if (g_lifecyclePending.exchange(false)) ++g_mainPumps;
    std::this_thread::yield();
    return 1;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_owner_violations() { return g_ownerViolations; }
TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_owner_operations() { return g_ownerOperations; }
TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_main_pumps() { return g_mainPumps; }

TEST_EXPORT void *rrv_pcsx2_gs_bridge_create(const Config *config, Capabilities *capabilities,
                                               char *error,
                                               uint32_t errorCapacity)
{
    g_ownerEnabled = config && (config->surface.flags & 4u) != 0;
    noteOwner(1);
    mainLifecycleCallback();
    if (!capabilities || capabilities->structSize < sizeof(Capabilities))
    {
        errorText(error, errorCapacity, "bad capabilities");
        return nullptr;
    }
    const bool fullEnabled = [] {
        const char *value = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_FULL");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool directEnabled = [] {
        const char *value = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_DIRECT");
        return value && std::strcmp(value, "1") == 0;
    }();
    capabilities->supportedRenderModes = 1u | (fullEnabled ? 2u : 0u);
    capabilities->rendererKind = config ? config->rendererKind : 0u;
    capabilities->capabilityFlags = (1u << 3) | (1u << 4) |
        (directEnabled ? (1u << 2) : 0u);
    capabilities->rendererName = "Fake PCSX2";
    if (!config || config->structSize < sizeof(Config))
    {
        errorText(error, errorCapacity, "bad config");
        return nullptr;
    }
    if (config->renderMode > 1u || (capabilities->supportedRenderModes & (1u << config->renderMode)) == 0u)
    {
        errorText(error, errorCapacity,
                  config->renderMode == 1u
                      ? "RRV_GS_RENDER_MODE=full is not implemented by Fake PCSX2 backend"
                      : "unsupported fake GS render mode");
        return nullptr;
    }
    if (config->presentationMode == 1u && (config->surface.kind != 1u ||
        !config->surface.view || !config->surface.layer || config->surface.width == 0u ||
        config->surface.height == 0u || config->surface.scale <= 0.0f))
    {
        errorText(error, errorCapacity, "fake direct surface is unsupported or invalid");
        return nullptr;
    }
    resetDigests(*config);
    g_lastRenderMode = config->renderMode;
    auto *bridge = new FakeBridge{};
    bridge->directMode = config->presentationMode == 1u;
    const char *drawable = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_DRAWABLE");
    bridge->drawableAvailable = !drawable || std::strcmp(drawable, "0") != 0;
    const char *blank_startup = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_BLANK_STARTUP");
    bridge->blankStartup = blank_startup && std::strcmp(blank_startup, "1") == 0;
    return bridge;
}

// Test-only exports: this fake dylib is never the production bridge ABI. The
// backend lifecycle test compares these digests after two field selections to
// prove their forwarded bridge configuration and command sequence are exact.
TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_config_digest()
{
    return g_configDigest;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_event_digest()
{
    return g_eventDigest;
}

TEST_EXPORT uint32_t rrv_test_fake_pcsx2_gs_bridge_render_mode()
{
    return g_lastRenderMode;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_receipt_mixed_vsync_ack()
{
    return g_lastReceiptMixedVsyncAck;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_receipt_arm_ack()
{
    return g_lastReceiptArmAck;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_receipt_return_ack()
{
    return g_lastReceiptReturnAck;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_receipt_first_post_event_ack()
{
    return g_lastReceiptFirstPostEventAck;
}

TEST_EXPORT uint64_t rrv_test_fake_pcsx2_gs_bridge_receipt_first_post_transfer_ack()
{
    return g_lastReceiptFirstPostTransferAck;
}

TEST_EXPORT void rrv_pcsx2_gs_bridge_destroy(void *bridge)
{
    noteOwner(2);
    mainLifecycleCallback();
    delete static_cast<FakeBridge *>(bridge);
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_reset_gs(void *bridge,
                                         char *error, uint32_t errorCapacity)
{
    noteOwner(14);
    if (!bridge)
    {
        errorText(error, errorCapacity, "reset requires bridge");
        return 0;
    }
    if (const char *fail = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_FAIL_RESET");
        fail && std::strcmp(fail, "1") == 0)
    {
        errorText(error, errorCapacity, "forced reset failure");
        return 0;
    }
    // Renderer reset does not copy or clear the privileged snapshot or VRAM.
    noteEvent(14u, nullptr, 0);
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_submit(void *bridge, uint32_t path,
                                           const uint8_t *data, uint32_t bytes,
                                           char *error, uint32_t errorCapacity)
{
    noteOwner(3);
    const char *fail = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SUBMIT");
    if (fail && std::strcmp(fail, "1") == 0)
    {
        errorText(error, errorCapacity, "forced submit failure");
        return 0;
    }
    if (!bridge || path < 1u || path > 3u || (bytes & 15u) != 0u)
    {
        errorText(error, errorCapacity, "bad packet");
        return 0;
    }
    noteEvent(1u, &path, sizeof(path));
    noteEvent(2u, &bytes, sizeof(bytes));
    if (bytes != 0u)
        noteEvent(3u, data, bytes);
    auto *fake = static_cast<FakeBridge *>(bridge);
    if (bytes != 0u && fake->receiptArmed)
    {
        ++fake->receiptConsumerAckOrdinal;
        auto &ordering = fake->receiptBoundaryOrdering;
        if (ordering.firstPostArmConsumerAckOrdinal == 0u)
        {
            ordering.firstPostArmConsumerAckOrdinal = fake->receiptConsumerAckOrdinal;
            ordering.firstPostArmConsumerEventType = 1u;
        }
        if (ordering.firstPostArmTransferAckOrdinal == 0u)
            ordering.firstPostArmTransferAckOrdinal = fake->receiptConsumerAckOrdinal;
        g_lastReceiptFirstPostEventAck = ordering.firstPostArmConsumerAckOrdinal;
        g_lastReceiptFirstPostTransferAck = ordering.firstPostArmTransferAckOrdinal;
    }
    return 1;
}

// Optional M2 diagnostic exports. They are no-ops in the ABI fake: the
// canonical collector itself has a separate portable contract test.
TEST_EXPORT int rrv_pcsx2_gs_bridge_receipt_arm(void *bridge, char *, uint32_t)
{
    noteOwner(13);
    return bridge ? 1 : 0;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_receipt_arm_after_vsync(void *bridge, char *, uint32_t)
{
    noteOwner(13);
    if (!bridge)
        return 0;
    auto *fake = static_cast<FakeBridge *>(bridge);
    if (!fake->receiptArmAfterVsyncPending && !fake->receiptArmed)
    {
        fake->receiptBoundaryOrdering = {56u};
        fake->receiptBoundaryOrdering.armScheduled = 1u;
        fake->receiptConsumerAckOrdinal = 0u;
        fake->receiptArmAfterVsyncPending = true;
    }
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_receipt_boundary_ordering(
    void *bridge, void *output, char *, uint32_t)
{
    noteOwner(13);
    if (!bridge || !output)
        return 0;
    *static_cast<FakeBridge::ReceiptBoundaryOrdering *>(output) =
        static_cast<FakeBridge *>(bridge)->receiptBoundaryOrdering;
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_receipt_set_epoch_identity(
    void *bridge, uint64_t, uint64_t, char *, uint32_t)
{
    noteOwner(13);
    return bridge ? 1 : 0;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_readback(void *bridge, uint64_t *csr,
                                             uint64_t *siglblid, char *, uint32_t)
{
    noteOwner(4);
    if (!bridge || !csr || !siglblid)
        return 0;
    const auto *fake = static_cast<FakeBridge *>(bridge);
    *csr = fake->csr;
    *siglblid = fake->siglblid;
    noteEvent(4u, csr, sizeof(*csr));
    noteEvent(5u, siglblid, sizeof(*siglblid));
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_vsync(void *bridge, const uint64_t *regs,
                                          uint64_t field, uint32_t parity,
                                          char *error, uint32_t errorCapacity)
{
    noteOwner(5);
    const char *fail = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC");
    if (fail && std::strcmp(fail, "1") == 0)
    {
        errorText(error, errorCapacity, "forced vsync failure");
        return 0;
    }
    if (!bridge || !regs || parity > 1u)
        return 0;
    auto *fake = static_cast<FakeBridge *>(bridge);
    fake->csr = regs[15];
    fake->siglblid = regs[18];
    fake->sequence = field;
    if (fake->directMode && fake->drawableAvailable)
        ++fake->directPresents;
    noteEvent(6u, regs, 19u * sizeof(uint64_t));
    noteEvent(7u, &field, sizeof(field));
    noteEvent(8u, &parity, sizeof(parity));
    if (fake->receiptArmAfterVsyncPending)
    {
        ++fake->receiptConsumerAckOrdinal;
        auto &ordering = fake->receiptBoundaryOrdering;
        ordering.mixedVsyncConsumerAckOrdinal = fake->receiptConsumerAckOrdinal;
        ordering.armConsumerAckOrdinal = fake->receiptConsumerAckOrdinal;
        fake->receiptArmAfterVsyncPending = false;
        fake->receiptArmed = true;
        ordering.armedAfterSuccessfulVsync = 1u;
        ordering.bridgeVsyncReturnConsumerAckOrdinal = fake->receiptConsumerAckOrdinal;
        g_lastReceiptMixedVsyncAck = ordering.mixedVsyncConsumerAckOrdinal;
        g_lastReceiptArmAck = ordering.armConsumerAckOrdinal;
        g_lastReceiptReturnAck = ordering.bridgeVsyncReturnConsumerAckOrdinal;
    }
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_snapshot(void *bridge, Frame *frame,
                                             char *, uint32_t)
{
    noteOwner(6);
    if (!bridge || !frame || frame->structSize < sizeof(Frame))
        return 0;
    auto *fake = static_cast<FakeBridge *>(bridge);
    // Mirrors the real bridge's deterministic pre-display blank: it returns
    // CPU-owned zeros without invoking a GPU download or waiting for it.
    if (fake->blankStartup && fake->sequence == 0u)
    {
        frame->width = 2u;
        frame->height = 2u;
        frame->stride = 8u;
        frame->rgba = blankPixels;
        frame->sequence = 0u;
        return 1;
    }
    frame->width = 2u;
    frame->height = 2u;
    frame->stride = 12u;
    frame->rgba = pixels;
    frame->sequence = fake->sequence;
    ++fake->readbacks;
    ++fake->cpuWaits;
    if (fake->directMode && !fake->captureInProgress)
        ++fake->unexpectedReadbacks;
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_resize(void *bridge, uint32_t width, uint32_t height,
                                           float scale, char *error, uint32_t errorCapacity)
{
    noteOwner(7);
    if (!bridge || width == 0u || height == 0u || scale <= 0.0f)
    {
        errorText(error, errorCapacity, "bad direct resize");
        return 0;
    }
    noteEvent(9u, &width, sizeof(width));
    noteEvent(10u, &height, sizeof(height));
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_capture(void *bridge, const CaptureRequest *request,
                                            CaptureResult *result, char *error,
                                            uint32_t errorCapacity)
{
    noteOwner(8);
    if (!bridge || !request || !result || request->structSize < sizeof(CaptureRequest) ||
        result->structSize < sizeof(CaptureResult))
    {
        errorText(error, errorCapacity, "bad capture");
        return 0;
    }
    auto *fake = static_cast<FakeBridge *>(bridge);
    ++fake->requestedCaptures;
    const uint64_t observedPresent = fake->directMode ? fake->directPresents : fake->sequence;
    if (request->fieldIndex != fake->sequence || request->presentIndex != observedPresent)
    {
        errorText(error, errorCapacity, "stale capture tag");
        return 0;
    }
    Frame frame{sizeof(Frame), 0u, 0u, 0u, nullptr, 0u};
    fake->captureInProgress = true;
    if (!rrv_pcsx2_gs_bridge_snapshot(bridge, &frame, error, errorCapacity))
    {
        fake->captureInProgress = false;
        return 0;
    }
    fake->captureInProgress = false;
    result->flags = 0u;
    result->guestTick = request->guestTick;
    result->fieldIndex = request->fieldIndex;
    result->presentIndex = request->presentIndex;
    result->frame = frame;
    ++fake->completedCaptures;
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_get_stats(void *bridge, Stats *stats, char *error,
                                              uint32_t errorCapacity)
{
    noteOwner(9);
    auto *fake = static_cast<FakeBridge *>(bridge);
    if (!fake || !stats || stats->structSize < sizeof(Stats))
    {
        errorText(error, errorCapacity, "bad stats");
        return 0;
    }
    stats->presentedFrames = fake->directMode ? fake->directPresents : fake->sequence;
    stats->directGpuPresents = fake->directPresents;
    stats->requestedCaptures = fake->requestedCaptures;
    stats->completedCaptures = fake->completedCaptures;
    stats->synchronousCpuReadbacks = fake->readbacks;
    stats->unexpectedReadbacks = fake->unexpectedReadbacks;
    stats->cpuWaits = fake->cpuWaits;
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_read_local_memory(
    void *bridge, uint8_t *destination, uint32_t bytes, uint64_t, uint64_t,
    uint64_t, char *, uint32_t)
{
    noteOwner(10);
    if (!bridge || bytes == 0u || !destination)
        return 0;
    for (uint32_t index = 0u; index < bytes; ++index)
        destination[index] = static_cast<uint8_t>(index ^ 0x5au);
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_snapshot_local_memory(
    void *bridge, uint8_t *destination, uint32_t bytes, char *, uint32_t)
{
    noteOwner(11);
    auto *fake = static_cast<FakeBridge *>(bridge);
    if (!fake || !destination || bytes != fake->localMemory.size())
        return 0;
    if (const char *fail = std::getenv("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SNAPSHOT");
        fail && std::strcmp(fail, "1") == 0)
    {
        // Deliberately prove that a bridge failure may dirty its supplied
        // temporary destination.  Backend owns the caller replacement rule.
        std::memset(destination, 0xd3, std::min<size_t>(bytes, 64u));
        return 0;
    }
    std::memcpy(destination, fake->localMemory.data(), bytes);
    return 1;
}

TEST_EXPORT int rrv_pcsx2_gs_bridge_restore_local_memory(
    void *bridge, const uint8_t *source, uint32_t bytes, char *, uint32_t)
{
    noteOwner(12);
    auto *fake = static_cast<FakeBridge *>(bridge);
    if (!fake || !source || bytes != fake->localMemory.size())
        return 0;
    std::memcpy(fake->localMemory.data(), source, bytes);
    return 1;
}
