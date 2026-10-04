#include "rrv_gs_backend.h"
#if !defined(RRV_GS_TEST_FAKE_BRIDGE_PATH)
#include "rrv_resource_package.h"
#endif
#include "rrv_gs_trace_hooks.h"
#include "rrv_m2_neutrality.h"
#include "rrv_m2_causal_trace.h"

#include "rrv_cadence_diag.h" // P0-cadence (C0) frame-budget probe; default off
#include "rrv_guest_trace_counters.h" // phase-6 matched guest-state oracle; default off
#include "rrv_gif_guard.h"    // G1 packet-lifetime probe (RRV_GIF_GUARD); default off

#include <algorithm>
#include <array>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

#if defined(RRV_GS_TEST_FAKE_BRIDGE_PATH)
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace rrv::gsbackend
{
namespace
{
    constexpr uint32_t kBridgeAbiVersion = 5u;
    constexpr uint32_t kErrorCapacity = 512u;
    constexpr uint32_t kBridgeLocalMemoryBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kPresentationLegacyCpuSnapshot = 0u;
    constexpr uint32_t kPresentationDirectGpu = 1u;
    constexpr uint32_t kSurfaceNone = 0u;
    constexpr uint32_t kSurfaceMacosViewMetalLayer = 1u;
    constexpr uint32_t kSurfaceLinuxX11 = 2u;     // Gate 5 (Linux/Steam Deck)
    constexpr uint32_t kSurfaceLinuxWayland = 3u; // Gate 5 (Linux/Steam Deck)
    constexpr uint32_t kSurfaceCallerOwnsHandles = 1u << 0;
    constexpr uint32_t kSurfaceMainThreadPrepared = 1u << 1;
    constexpr uint32_t kCapabilityDirectPresent = 1u << 2;
    constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kFnv1aPrime = 1099511628211ull;
    constexpr char kEpochHashDomain[] = "RRV-GS-EPOCH-V1";
    constexpr uint8_t kEpochHashSubmitTag = 0x01u;
    constexpr uint8_t kEpochHashEndTag = 0xffu;
    std::atomic<uint32_t> g_activeRendererKind{
        static_cast<uint32_t>(RendererKind::Unknown)};
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    std::atomic<bool> g_consumerReceiptArmRequested{false};
    std::atomic<bool> g_consumerReceiptArmIssued{false};
#endif
#if defined(RRV_M2_NEUTRALITY_CONTROL)
    std::atomic<Backend *> g_activeM2Backend{nullptr};
#endif

    // Mirrors tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h.  Keep this local:
    // the runtime may be built without that optional bridge source tree.
    struct BridgeSurface
    {
        uint32_t struct_size;
        uint32_t kind;
        uint32_t flags;
        uint32_t reserved;
        void *native_view;
        void *native_layer;
        uint32_t width_pixels;
        uint32_t height_pixels;
        float backing_scale;
        uint32_t reserved2;
    };

    struct BridgeConfig
    {
        uint32_t struct_size;
        uint32_t snapshot_width;
        uint32_t snapshot_height;
        uint32_t sw_threads;
        uint32_t gs_render_mode;
        uint32_t presentation_mode;
        uint32_t renderer_kind;
        uint32_t reserved;
        BridgeSurface surface;
    };

    // Mirrors the optional sideband structure in
    // tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h. It is resolved dynamically
    // alongside the existing optional receipt ABI so production ABI v5 stays
    // unchanged when diagnostics are compiled out.
    struct BridgeReceiptBoundaryOrdering
    {
        uint32_t struct_size;
        uint32_t arm_scheduled;
        uint32_t armed_after_successful_vsync;
        uint32_t first_post_arm_consumer_event_type;
        uint64_t mixed_vsync_consumer_ack_ordinal;
        uint64_t arm_consumer_ack_ordinal;
        uint64_t bridge_vsync_return_consumer_ack_ordinal;
        uint64_t first_post_arm_consumer_ack_ordinal;
        uint64_t first_post_arm_transfer_ack_ordinal;
    };
    static_assert(sizeof(BridgeReceiptBoundaryOrdering) == 56u);

    struct BridgeCapabilities
    {
        uint32_t struct_size;
        uint32_t supported_render_modes;
        uint32_t renderer_kind;
        uint32_t capability_flags;
        const char *renderer_name;
    };

    struct BridgeFrame
    {
        uint32_t struct_size;
        uint32_t width;
        uint32_t height;
        uint32_t stride_bytes;
        const uint8_t *rgba;
        uint64_t sequence;
    };

    struct BridgeCaptureRequest
    {
        uint32_t struct_size;
        uint32_t flags;
        uint64_t guest_tick;
        uint64_t field_index;
        uint64_t present_index;
    };

    struct BridgeCaptureResult
    {
        uint32_t struct_size;
        uint32_t flags;
        uint64_t guest_tick;
        uint64_t field_index;
        uint64_t present_index;
        BridgeFrame frame;
    };

    struct BridgeStats
    {
        uint32_t struct_size;
        uint32_t reserved;
        uint64_t presented_frames;
        uint64_t direct_gpu_presents;
        uint64_t requested_captures;
        uint64_t completed_captures;
        uint64_t synchronous_cpu_readbacks;
        uint64_t unexpected_readbacks;
        uint64_t cpu_waits;
    };

    static_assert(sizeof(BridgeSurface) == 48u, "PCSX2 bridge surface ABI changed");
    static_assert(alignof(BridgeSurface) == alignof(void*), "PCSX2 bridge surface ABI alignment changed");
    static_assert(sizeof(BridgeConfig) == 80u, "PCSX2 bridge config ABI changed");
    static_assert(alignof(BridgeConfig) == alignof(void*), "PCSX2 bridge config ABI alignment changed");
    static_assert(sizeof(BridgeCapabilities) == 24u, "PCSX2 bridge capability ABI changed");
    static_assert(offsetof(BridgeFrame, rgba) == 16u, "PCSX2 bridge frame ABI changed");
    static_assert(sizeof(BridgeCaptureRequest) == 32u, "PCSX2 bridge capture request ABI changed");
    static_assert(sizeof(BridgeCaptureResult) == 64u, "PCSX2 bridge capture result ABI changed");
    static_assert(sizeof(BridgeStats) == 64u, "PCSX2 bridge stats ABI changed");

    // How many EXTRA threads PCSX2's software rasterizer gets. This is the
    // single biggest lever on the guest thread's frame budget: with 0 the whole
    // scanline pass runs inside our submit() call, on the thread that also runs
    // VU1 and the EE. Measured on the 3D attract phases (docs/TESTING.md
    // T-GS-SWTHREADS): the `gs` bucket falls from ~9.0 ms to ~3.7 ms per guest
    // display list, which is what moves six phases from 2 fields/list to 1.
    //
    // 3 was measured best on this host (M3 Pro, 6P+5E); 2 is within noise and 5
    // is worse — the workers then contend with the guest thread for P cores. The
    // portable rule keeps a floor of 2 (PCSX2's own default) and never asks for
    // more than the machine has spare, because the guest thread, the vblank
    // worker and the presenter each need one.
    //
    // RRV_PCSX2_GS_SW_THREADS=0 is the rollback to the old single-threaded
    // behaviour, and the ONLY configuration in which a bridge without
    // GSSyncRasterizer() would be correct.
    uint32_t defaultSwThreads()
    {
        const unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0u)
            return 2u;
        const unsigned spare = (hw > 3u) ? (hw - 3u) : 1u;
        return static_cast<uint32_t>(std::min(3u, std::max(2u, spare)));
    }

    uint32_t configuredSwThreads()
    {
        const char *value = std::getenv("RRV_PCSX2_GS_SW_THREADS");
        if (!value || value[0] == '\0')
            return defaultSwThreads();

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed > std::numeric_limits<uint32_t>::max())
        {
            std::cerr << "[gs-backend] ignoring invalid RRV_PCSX2_GS_SW_THREADS="
                      << value << " (expected 0.." << std::numeric_limits<uint32_t>::max() << ")\n";
            return defaultSwThreads();
        }
        return static_cast<uint32_t>(parsed);
    }

    bool setError(std::string *out, const std::string &message)
    {
        if (out)
            *out = message;
        return false;
    }

    bool parseRenderMode(rrv::gs::GsRenderMode &mode, std::string *error)
    {
        const char *value = std::getenv("RRV_GS_RENDER_MODE");
        // Gate 6: `full` is rendered inside the PCSX2 bridge, so field-only
        // (no F7) builds such as rrv-legacy-live accept it as well; the bridge
        // rejects it if its renderer cannot provide it.
        if (!value)
        {
            mode = rrv::gs::GsRenderMode::Field;
            return true;
        }
        if (std::strcmp(value, "field") == 0)
        {
            mode = rrv::gs::GsRenderMode::Field;
            return true;
        }
        if (std::strcmp(value, "full") == 0)
        {
            mode = rrv::gs::GsRenderMode::FullFrame;
            return true;
        }
        return setError(error, std::string("invalid RRV_GS_RENDER_MODE=") +
                               (value[0] == '\0' ? "<empty>" : value) +
                               " (expected field or full)");
    }

    std::string bridgeError(const char *operation, const std::array<char, kErrorCapacity> &detail)
    {
        std::ostringstream out;
        out << operation << " failed";
        if (detail[0] != '\0')
            out << ": " << detail.data();
        return out.str();
    }

    bool bridgeDiagVerbose()
    {
        static const bool verbose = [] {
            const char *value = std::getenv("RRV_PCSX2_GS_DIAG");
            return value && std::strcmp(value, "verbose") == 0;
        }();
        return verbose;
    }

    uint64_t fnv1a(uint64_t hash, const uint8_t *bytes, size_t size)
    {
        for (size_t index = 0u; index != size; ++index)
        {
            hash ^= bytes[index];
            hash *= kFnv1aPrime;
        }
        return hash;
    }

    uint64_t hashEpochPrologue()
    {
        return fnv1a(kFnv1aOffsetBasis,
                     reinterpret_cast<const uint8_t *>(kEpochHashDomain),
                     sizeof(kEpochHashDomain) - 1u);
    }

    uint64_t hashLittleEndianU32(uint64_t hash, uint32_t value)
    {
        std::array<uint8_t, 4u> bytes{};
        for (uint32_t index = 0u; index != bytes.size(); ++index)
            bytes[index] = static_cast<uint8_t>(value >> (index * 8u));
        return fnv1a(hash, bytes.data(), bytes.size());
    }

    uint64_t finalizeEpochHash(uint64_t hash)
    {
        return fnv1a(hash, &kEpochHashEndTag, sizeof(kEpochHashEndTag));
    }
} // namespace

