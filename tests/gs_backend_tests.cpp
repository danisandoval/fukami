#include "rrv_gs_backend.h"
#include "rrv_gs_record_format.h"
#include "rrv_gs_record_hooks.h"
#include "rrv_gs_trace.h"
#include "rrv_gs_trace_hooks.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef RRV_TEST_FAKE_GS_BRIDGE
#error RRV_TEST_FAKE_GS_BRIDGE must name the test bridge
#endif

namespace
{
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void setEnvironment(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

template <typename Header>
bool readHeader(const char *path, Header &header)
{
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    const bool ok = std::fread(&header, sizeof(header), 1u, file) == 1u;
    std::fclose(file);
    return ok;
}

using DigestFn = uint64_t (*)();
using RenderModeFn = uint32_t (*)();

// Keep this private fixture declaration local to the asset-free ABI test.  It
// is layout-compatible with the public ABI but does not make the backend link
// against the production bridge header or implementation.
struct FakeBridgeSurface
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
struct FakeBridgeConfig
{
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    uint32_t threads;
    uint32_t renderMode;
    uint32_t presentationMode;
    uint32_t rendererKind;
    uint32_t reserved;
    FakeBridgeSurface surface;
};
struct FakeBridgeCapabilities
{
    uint32_t structSize;
    uint32_t supportedRenderModes;
    uint32_t rendererKind;
    uint32_t capabilityFlags;
    const char *rendererName;
};
using FakeBridgeCreateFn = void *(*)(const FakeBridgeConfig *, FakeBridgeCapabilities *, char *, uint32_t);
using FakeBridgeDestroyFn = void (*)(void *);
using FakeBridgeReadLocalMemoryFn = int (*)(void *, uint8_t *, uint32_t, uint64_t,
                                            uint64_t, uint64_t, char *, uint32_t);

DigestFn fakeDigestFunction(const char *name)
{
#if defined(_WIN32)
    HMODULE module = LoadLibraryA(RRV_TEST_FAKE_GS_BRIDGE);
    return module ? reinterpret_cast<DigestFn>(GetProcAddress(module, name)) : nullptr;
#else
    void *module = dlopen(RRV_TEST_FAKE_GS_BRIDGE, RTLD_NOW | RTLD_LOCAL);
    return module ? reinterpret_cast<DigestFn>(dlsym(module, name)) : nullptr;
#endif
}

void testExportedReadLocalMemoryValidation()
{
#if defined(_WIN32)
    HMODULE module = LoadLibraryA(RRV_TEST_FAKE_GS_BRIDGE);
    const auto create = module ? reinterpret_cast<FakeBridgeCreateFn>(
        GetProcAddress(module, "rrv_pcsx2_gs_bridge_create")) : nullptr;
    const auto destroy = module ? reinterpret_cast<FakeBridgeDestroyFn>(
        GetProcAddress(module, "rrv_pcsx2_gs_bridge_destroy")) : nullptr;
    const auto read = module ? reinterpret_cast<FakeBridgeReadLocalMemoryFn>(
        GetProcAddress(module, "rrv_pcsx2_gs_bridge_read_local_memory")) : nullptr;
#else
    void *module = dlopen(RRV_TEST_FAKE_GS_BRIDGE, RTLD_NOW | RTLD_LOCAL);
    const auto create = module ? reinterpret_cast<FakeBridgeCreateFn>(
        dlsym(module, "rrv_pcsx2_gs_bridge_create")) : nullptr;
    const auto destroy = module ? reinterpret_cast<FakeBridgeDestroyFn>(
        dlsym(module, "rrv_pcsx2_gs_bridge_destroy")) : nullptr;
    const auto read = module ? reinterpret_cast<FakeBridgeReadLocalMemoryFn>(
        dlsym(module, "rrv_pcsx2_gs_bridge_read_local_memory")) : nullptr;
#endif
    check(create && destroy && read, "fake bridge exports local-memory ABI entry points");
    if (!create || !destroy || !read) return;

    FakeBridgeConfig config{};
    config.structSize = sizeof(config);
    config.width = 640u;
    config.height = 448u;
    FakeBridgeCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    std::array<char, 128> error{};
    void *bridge = create(&config, &capabilities, error.data(), static_cast<uint32_t>(error.size()));
    check(bridge != nullptr, "fake bridge creates for exported read validation");
    if (!bridge) return;

    std::array<uint8_t, 16> destination{};
    destination.fill(0xa5u);
    check(read(bridge, destination.data(), 0u, 0u, 0u, 0u, error.data(),
               static_cast<uint32_t>(error.size())) == 0 &&
              std::all_of(destination.begin(), destination.end(), [](uint8_t value) { return value == 0xa5u; }),
          "exported bridge entry rejects zero-byte reads without touching the destination");
    check(read(bridge, nullptr, 16u, 0u, 0u, 0u, error.data(),
               static_cast<uint32_t>(error.size())) == 0,
          "exported bridge entry rejects non-empty reads with a null destination");
    destroy(bridge);
}

struct BridgeDigests
{
    uint64_t config = 0u;
    uint64_t events = 0u;
};

struct ExpectedSubmit
{
    uint8_t pathId;
    const uint8_t *bytes;
    uint32_t sizeBytes;
};

uint64_t fnv1a(uint64_t hash, const uint8_t *bytes, size_t size)
{
    for (size_t index = 0u; index != size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t expectedEpochHash(std::initializer_list<ExpectedSubmit> submits)
{
    static constexpr uint8_t domain[] = {
        'R', 'R', 'V', '-', 'G', 'S', '-', 'E', 'P', 'O', 'C', 'H', '-', 'V', '1'};
    uint64_t hash = fnv1a(14695981039346656037ull, domain, sizeof(domain));
    for (const ExpectedSubmit &submit : submits)
    {
        static constexpr uint8_t submitTag = 0x01u;
        hash = fnv1a(hash, &submitTag, sizeof(submitTag));
        hash = fnv1a(hash, &submit.pathId, sizeof(submit.pathId));
        const uint8_t size[4] = {
            static_cast<uint8_t>(submit.sizeBytes),
            static_cast<uint8_t>(submit.sizeBytes >> 8u),
            static_cast<uint8_t>(submit.sizeBytes >> 16u),
            static_cast<uint8_t>(submit.sizeBytes >> 24u),
        };
        hash = fnv1a(hash, size, sizeof(size));
        if (submit.sizeBytes != 0u)
            hash = fnv1a(hash, submit.bytes, submit.sizeBytes);
    }
    static constexpr uint8_t endEpochTag = 0xffu;
    hash = fnv1a(hash, &endEpochTag, sizeof(endEpochTag));
    return hash;
}

bool workloadHashMatches(const rrv::gsbackend::CompletedFieldEpoch &epoch,
                         uint64_t expected)
{
#if defined(RRV_GS_EPOCH_DIAGNOSTICS)
    return epoch.canonicalWorkloadHashAvailable && epoch.canonicalWorkloadHash == expected;
#else
    (void)expected;
    return !epoch.canonicalWorkloadHashAvailable && epoch.canonicalWorkloadHash == 0u;
#endif
}

bool runDeterministicFieldSequence(BridgeDigests &digests, std::string &error)
{
    rrv::gsbackend::Backend backend;
    if (!backend.initialize(640u, 448u, &error))
        return false;
    const uint8_t packet[16] = {0x72u, 0x52u, 0x56u, 0x46u};
    uint64_t regs[19]{};
    regs[15] = 0x2000u;
    regs[18] = 0xabcdefu;
    uint64_t csr = 0u;
    uint64_t siglblid = 0u;
    const bool ok = backend.submit(3u, packet, sizeof(packet), &error) &&
                    backend.vsync(regs, 41u, 1u, &error) &&
                    backend.readback(csr, siglblid, &error) &&
                    csr == regs[15] && siglblid == regs[18];
    if (const DigestFn config = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_config_digest"))
        digests.config = config();
    if (const DigestFn events = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_event_digest"))
        digests.events = events();
    backend.shutdown();
    return ok && digests.config != 0u && digests.events != 0u;
}

void testDedicatedOwner()
{
    const auto violations = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_owner_violations");
    const auto operations = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_owner_operations");
    const auto pumps = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_main_pumps");
    const auto digest = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_event_digest");
    check(violations && operations && pumps && digest, "fake owner instrumentation exports exist");
    if (!violations || !operations || !pumps || !digest) return;
    setEnvironment("RRV_GS_BACKEND", "pcsx2");
    setEnvironment("RRV_GS_RENDER_MODE", "field");
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", "1");
    uint64_t inlineDigest = 0;
    for (const char* mode : {"inline", "worker-sync"})
    {
        setEnvironment("RRV_GS_EXECUTION", mode);
        rrv::gsbackend::InitializeOptions options{};
        options.snapshotWidth = 640; options.snapshotHeight = 448;
        options.rendererKind = rrv::gsbackend::RendererKind::Metal;
        options.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
        options.surface.kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
        options.surface.nativeView = reinterpret_cast<void*>(uintptr_t{1});
        options.surface.nativeLayer = reinterpret_cast<void*>(uintptr_t{2});
        options.surface.widthPixels = 640; options.surface.heightPixels = 448;
        options.surface.backingScale = 2; options.surface.mainThreadPrepared = true;
        rrv::gsbackend::Backend backend;
        std::string error;
        const bool initialized = backend.initialize(options, &error);
        check(initialized, "owner comparison initializes native fake surface");
        if (!initialized) { std::cerr << error << '\n'; continue; }
        // Startup selection is fixed even if the environment changes later.
        setEnvironment("RRV_GS_EXECUTION", "invalid-live-change");
        std::array<uint8_t, 16> packet{0x71, 0x25, 0xa3};
        const auto originalPacket = packet;
        check(backend.submit(3, packet.data(), packet.size(), &error), "owner packet completes");
        packet.fill(0xff);
        std::array<uint64_t, 19> regs{};
        regs[0] = 3; regs[15] = 0x3000; regs[18] = 0x123456789abcdef0ull;
        const auto originalRegs = regs;
        // This is the other actual production entry class: a tick thread may
        // VSync while GameThread produces packets, under Backend serialization.
        bool tickResult = false;
        std::thread tick([&] { tickResult = backend.vsync(regs.data(), 42, 0, &error); });
        tick.join();
        check(tickResult, "tick producer submits field through the owner");
        regs.fill(0);
        uint64_t csr = 0, siglblid = 0;
        check(backend.readback(csr, siglblid, &error) && csr == originalRegs[15] &&
            siglblid == originalRegs[18], "owner feedback observes consumed register snapshot");
        const auto epochs = backend.completedFieldEpochs();
        check(epochs.size() == 1 && epochs[0].guestFieldId == 42 && epochs[0].fieldParity == 0 &&
            epochs[0].submitCount == 1 && workloadHashMatches(epochs[0],
                expectedEpochHash({{3, originalPacket.data(), uint32_t(originalPacket.size())}})),
            "owner preserves same-run packet and field association");
        check(backend.resize(800, 600, 2, &error), "surface caller forwards resize to owner");
        bool foreignResize = true;
        std::thread foreign([&] { foreignResize = backend.resize(900, 600, 2, &error); });
        foreign.join();
        check(!foreignResize && error.find("thread which created the native surface") != std::string::npos,
            "worker resize still requires surface caller thread");
        std::array<uint8_t, 16> local{};
        check(backend.readLocalMemory(local.data(), local.size(), 0, 0, 0, &error) &&
            local[0] == 0x5a && local[15] == (15 ^ 0x5a), "owner local-to-host result is complete");
        std::vector<uint8_t> memory{0x17u, 0x28u, 0x39u};
        const auto priorSnapshot = memory;
        setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SNAPSHOT", "1");
        check(!backend.snapshotLocalMemory(memory, &error) && memory == priorSnapshot,
            "owner bridge snapshot failure preserves the caller's prior bytes");
        setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SNAPSHOT", nullptr);
        check(backend.snapshotLocalMemory(memory, &error) && memory.size() == 4u * 1024u * 1024u &&
                  memory.front() == 0x3cu && memory.back() == 0x3cu,
            "owner snapshot replaces the caller bytes only after complete success");
        if (!memory.empty())
        {
            memory[31] = 0xc7;
            check(backend.restoreLocalMemory(memory.data(), memory.size(), &error), "owner restore completes");
            check(backend.resetGs(&error), "ordered owner CSR soft reset completes");
            memory.clear();
            check(backend.snapshotLocalMemory(memory, &error) && memory[31] == 0xc7,
                "ordered owner soft reset preserves restored GS local memory");
        }
        check(backend.readback(csr, siglblid, &error) && csr == originalRegs[15] &&
            siglblid == originalRegs[18], "soft reset does not invent producer register snapshots");
        setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_RESET", "1");
        check(!backend.resetGs(&error) && error.find("forced reset failure") != std::string::npos,
            "reset operation failures reach the synchronous caller");
        setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_RESET", nullptr);
        rrv::gsbackend::CaptureResult capture;
        check(backend.capture({84, 42, 1}, capture, &error) && capture.tag.fieldIndex == 42 &&
            capture.rgba.size() == 16, "owner explicit capture returns correct field and owned pixels");
        rrv::gsbackend::PresentationStats stats;
        check(backend.presentationStats(stats, &error) && stats.directGpuPresents == 1 &&
            stats.completedCaptures == 1, "owner stats follow completed operations");
        const auto consumedDigest = digest();
        backend.shutdown();
        check(!backend.resetGs(&error), "reset fails after owner shutdown");
        if (std::string(mode) == "inline") inlineDigest = consumedDigest;
        else
        {
            check(consumedDigest == inlineDigest, "identical input yields identical inline/owner event digest");
            check(violations() == 0, "all bridge operations remain on initialized owner");
            check((operations() & 0x1fffu) == 0x1fffu, "owner exercised init/create/destroy and every GS operation");
            check((operations() & (1u << 14)) != 0, "soft reset executes on GS owner");
            check(pumps() == 2, "main thread services both create and destroy lifecycle callbacks");
        }
    }
    setEnvironment("RRV_GS_EXECUTION", nullptr);
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", nullptr);
}
// Gate-5 split: a field transition queued from rrv-vu1 (vsyncDeferred) hands
// the bridge the same calls, in the same order and with the same registers, as
// the waiting transition (drain, patch, vsync, readback) it replaces.
void testDeferredFieldTransition()
{
    const auto violations = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_owner_violations");
    const auto digest = fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_event_digest");
    check(violations && digest, "fake owner instrumentation exports exist (deferred field)");
    if (!violations || !digest) return;
    setEnvironment("RRV_GS_BACKEND", "pcsx2");
    setEnvironment("RRV_GS_RENDER_MODE", "field");
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", "1");
    setEnvironment("RRV_GS_EXECUTION", "worker-sync");
    uint64_t waitingDigest = 0, waitingCsr = 0, waitingSiglblid = 0;
    for (const bool deferred : {false, true})
    {
        rrv::gsbackend::InitializeOptions options{};
        options.snapshotWidth = 640; options.snapshotHeight = 448;
        options.rendererKind = rrv::gsbackend::RendererKind::Metal;
        options.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
        options.surface.kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
        options.surface.nativeView = reinterpret_cast<void*>(uintptr_t{1});
        options.surface.nativeLayer = reinterpret_cast<void*>(uintptr_t{2});
        options.surface.widthPixels = 640; options.surface.heightPixels = 448;
        options.surface.backingScale = 2; options.surface.mainThreadPrepared = true;
        rrv::gsbackend::Backend backend;
        std::string error;
        const bool initialized = backend.initialize(options, &error);
        check(initialized, "deferred-field comparison initializes the fake bridge");
        if (!initialized) { std::cerr << error << '\n'; continue; }
        // The sink runs on rrv-gs-owner; the test reads it only behind a fence.
        uint64_t sinkCsr = 0, sinkSiglblid = 0, sinkCalls = 0;
        check(backend.startVuSplit([&](uint64_t csr, uint64_t siglblid)
                                   { sinkCsr = csr; sinkSiglblid = siglblid; ++sinkCalls; }, &error),
              "split starts");
        check(!backend.fieldDeferrable(), "a field is deferrable only from rrv-vu1");
        bool commandOk = true, deferrableOnVu = false;
        std::string commandError;
        // Owner state the queued packets produce: the patch must see the
        // readback of the packet submitted just before the transition.
        const auto patch = [&](uint64_t *regs) { regs[15] = sinkCsr + 0x10u; regs[18] = sinkCalls; };
        for (uint64_t field = 42; field < 46; ++field)
        {
            backend.ownerPost([&, field]
            {
                deferrableOnVu = backend.fieldDeferrable();
                const std::array<uint8_t, 16> packet{0x71, 0x25, static_cast<uint8_t>(field)};
                commandOk = backend.submit(3, packet.data(), packet.size(), &commandError) && commandOk;
                std::array<uint64_t, 19> regs{};
                regs[0] = 3; regs[15] = 0x3000; regs[18] = field;
                if (deferred)
                {
                    commandOk = backend.vsyncDeferred(regs.data(), field, static_cast<uint32_t>(field & 1u),
                                                      patch, &commandError) && commandOk;
                    return;
                }
                backend.drainGsFromVu();
                patch(regs.data());
                uint64_t csr = 0, siglblid = 0;
                commandOk = backend.vsync(regs.data(), field, static_cast<uint32_t>(field & 1u), &commandError) &&
                            backend.readback(csr, siglblid, &commandError) && commandOk;
                sinkCsr = csr; sinkSiglblid = siglblid; ++sinkCalls;
            }, 16, 1);
        }
        backend.ownerFence();
        check(commandOk, deferred ? "deferred field commands succeed" : "waiting field commands succeed");
        if (!commandOk) std::cerr << commandError << '\n';
        check(deferrableOnVu, "a field is deferrable on rrv-vu1 without diagnostics");
        // Per field: one packet readback and one post-vsync readback.
        check(sinkCalls == 8, "every packet batch and every field delivers its readback");
        const auto epochs = backend.completedFieldEpochs();
        check(epochs.size() == 4 && epochs[0].guestFieldId == 42 && epochs[3].guestFieldId == 45 &&
                  epochs[3].submitCount == 1,
              "deferred fields keep the packet and field association");
        const auto consumedDigest = digest();
        if (!deferred)
        {
            waitingDigest = consumedDigest; waitingCsr = sinkCsr; waitingSiglblid = sinkSiglblid;
        }
        else
        {
            check(consumedDigest == waitingDigest, "deferred fields give the bridge the waiting path's exact call stream");
            // SIGLBLID carries the sink count the last patch saw: 3 fields x 2 readbacks + its own packet's.
            check(sinkCsr == waitingCsr && sinkSiglblid == waitingSiglblid && sinkSiglblid == 7,
                  "deferred fields patch after the packet readback and read back the same CSR/SIGLBLID");
            check(violations() == 0, "deferred bridge calls stay on the GS owner");
            // A failing deferred transition reaches the next barrier.
            setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", "1");
            backend.ownerPost([&]
            {
                std::array<uint64_t, 19> regs{};
                backend.vsyncDeferred(regs.data(), 46, 0, patch, &commandError);
            }, 16, 1);
            bool rethrown = false;
            try { backend.ownerFence(); }
            catch (const std::exception &e)
            {
                rethrown = std::string(e.what()).find("forced vsync failure") != std::string::npos;
            }
            setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", nullptr);
            check(rethrown, "a failed deferred field transition is rethrown at the next fence");
        }
        try { backend.shutdown(); } catch (const std::exception &) {}
    }
    setEnvironment("RRV_GS_EXECUTION", nullptr);
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", nullptr);
}
} // namespace

int main()
{
    setEnvironment("RRV_GS_EXECUTION", nullptr);
    const std::string tracePath =
        (std::filesystem::temp_directory_path() / "rrv-gs-backend-tests.gstrace").string();
    const std::string recordPath =
        (std::filesystem::temp_directory_path() / "rrv-gs-backend-tests.gsr").string();
    setEnvironment("RRV_PCSX2_GS_BRIDGE", RRV_TEST_FAKE_GS_BRIDGE);
    setEnvironment("RRV_GS_BACKEND", "pcsx2");
    setEnvironment("RRV_GS_TRACE", tracePath.c_str());
    setEnvironment("RRV_GS_RECORD", recordPath.c_str());
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FULL", "1");
    setEnvironment("RRV_GS_RENDER_MODE", "full");
    // The portable product request must win over this legacy diagnostic
    // environment selector at the bridge boundary.
    setEnvironment("RRV_PCSX2_GS_RENDERER", "software");
    testExportedReadLocalMemoryValidation();

    // Direct GPU is a hard request: an ABI-v5 bridge which does not advertise
    // it must not quietly create the old CPU-snapshot presentation path.
    rrv::gsbackend::InitializeOptions rejectedDirect{};
    rejectedDirect.snapshotWidth = 640u;
    rejectedDirect.snapshotHeight = 448u;
    rejectedDirect.rendererKind = rrv::gsbackend::RendererKind::Metal;
    rejectedDirect.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
    rejectedDirect.surface.kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
    rejectedDirect.surface.nativeView = reinterpret_cast<void *>(static_cast<uintptr_t>(1u));
    rejectedDirect.surface.nativeLayer = reinterpret_cast<void *>(static_cast<uintptr_t>(2u));
    rejectedDirect.surface.widthPixels = 640u;
    rejectedDirect.surface.heightPixels = 448u;
    rejectedDirect.surface.backingScale = 2.0f;
    rejectedDirect.surface.mainThreadPrepared = true;
    rrv::gsbackend::Backend rejectedDirectBackend;
    std::string directError;
    check(!rejectedDirectBackend.initialize(rejectedDirect, &directError),
          "direct presentation fails closed when the bridge lacks its capability");
    check(directError.find("did not prove support") != std::string::npos,
          "direct-present capability failure is actionable");

    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", "1");
    rrv::gsbackend::Backend directBackend;
    directError.clear();
    check(directBackend.initialize(rejectedDirect, &directError),
          "direct presentation initializes only with a matching surface/capability");
    check(directBackend.presentationMode() == rrv::gsbackend::PresentationMode::DirectGpu,
          "direct presentation selection is retained by the backend");
    check(rrv::gsbackend::activeRendererKind() == rrv::gsbackend::RendererKind::Metal,
          "explicit Metal renderer request crosses the portable bridge ABI");
    check((directBackend.capabilityFlags() & (1u << 2)) != 0u,
          "direct-present capability is published after bridge creation");
    check(directBackend.resize(800u, 600u, 2.0f, &directError),
          "direct surface resize is serialized through the bridge");
    check(!directBackend.resize(0u, 600u, 2.0f, &directError),
          "invalid direct surface resize is rejected before it can mutate a drawable");
    bool foreignResizeResult = true;
    std::string foreignResizeError;
    std::thread foreignResize([&] {
        foreignResizeResult = directBackend.resize(800u, 600u, 2.0f, &foreignResizeError);
    });
    foreignResize.join();
    check(!foreignResizeResult &&
              foreignResizeError.find("thread which created the native surface") != std::string::npos,
          "direct resize rejects a non-UI thread while retaining serialized GS access");
    uint64_t directRegs[19]{};
    directRegs[0] = 3u;
    check(directBackend.vsync(directRegs, 17u, 1u, &directError),
          "direct field transition reaches the fake bridge");
    const auto directEpochsBeforeCapture = directBackend.completedFieldEpochs();
    check(directEpochsBeforeCapture.size() == 1u &&
              directEpochsBeforeCapture[0].guestFieldId == 17u &&
              directEpochsBeforeCapture[0].gsFieldEpochId == 1u &&
              directEpochsBeforeCapture[0].presentationCandidateId == 1u &&
              directEpochsBeforeCapture[0].fieldParity == 1u &&
              directEpochsBeforeCapture[0].submitCount == 0u &&
              workloadHashMatches(directEpochsBeforeCapture[0], expectedEpochHash({})),
          "a completed vsync creates one drawable-independent empty field epoch");
    rrv::gsbackend::PresentationStats directBeforeCapture{};
    check(directBackend.presentationStats(directBeforeCapture, &directError),
          "direct-mode counters are available before diagnostic capture");
    check(directBeforeCapture.directGpuPresents == 1u &&
              directBeforeCapture.synchronousCpuReadbacks == 0u &&
              directBeforeCapture.cpuWaits == 0u &&
              directBeforeCapture.unexpectedReadbacks == 0u,
          "normal direct field performs one drawable present and no readback or CPU wait");
    std::vector<uint8_t> directCopyPixels{0xffu};
    uint32_t directCopyWidth = 99u;
    uint32_t directCopyHeight = 99u;
    check(!directBackend.copyFrame(directCopyPixels, directCopyWidth, directCopyHeight, &directError),
          "direct presentation rejects the legacy continuous copyFrame path");
    check(directError.find("forbids copyFrame") != std::string::npos && directCopyPixels.empty() &&
              directCopyWidth == 0u && directCopyHeight == 0u,
          "direct copyFrame rejection is fail-closed and leaves no CPU frame");
    rrv::gsbackend::CaptureResult directCapture{};
    const rrv::gsbackend::CaptureTag staleDirectTag{1001u, 16u, 1u};
    check(!directBackend.capture(staleDirectTag, directCapture, &directError),
          "capture rejects a stale field tag rather than hashing a different field");
    const rrv::gsbackend::CaptureTag directTag{1001u, 17u, 1u};
    check(directBackend.capture(directTag, directCapture, &directError),
          "explicit direct-mode diagnostic capture succeeds");
    check(directCapture.tag.guestTick == directTag.guestTick &&
              directCapture.tag.fieldIndex == directTag.fieldIndex &&
              directCapture.tag.presentIndex == directTag.presentIndex &&
              directCapture.captureId == 1u && directCapture.hasSourceGsFieldEpoch &&
              directCapture.sourceGsFieldEpochId == 1u,
          "capture tags survive the ABI boundary");
    const auto directEpochsAfterCapture = directBackend.completedFieldEpochs();
    check(directEpochsAfterCapture.size() == directEpochsBeforeCapture.size() &&
              directEpochsAfterCapture[0].gsFieldEpochId ==
                  directEpochsBeforeCapture[0].gsFieldEpochId &&
              directEpochsAfterCapture[0].canonicalWorkloadHash ==
                  directEpochsBeforeCapture[0].canonicalWorkloadHash,
          "an on-demand capture allocates only a capture id and does not create or alter an epoch");
    rrv::gsbackend::PresentationStats directStats{};
    check(directBackend.presentationStats(directStats, &directError),
          "direct-mode presentation statistics are available");
    check(directStats.requestedCaptures == 1u && directStats.completedCaptures == 1u &&
              directStats.synchronousCpuReadbacks == 1u && directStats.cpuWaits == 1u &&
              directStats.unexpectedReadbacks == 0u,
              "a pre-bridge rejected capture and one completed capture do not enable continuous or unexpected readback");
    directBackend.shutdown();

    // A field transition is not automatically a direct presentation: occluded
    // windows may not supply a CAMetalLayer drawable. The renderer-side
    // direct-present counter must remain zero in that case.
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DRAWABLE", "0");
    rrv::gsbackend::Backend noDrawableBackend;
    directError.clear();
    check(noDrawableBackend.initialize(rejectedDirect, &directError),
          "direct bridge supports a valid surface with no available drawable");
    check(noDrawableBackend.vsync(directRegs, 18u, 0u, &directError),
          "direct field reaches bridge when the drawable is unavailable");
    const auto noDrawableEpochs = noDrawableBackend.completedFieldEpochs();
    check(noDrawableEpochs.size() == 1u &&
              noDrawableEpochs[0].guestFieldId == 18u &&
              noDrawableEpochs[0].gsFieldEpochId == 1u &&
              noDrawableEpochs[0].presentationCandidateId == 1u,
          "a no-drawable field still closes one epoch and produces one presentation candidate");
    rrv::gsbackend::PresentationStats noDrawableStats{};
    check(noDrawableBackend.presentationStats(noDrawableStats, &directError),
          "no-drawable direct counters are available");
    check(noDrawableStats.presentedFrames == 0u && noDrawableStats.directGpuPresents == 0u &&
              noDrawableStats.synchronousCpuReadbacks == 0u && noDrawableStats.cpuWaits == 0u,
          "a no-drawable field is not misreported as a GPU present or a readback");
    const rrv::gsbackend::CaptureTag noDrawableTag{1002u, 18u, 0u};
    check(noDrawableBackend.capture(noDrawableTag, directCapture, &directError),
          "on-demand capture is still possible when no drawable was presented");
    check(directCapture.hasSourceGsFieldEpoch && directCapture.sourceGsFieldEpochId == 1u,
          "a no-drawable capture still joins its retained completed epoch");
    noDrawableBackend.shutdown();
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DRAWABLE", nullptr);

    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_BLANK_STARTUP", "1");
    rrv::gsbackend::Backend blankStartupBackend;
    directError.clear();
    check(blankStartupBackend.initialize(rejectedDirect, &directError),
          "direct bridge initializes for a deterministic pre-display blank capture");
    const rrv::gsbackend::CaptureTag blankStartupTag{1003u, 0u, 0u};
    check(blankStartupBackend.capture(blankStartupTag, directCapture, &directError),
          "pre-display direct capture returns a deterministic CPU blank");
    check(!directCapture.hasSourceGsFieldEpoch && directCapture.sourceGsFieldEpochId == 0u,
          "a pre-display blank capture carries no fabricated completed epoch identity");
    check(blankStartupBackend.vsync(directRegs, 0u, 0u, &directError),
          "guest field zero can complete without colliding with pre-display identity");
    const rrv::gsbackend::CaptureTag fieldZeroTag{1004u, 0u, 1u};
    check(blankStartupBackend.capture(fieldZeroTag, directCapture, &directError) &&
              directCapture.hasSourceGsFieldEpoch &&
              directCapture.sourceGsFieldEpochId == 1u,
          "a capture of completed guest field zero joins its explicit epoch identity");
    rrv::gsbackend::PresentationStats blankStartupStats{};
    check(blankStartupBackend.presentationStats(blankStartupStats, &directError),
          "pre-display blank capture counters are available");
    check(blankStartupStats.requestedCaptures == 2u && blankStartupStats.completedCaptures == 2u &&
              blankStartupStats.synchronousCpuReadbacks == 0u && blankStartupStats.cpuWaits == 0u &&
              blankStartupStats.unexpectedReadbacks == 0u,
          "pre-display CPU blank is not reported as a GPU readback or CPU wait");
    blankStartupBackend.shutdown();
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_BLANK_STARTUP", nullptr);
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", nullptr);

    // The fake only advertises FullFrame when its test-only switch is set. Its
    // create call also rejects a mismatched config value, proving that the
    // backend carried the parsed mode across ABI v4 rather than falling back.
    rrv::gsbackend::Backend fullBackend;
    std::string error;
    check(fullBackend.initialize(640u, 448u, &error), "full mode initializes when the renderer advertises it");
    check(fullBackend.active(), "full-capable fake bridge becomes active");
    check(fullBackend.outputRenderMode() == rrv::gs::GsRenderMode::FullFrame,
          "explicit full selects FullFrame output");
    check(fullBackend.renderMode() == rrv::gs::GsRenderMode::Field,
          "Gate-6 full-frame is bridge-native: the runtime composes nothing (no F7 live path)");
    check((fullBackend.supportedRenderModes() &
           rrv::gs::renderModeCapability(rrv::gs::GsRenderMode::FullFrame)) != 0u,
          "bridge advertises FullFrame capability");
    check(std::string(fullBackend.displayName()) == "Fake PCSX2",
          "backend uses the bridge-reported stable renderer identity");
    if (const auto transportMode = reinterpret_cast<RenderModeFn>(fakeDigestFunction("rrv_test_fake_pcsx2_gs_bridge_render_mode")))
    {
        check(transportMode() == static_cast<uint32_t>(rrv::gs::GsRenderMode::FullFrame),
              "FullFrame is forwarded to the bridge, which renders it natively (Gate 6)");
    }

    // Capture writers read the already-published backend selection. They do
    // not consult RRV_GS_RENDER_MODE themselves, so these headers stay FullFrame
    // even though no GS packet is submitted here.
    rrv::gstrace::configure();
    rrv::gstrace::openWith({}, nullptr);
    rrv::gstrace::finish();
    rrv::gstrace::TraceFileHeader traceHeader{};
    check(readHeader(tracePath.c_str(), traceHeader),
          "trace header is written");
    check(traceHeader.renderMode == static_cast<uint32_t>(rrv::gs::GsRenderMode::FullFrame),
          "trace header serializes FullFrame in reserved v1 bytes");

    std::array<uint64_t, 19u> recordRegs{};
    rrv::gsrecord::rrv_gs_record_init_from_env(nullptr, 0u, recordRegs.data());
    rrv::gsrecord::rrv_gs_record_shutdown();
    rrv::gsrecord::GsrFileHeader recordHeader{};
    check(readHeader(recordPath.c_str(), recordHeader),
          "record header is written");
    check(recordHeader.renderMode == static_cast<uint32_t>(rrv::gs::GsRenderMode::FullFrame),
          "record header serializes FullFrame in reserved v1 bytes");
    std::remove(tracePath.c_str());
    std::remove(recordPath.c_str());
    fullBackend.shutdown();

    setEnvironment("RRV_GS_TRACE", nullptr);
    setEnvironment("RRV_GS_RECORD", nullptr);
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FULL", nullptr);
    setEnvironment("RRV_GS_RENDER_MODE", nullptr);

    BridgeDigests unsetFieldDigests{};
    error.clear();
    check(runDeterministicFieldSequence(unsetFieldDigests, error),
          "unset mode completes the deterministic field bridge sequence");
    setEnvironment("RRV_GS_RENDER_MODE", "field");
    BridgeDigests explicitFieldDigests{};
    error.clear();
    check(runDeterministicFieldSequence(explicitFieldDigests, error),
          "explicit field completes the deterministic field bridge sequence");
    check(unsetFieldDigests.config == explicitFieldDigests.config,
          "unset and explicit field pass identical bridge configuration");
    check(unsetFieldDigests.events == explicitFieldDigests.events,
          "unset and explicit field submit identical bridge event sequences");
    setEnvironment("RRV_GS_RENDER_MODE", nullptr);

    rrv::gsbackend::Backend backend;
    check(backend.initialize(640u, 448u, &error), "explicit PCSX2 selection initializes");
    check(backend.active(), "backend becomes active");
    check(std::string(backend.name()) == "pcsx2", "backend reports PCSX2");
    check(backend.renderMode() == rrv::gs::GsRenderMode::Field,
          "unset render mode selects Field");
    check((backend.supportedRenderModes() &
           rrv::gs::renderModeCapability(rrv::gs::GsRenderMode::Field)) != 0u,
          "field capability is advertised");

    const uint8_t packet[16]{};
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SUBMIT", "1");
    check(!backend.submit(1u, packet, sizeof(packet), &error),
          "a bridge-rejected submit fails without entering the open epoch");
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_SUBMIT", nullptr);
    check(backend.submit(1u, packet, sizeof(packet), &error), "post-arbitration packet is submitted");
    check(!backend.submit(4u, packet, sizeof(packet), &error), "invalid path is rejected");
    check(!backend.submit(1u, packet, 15u, &error), "unaligned byte count is rejected");

    uint64_t regs[19]{};
    regs[15] = 0x2000u;
    regs[18] = 0xabcdefu;
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", "1");
    check(!backend.vsync(regs, 9u, 1u, &error) &&
              backend.completedFieldEpochs().empty(),
          "a bridge-rejected vsync does not manufacture or consume an epoch");
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", nullptr);
    check(backend.vsync(regs, 9u, 1u, &error), "field transition is forwarded");
    auto epochs = backend.completedFieldEpochs();
    check(epochs.size() == 1u && epochs[0].guestFieldId == 9u &&
              epochs[0].gsFieldEpochId == 1u && epochs[0].presentationCandidateId == 1u &&
              epochs[0].submitCount == 1u && epochs[0].submitBytes == sizeof(packet) &&
              epochs[0].pathEventCounts[0] == 1u && epochs[0].pathByteCounts[0] == sizeof(packet) &&
              epochs[0].pathEventCounts[1] == 0u && epochs[0].pathEventCounts[2] == 0u &&
              workloadHashMatches(epochs[0],
                  expectedEpochHash({{1u, packet, static_cast<uint32_t>(sizeof(packet))}})),
          "a normal field records its canonical ordered post-GIF workload");

    const uint8_t path1Packet[16] = {1u};
    const uint8_t path2Packet[32] = {2u};
    const uint8_t path3Packet[16] = {3u};
    check(backend.submit(1u, path1Packet, sizeof(path1Packet), &error) &&
              backend.submit(2u, path2Packet, sizeof(path2Packet), &error) &&
              backend.submit(3u, path3Packet, sizeof(path3Packet), &error) &&
              backend.vsync(regs, 10u, 0u, &error),
          "multiple PATH submissions close as one following field epoch");
    epochs = backend.completedFieldEpochs();
    check(epochs.size() == 2u && epochs[1].guestFieldId == 10u &&
              epochs[1].gsFieldEpochId == 2u && epochs[1].presentationCandidateId == 2u &&
              epochs[1].pathEventCounts[0] == 1u && epochs[1].pathEventCounts[1] == 1u &&
              epochs[1].pathEventCounts[2] == 1u &&
              epochs[1].pathByteCounts[0] == sizeof(path1Packet) &&
              epochs[1].pathByteCounts[1] == sizeof(path2Packet) &&
              epochs[1].pathByteCounts[2] == sizeof(path3Packet) &&
              workloadHashMatches(epochs[1], expectedEpochHash({
                  {1u, path1Packet, static_cast<uint32_t>(sizeof(path1Packet))},
                  {2u, path2Packet, static_cast<uint32_t>(sizeof(path2Packet))},
                  {3u, path3Packet, static_cast<uint32_t>(sizeof(path3Packet))}})),
          "PATH1/PATH2/PATH3 ordering and byte counts are canonicalized inside one epoch");

    check(backend.submit(1u, packet, sizeof(packet), &error) &&
              backend.vsync(regs, 11u, 1u, &error),
          "an identical workload completes in the next guest field");
    epochs = backend.completedFieldEpochs();
    check(epochs.size() == 3u && epochs[2].gsFieldEpochId == 3u &&
              epochs[2].presentationCandidateId == 3u &&
              epochs[2].canonicalWorkloadHashAvailable ==
                  epochs[0].canonicalWorkloadHashAvailable &&
              epochs[2].canonicalWorkloadHash == epochs[0].canonicalWorkloadHash,
          "repeated identical workloads retain their hash but receive unique epoch identity");
#if defined(RRV_GS_EPOCH_DIAGNOSTICS)
    check(!backend.vsync(regs, 11u, 1u, &error) &&
              error.find("duplicate or non-monotonic") != std::string::npos &&
              !backend.vsync(regs, 10u, 0u, &error) &&
              error.find("duplicate or non-monotonic") != std::string::npos &&
              !backend.vsync(regs, 12u, 1u, &error) &&
              error.find("non-alternating") != std::string::npos &&
              backend.completedFieldEpochs().size() == 3u,
          "duplicate, non-monotonic and non-alternating fields fail before they can close an epoch");
    check(backend.vsync(regs, 12u, 0u, &error) &&
              backend.completedFieldEpochs().size() == 4u,
          "a rejected transition leaves the open epoch intact for the valid following field");
#else
    check(backend.vsync(regs, 12u, 0u, &error) &&
              backend.completedFieldEpochs().size() == 4u,
          "production accepts the next valid field without diagnostic-only fatal assertions");
#endif
    const auto shortEpochHistory = backend.completedFieldEpochHistory();
    check(shortEpochHistory.totalCompletedEpochs == 4u &&
              shortEpochHistory.droppedCompletedEpochs == 0u &&
              shortEpochHistory.epochs.size() == 4u,
          "short deterministic intervals report complete retained epoch history without loss");
    uint64_t csr = 0u;
    uint64_t siglblid = 0u;
    check(backend.readback(csr, siglblid, &error), "guest-visible state reads back");
    check(csr == regs[15] && siglblid == regs[18], "readback values are preserved");

    uint8_t local[7]{};
    check(backend.readLocalMemory(local, sizeof(local), 1u, 2u, 3u, &error),
          "local-to-host transfer uses active backend");
    check(local[0] == 0x5au && local[6] == (6u ^ 0x5au),
          "local-memory bytes came from bridge");

    std::array<uint8_t, 16> zeroRead{};
    zeroRead.fill(0xa5u);
    check(!backend.readLocalMemory(zeroRead.data(), 0u, 1u, 2u, 3u, &error) &&
              std::all_of(zeroRead.begin(), zeroRead.end(), [](uint8_t value) { return value == 0xa5u; }) &&
              error.find("non-zero") != std::string::npos,
          "active backend rejects zero-byte local-memory reads without touching the destination");
    check(!backend.readLocalMemory(nullptr, 7u, 1u, 2u, 3u, &error) &&
              error.find("null destination") != std::string::npos,
          "active backend rejects a non-empty local-memory read with a null destination");
    check(!backend.readLocalMemory(nullptr, 0u, 1u, 2u, 3u, &error) &&
              error.find("non-zero") != std::string::npos,
          "zero-byte validation precedes a null destination at the backend boundary");

    std::vector<uint8_t> localSnapshot;
    check(backend.snapshotLocalMemory(localSnapshot, &error),
          "complete local-memory snapshot uses active backend");
    check(localSnapshot.size() == 4u * 1024u * 1024u &&
              localSnapshot.front() == 0x3cu && localSnapshot.back() == 0x3cu,
          "local-memory snapshot preserves the complete 4 MiB image");
    localSnapshot.front() = 0x91u;
    localSnapshot.back() = 0xa7u;
    check(backend.restoreLocalMemory(localSnapshot.data(),
                                     static_cast<uint32_t>(localSnapshot.size()), &error),
          "complete local-memory restore uses active backend");
    std::vector<uint8_t> restoredSnapshot;
    check(backend.snapshotLocalMemory(restoredSnapshot, &error) &&
              restoredSnapshot.front() == 0x91u && restoredSnapshot.back() == 0xa7u,
          "local-memory restore round-trips through bridge authority");
    check(!backend.restoreLocalMemory(localSnapshot.data(), 16u, &error),
          "partial local-memory restore is rejected");

    std::vector<uint8_t> rgba;
    uint32_t width = 0u;
    uint32_t height = 0u;
    check(backend.copyFrame(rgba, width, height, &error), "snapshot copies successfully");
    check(width == 2u && height == 2u && rgba.size() == 16u,
          "snapshot is tightly repacked");
    check(rgba[0] == 1u && rgba[7] == 8u && rgba[8] == 9u && rgba[15] == 16u,
          "snapshot row padding is excluded");

    backend.shutdown();
    check(!backend.active() && std::string(backend.name()) == "legacy",
          "shutdown unloads the optional backend");
    std::vector<uint8_t> inactiveSnapshot{0x42u, 0x63u};
    const auto inactiveSnapshotBefore = inactiveSnapshot;
    check(!backend.snapshotLocalMemory(inactiveSnapshot, &error) && inactiveSnapshot == inactiveSnapshotBefore,
          "inactive backend snapshot failure preserves caller bytes");
    zeroRead.fill(0xa5u);
    check(!backend.readLocalMemory(zeroRead.data(), 0u, 1u, 2u, 3u, &error) &&
              std::all_of(zeroRead.begin(), zeroRead.end(), [](uint8_t value) { return value == 0xa5u; }) &&
              error.find("not active") != std::string::npos,
          "inactive backend preserves its existing false result before zero-byte validation");

    rrv::gsbackend::Backend ringBackend;
    check(ringBackend.initialize(640u, 448u, &error),
          "bounded epoch-history test initializes the fake bridge");
    bool completedRingRun = true;
    for (uint64_t field = 0u;
         field != rrv::gsbackend::kCompletedFieldEpochHistoryCapacity + 2u; ++field)
    {
        completedRingRun = ringBackend.vsync(regs, field,
                                             static_cast<uint32_t>(field & 1u), &error) &&
                           completedRingRun;
    }
    const auto boundedHistory = ringBackend.completedFieldEpochHistory();
    check(completedRingRun &&
              boundedHistory.totalCompletedEpochs ==
                  rrv::gsbackend::kCompletedFieldEpochHistoryCapacity + 2u &&
              boundedHistory.droppedCompletedEpochs == 2u &&
              boundedHistory.epochs.size() == rrv::gsbackend::kCompletedFieldEpochHistoryCapacity &&
              boundedHistory.epochs.front().guestFieldId == 2u &&
              boundedHistory.epochs.back().guestFieldId ==
                  rrv::gsbackend::kCompletedFieldEpochHistoryCapacity + 1u,
          "completed epoch history retains a bounded chronological ring and reports evictions");
    ringBackend.shutdown();

    setEnvironment("RRV_GS_BACKEND", nullptr);
    error.clear();
    check(backend.initialize(640u, 448u, &error),
          "configured bridge is the implicit primary backend");
    check(backend.active(), "implicit primary selects PCSX2");
    check(backend.vsync(regs, 1u, 1u, &error),
          "reinitialized backend accepts a fresh guest field sequence");
    const auto reinitializedEpochs = backend.completedFieldEpochs();
    check(reinitializedEpochs.size() == 1u &&
              reinitializedEpochs[0].gsFieldEpochId == 5u &&
              reinitializedEpochs[0].presentationCandidateId == 5u,
          "backend-owned epoch and candidate IDs remain monotonic across reinitialization");
    backend.shutdown();

    setEnvironment("RRV_GS_RENDER_MODE", "field");
    error.clear();
    check(backend.initialize(640u, 448u, &error), "explicit field initializes");
    check(backend.renderMode() == rrv::gs::GsRenderMode::Field,
          "explicit field selects Field");
    backend.shutdown();

    setEnvironment("RRV_GS_BACKEND", "legacy");
    check(backend.initialize(640u, 448u, &error), "legacy supports field mode");
    check(!backend.active(), "explicit legacy rollback bypasses the bridge");

    setEnvironment("RRV_GS_RENDER_MODE", "full");
    error.clear();
    check(!backend.initialize(640u, 448u, &error), "legacy rejects full mode");
    check(error.find("RRV_GS_RENDER_MODE=full is not implemented by Legacy SW backend") != std::string::npos,
          "legacy full failure is actionable");
    check(!backend.active(), "legacy full failure does not activate a field fallback");

    setEnvironment("RRV_GS_BACKEND", "pcsx2");
    error.clear();
    check(!backend.initialize(640u, 448u, &error),
          "a field-only bridge rejects full mode (no composed F7 fallback, Gate 6)");
    check(error.find("RRV_GS_RENDER_MODE=full is not implemented") != std::string::npos,
          "field-only bridge full failure is actionable");
    check(!backend.active(), "field-only bridge full failure does not activate a field fallback");

    setEnvironment("RRV_GS_RENDER_MODE", "invalid");
    error.clear();
    check(!backend.initialize(640u, 448u, &error), "invalid render mode is rejected");
    check(error.find("invalid RRV_GS_RENDER_MODE=invalid") != std::string::npos,
          "invalid render mode error is actionable");
    setEnvironment("RRV_GS_RENDER_MODE", "");
    error.clear();
    check(!backend.initialize(640u, 448u, &error), "empty render mode is rejected");
    check(error.find("invalid RRV_GS_RENDER_MODE=<empty>") != std::string::npos,
          "empty render mode error is actionable");

#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    // The live receipt boundary is requested before the next VSync, but the
    // fake does not mark it armed until that VSync succeeds. A failed VSync
    // therefore cannot manufacture a receipt boundary or consume field data.
    setEnvironment("RRV_GS_RENDER_MODE", "field");
    setEnvironment("RRV_GS_BACKEND", "pcsx2");
    rrv::gsbackend::Backend receiptBoundaryBackend;
    error.clear();
    check(receiptBoundaryBackend.initialize(640u, 448u, &error),
          "receipt-boundary fake backend initializes");
    rrv::gsbackend::requestCanonicalConsumerReceipt();
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", "1");
    check(!receiptBoundaryBackend.vsync(regs, 1u, 1u, &error) &&
              receiptBoundaryBackend.completedFieldEpochs().empty(),
          "failed boundary VSync leaves the pending arm and local epoch unclosed");
    setEnvironment("RRV_TEST_FAKE_GS_BRIDGE_FAIL_VSYNC", nullptr);
    check(receiptBoundaryBackend.vsync(regs, 1u, 1u, &error),
          "first successful boundary VSync closes the discarded mixed epoch");
    check(receiptBoundaryBackend.submit(1u, packet, sizeof(packet), &error) &&
              receiptBoundaryBackend.vsync(regs, 2u, 0u, &error),
          "first post-boundary transfer and complete following field succeed");
    const DigestFn mixedAck = fakeDigestFunction(
        "rrv_test_fake_pcsx2_gs_bridge_receipt_mixed_vsync_ack");
    const DigestFn armAck = fakeDigestFunction(
        "rrv_test_fake_pcsx2_gs_bridge_receipt_arm_ack");
    const DigestFn returnAck = fakeDigestFunction(
        "rrv_test_fake_pcsx2_gs_bridge_receipt_return_ack");
    const DigestFn firstEventAck = fakeDigestFunction(
        "rrv_test_fake_pcsx2_gs_bridge_receipt_first_post_event_ack");
    const DigestFn firstTransferAck = fakeDigestFunction(
        "rrv_test_fake_pcsx2_gs_bridge_receipt_first_post_transfer_ack");
    check(mixedAck && armAck && returnAck && firstEventAck && firstTransferAck &&
              mixedAck() == 1u && armAck() == mixedAck() &&
              returnAck() == armAck() && firstEventAck() > returnAck() &&
              firstTransferAck() == firstEventAck(),
          "post-VSync arm occurs at the bridge return edge and the next transfer is first");
    receiptBoundaryBackend.shutdown();
#endif

    testDedicatedOwner();
    testDeferredFieldTransition();
    setEnvironment("RRV_GS_RENDER_MODE", nullptr);
    setEnvironment("RRV_GS_BACKEND", nullptr);

    return failures == 0 ? 0 : 1;
}
