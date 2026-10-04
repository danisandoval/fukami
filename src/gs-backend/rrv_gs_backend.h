#ifndef RRV_GS_BACKEND_H
#define RRV_GS_BACKEND_H

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include "rrv_gs_worker.h"
#include <string>
#include <thread>
#include <vector>

#include "rrv_gs_render_mode.h"

namespace rrv::m2neutral { struct ConsumerBoundaryOrdering; }

// RRV-owned boundary around an optional PCSX2 GS bridge.  Nothing in this
// header depends on PCSX2 headers or C++ types: the bridge is loaded at run
// time through its versioned C ABI.
namespace rrv::gsbackend
{
    enum class RendererKind : uint32_t
    {
        Unknown = 0u,
        Software = 1u,
        Metal = 2u,
        Auto = 3u,
        Vulkan = 4u, // Gate 5 (Linux/Steam Deck)
    };

    enum class PresentationMode : uint32_t
    {
        LegacyCpuSnapshot = 0u,
        DirectGpu = 1u,
    };

    enum class NativeSurfaceKind : uint32_t
    {
        None = 0u,
        MacosViewMetalLayer = 1u,
        // Gate 5 (Linux/Steam Deck); same values as the bridge ABI.
        LinuxX11 = 2u,
        LinuxWayland = 3u,
    };

    // Portable native-surface description. Handles are opaque and borrowed:
    // the platform shell retains them until Backend::shutdown() returns. On
    // macOS SDL owns an SDL_Window and supplies its SDL_MetalView/CAMetalLayer
    // pair; only the bridge's Objective-C++ adapter interprets the objects.
    // RRV serializes initialize/resize/capture/shutdown with GS work. Surface
    // initialization and resize run on the platform main thread; Backend
    // rejects a resize from another thread.
    struct NativeSurface
    {
        NativeSurfaceKind kind = NativeSurfaceKind::None;
        void *nativeView = nullptr;
        void *nativeLayer = nullptr;
        uint32_t widthPixels = 0u;
        uint32_t heightPixels = 0u;
        float backingScale = 1.0f;
        bool mainThreadPrepared = false;
    };

    struct InitializeOptions
    {
        uint32_t snapshotWidth = 0u;
        uint32_t snapshotHeight = 0u;
        // Explicit portable renderer request. Unknown preserves the diagnostic
        // bridge's environment-selected legacy behavior; product targets must
        // request their renderer directly through the ABI.
        RendererKind rendererKind = RendererKind::Unknown;
        PresentationMode presentationMode = PresentationMode::LegacyCpuSnapshot;
        NativeSurface surface{};
    };

    struct CaptureTag
    {
        uint64_t guestTick = 0u;
        uint64_t fieldIndex = 0u;
        uint64_t presentIndex = 0u;
    };

    struct CaptureResult
    {
        CaptureTag tag{};
        // Backend-local diagnostic identity. This is allocated only after an
        // explicit capture succeeds; it never authorizes guest work or a GS
        // field epoch.
        uint64_t captureId = 0u;
        // Completed epoch observed by this capture. Pre-display blank capture
        // is represented explicitly instead of overloading guest field zero.
        bool hasSourceGsFieldEpoch = false;
        uint64_t sourceGsFieldEpochId = 0u;
        uint64_t sequence = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::vector<uint8_t> rgba;
    };

    struct PresentationStats
    {
        uint64_t presentedFrames = 0u;
        uint64_t directGpuPresents = 0u;
        uint64_t requestedCaptures = 0u;
        uint64_t completedCaptures = 0u;
        uint64_t synchronousCpuReadbacks = 0u;
        uint64_t unexpectedReadbacks = 0u;
        uint64_t cpuWaits = 0u;
    };