namespace {
uint64_t workerDigest(uint64_t hash, uint64_t kind, uint64_t identity,
                      const void* data, size_t size)
{
    auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ull; };
    for (uint64_t value : {kind, identity, static_cast<uint64_t>(size)})
        for (unsigned shift = 0; shift < 64; shift += 8) byte(value >> shift);
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) byte(bytes[i]);
    return hash;
}
}
uint64_t Backend::verifyEnqueue(uint64_t kind, uint64_t identity, const void* data, size_t size)
{
    if (!m_workerVerify) return 0;
    m_verifyProducerHash = workerDigest(m_verifyProducerHash, kind, identity, data, size);
    return ++m_verifySubmitted;
}
void Backend::verifyConsume(uint64_t sequence, uint64_t kind, uint64_t identity,
                            const void* data, size_t size)
{
    if (!sequence) return;
    if (sequence != ++m_verifyConsumed)
        throw std::runtime_error("GS worker verification command order mismatch");
    m_verifyConsumerHash = workerDigest(m_verifyConsumerHash, kind, identity, data, size);
    // worker-sync's producer cannot mutate its next digest before this wait.
    if (m_verifyConsumerHash != m_verifyProducerHash)
        throw std::runtime_error("GS worker verification payload/field mismatch");
}

#if defined(RRV_GS_TEST_FAKE_BRIDGE_PATH)
// Asset-free fixture only. It is compiled with a target-derived fake image path
// and is never part of a package reader or product target.
struct Backend::BridgeImage
{
    void *library = nullptr;
    ~BridgeImage()
    {
#if defined(_WIN32)
        if (library)
            FreeLibrary(static_cast<HMODULE>(library));
#else
        if (library)
            dlclose(library);
#endif
    }
};
#else
// The package owns both the reader lease and the native image handle. It stays
// alive until bridge destruction and any worker teardown have completed, then
// performs the final dlclose in its destructor.
struct Backend::BridgeImage
{
    rrv::resource_package::Package package;
};
#endif

void Backend::BridgeImageDeleter::operator()(BridgeImage *image) const noexcept
{
    delete image;
}

Backend::Backend() = default;

Backend::~Backend()
{
    shutdown();
}

bool Backend::initialize(uint32_t snapshotWidth, uint32_t snapshotHeight,
                         std::string *error)
{
    InitializeOptions options{};
    options.snapshotWidth = snapshotWidth;
    options.snapshotHeight = snapshotHeight;
    return initialize(options, error);
}

bool Backend::initialize(const InitializeOptions &options, std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    shutdownUnlocked();
    resetEpochBookkeepingUnlocked();
    const char *execution = std::getenv("RRV_GS_EXECUTION");
    m_workerVerify = std::getenv("RRV_GS_WORKER_VERIFY") != nullptr;
    m_verifySubmitted = m_verifyConsumed = 0;
    m_verifyProducerHash = m_verifyConsumerHash = 14695981039346656037ull;
    m_workerRequested = execution && std::strcmp(execution, "worker-sync") == 0;
    if (execution && execution[0] && std::strcmp(execution, "inline") != 0 &&
        !m_workerRequested)
        return setError(error, "RRV_GS_EXECUTION must be inline or worker-sync (worker-async not yet qualified)");

    if (!parseRenderMode(m_renderMode, error))
        return false;
    rrv::gs::publishCaptureRenderMode(m_renderMode);
    m_supportedRenderModes = rrv::gs::renderModeCapability(rrv::gs::GsRenderMode::Field);
    m_presentationMode = options.presentationMode;
    m_capabilityFlags = 0u;

    const char *selection = std::getenv("RRV_GS_BACKEND");
    if (selection && std::strcmp(selection, "legacy") == 0)
    {
        if (m_presentationMode == PresentationMode::DirectGpu)
            return setError(error, "direct GPU presentation requires the PCSX2 bridge, not Legacy SW");
        if (m_renderMode != rrv::gs::GsRenderMode::Field)
        {
            return setError(error, "RRV_GS_RENDER_MODE=full is not implemented by Legacy SW backend");
        }
        std::cerr << "[gs-backend] selected legacy renderer (render mode="
                  << rrv::gs::renderModeName(m_renderMode) << ")\n";
        return true;
    }
    if (!selection || selection[0] == '\0')
    {
        // PCSX2 GS is the normal runtime backend, not an optional upgrade. The
        // legacy rasterizer is a DIAGNOSTIC backend and is never selected
        // implicitly: it is not equivalent (it silently drops sprite classes
        // the GS draws — docs/TESTING.md T-BACKEND-LEGACY-GAP), it has no
        // GS-consumption trace hook, and a run that lands on it produces no
        // oracle artefact while looking like a tooling bug somewhere else.
        //
        // This used to warn and continue. A warning is not enough: a whole
        // session was spent attributing the legacy rasterizer's missing sprites
        // and 4x frame time to the recompiled game. Fail instead, and say
        // exactly how to fix it.
        return loadBridge(options, error);
    }
    if (std::strcmp(selection, "pcsx2") != 0)
    {
        return setError(error, std::string("invalid RRV_GS_BACKEND=") + selection +
                               " (expected legacy or pcsx2)");
    }

    return loadBridge(options, error);
}