    // A completed GuestFieldEpoch at the post-GIF/PCSX2 bridge boundary.
    //
    // `guestFieldId` is the field index supplied to vsync(). `gsFieldEpochId`
    // and `presentationCandidateId` are backend-local, monotonically
    // increasing identities allocated after a successful GSvsync. The latter
    // exists even when the host has no drawable; actual drawable presents are
    // intentionally not used to define an epoch.
    //
    // In RRV_GS_EPOCH_DIAGNOSTICS builds, `canonicalWorkloadHash` is standard
    // 64-bit FNV-1a over this exact byte stream:
    //   ASCII bytes "RRV-GS-EPOCH-V1" (without a NUL), followed for each
    //   successfully forwarded submit(), in call order, by:
    //     Submit tag 0x01; pathId as u8; sizeBytes as little-endian u32;
    //     raw GIF bytes; then a single EndEpoch tag 0xff.
    // The explicit record and end tags make this serialization self-delimiting
    // even when a tool serializes multiple field workloads consecutively.
    // No field/candidate/presentation identity is included, so identical
    // workloads in distinct fields intentionally retain the same hash. The
    // production build does not hash payload bytes while guest scheduling is
    // wall-clock coupled; it still retains identities and counts.
    struct CompletedFieldEpoch
    {
        uint64_t guestFieldId = 0u;
        uint64_t gsFieldEpochId = 0u;
        uint64_t presentationCandidateId = 0u;
        uint32_t fieldParity = 0u;
        uint64_t submitCount = 0u;
        uint64_t submitBytes = 0u;
        std::array<uint64_t, 3u> pathEventCounts{};
        std::array<uint64_t, 3u> pathByteCounts{};
        // Hashing every payload byte is diagnostic-only because doing it in a
        // wall-clock-coupled product run could itself move guest work between
        // fields. Production identity/count instrumentation leaves this false.
        bool canonicalWorkloadHashAvailable = false;
        uint64_t canonicalWorkloadHash = 0u;
    };

    inline constexpr size_t kCompletedFieldEpochHistoryCapacity = 4096u;

    // The recent completed-epoch ring is bounded in production. Totals retain
    // loss visibility for long runs without retaining all game-derived event
    // metadata in memory; `epochs` is ordered oldest-to-newest.
    struct CompletedFieldEpochHistory
    {
        uint64_t totalCompletedEpochs = 0u;
        uint64_t droppedCompletedEpochs = 0u;
        std::vector<CompletedFieldEpoch> epochs;
    };

    // Actual renderer kind reported by the bridge after GSopen, never the
    // requested option. Unknown when no bridge is active.
    RendererKind activeRendererKind();

    // Semantic-anchor hook for the optional M2 consumer-side receipt client.
    // It is a compiled no-op unless RRV_GS_CONSUMER_RECEIPT_CLIENT is enabled;
    // it never closes or authorizes a guest field.
    void requestCanonicalConsumerReceipt();
#if defined(RRV_M2_NEUTRALITY_CONTROL)
    bool captureActiveCompletedField(uint64_t guestFieldId, CaptureResult &result,
                                     std::string *error);
#endif

    class Backend
    {
    public:
        Backend();
        ~Backend();

        Backend(const Backend &) = delete;
        Backend &operator=(const Backend &) = delete;

        // Reads RRV_GS_BACKEND (legacy|pcsx2). PCSX2 opens only the bridge
        // named by the generated source-derived package binding; environment,
        // bundle and build-path bridge selection are not runtime authority.
        // The package lease remains held until bridge shutdown and dlclose.
        bool initialize(uint32_t snapshotWidth, uint32_t snapshotHeight,
                        std::string *error);
        bool initialize(const InitializeOptions &options, std::string *error);
        void shutdown();

        bool active() const;
        const char *name() const;
        // Human-readable renderer for the window title / logs, e.g.
        // "PCSX2 Software", "PCSX2 Metal", "Legacy SW". See the note on the
        // implementation: the PCSX2 half mirrors the bridge's own env parsing.
        const char *displayName() const;
        // Parsed once during initialize(). This is policy owned by RRV, not a
        // renderer-specific setting or a hot-switchable mode.
        // renderMode(): what the runtime must compose above the renderer. A
        // PCSX2 bridge renders Gate-6 full-frame itself from the unchanged
        // field stream, so this is Field whenever the bridge is active.
        rrv::gs::GsRenderMode renderMode() const;
        // outputRenderMode(): the mode the player sees (field or full).
        rrv::gs::GsRenderMode outputRenderMode() const;
        // Bitset of rrv::gs::renderModeCapability() values advertised by the
        // selected renderer. Legacy advertises Field only.
        uint32_t supportedRenderModes() const;
        PresentationMode presentationMode() const;
        uint32_t capabilityFlags() const;

        // `pathId` is post-arbitration provenance (1..3).  The bridge receives
        // the packet verbatim and must not re-run RRV's VIF/DMAC/GIF ownership.
        bool submit(uint32_t pathId, const uint8_t *gifBytes, uint32_t sizeBytes,
                    std::string *error);
        // Legacy renderer register feedback. This reflects the renderer's
        // privileged snapshot, not an independent guest event implementation;
        // producer-control candidates must not copy it into their guest bank.
        bool readback(uint64_t &outCsr, uint64_t &outSiglblid, std::string *error);

        // Optional producer-control extension: ordered, synchronous CSR soft
        // reset (PCSX2 GSreset(false)). Resets renderer/parser state, retains
        // local memory, and never publishes guest events or changes clocks.
        // Legacy ABI-5 bridges remain usable; this call fails if unsupported.
        bool resetGs(std::string *error);

        // Field state and the complete 19-register privileged-GS snapshot are
        // forwarded at the host presentation boundary.  `copyFrame()` returns
        // a tightly packed RGBA8 image after this call has completed.
        bool vsync(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                   std::string *error);
        // Copy a completed GS local-to-host transfer from PCSX2's local
        // memory. The transfer descriptors are the guest's original A+D
        // values, so RRV remains responsible for guest memory ownership.
        bool readLocalMemory(uint8_t *outBytes, uint32_t byteCount,
                             uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                             std::string *error);
        // Complete 4 MiB GS local-memory state for RRV snapshot/restore.
        // Calls are serialized with packet submission, readback and vsync.
        bool snapshotLocalMemory(std::vector<uint8_t> &outBytes, std::string *error);
        bool restoreLocalMemory(const uint8_t *bytes, uint32_t byteCount,
                                std::string *error);
        // Legacy CPU presentation only. DirectGpu fails closed here so every
        // direct-path CPU readback is an explicit capture() request.
        bool copyFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth,
                       uint32_t &outHeight, std::string *error);
        bool resize(uint32_t widthPixels, uint32_t heightPixels, float backingScale,
                    std::string *error);
        bool capture(const CaptureTag &tag, CaptureResult &result, std::string *error);
        bool presentationStats(PresentationStats &stats, std::string *error);
        // Field transitions handed to the bridge so far (vsync or vsyncDeferred).
        // Lock-free and without a GS-thread round trip: an upper bound on the
        // direct presents, for a watcher that must not disturb the producers
        // (presentationStats() holds the producer mutex while it waits for the
        // GS thread).
        uint64_t fieldTransitionCount() const { return m_fieldTransitions.load(std::memory_order_relaxed); }

        // Gate-4 VU1+GS owner stream (docs/evidence/GATE4_VU1_GS_WORKER_DESIGN_2026-09-25.md
        // S2/S3). Available only with the GS worker (RRV_GS_EXECUTION=worker-sync).
        // ownerPost() queues an OWNED command on the GS owner thread. Code running
        // there may call this Backend directly: owner-thread calls bypass the
        // worker queue and m_mutex, because the owner already serializes every
        // bridge call (other threads reach the bridge only through the queue).
        bool ownerStreamAvailable() const { return m_worker != nullptr; }
        uint64_t ownerPost(std::function<void()> command, size_t bytes, size_t fields);
        void ownerWait(uint64_t sequence);
        void ownerFence();