bool Backend::loadBridge(const InitializeOptions &options, std::string *error)
{
    try
    {
#if defined(RRV_GS_TEST_FAKE_BRIDGE_PATH)
#if defined(_WIN32)
        void *library = LoadLibraryA(RRV_GS_TEST_FAKE_BRIDGE_PATH);
        const auto required = [this](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(
                static_cast<HMODULE>(m_bridgeImage->library), name));
        };
#else
        void *library = dlopen(RRV_GS_TEST_FAKE_BRIDGE_PATH, RTLD_NOW | RTLD_LOCAL);
        const auto required = [this](const char* name) -> void* {
            return dlsym(m_bridgeImage->library, name);
        };
#endif
        if (!library)
            return setError(error, "asset-free fake bridge could not load");
        m_bridgeImage.reset(new BridgeImage{library});
        const auto optional = required;
#else
        m_bridgeImage.reset(new BridgeImage{rrv::resource_package::Package::Open(
            rrv::resource_package::rrv_resource_package_binding)});
        m_bridgeImage->package.open_bridge();
        const auto required = [this](const char* name) {
            return m_bridgeImage->package.symbol(name);
        };
        const auto optional = [this](const char* name) -> void* {
            return m_bridgeImage->package.optional_symbol(name);
        };
#endif
        m_version = reinterpret_cast<VersionFn>(required("rrv_pcsx2_gs_bridge_version"));
        m_create = reinterpret_cast<CreateFn>(required("rrv_pcsx2_gs_bridge_create"));
        m_destroy = reinterpret_cast<DestroyFn>(required("rrv_pcsx2_gs_bridge_destroy"));
        m_submit = reinterpret_cast<SubmitFn>(required("rrv_pcsx2_gs_bridge_submit"));
        m_vsync = reinterpret_cast<VsyncFn>(required("rrv_pcsx2_gs_bridge_vsync"));
        m_snapshot = reinterpret_cast<SnapshotFn>(required("rrv_pcsx2_gs_bridge_snapshot"));
        m_resize = reinterpret_cast<ResizeFn>(required("rrv_pcsx2_gs_bridge_resize"));
        m_capture = reinterpret_cast<CaptureFn>(required("rrv_pcsx2_gs_bridge_capture"));
        m_stats = reinterpret_cast<StatsFn>(required("rrv_pcsx2_gs_bridge_get_stats"));
        m_readback = reinterpret_cast<ReadbackFn>(required("rrv_pcsx2_gs_bridge_readback"));
        m_resetGs = reinterpret_cast<ResetGsFn>(optional("rrv_pcsx2_gs_bridge_reset_gs"));
        m_readLocalMemory = reinterpret_cast<ReadLocalMemoryFn>(required("rrv_pcsx2_gs_bridge_read_local_memory"));
        m_snapshotLocalMemory = reinterpret_cast<SnapshotLocalMemoryFn>(required("rrv_pcsx2_gs_bridge_snapshot_local_memory"));
        m_restoreLocalMemory = reinterpret_cast<RestoreLocalMemoryFn>(required("rrv_pcsx2_gs_bridge_restore_local_memory"));
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
        m_receiptArm = reinterpret_cast<ReceiptArmFn>(optional("rrv_pcsx2_gs_bridge_receipt_arm"));
        m_receiptArmAfterVsync = reinterpret_cast<ReceiptArmAfterVsyncFn>(
            optional("rrv_pcsx2_gs_bridge_receipt_arm_after_vsync"));
        m_receiptBoundaryOrdering = reinterpret_cast<ReceiptBoundaryOrderingFn>(
            optional("rrv_pcsx2_gs_bridge_receipt_boundary_ordering"));
        m_receiptSetEpochIdentity = reinterpret_cast<ReceiptSetEpochIdentityFn>(
            optional("rrv_pcsx2_gs_bridge_receipt_set_epoch_identity"));
#endif
    if (!m_version || !m_create || !m_destroy || !m_submit || !m_vsync || !m_snapshot ||
        !m_resize || !m_capture || !m_stats || !m_readback || !m_readLocalMemory ||
        !m_snapshotLocalMemory || !m_restoreLocalMemory)
    {
        shutdownUnlocked();
        return setError(error, "PCSX2 GS bridge package has incompatible ABI");
    }
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    if (!m_receiptArm || !m_receiptArmAfterVsync || !m_receiptBoundaryOrdering ||
        !m_receiptSetEpochIdentity)
    {
        shutdownUnlocked();
        return setError(error,
                        "PCSX2 GS bridge lacks compiled M2 canonical receipt support");
    }
#endif
    if (m_version() != kBridgeAbiVersion)
    {
        const uint32_t reportedVersion = m_version();
        shutdownUnlocked();
        return setError(error, "PCSX2 GS bridge package reports ABI v" +
                               std::to_string(reportedVersion) + ", but RRV requires v" +
                               std::to_string(kBridgeAbiVersion));
    }

    // PCSX2 owns physical GS submission and per-field CRTC timing in both
    // RRV modes.  Gate 6: FullFrame is rendered INSIDE the PCSX2 hardware GS
    // (docs/evidence/GATE6_FULL_FRAME_PLAN_2026-09-26.md), so the parsed mode
    // is forwarded and a bridge without that capability fails create.  The
    // runtime keeps submitting the identical field stream in both modes.
    const bool direct = options.presentationMode == PresentationMode::DirectGpu;
    uint32_t surfaceKind = kSurfaceNone;
    switch (options.surface.kind)
    {
    case NativeSurfaceKind::MacosViewMetalLayer: surfaceKind = kSurfaceMacosViewMetalLayer; break;
    case NativeSurfaceKind::LinuxX11: surfaceKind = kSurfaceLinuxX11; break;
    case NativeSurfaceKind::LinuxWayland: surfaceKind = kSurfaceLinuxWayland; break;
    case NativeSurfaceKind::None: break;
    }
    BridgeSurface surface{sizeof(BridgeSurface), surfaceKind,
                          direct ? kSurfaceCallerOwnsHandles : 0u,
                          0u, options.surface.nativeView, options.surface.nativeLayer,
                          options.surface.widthPixels, options.surface.heightPixels,
                          options.surface.backingScale, 0u};
    if (direct && options.surface.mainThreadPrepared)
        surface.flags |= kSurfaceMainThreadPrepared;
    std::array<char, kErrorCapacity> ownerDetail{};
    if (m_workerRequested)
    {
        m_ownerCommand = reinterpret_cast<OwnerCommandFn>(optional("rrv_pcsx2_gs_bridge_owner_command"));
        m_pumpMain = reinterpret_cast<PumpMainFn>(optional("rrv_pcsx2_gs_bridge_pump_main_thread"));
        auto ownerInit = reinterpret_cast<OwnerInitFn>(optional("rrv_pcsx2_gs_bridge_owner_thread_init"));
        auto prepare = reinterpret_cast<PrepareOwnerFn>(optional("rrv_pcsx2_gs_bridge_prepare_owner_surface"));
        if (!m_ownerCommand || !m_pumpMain || !ownerInit || !prepare ||
            !direct || !prepare(&surface, ownerDetail.data(), ownerDetail.size()))
        {
            shutdownUnlocked();
            return setError(error, "GS worker requires owner-capable bridge and main-thread SDL surface: " +
                                   std::string(ownerDetail.data()));
        }
        m_worker = std::make_unique<rrv::gs::Worker>(rrv::gs::Worker::Limits{},
            [this](const std::function<void()>& command) {
                struct Invocation {
                    const std::function<void()>* command;
                    std::exception_ptr failure;
                } invocation{&command, {}};
                m_ownerCommand([](void* context) {
                    auto& call = *static_cast<Invocation*>(context);
                    try { (*call.command)(); }
                    catch (...) { call.failure = std::current_exception(); }
                }, &invocation);
                // Do not unwind through the PCSX2 -fno-exceptions adapter.
                if (invocation.failure) std::rethrow_exception(invocation.failure);
            }, [this] {
                // Also runs after worker failure. The library and SDL surface
                // remain retained until requestStop/pump/join has completed.
                if (m_bridge && m_destroy) m_destroy(m_bridge);
            });
        m_worker->invoke(ownerInit);
    }
    const BridgeConfig config{sizeof(BridgeConfig), options.snapshotWidth, options.snapshotHeight,
                              configuredSwThreads(),
                              static_cast<uint32_t>(m_renderMode),
                              direct ? kPresentationDirectGpu : kPresentationLegacyCpuSnapshot,
                              static_cast<uint32_t>(options.rendererKind), 0u,
                              surface};
    BridgeCapabilities capabilities{sizeof(BridgeCapabilities), 0u, 0u, 0u, nullptr};
    std::array<char, kErrorCapacity> detail{};
    if (m_worker)
    {
        const auto sequence = m_worker->submit([&] {
            m_bridge = m_create(&config, &capabilities, detail.data(), detail.size());
        });
        // Pinned Metal attaches/detaches its surface using dispatch_sync(main)
        // (macOS). On Linux the bridge's pump is a no-op; the wait is the same.
        // Startup owns no guest-execution lock. Service only this lifecycle wait.
        while (true)
        {
            const auto state = m_worker->stats();
            if (state.completed >= sequence || state.failed) break;
            m_pumpMain();
        }
        m_worker->wait(sequence);
    }
    else
        m_bridge = m_create(&config, &capabilities, detail.data(), detail.size());
    if (capabilities.struct_size >= sizeof(BridgeCapabilities))
    {
        m_supportedRenderModes = capabilities.supported_render_modes;
        m_rendererName = capabilities.renderer_name ? capabilities.renderer_name : "PCSX2";
        m_capabilityFlags = capabilities.capability_flags;
        g_activeRendererKind.store(capabilities.renderer_kind, std::memory_order_release);
    }
    if (!m_bridge)
    {
        shutdownUnlocked();
        return setError(error, "PCSX2 GS bridge package " + bridgeError("create", detail));
    }
    if (direct && (m_capabilityFlags & kCapabilityDirectPresent) == 0u)
    {
        shutdownUnlocked();
        return setError(error, "direct GPU presentation was requested but the PCSX2 bridge did not prove support");
    }

    if (direct)
        m_directSurfaceThread = std::this_thread::get_id();

    if ((m_supportedRenderModes & rrv::gs::renderModeCapability(m_renderMode)) == 0u)
    {
        const std::string renderer = m_rendererName.empty() ? "PCSX2" : m_rendererName;
        shutdownUnlocked();
        return setError(error, std::string("RRV_GS_RENDER_MODE=") +
                               rrv::gs::renderModeName(m_renderMode) +
                               " is not implemented by " + renderer + " backend");
    }

    m_active.store(true, std::memory_order_release);
#if defined(RRV_M2_NEUTRALITY_CONTROL)
    g_activeM2Backend.store(this, std::memory_order_release);
#endif
    std::cerr << "[gs-backend] selected " << displayName()
#if defined(RRV_GS_TEST_FAKE_BRIDGE_PATH)
              << " asset-free fake bridge: " << RRV_GS_TEST_FAKE_BRIDGE_PATH
#else
              << " package bridge: "
              << m_bridgeImage->package.bridge_path()
#endif
              << " (execution=" << (m_worker ? "worker-sync" : "inline")
              << ", ABI v" << kBridgeAbiVersion << ", render mode="
              << rrv::gs::renderModeName(m_renderMode) << ")\n";
        return true;
    }
    catch (const std::exception& failure)
    {
        shutdownUnlocked();
        return setError(error, std::string("cannot open source-derived PCSX2 GS package: ") + failure.what());
    }
}

void Backend::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    shutdownUnlocked();
}

void Backend::shutdownUnlocked()
{
#if defined(RRV_M2_NEUTRALITY_CONTROL)
    Backend *expected = this;
    g_activeM2Backend.compare_exchange_strong(expected, nullptr,
                                               std::memory_order_acq_rel);
#endif
    m_active.store(false, std::memory_order_release);
    if (m_vuWorker)
    {
        // Drain rrv-vu1 first: its commands still queue packets to rrv-gs-owner.
        try { m_vuWorker->stop(); }
        catch (const std::exception& failure) {
            std::fprintf(stderr, "[vu1-worker] shutdown after failure: %s\n", failure.what());
        }
        const auto state = m_vuWorker->stats();
        std::fprintf(stderr, "[vu1-worker] completed=%llu submitted=%llu hwm_bytes=%zu hwm_commands=%zu failed=%d\n",
            (unsigned long long)state.completed, (unsigned long long)state.submitted,
            state.highWaterBytes, state.highWaterCommands, state.failed);
        m_vuWorker.reset();
        m_readbackSink = {};
    }
    if (m_worker)
    {
        m_worker->requestStop();
        while (!m_worker->finished())
        {
            if (!m_pumpMain || !m_pumpMain())
                std::this_thread::yield();
        }
        try { m_worker->stop(); }
        catch (const std::exception& failure) {
            std::fprintf(stderr, "[gs-worker] shutdown after failure: %s\n", failure.what());
        }
        const auto state = m_worker->stats();
        std::fprintf(stderr, "[gs-worker] completed=%llu submitted=%llu fields=%llu/%llu "
            "hwm_bytes=%zu hwm_fields=%zu hwm_commands=%zu backlog=%zu failed=%d\n",
            (unsigned long long)state.completed, (unsigned long long)state.submitted,
            (unsigned long long)state.completedFields, (unsigned long long)state.submittedFields,
            state.highWaterBytes, state.highWaterFields, state.highWaterCommands,
            state.outstandingCommands, state.failed);
        if (m_workerVerify)
            std::fprintf(stderr, "[gs-worker-verify] commands=%llu/%llu producer=%016llx consumer=%016llx match=%d\n",
                (unsigned long long)m_verifyConsumed, (unsigned long long)m_verifySubmitted,
                (unsigned long long)m_verifyProducerHash, (unsigned long long)m_verifyConsumerHash,
                m_verifyConsumed == m_verifySubmitted && m_verifyProducerHash == m_verifyConsumerHash);
        m_worker.reset();
    }
    else if (m_bridge && m_destroy)
        m_destroy(m_bridge);
    m_ownerCommand = nullptr;
    m_pumpMain = nullptr;
    m_bridge = nullptr;
    m_create = nullptr;
    m_destroy = nullptr;
    m_submit = nullptr;
    m_vsync = nullptr;
    m_snapshot = nullptr;
    m_resize = nullptr;
    m_capture = nullptr;
    m_stats = nullptr;
    m_readback = nullptr;
    m_resetGs = nullptr;
    m_readLocalMemory = nullptr;
    m_snapshotLocalMemory = nullptr;
    m_restoreLocalMemory = nullptr;
    m_version = nullptr;
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    m_receiptArm = nullptr;
    m_receiptArmAfterVsync = nullptr;
    m_receiptBoundaryOrdering = nullptr;
    m_receiptSetEpochIdentity = nullptr;
#endif
    m_supportedRenderModes = rrv::gs::renderModeCapability(rrv::gs::GsRenderMode::Field);
    m_rendererName.clear();
    m_capabilityFlags = 0u;
    m_presentationMode = PresentationMode::LegacyCpuSnapshot;
    m_directSurfaceThread = {};
    g_activeRendererKind.store(static_cast<uint32_t>(RendererKind::Unknown),
                               std::memory_order_release);
    m_bridgeImage.reset();
}