        // Gate-5 VU1/GS split (RRV_VU1GS_SPLIT=1, owner-async only). Owner
        // commands (VIF1, VU1, the GIF arbiter) move to a second thread,
        // rrv-vu1; rrv-gs-owner keeps only the bridge. From rrv-vu1, submit()
        // queues an owned packet copy WITHOUT waiting and reads CSR/SIGLBLID
        // on rrv-gs-owner right after that packet, handing them to the sink
        // (so readbackDeferred() tells the caller not to read back itself).
        // ownerWait()/ownerFence() then wait for rrv-vu1 AND rrv-gs-owner, so
        // the guest sees owner results at exactly the same barriers as before.
        using ReadbackSink = std::function<void(uint64_t csr, uint64_t siglblid)>;
        bool startVuSplit(ReadbackSink sink, std::string *error);
        bool vuSplitActive() const { return m_vuWorker != nullptr; }
        bool readbackDeferred() const { return m_vuWorker && m_vuWorker->ownsThisThread(); }
        // From rrv-vu1: wait until rrv-gs-owner has run every packet queued so
        // far (and so delivered every deferred readback). No-op elsewhere.
        void drainGsFromVu();
        // From rrv-vu1: hand the packets gathered so far to rrv-gs-owner as one
        // command (see SplitBatch). Called at the end of every owner command and
        // before any call that reaches the bridge synchronously.
        void flushSplitBatch();

        // Gate-5 split, from rrv-vu1: the field transition without the wait.
        // vsync() + readback() make rrv-vu1 sleep until rrv-gs-owner has run
        // the field's packets and presented (3.2-3.8 ms per field on the Steam
        // Deck, docs/evidence/GATE5_STEAMDECK_PROFILE_2026-10-02.md). This
        // queues the transition behind the field's packets and returns. On
        // rrv-gs-owner, in order: `prepare` (patches the 19-register snapshot
        // with owner state the queued packets produce), the bridge vsync, then
        // CSR/SIGLBLID to the readback sink. At most one transition is pending,
        // so rrv-vu1 runs at most one field ahead of the GS. A bridge failure
        // fails the GS worker; the next wait or fence rethrows it. The guest
        // sees the result at the same barriers as before (ownerWait/ownerFence
        // wait for both threads). fieldDeferrable() is false off rrv-vu1 and
        // whenever a diagnostic needs the synchronous order (GS trace, worker
        // verification, the consumer receipt client): use vsync() then.
        using FieldPrepare = std::function<void(uint64_t *regs19)>;
        bool fieldDeferrable() const;
        bool vsyncDeferred(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                           FieldPrepare prepare, std::string *error);

        // Returns immutable snapshots of epochs already closed by a successful
        // vsync. It intentionally exposes no mutable in-progress epoch and is
        // suitable for diagnostic manifests and deterministic tests.
        std::vector<CompletedFieldEpoch> completedFieldEpochs() const;
        CompletedFieldEpochHistory completedFieldEpochHistory() const;

    private:
        bool loadBridge(const InitializeOptions &options, std::string *error);
        bool fieldTransition(const uint64_t *regs19, uint64_t fieldIndex, uint32_t fieldParity,
                             FieldPrepare *deferred, std::string *error);
        void shutdownUnlocked();
        void resetEpochBookkeepingUnlocked();
        bool validateNextGuestFieldUnlocked(uint64_t fieldIndex, uint32_t fieldParity,
                                            std::string *error) const;
        void noteSubmittedPacketUnlocked(uint32_t pathId, const uint8_t *gifBytes,
                                         uint32_t sizeBytes);
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
        bool scheduleConsumerReceiptAfterVsyncIfRequestedUnlocked(std::string *error);
        bool consumerReceiptBoundaryOrderingUnlocked(
            rrv::m2neutral::ConsumerBoundaryOrdering *ordering,
            std::string *error);
#endif