#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
bool Backend::scheduleConsumerReceiptAfterVsyncIfRequestedUnlocked(std::string *error)
{
    if (!g_consumerReceiptArmRequested.load(std::memory_order_acquire) ||
        g_consumerReceiptArmIssued.load(std::memory_order_relaxed))
    {
        return true;
    }
    std::array<char, kErrorCapacity> detail{};
    if (!m_receiptArmAfterVsync ||
        ownerCall(m_receiptArmAfterVsync, m_bridge, detail.data(),
                                static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("schedule canonical consumer receipt after VSync", detail));
    }
    g_consumerReceiptArmIssued.store(true, std::memory_order_release);
    g_consumerReceiptArmRequested.store(false, std::memory_order_release);
    return true;
}

bool Backend::consumerReceiptBoundaryOrderingUnlocked(
    rrv::m2neutral::ConsumerBoundaryOrdering *ordering, std::string *error)
{
    if (!ordering)
        return setError(error, "canonical consumer receipt boundary output is null");
    *ordering = {};
    if (!g_consumerReceiptArmIssued.load(std::memory_order_acquire))
        return true;
    BridgeReceiptBoundaryOrdering bridgeOrdering{sizeof(BridgeReceiptBoundaryOrdering)};
    std::array<char, kErrorCapacity> detail{};
    if (!m_receiptBoundaryOrdering ||
        ownerCall(m_receiptBoundaryOrdering, m_bridge, &bridgeOrdering, detail.data(),
                                  static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("read canonical consumer boundary ordering", detail));
    }
    if (bridgeOrdering.struct_size < sizeof(BridgeReceiptBoundaryOrdering))
        return setError(error, "PCSX2 GS bridge returned truncated canonical boundary ordering");
    ordering->available = true;
    ordering->armScheduled = bridgeOrdering.arm_scheduled != 0u;
    ordering->armedAfterSuccessfulVsync =
        bridgeOrdering.armed_after_successful_vsync != 0u;
    ordering->firstPostArmConsumerEventType =
        bridgeOrdering.first_post_arm_consumer_event_type;
    ordering->mixedVsyncConsumerAckOrdinal =
        bridgeOrdering.mixed_vsync_consumer_ack_ordinal;
    ordering->armConsumerAckOrdinal = bridgeOrdering.arm_consumer_ack_ordinal;
    ordering->bridgeVsyncReturnConsumerAckOrdinal =
        bridgeOrdering.bridge_vsync_return_consumer_ack_ordinal;
    ordering->firstPostArmConsumerAckOrdinal =
        bridgeOrdering.first_post_arm_consumer_ack_ordinal;
    ordering->firstPostArmTransferAckOrdinal =
        bridgeOrdering.first_post_arm_transfer_ack_ordinal;
    return true;
}
#endif

void Backend::resetEpochBookkeepingUnlocked()
{
    m_openFieldEpoch = {};
#if defined(RRV_GS_EPOCH_DIAGNOSTICS)
    m_openFieldEpoch.canonicalWorkloadHashAvailable = true;
    m_openFieldEpoch.canonicalWorkloadHash = hashEpochPrologue();
#endif
    m_completedFieldEpochRingStart = 0u;
    m_completedFieldEpochRingSize = 0u;
    m_totalCompletedFieldEpochs = 0u;
    m_droppedCompletedFieldEpochs = 0u;
    m_lastGuestFieldId = 0u;
    m_lastFieldParity = 0u;
    m_hasCompletedGuestField = false;
}

bool Backend::validateNextGuestFieldUnlocked(uint64_t fieldIndex, uint32_t fieldParity,
                                             std::string *error) const
{
    if (m_nextGsFieldEpochId == 0u || m_nextPresentationCandidateId == 0u)
    {
        return setError(error, "GuestFieldEpoch identifier space is exhausted");
    }
    if (!m_hasCompletedGuestField)
        return true;
#if defined(RRV_GS_EPOCH_DIAGNOSTICS)
    if (fieldIndex <= m_lastGuestFieldId)
    {
        return setError(error,
                        "GuestFieldEpoch rejected duplicate or non-monotonic guest field id");
    }
    if (m_lastGuestFieldId != std::numeric_limits<uint64_t>::max() &&
        fieldIndex != m_lastGuestFieldId + 1u)
    {
        return setError(error, "GuestFieldEpoch rejected skipped guest field id");
    }
    if (fieldParity == m_lastFieldParity)
    {
        return setError(error, "GuestFieldEpoch rejected non-alternating field parity");
    }
#endif
    return true;
}

void Backend::noteSubmittedPacketUnlocked(uint32_t pathId, const uint8_t *gifBytes,
                                          uint32_t sizeBytes)
{
    const size_t pathIndex = static_cast<size_t>(pathId - 1u);
    ++m_openFieldEpoch.submitCount;
    m_openFieldEpoch.submitBytes += sizeBytes;
    ++m_openFieldEpoch.pathEventCounts[pathIndex];
    m_openFieldEpoch.pathByteCounts[pathIndex] += sizeBytes;

    if (!m_openFieldEpoch.canonicalWorkloadHashAvailable)
        return;
    const uint8_t canonicalPath = static_cast<uint8_t>(pathId);
    m_openFieldEpoch.canonicalWorkloadHash = fnv1a(
        m_openFieldEpoch.canonicalWorkloadHash, &kEpochHashSubmitTag,
        sizeof(kEpochHashSubmitTag));
    m_openFieldEpoch.canonicalWorkloadHash = fnv1a(
        m_openFieldEpoch.canonicalWorkloadHash, &canonicalPath, sizeof(canonicalPath));
    m_openFieldEpoch.canonicalWorkloadHash = hashLittleEndianU32(
        m_openFieldEpoch.canonicalWorkloadHash, sizeBytes);
    if (sizeBytes != 0u)
    {
        m_openFieldEpoch.canonicalWorkloadHash = fnv1a(
            m_openFieldEpoch.canonicalWorkloadHash, gifBytes, sizeBytes);
    }
}

bool Backend::active() const
{
    return m_active.load(std::memory_order_acquire);
}

RendererKind activeRendererKind()
{
    return static_cast<RendererKind>(
        g_activeRendererKind.load(std::memory_order_acquire));
}

void requestCanonicalConsumerReceipt()
{
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    if (!g_consumerReceiptArmIssued.load(std::memory_order_relaxed))
        g_consumerReceiptArmRequested.store(true, std::memory_order_release);
#endif
}

#if defined(RRV_M2_NEUTRALITY_CONTROL)
bool captureActiveCompletedField(uint64_t guestFieldId, CaptureResult &result,
                                 std::string *error)
{
    Backend *backend = g_activeM2Backend.load(std::memory_order_acquire);
    if (!backend)
        return setError(error, "no active GS backend for deferred M2 capture");
    const CompletedFieldEpochHistory history = backend->completedFieldEpochHistory();
    if (history.epochs.empty() || history.epochs.back().guestFieldId != guestFieldId)
    {
        return setError(error,
                        "deferred M2 capture no longer matches the latest completed field");
    }
    const CompletedFieldEpoch &epoch = history.epochs.back();
    const CaptureTag tag{guestFieldId, guestFieldId,
                         epoch.presentationCandidateId};
    return backend->capture(tag, result, error);
}
#endif

const char *Backend::name() const
{
    return active() ? "pcsx2" : "legacy";
}

const char *Backend::displayName() const
{
    if (!active())
    {
        // Our own tile-binned rasterizer (ps2_gs_gpu.cpp). Named distinctly on
        // purpose: it is a diagnostic backend, not a correctness path
        // (docs/TESTING.md T-BACKEND-LEGACY-GAP), and "software" alone would
        // read as PCSX2's software renderer.
        return "Legacy SW";
    }
    return m_rendererName.empty() ? "PCSX2" : m_rendererName.c_str();
}

rrv::gs::GsRenderMode Backend::renderMode() const
{
    // What the RUNTIME must compose above the bridge.  Gate-6 full-frame is
    // produced inside the PCSX2 bridge from the unchanged field stream, so the
    // runtime composes nothing extra (the retired F7 live path keys off
    // FullFrame here and must stay off).  outputRenderMode() is the mode the
    // player sees.
    return active() ? rrv::gs::GsRenderMode::Field : m_renderMode;
}

rrv::gs::GsRenderMode Backend::outputRenderMode() const
{
    return m_renderMode;
}

uint32_t Backend::supportedRenderModes() const
{
    return m_supportedRenderModes;
}

PresentationMode Backend::presentationMode() const
{
    return m_presentationMode;
}

uint32_t Backend::capabilityFlags() const
{
    return m_capabilityFlags;
}

namespace
{
    // KNOWN_ISSUES #22: the authoritative oracle boundary. Opening is deferred
    // until the first traced call after arming so the initial local-memory
    // snapshot is taken through the live bridge, giving all three replay paths
    // an identical seed. Called with Backend's mutex already held.
    template <typename SnapFn>
    void traceOpenIfWanted(SnapFn &&snapshot, const uint64_t *regs19)
    {
        rrv::gstrace::configure();
        if (!rrv::gstrace::wantsOpen())
            return;
        std::vector<uint8_t> vram;
        snapshot(vram);
        rrv::gstrace::openWith(vram, regs19);
    }
}  // namespace

#if defined(__linux__)
// Gate 5 [cpu] log: GS time for the owner split (src/host/rrv_thread_cpu_log.cpp).
// Weak, so binaries without the logger (tests) link and skip it.
extern "C" void rrv_cpu_log_add_gs(unsigned long long ns) __attribute__((weak));
namespace
{
struct CpuLogGsScope
{
    // Off on rrv-vu1 under the split: there submit() only queues, and the GS
    // time is taken where the packet actually runs, on rrv-gs-owner.
    bool on = true;
    std::chrono::steady_clock::time_point start = on && rrv_cpu_log_add_gs ? std::chrono::steady_clock::now()
                                                                           : std::chrono::steady_clock::time_point{};
    ~CpuLogGsScope()
    {
        if (on && rrv_cpu_log_add_gs)
            rrv_cpu_log_add_gs(static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    }
};
} // namespace
#define RRV_CPU_LOG_GS_SCOPE_IF(cond) CpuLogGsScope cpuLogGs{static_cast<bool>(cond)}
#else
#define RRV_CPU_LOG_GS_SCOPE_IF(cond) ((void)0)
#endif
#define RRV_CPU_LOG_GS_SCOPE() RRV_CPU_LOG_GS_SCOPE_IF(true)

namespace
{
// Gate-5 split: a packet queued from rrv-vu1 runs on rrv-gs-owner under the
// floating-point control word rrv-vu1 had (the EE's, set by the owner stream),
// exactly as when the owner ran VU1 and GS on one thread.
#if defined(__aarch64__)
inline uint64_t splitReadFpControl() { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
inline void splitWriteFpControl(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
#elif defined(__x86_64__) || defined(_M_X64)
inline uint64_t splitReadFpControl() { return _mm_getcsr(); }
inline void splitWriteFpControl(uint64_t v) { _mm_setcsr(static_cast<unsigned>(v)); }
#endif
struct SplitFpControlScope
{
    uint64_t saved;
    explicit SplitFpControlScope(uint64_t value) : saved(splitReadFpControl())
    {
        if (value != saved) splitWriteFpControl(value);
    }
    ~SplitFpControlScope()
    {
        if (splitReadFpControl() != saved) splitWriteFpControl(saved);
    }
};
} // namespace

bool Backend::submit(uint32_t pathId, const uint8_t *gifBytes, uint32_t sizeBytes,
                     std::string *error)
{
    const bool split = readbackDeferred();
    RRV_CPU_LOG_GS_SCOPE_IF(!split);
    // C0 frame budget: PCSX2 GS is synchronous for G0, so packet decode and
    // rasterisation are charged to whichever thread submitted — normally the
    // guest thread. `gs` is opened before the mutex so seam contention counts,
    // and `gsLock` records the contended part separately: the frontend's
    // presentation snapshot takes this same mutex, and "GS is slow" and "the
    // present path is holding the seam" have entirely different fixes.
    auto cadenceGs = rrv::cadence::gsScope();
    rrv::cadence::noteGifPacket(sizeBytes);
    rrv::guesttrace::noteGsSubmit(sizeBytes);
    // G1: second observation point, at the seam itself. Comparing this with the
    // arbiter-side check localises a dead buffer to before or after the seam.
    rrv::gifguard::checkSpan("seam", pathId, gifBytes, sizeBytes);
    std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
    const bool direct = ownerDirect();
    if (direct)
    {
        // Owner-stream command on the GS owner: already serialized (see ownerPost).
    }
    else if (!rrv::cadence::enabled())
    {
        lock.lock();
    }
    else if (!lock.try_lock())
    {
        auto cadenceGsLock = rrv::cadence::gsLockScope();
        lock.lock();
    }
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    // Empty arbiter records are legal synchronization no-ops and already
    // occur on the legacy path. Preserve them at the seam; only a non-empty
    // packet requires storage.
    if ((sizeBytes != 0u && !gifBytes) || (sizeBytes & 0x0fu) != 0u ||
        pathId < 1u || pathId > 3u)
    {
        return setError(error, "invalid post-arbitration GIF packet for PCSX2 GS bridge");
    }

    std::array<char, kErrorCapacity> detail{};
    if (bridgeDiagVerbose())
        std::fprintf(stderr, "[gs-backend:submit] begin path=%u bytes=%u\n", pathId, sizeBytes);
    traceOpenIfWanted([&](std::vector<uint8_t> &out) {
        out.resize(kBridgeLocalMemoryBytes);
        std::array<char, kErrorCapacity> snapDetail{};
        if (ownerCall(m_snapshotLocalMemory, m_bridge, out.data(), kBridgeLocalMemoryBytes,
                                  snapDetail.data(),
                                  static_cast<uint32_t>(snapDetail.size())) == 0)
            out.clear();
    }, nullptr);
    if (rrv::gstrace::recording())
        rrv::gstrace::write(rrv::gstrace::EventTag::Submit,
                            static_cast<uint8_t>(pathId), gifBytes, sizeBytes);
    int submitted = 0;
    if (split)
    {
        // Gate-5 split: gather into the rrv-vu1 batch and return; see
        // flushSplitBatch(). A failing packet fails the GS worker, which the
        // next wait/fence on either side rethrows.
        if (sizeBytes > rrv::gs::Worker::Limits{}.bytes)
            return setError(error, "GS worker packet exceeds 16 MiB capacity");
        constexpr size_t kBatchPackets = 64u, kBatchBytes = 256u * 1024u;
        if (m_splitBatch.bytes.size() + sizeBytes > rrv::gs::Worker::Limits{}.bytes)
            flushSplitBatch();
        const auto verification = verifyEnqueue(1, pathId, gifBytes, sizeBytes);
        m_splitBatch.bytes.insert(m_splitBatch.bytes.end(), gifBytes, gifBytes + sizeBytes);
        m_splitBatch.packets.push_back({pathId, sizeBytes, verification});
        if (m_splitBatch.packets.size() >= kBatchPackets || m_splitBatch.bytes.size() >= kBatchBytes)
            flushSplitBatch();
        submitted = 1;
    }
    else if (m_worker && !direct)
    {
        // Reject before allocation; never split a GIF command across fences.
        if (sizeBytes > rrv::gs::Worker::Limits{}.bytes)
            return setError(error, "GS worker packet exceeds 16 MiB capacity");
        // Copy before return: the arbiter/guest scratch storage is reusable.
        std::vector<uint8_t> payload;
        if (sizeBytes) payload.assign(gifBytes, gifBytes + sizeBytes);
        const auto verification = verifyEnqueue(1, pathId, payload.data(), payload.size());
        submitted = m_worker->invoke([this, pathId, verification, payload = std::move(payload), &detail] {
            verifyConsume(verification, 1, pathId, payload.data(), payload.size());
            return m_submit(m_bridge, pathId, payload.data(),
                            static_cast<uint32_t>(payload.size()), detail.data(),
                            static_cast<uint32_t>(detail.size()));
        }, sizeBytes);
    }
    else
        submitted = m_submit(m_bridge, pathId, gifBytes, sizeBytes, detail.data(),
                             static_cast<uint32_t>(detail.size()));
    if (submitted == 0)
    {
        return setError(error, bridgeError("submit", detail));
    }
    rrv::m2causal::event(rrv::m2causal::EventType::TransferAcknowledged,
                         rrv::m2causal::Source::Backend, pathId, sizeBytes, sizeBytes / 16u);
    if (pathId == 2u)
        rrv::m2causal::packetFingerprint(rrv::m2causal::Source::Backend,
                                         gifBytes, sizeBytes, 0u);
    rrv::m2neutral::noteSubmittedPacket(pathId, gifBytes, sizeBytes);
    noteSubmittedPacketUnlocked(pathId, gifBytes, sizeBytes);
    if (bridgeDiagVerbose())
        std::fprintf(stderr, "[gs-backend:submit] end path=%u bytes=%u\n", pathId, sizeBytes);
    return true;
}

bool Backend::resetGs(std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (!m_resetGs)
        return setError(error, "PCSX2 GS bridge lacks optional producer-control reset operation");
    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_resetGs, m_bridge, detail.data(),
                  static_cast<uint32_t>(detail.size())) == 0)
        return setError(error, bridgeError("reset GS", detail));
    return true;
}

bool Backend::readback(uint64_t &outCsr, uint64_t &outSiglblid, std::string *error)
{
    const auto lock = producerLock();
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");

    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_readback, m_bridge, &outCsr, &outSiglblid, detail.data(),
                   static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("readback", detail));
    }
    if (rrv::gstrace::recording())
    {
        rrv::gstrace::ReadbackPayload payload{outCsr, outSiglblid};
        rrv::gstrace::write(rrv::gstrace::EventTag::Readback, 0u, &payload,
                            sizeof(payload));
    }
    return true;
}

bool Backend::vsync(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                    std::string *error)
{
    return fieldTransition(regs19, fieldIndex, fieldParity, nullptr, error);
}

bool Backend::fieldDeferrable() const
{
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    return false;
#else
    if (!readbackDeferred() || m_workerVerify)
        return false;
    // A GS trace records the vsync registers on the calling thread, before
    // `prepare` could patch them: keep the wait while one may still record.
    rrv::gstrace::configure();
    const auto &trace = rrv::gstrace::state();
    return !trace.enabled || trace.finished;
#endif
}