        // Producer serialization remains m_mutex. Worker callbacks never acquire
        // it or any guest lock; synchronous borrowed arguments remain live until
        // the fence returns. Asynchronous packet commands use owned storage.
        bool ownerDirect() const { return m_worker && m_worker->ownsThisThread(); }
        std::unique_lock<std::mutex> producerLock()
        {
            return ownerDirect() ? std::unique_lock<std::mutex>() : std::unique_lock<std::mutex>(m_mutex);
        }
        template<class F, class... Args>
        auto ownerCall(F function, Args... args) -> decltype(function(args...))
        {
            if (!m_worker || m_worker->ownsThisThread())
                return function(args...);
            flushSplitBatch(); // rrv-vu1: earlier packets first (no-op elsewhere)
            return m_worker->invoke([=] { return function(args...); });
}
        std::unique_ptr<rrv::gs::Worker> m_worker;
        std::unique_ptr<rrv::gs::Worker> m_vuWorker; // Gate-5 split; see startVuSplit()
        ReadbackSink m_readbackSink;
        // Packets queued from rrv-vu1 travel in batches (one GS command per up to
        // 64 packets / 256 KiB instead of one per packet); rrv-vu1 only.
        struct SplitBatch
        {
            std::vector<uint8_t> bytes;
            struct Packet { uint32_t pathId, size; uint64_t verification; };
            std::vector<Packet> packets;
        };
        SplitBatch m_splitBatch;
        using OwnerCommandFn = void (*)(void (*)(void *), void *);
        using OwnerInitFn = void (*)();
        using PumpMainFn = int (*)();
        using PrepareOwnerFn = int (*)(void *, char *, uint32_t);
        OwnerCommandFn m_ownerCommand = nullptr;
        PumpMainFn m_pumpMain = nullptr;
        bool m_workerRequested = false;
        bool m_workerVerify = false;
        uint64_t m_verifySubmitted = 0, m_verifyConsumed = 0;
        uint64_t m_verifyProducerHash = 14695981039346656037ull;
        uint64_t m_verifyConsumerHash = 14695981039346656037ull;
        uint64_t verifyEnqueue(uint64_t kind, uint64_t identity, const void* data, size_t size);
        void verifyConsume(uint64_t sequence, uint64_t kind, uint64_t identity,
                           const void* data, size_t size);


        using CreateFn = void *(*)(const void *, void *, char *, uint32_t);
        using DestroyFn = void (*)(void *);
        using SubmitFn = int (*)(void *, uint32_t, const uint8_t *, uint32_t,
                                 char *, uint32_t);
        using VsyncFn = int (*)(void *, const uint64_t[19], uint64_t, uint32_t,
                                char *, uint32_t);
        using SnapshotFn = int (*)(void *, void *, char *, uint32_t);
        using ResizeFn = int (*)(void *, uint32_t, uint32_t, float, char *, uint32_t);
        using CaptureFn = int (*)(void *, const void *, void *, char *, uint32_t);
        using StatsFn = int (*)(void *, void *, char *, uint32_t);
        using ReadbackFn = int (*)(void *, uint64_t *, uint64_t *, char *, uint32_t);
        using ResetGsFn = int (*)(void *, char *, uint32_t);
        using ReadLocalMemoryFn = int (*)(void *, uint8_t *, uint32_t, uint64_t,
                                          uint64_t, uint64_t, char *, uint32_t);
        using SnapshotLocalMemoryFn = int (*)(void *, uint8_t *, uint32_t,
                                              char *, uint32_t);
        using RestoreLocalMemoryFn = int (*)(void *, const uint8_t *, uint32_t,
                                             char *, uint32_t);
        using VersionFn = uint32_t (*)();
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
        using ReceiptArmFn = int (*)(void *, char *, uint32_t);
        using ReceiptArmAfterVsyncFn = int (*)(void *, char *, uint32_t);
        using ReceiptBoundaryOrderingFn = int (*)(void *, void *, char *, uint32_t);
        using ReceiptSetEpochIdentityFn = int (*)(void *, uint64_t, uint64_t,
                                                  char *, uint32_t);
#endif