bool Backend::vsyncDeferred(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                            FieldPrepare prepare, std::string *error)
{
    if (!prepare || !fieldDeferrable())
        return setError(error, "PCSX2 GS bridge field transition cannot be deferred here");
    return fieldTransition(regs19, fieldIndex, fieldParity, &prepare, error);
}

// `deferred` (rrv-vu1, Gate-5 split): queue the bridge call and its readback
// instead of waiting for them; see vsyncDeferred() in the header.
bool Backend::fieldTransition(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                              FieldPrepare *deferred, std::string *error)
{
    RRV_CPU_LOG_GS_SCOPE_IF(!readbackDeferred());
    flushSplitBatch(); // rrv-vu1: the field's packets run before its vsync
    rrv::guesttrace::noteVsync();
    // Runs on the vblank worker while it holds the guest-execution lock, so
    // this is one of the two ways another thread stalls the guest thread.
    const uint64_t cadenceStart = rrv::cadence::enabled() ? rrv::cadence::nowNs() : 0u;
    struct CadenceField
    {
        uint64_t start;
        ~CadenceField()
        {
            rrv::cadence::addCrossThread(rrv::cadence::g_fieldGsNs,
                                         rrv::cadence::g_fieldGsCalls, start);
        }
    } cadenceField{cadenceStart};
    const bool direct = ownerDirect();
    const auto lock = producerLock();
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (!regs19 || fieldParity > 1u)
    {
        return setError(error, "invalid privileged GS snapshot or field parity for PCSX2 GS bridge");
    }
    if (!validateNextGuestFieldUnlocked(fieldIndex, fieldParity, error))
        return false;

    // G1 presentation-boundary probe: the CRTC state we hand PCSX2 each field.
    // The 19-slot order is the GSRegisters ABI (PMODE, SMODE1, SMODE2, ...,
    // DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, ...). PMODE selects WHICH read
    // circuit PCSX2 scans out; picking the wrong one reads the wrong DISPFB.
    if (std::getenv("RRV_GS_STAGE_DUMP"))
    {
        static uint64_t s_lastPmode = ~0ull, s_lastSmode2 = ~0ull;
        static uint64_t s_lastDispfb1 = ~0ull, s_lastDispfb2 = ~0ull;
        static uint64_t s_lastDisplay1 = ~0ull, s_lastDisplay2 = ~0ull;
        if (regs19[0] != s_lastPmode || regs19[2] != s_lastSmode2 ||
            regs19[7] != s_lastDispfb1 || regs19[9] != s_lastDispfb2 ||
            regs19[8] != s_lastDisplay1 || regs19[10] != s_lastDisplay2)
        {
            std::fprintf(stderr,
                         "[stage1:crtc] field=%llu PMODE=%016llx SMODE2=%016llx "
                         "DISPFB1=%016llx DISPLAY1=%016llx DISPFB2=%016llx DISPLAY2=%016llx\n",
                         (unsigned long long)fieldIndex,
                         (unsigned long long)regs19[0], (unsigned long long)regs19[2],
                         (unsigned long long)regs19[7], (unsigned long long)regs19[8],
                         (unsigned long long)regs19[9], (unsigned long long)regs19[10]);
            s_lastPmode = regs19[0]; s_lastSmode2 = regs19[2];
            s_lastDispfb1 = regs19[7]; s_lastDisplay1 = regs19[8];
            s_lastDispfb2 = regs19[9]; s_lastDisplay2 = regs19[10];
        }
    }

    // G1 bounded causality experiment (RRV_GS_FORCE_PMODE=<hex>, default off).
    // Our runtime hands PCSX2 PMODE=0x8001 (read circuit 1, CRTMD=0 which is an
    // invalid encoding) where hardware programs 0x66 (circuit 2, CRTMD=1,
    // MMOD/AMOD set). Under the legacy GS PMODE was inert, so B0b scored this
    // "real but not dominant"; PCSX2 runs a real CRTC, so it may not be. This
    // exists ONLY to prove or refute that; it is not a fix.
    std::array<uint64_t, 19u> regsOverride{};
    static const uint64_t s_forcePmode = [] {
        const char *v = std::getenv("RRV_GS_FORCE_PMODE");
        return v && v[0] ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    if (s_forcePmode != 0ull)
    {
        std::memcpy(regsOverride.data(), regs19, sizeof(regsOverride));
        regsOverride[0] = s_forcePmode;
        regs19 = regsOverride.data();
    }

    std::array<char, kErrorCapacity> detail{};
    rrv::m2neutral::ConsumerBoundaryOrdering consumerBoundaryOrdering{};
    if (bridgeDiagVerbose())
        std::fprintf(stderr, "[gs-backend:vsync] begin field=%llu parity=%u\n",
                     static_cast<unsigned long long>(fieldIndex), fieldParity);
    // The trace must carry the registers ACTUALLY delivered, i.e. after any
    // override above, not the caller's originals.
    traceOpenIfWanted([&](std::vector<uint8_t> &out) {
        out.resize(kBridgeLocalMemoryBytes);
        std::array<char, kErrorCapacity> snapDetail{};
        if (ownerCall(m_snapshotLocalMemory, m_bridge, out.data(), kBridgeLocalMemoryBytes,
                                  snapDetail.data(),
                                  static_cast<uint32_t>(snapDetail.size())) == 0)
            out.clear();
    }, regs19);
    if (rrv::gstrace::recording())
    {
        rrv::gstrace::VsyncPayload payload{};
        std::memcpy(payload.regs19, regs19, sizeof(payload.regs19));
        payload.fieldIndex = fieldIndex;
        payload.fieldParity = fieldParity;
        rrv::gstrace::write(rrv::gstrace::EventTag::Vsync, 0u, &payload,
                            sizeof(payload));
    }
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    // The selector edge may land inside this open field. Schedule the arm
    // before entering the bridge; the bridge performs it only after its
    // successful GSvsync closure and before this synchronous call returns.
    if (!scheduleConsumerReceiptAfterVsyncIfRequestedUnlocked(error))
        return false;
    if (!m_receiptSetEpochIdentity ||
        ownerCall(m_receiptSetEpochIdentity, m_bridge, fieldIndex, m_nextGsFieldEpochId,
                                  detail.data(),
                                  static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error,
                        bridgeError("set canonical consumer epoch identity", detail));
    }
    detail.fill('\0');
#endif
    std::array<uint64_t, 19> fieldRegs{};
    std::copy_n(regs19, fieldRegs.size(), fieldRegs.begin());
    const auto verification = m_worker && !direct ? verifyEnqueue(2 + fieldParity, fieldIndex,
                                                       fieldRegs.data(), sizeof(fieldRegs)) : 0;
    auto consumeField = [this, fieldRegs, fieldIndex, fieldParity, verification, &detail] {
        verifyConsume(verification, 2 + fieldParity, fieldIndex, fieldRegs.data(), sizeof(fieldRegs));
        return m_vsync(m_bridge, fieldRegs.data(), fieldIndex, fieldParity,
                       detail.data(), static_cast<uint32_t>(detail.size()));
    };
    if (deferred)
    {
        // Charged as a full field budget: a second transition waits for the
        // first, so rrv-vu1 is never more than one field ahead of the GS.
        m_worker->submit([this, fieldRegs, fieldIndex, fieldParity, verification,
                          prepare = std::move(*deferred)]() mutable {
            RRV_CPU_LOG_GS_SCOPE();
            std::array<char, kErrorCapacity> ownerDetail{};
            verifyConsume(verification, 2 + fieldParity, fieldIndex, fieldRegs.data(), sizeof(fieldRegs));
            prepare(fieldRegs.data());
            if (m_vsync(m_bridge, fieldRegs.data(), fieldIndex, fieldParity, ownerDetail.data(),
                        static_cast<uint32_t>(ownerDetail.size())) == 0)
                throw std::runtime_error("PCSX2 GS bridge field transition failed: " + bridgeError("vsync", ownerDetail));
            uint64_t csr = 0u, siglblid = 0u;
            if (m_readback(m_bridge, &csr, &siglblid, ownerDetail.data(),
                           static_cast<uint32_t>(ownerDetail.size())) == 0)
                throw std::runtime_error("PCSX2 GS bridge post-vsync readback failed: " + bridgeError("readback", ownerDetail));
            m_readbackSink(csr, siglblid);
        }, sizeof(fieldRegs), rrv::gs::Worker::Limits{}.fields);
    }
    else
    {
        const int fieldResult = m_worker && !direct ? m_worker->invoke(consumeField, sizeof(fieldRegs), 1) : consumeField();
        if (fieldResult == 0)
        {
            return setError(error, bridgeError("vsync", detail));
        }
    }
    rrv::m2causal::event(rrv::m2causal::EventType::GsVsyncAcknowledged,
                         rrv::m2causal::Source::Backend, fieldIndex, fieldParity,
                         m_nextGsFieldEpochId);
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    if (!consumerReceiptBoundaryOrderingUnlocked(&consumerBoundaryOrdering, error))
        return false;
#endif
    // Do not use drawable/present statistics here. A host may have no drawable
    // (occlusion, minimize, lifecycle transition) while the guest field and
    // its completed GS workload remain valid and must receive one candidate.
    m_openFieldEpoch.guestFieldId = fieldIndex;
    m_openFieldEpoch.gsFieldEpochId = m_nextGsFieldEpochId++;
    m_openFieldEpoch.presentationCandidateId = m_nextPresentationCandidateId++;
    m_openFieldEpoch.fieldParity = fieldParity;
    if (m_openFieldEpoch.canonicalWorkloadHashAvailable)
    {
        m_openFieldEpoch.canonicalWorkloadHash =
            finalizeEpochHash(m_openFieldEpoch.canonicalWorkloadHash);
    }
    if (m_completedFieldEpochRingSize == kCompletedFieldEpochHistoryCapacity)
    {
        m_completedFieldEpochRing[m_completedFieldEpochRingStart] = m_openFieldEpoch;
        m_completedFieldEpochRingStart =
            (m_completedFieldEpochRingStart + 1u) % kCompletedFieldEpochHistoryCapacity;
        ++m_droppedCompletedFieldEpochs;
    }
    else
    {
        const size_t writeIndex =
            (m_completedFieldEpochRingStart + m_completedFieldEpochRingSize) %
            kCompletedFieldEpochHistoryCapacity;
        m_completedFieldEpochRing[writeIndex] = m_openFieldEpoch;
        ++m_completedFieldEpochRingSize;
    }
    ++m_totalCompletedFieldEpochs;
    m_fieldTransitions.fetch_add(1u, std::memory_order_relaxed);
    rrv::m2neutral::noteCompletedField(
        m_openFieldEpoch.guestFieldId, m_openFieldEpoch.gsFieldEpochId,
        m_openFieldEpoch.fieldParity, m_openFieldEpoch.pathEventCounts.data(),
        m_openFieldEpoch.canonicalWorkloadHashAvailable,
        m_openFieldEpoch.canonicalWorkloadHash, &consumerBoundaryOrdering);
    m_openFieldEpoch = {};
#if defined(RRV_GS_EPOCH_DIAGNOSTICS)
    m_openFieldEpoch.canonicalWorkloadHashAvailable = true;
    m_openFieldEpoch.canonicalWorkloadHash = hashEpochPrologue();
#endif
    m_lastGuestFieldId = fieldIndex;
    m_lastFieldParity = fieldParity;
    m_hasCompletedGuestField = true;
#if defined(RRV_GS_FIELD_ONLY)
    static std::atomic<bool> s_reportedFieldVsync{false};
    if (!s_reportedFieldVsync.exchange(true, std::memory_order_relaxed))
    {
        std::fprintf(stderr,
                     "[m0r-runtime] field-mode path executed field=%llu parity=%u\n",
                     static_cast<unsigned long long>(fieldIndex), fieldParity);
    }
#endif
    if (bridgeDiagVerbose())
        std::fprintf(stderr, "[gs-backend:vsync] end field=%llu parity=%u\n",
                     static_cast<unsigned long long>(fieldIndex), fieldParity);
    return true;
}

uint64_t Backend::ownerPost(std::function<void()> command, size_t bytes, size_t fields)
{
    if (!m_worker)
        throw std::logic_error("GS owner stream requires RRV_GS_EXECUTION=worker-sync");
    if (m_vuWorker)
        return m_vuWorker->submit([this, command = std::move(command)] {
            command();
            flushSplitBatch(); // a batch never outlives its owner command
        }, bytes, fields);
    return m_worker->submit(std::move(command), bytes, fields);
}

void Backend::ownerWait(uint64_t sequence)
{
    if (m_vuWorker)
    {
        // The command's own packets (and their readbacks) run on rrv-gs-owner.
        m_vuWorker->wait(sequence);
        m_worker->fence();
        return;
    }
    if (m_worker)
        m_worker->wait(sequence);
}

void Backend::ownerFence()
{
    if (m_vuWorker)
        m_vuWorker->fence(); // everything rrv-vu1 will queue is queued after this
    if (m_worker)
        m_worker->fence();
}

bool Backend::startVuSplit(ReadbackSink sink, std::string *error)
{
    if (!m_worker)
        return setError(error, "RRV_VU1GS_SPLIT requires RRV_GS_EXECUTION=worker-sync");
    if (m_vuWorker)
        return true;
    if (!sink)
        return setError(error, "RRV_VU1GS_SPLIT needs a readback sink");
    m_readbackSink = std::move(sink);
    rrv::gs::Worker::Limits limits;
    limits.threadName = "rrv-vu1";
    m_vuWorker = std::make_unique<rrv::gs::Worker>(limits);
    return true;
}

void Backend::drainGsFromVu()
{
    if (!readbackDeferred())
        return;
    flushSplitBatch();
    m_worker->fence();
}

void Backend::flushSplitBatch()
{
    if (!readbackDeferred() || m_splitBatch.packets.empty())
        return;
    auto batch = std::make_shared<SplitBatch>(std::move(m_splitBatch));
    m_splitBatch = SplitBatch{};
    const size_t bytes = batch->bytes.size();
    // CSR/SIGLBLID once after the batch: the owner copy keeps only the latest
    // readback and the guest sees it only at a barrier, which flushes first.
    m_worker->submit([this, batch, fp = splitReadFpControl()] {
        RRV_CPU_LOG_GS_SCOPE();
        SplitFpControlScope scoped(fp);
        std::array<char, kErrorCapacity> ownerDetail{};
        size_t offset = 0u;
        for (const auto &packet : batch->packets)
        {
            const uint8_t *data = packet.size ? batch->bytes.data() + offset : nullptr;
            verifyConsume(packet.verification, 1, packet.pathId, data, packet.size);
            if (m_submit(m_bridge, packet.pathId, data, packet.size, ownerDetail.data(),
                         static_cast<uint32_t>(ownerDetail.size())) == 0)
                throw std::runtime_error("PCSX2 GS bridge packet dispatch failed: " + bridgeError("submit", ownerDetail));
            offset += packet.size;
        }
        uint64_t csr = 0u, siglblid = 0u;
        if (m_readback(m_bridge, &csr, &siglblid, ownerDetail.data(),
                       static_cast<uint32_t>(ownerDetail.size())) == 0)
            throw std::runtime_error("PCSX2 GS bridge readback failed: " + bridgeError("readback", ownerDetail));
        m_readbackSink(csr, siglblid);
    }, bytes);
}

bool Backend::readLocalMemory(uint8_t *outBytes, uint32_t byteCount,
                              uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                              std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (byteCount == 0u)
        return setError(error, "PCSX2 GS local-memory read requires a non-zero byte count");
    if (!outBytes)
        return setError(error, "PCSX2 GS local-memory read has a null destination");

    std::array<char, kErrorCapacity> detail{};
    if (rrv::gstrace::recording())
    {
        rrv::gstrace::ReadLocalPayload payload{bitbltbuf, trxpos, trxreg, byteCount, 0u};
        rrv::gstrace::write(rrv::gstrace::EventTag::ReadLocal, 0u, &payload,
                            sizeof(payload));
    }
    if (ownerCall(m_readLocalMemory, m_bridge, outBytes, byteCount, bitbltbuf, trxpos, trxreg,
                          detail.data(), static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("local-memory read", detail));
    }
    return true;
}

bool Backend::snapshotLocalMemory(std::vector<uint8_t> &outBytes, std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");

    // The bridge is allowed to have written a partial temporary image when it
    // reports a failure.  The caller's prior snapshot remains its only valid
    // state until the complete native image has been acknowledged.
    std::vector<uint8_t> candidate(kBridgeLocalMemoryBytes);
    std::array<char, kErrorCapacity> detail{};
    if (rrv::gstrace::recording())
        rrv::gstrace::write(rrv::gstrace::EventTag::SnapshotLocal, 0u, nullptr, 0u);
    if (ownerCall(m_snapshotLocalMemory, m_bridge, candidate.data(), kBridgeLocalMemoryBytes,
                              detail.data(), static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("local-memory snapshot", detail));
    }
    outBytes = std::move(candidate);
    return true;
}

bool Backend::restoreLocalMemory(const uint8_t *bytes, uint32_t byteCount,
                                 std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (!bytes || byteCount != kBridgeLocalMemoryBytes)
        return setError(error, "PCSX2 GS local-memory restore requires a complete 4 MiB image");

    std::array<char, kErrorCapacity> detail{};
    if (rrv::gstrace::recording())
        rrv::gstrace::write(rrv::gstrace::EventTag::RestoreLocal, 0u, bytes, byteCount);
    if (ownerCall(m_restoreLocalMemory, m_bridge, bytes, byteCount, detail.data(),
                             static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("local-memory restore", detail));
    }
    rrv::m2causal::event(rrv::m2causal::EventType::LocalMemoryRestoreAcknowledged,
                         rrv::m2causal::Source::Backend, byteCount);
    return true;
}

bool Backend::copyFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth,
                        uint32_t &outHeight, std::string *error)
{
    // Runs on the frontend/present thread and takes the same backend mutex as
    // submit(), so it is the second way another thread stalls the guest thread.
    const uint64_t cadenceStart = rrv::cadence::enabled() ? rrv::cadence::nowNs() : 0u;
    struct CadenceSnap
    {
        uint64_t start;
        ~CadenceSnap()
        {
            rrv::cadence::addCrossThread(rrv::cadence::g_snapGsNs,
                                         rrv::cadence::g_snapGsCalls, start);
        }
    } cadenceSnap{cadenceStart};
    std::lock_guard<std::mutex> lock(m_mutex);
    outPixels.clear();
    outWidth = 0u;
    outHeight = 0u;
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (m_presentationMode == PresentationMode::DirectGpu)
    {
        return setError(error,
                        "direct GPU presentation forbids copyFrame; request an explicit diagnostic capture instead");
    }

    BridgeFrame frame{sizeof(BridgeFrame), 0u, 0u, 0u, nullptr, 0u};
    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_snapshot, m_bridge, &frame, detail.data(), static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("snapshot", detail));
    }

    const uint64_t minimumStride = static_cast<uint64_t>(frame.width) * 4u;
    const uint64_t bytes = static_cast<uint64_t>(frame.stride_bytes) * frame.height;
    if (!frame.rgba || frame.width == 0u || frame.height == 0u || frame.stride_bytes < minimumStride ||
        bytes > std::numeric_limits<size_t>::max())
    {
        return setError(error, "PCSX2 GS bridge returned an invalid RGBA frame");
    }

    const size_t packedBytes = static_cast<size_t>(minimumStride) * frame.height;
    outPixels.resize(packedBytes);
    for (uint32_t row = 0u; row < frame.height; ++row)
    {
        std::memcpy(outPixels.data() + static_cast<size_t>(row) * minimumStride,
                    frame.rgba + static_cast<size_t>(row) * frame.stride_bytes,
                    static_cast<size_t>(minimumStride));
    }
    outWidth = frame.width;
    outHeight = frame.height;
#if defined(RRV_GS_FIELD_ONLY)
    static std::atomic<bool> s_reportedCpuSnapshot{false};
    if (!s_reportedCpuSnapshot.exchange(true, std::memory_order_relaxed))
    {
        std::fprintf(stderr,
                     "[m0r-runtime] diagnostic CPU RGBA snapshot executed "
                     "sequence=%llu size=%ux%u\n",
                     static_cast<unsigned long long>(frame.sequence),
                     frame.width, frame.height);
    }
#endif

    // RRV_FRAME_FLATNESS=1 (default off) — per-present detail census.
    //
    // Locating a "a few polygons fill the whole screen" glitch by eye means
    // scrubbing hundreds of frames. A frame owned by one or two huge primitives
    // is SMOOTH: its horizontal luma gradient collapses, because there are no
    // texture or silhouette edges left. A correct frame of the same scene is
    // full of them. This prints one number per present so the offending frames
    // can be named by index instead of guessed at, and a bounded capture aimed
    // only at them. Observational only.
    static const bool s_flatness = [] {
        const char *v = std::getenv("RRV_FRAME_FLATNESS");
        return v && v[0] && v[0] != '0';
    }();
    if (s_flatness && frame.width >= 2u && frame.height >= 2u)
    {
        static uint64_t s_present = 0u;
        const uint32_t w = frame.width, h = frame.height;
        // Subsample: every 4th row, every 2nd pixel. Enough for a ratio, cheap
        // enough not to perturb the present path.
        uint64_t sum = 0u, n = 0u;
        for (uint32_t y = 0u; y < h; y += 4u)
        {
            const uint8_t *row = outPixels.data() + static_cast<size_t>(y) * minimumStride;
            for (uint32_t x = 2u; x < w; x += 2u)
            {
                const uint8_t *a = row + static_cast<size_t>(x - 2u) * 4u;
                const uint8_t *b = row + static_cast<size_t>(x) * 4u;
                // Rec.601-ish luma, integer.
                const int la = (a[0] * 77 + a[1] * 150 + a[2] * 29) >> 8;
                const int lb = (b[0] * 77 + b[1] * 150 + b[2] * 29) >> 8;
                sum += static_cast<uint64_t>(la > lb ? la - lb : lb - la);
                ++n;
            }
        }
        const double detail = n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
        std::fprintf(stderr, "[frame:flat] present=%llu %ux%u detail=%.4f\n",
                     (unsigned long long)s_present++, w, h, detail);
    }

    // G1 presentation-boundary probe (RRV_GS_STAGE_DUMP=<dir>, default off).
    // Stage 2 of three: the EXACT RGBA the ABI-v3 bridge returned, before any
    // RRV presentation processing. Logs the requested-vs-returned geometry and
    // the row pitch, because the suspected defect is a composed 640x448 frame
    // being treated downstream as a 640x224 field.
    {
        static const char *s_dir = std::getenv("RRV_GS_STAGE_DUMP");
        static uint64_t s_idx = 0;
        if (s_dir && s_dir[0])
        {
            static uint32_t s_lastW = 0, s_lastH = 0, s_lastStride = 0;
            if (frame.width != s_lastW || frame.height != s_lastH ||
                frame.stride_bytes != s_lastStride || s_idx < 3)
            {
                std::fprintf(stderr,
                             "[stage2:bridge] idx=%llu returned=%ux%u stride=%u "
                             "packedStride=%llu seq=%llu\n",
                             (unsigned long long)s_idx, frame.width, frame.height,
                             frame.stride_bytes, (unsigned long long)minimumStride,
                             (unsigned long long)frame.sequence);
                s_lastW = frame.width;
                s_lastH = frame.height;
                s_lastStride = frame.stride_bytes;
            }
            static const uint64_t s_from = [] {
                const char *v = std::getenv("RRV_GS_STAGE_DUMP_FROM");
                return v && v[0] ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            static const uint64_t s_to = [] {
                const char *v = std::getenv("RRV_GS_STAGE_DUMP_TO");
                return v && v[0] ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            if (s_idx >= s_from && s_idx <= s_to)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/stage2_%05llu.ppm", s_dir,
                              (unsigned long long)s_idx);
                if (FILE *fp = std::fopen(path, "wb"))
                {
                    std::fprintf(fp, "P6\n%u %u\n255\n", frame.width, frame.height);
                    for (uint32_t y = 0; y < frame.height; ++y)
                    {
                        const uint8_t *row = outPixels.data() + static_cast<size_t>(y) * minimumStride;
                        for (uint32_t x = 0; x < frame.width; ++x)
                            std::fwrite(row + x * 4u, 1, 3, fp);
                    }
                    std::fclose(fp);
                }
            }
            ++s_idx;
        }
    }
    return true;
}