        // Owner of the loaded bridge image. Its definition is private to the
        // backend translation unit: the product owns the ADR-0006 package
        // (reader lease plus native image, released only after bridge
        // destruction and worker teardown); the asset-free fake variant owns a
        // target-derived fixture handle and is never part of a package reader.
        // Keeping one declaration here gives both variants the same class
        // layout for every consumer compiled against this header.
        struct BridgeImage;
        struct BridgeImageDeleter
        {
            void operator()(BridgeImage *image) const noexcept;
        };
        std::unique_ptr<BridgeImage, BridgeImageDeleter> m_bridgeImage;
        void *m_bridge = nullptr;
        CreateFn m_create = nullptr;
        DestroyFn m_destroy = nullptr;
        SubmitFn m_submit = nullptr;
        VsyncFn m_vsync = nullptr;
        SnapshotFn m_snapshot = nullptr;
        ResizeFn m_resize = nullptr;
        CaptureFn m_capture = nullptr;
        StatsFn m_stats = nullptr;
        ReadbackFn m_readback = nullptr;
        ResetGsFn m_resetGs = nullptr;
        ReadLocalMemoryFn m_readLocalMemory = nullptr;
        SnapshotLocalMemoryFn m_snapshotLocalMemory = nullptr;
        RestoreLocalMemoryFn m_restoreLocalMemory = nullptr;
        VersionFn m_version = nullptr;
#if defined(RRV_GS_CONSUMER_RECEIPT_CLIENT)
        ReceiptArmFn m_receiptArm = nullptr;
        ReceiptArmAfterVsyncFn m_receiptArmAfterVsync = nullptr;
        ReceiptBoundaryOrderingFn m_receiptBoundaryOrdering = nullptr;
        ReceiptSetEpochIdentityFn m_receiptSetEpochIdentity = nullptr;
#endif
        rrv::gs::GsRenderMode m_renderMode = rrv::gs::GsRenderMode::Field;
        PresentationMode m_presentationMode = PresentationMode::LegacyCpuSnapshot;
        uint32_t m_supportedRenderModes =
            rrv::gs::renderModeCapability(rrv::gs::GsRenderMode::Field);
        std::string m_rendererName;
        uint32_t m_capabilityFlags = 0u;
        std::thread::id m_directSurfaceThread{};
        // The open accumulator accepts only successful bridge submit calls.
        // It is closed transactionally after bridge vsync succeeds, ensuring
        // failed calls cannot manufacture an epoch or move its boundary.
        CompletedFieldEpoch m_openFieldEpoch{};
        std::array<CompletedFieldEpoch, kCompletedFieldEpochHistoryCapacity>
            m_completedFieldEpochRing{};
        size_t m_completedFieldEpochRingStart = 0u;
        size_t m_completedFieldEpochRingSize = 0u;
        uint64_t m_totalCompletedFieldEpochs = 0u;
        uint64_t m_droppedCompletedFieldEpochs = 0u;
        uint64_t m_nextGsFieldEpochId = 1u;
        uint64_t m_nextPresentationCandidateId = 1u;
        uint64_t m_nextCaptureId = 1u;
        uint64_t m_lastGuestFieldId = 0u;
        uint32_t m_lastFieldParity = 0u;
        bool m_hasCompletedGuestField = false;
        // Read WITHOUT the mutex. active() is called once per GIF packet
        // (~900 per display list) and m_mutex is held for milliseconds at a
        // time by the presenter's copyFrame() and by every submit(); taking it
        // just to read a bool put the guest thread to sleep behind the present
        // path (measured: the top blocking site in a release profile). Every
        // operation that actually touches the bridge still takes the mutex --
        // this flag only says whether one was ever loaded.
        std::atomic<bool> m_active{false};
        std::atomic<uint64_t> m_fieldTransitions{0u};
        mutable std::mutex m_mutex;
    };
} // namespace rrv::gsbackend

#endif // RRV_GS_BACKEND_H