bool Backend::resize(uint32_t widthPixels, uint32_t heightPixels, float backingScale,
                     std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (m_presentationMode != PresentationMode::DirectGpu)
        return setError(error, "resize of a native presentation surface requires direct GPU mode");
    if (std::this_thread::get_id() != m_directSurfaceThread)
        return setError(error,
                        "direct presentation resize must run on the thread which created the native surface");
    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_resize, m_bridge, widthPixels, heightPixels, backingScale, detail.data(),
                 static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("direct-present resize", detail));
    }
    return true;
}

bool Backend::capture(const CaptureTag &tag, CaptureResult &result, std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    result = {};
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    if (m_nextCaptureId == 0u)
        return setError(error, "diagnostic capture identifier space is exhausted");
    bool hasSourceGsFieldEpoch = false;
    uint64_t sourceGsFieldEpochId = 0u;
    for (size_t index = 0u; index != m_completedFieldEpochRingSize; ++index)
    {
        const size_t ringIndex =
            (m_completedFieldEpochRingStart + index) % kCompletedFieldEpochHistoryCapacity;
        if (m_completedFieldEpochRing[ringIndex].guestFieldId == tag.fieldIndex)
        {
            hasSourceGsFieldEpoch = true;
            sourceGsFieldEpochId = m_completedFieldEpochRing[ringIndex].gsFieldEpochId;
            break;
        }
    }
    if (!hasSourceGsFieldEpoch && tag.fieldIndex != 0u)
    {
        return setError(error,
                        "diagnostic capture does not refer to a retained completed epoch");
    }

    const BridgeCaptureRequest request{sizeof(BridgeCaptureRequest), 0u, tag.guestTick,
                                       tag.fieldIndex, tag.presentIndex};
    BridgeCaptureResult bridgeResult{sizeof(BridgeCaptureResult), 0u, 0u, 0u, 0u,
                                     {sizeof(BridgeFrame), 0u, 0u, 0u, nullptr, 0u}};
    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_capture, m_bridge, &request, &bridgeResult, detail.data(),
                  static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("diagnostic capture", detail));
    }
    if (bridgeResult.guest_tick != tag.guestTick ||
        bridgeResult.field_index != tag.fieldIndex ||
        bridgeResult.present_index != tag.presentIndex)
    {
        return setError(error,
                        "PCSX2 GS bridge returned a diagnostic capture for a different field/present boundary");
    }

    const BridgeFrame &frame = bridgeResult.frame;
    const uint64_t packedStride = static_cast<uint64_t>(frame.width) * 4u;
    const uint64_t sourceBytes = static_cast<uint64_t>(frame.stride_bytes) * frame.height;
    if (!frame.rgba || frame.width == 0u || frame.height == 0u ||
        frame.stride_bytes < packedStride || sourceBytes > std::numeric_limits<size_t>::max())
    {
        return setError(error, "PCSX2 GS bridge returned an invalid diagnostic capture");
    }
    result.rgba.resize(static_cast<size_t>(packedStride) * frame.height);
    for (uint32_t row = 0u; row < frame.height; ++row)
    {
        std::memcpy(result.rgba.data() + static_cast<size_t>(row) * packedStride,
                    frame.rgba + static_cast<size_t>(row) * frame.stride_bytes,
                    static_cast<size_t>(packedStride));
    }
    result.tag = {bridgeResult.guest_tick, bridgeResult.field_index,
                  bridgeResult.present_index};
    result.captureId = m_nextCaptureId++;
    result.hasSourceGsFieldEpoch = hasSourceGsFieldEpoch;
    result.sourceGsFieldEpochId = sourceGsFieldEpochId;
    result.sequence = frame.sequence;
    result.width = frame.width;
    result.height = frame.height;
    return true;
}

bool Backend::presentationStats(PresentationStats &stats, std::string *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    stats = {};
    if (!m_active)
        return setError(error, "PCSX2 GS bridge is not active");
    BridgeStats bridgeStats{sizeof(BridgeStats), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    std::array<char, kErrorCapacity> detail{};
    if (ownerCall(m_stats, m_bridge, &bridgeStats, detail.data(),
                static_cast<uint32_t>(detail.size())) == 0)
    {
        return setError(error, bridgeError("presentation stats", detail));
    }
    stats = {bridgeStats.presented_frames, bridgeStats.direct_gpu_presents,
             bridgeStats.requested_captures, bridgeStats.completed_captures,
             bridgeStats.synchronous_cpu_readbacks, bridgeStats.unexpected_readbacks,
             bridgeStats.cpu_waits};
    return true;
}

std::vector<CompletedFieldEpoch> Backend::completedFieldEpochs() const
{
    return completedFieldEpochHistory().epochs;
}

CompletedFieldEpochHistory Backend::completedFieldEpochHistory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CompletedFieldEpochHistory history{};
    history.totalCompletedEpochs = m_totalCompletedFieldEpochs;
    history.droppedCompletedEpochs = m_droppedCompletedFieldEpochs;
    history.epochs.reserve(m_completedFieldEpochRingSize);
    for (size_t index = 0u; index != m_completedFieldEpochRingSize; ++index)
    {
        const size_t ringIndex =
            (m_completedFieldEpochRingStart + index) % kCompletedFieldEpochHistoryCapacity;
        history.epochs.push_back(m_completedFieldEpochRing[ringIndex]);
    }
    return history;
}
} // namespace rrv::gsbackend
