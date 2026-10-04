// rrv_gs_record.cpp — GS-stream recorder (milestone B4). Implements the hooks
// declared in rrv_gs_record_hooks.h: write-only, env-gated, writes the .gsr
// format declared in rrv_gs_record_format.h.
//
// Threading: GifArbiter::submit() runs on whichever thread calls
// PS2Memory::submitGifPacket (the EE/game thread in RRV's single-sim-thread
// model); GS::latchHostPresentationFrame runs on the present thread. Both
// funnels take the same file mutex, mirroring B1's approach (serialize under
// a mutex; the cost is off the hot per-pixel path).
#include "rrv_gs_record_hooks.h"
#include "rrv_gs_record_format.h"
#include "rrv_gs_render_mode.h"

#include <atomic>
#include <csignal>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rrv::gsrecord {

namespace {
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_inited{false};
std::mutex        g_fileMutex;
FILE*             g_file = nullptr;
uint64_t          g_eventCount = 0;
// Offset of the eventCount field in the header, so we can patch it in on
// shutdown without buffering the whole recording in memory.
long              g_eventCountFileOffset = 0;

// Armed (deferred-start) capture state — see rrv_gs_record_hooks.h. When
// g_armed is set, init_from_env only stashes these and returns; the file is
// created by rrv_gs_record_trigger(). The VRAM / privileged-register pointers
// are owned by PS2Runtime's memory object, which outlives every recording, so
// stashing them is safe (main.cpp passes them post-initialize).
std::atomic<bool> g_armed{false};
std::atomic<bool> g_signalArmEnabled{false};
// A process-directed signal can run on a different host thread from Present.
// Lock-free atomic operations are signal-handler-safe and synchronize the handoff.
std::atomic<unsigned int> g_signalPending{0};
static_assert(std::atomic<unsigned int>::is_always_lock_free);
using SignalHandler = void (*)(int);
SignalHandler g_previousSignalHandler = SIG_ERR;
std::atomic<bool> g_signalHandlerInstalled{false};
std::string       g_armedPath;
const uint8_t*    g_armedVram = nullptr;
uint32_t          g_armedVramSize = 0;
const uint64_t*   g_armedPrivRegs = nullptr;
VramSnapshotProvider g_vramProvider = nullptr;
void*             g_vramProviderContext = nullptr;
PostInitialSnapshotWriteHook g_postWriteHook = nullptr;
void*             g_postWriteContext = nullptr;
std::vector<uint8_t> g_vramSnapshot;
uint64_t          g_maxFrames = 0;   // 0 = unbounded
uint64_t          g_frameCount = 0;

// RRV_GS_RECORD_DELAY=<N> — hold off the armed trigger for N further presents
// after the anchor fires. The attract anchors are (phase, script, step)
// coordinates and phase 39 is the LAST one the guest ever enters, so the
// interesting part of that scene (the race itself, ~1400 fields after the
// anchor) could previously only be reached by recording every field from the
// anchor onwards — a 2.8 GB file for a 20-field question. With a delay the
// trigger-time VRAM snapshot is taken at the delayed point too, so the capture
// stays as self-contained as an undelayed one.
std::atomic<uint64_t> g_triggerDelay{0};
std::atomic<uint64_t> g_delayRemaining{0};
std::atomic<bool>     g_delayActive{false};

void onCaptureSignal(int) { g_signalPending.store(1, std::memory_order_relaxed); }

void restoreCaptureSignal() {
    g_signalArmEnabled.store(false, std::memory_order_relaxed);
    g_signalPending.store(0, std::memory_order_relaxed);
    if (g_signalHandlerInstalled.exchange(false, std::memory_order_relaxed) &&
        std::signal(SIGUSR1, g_previousSignalHandler) == SIG_ERR) {
        std::fprintf(stderr, "[rrv-gs-record] could not restore SIGUSR1 disposition\n");
    }
}

bool looksLikePath(const char* v) {
    if (!v || !*v) return false;
    if (std::strchr(v, '/')) return true;
    struct stat st{};
    return ::stat(v, &st) == 0 && S_ISDIR(st.st_mode);
}

void ensureParentDir(const std::string& filePath) {
    auto pos = filePath.find_last_of('/');
    if (pos == std::string::npos) return;
    std::string dir = filePath.substr(0, pos);
    // mkdir -p (shallow; recordings live under one or two path components).
    std::string accum;
    size_t start = 0;
    if (!dir.empty() && dir[0] == '/') { accum = "/"; start = 1; }
    while (start <= dir.size()) {
        auto slash = dir.find('/', start);
        std::string component = (slash == std::string::npos) ? dir.substr(start) : dir.substr(start, slash - start);
        if (!component.empty()) {
            accum += component;
            ::mkdir(accum.c_str(), 0755);
            accum += "/";
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
}

void writeEvent(EventTag tag, uint8_t pathId, const void* payload, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_fileMutex);
    if (!g_file) return;
    GsrEventHeader eh{};
    eh.tag = static_cast<uint8_t>(tag);
    eh.pathId = pathId;
    eh.reserved = 0;
    eh.payloadSize = size;
    std::fwrite(&eh, sizeof(eh), 1, g_file);
    if (size && payload) {
        std::fwrite(payload, 1, size, g_file);
    }
    ++g_eventCount;
}

const uint8_t* snapshotVramForWrite() {
    if (!g_vramProvider || g_armedVramSize == 0u) return g_armedVram;
    g_vramSnapshot.resize(g_armedVramSize);
    if (!g_vramProvider(g_vramProviderContext, g_vramSnapshot.data(), g_armedVramSize)) {
        std::fprintf(stderr,
                     "[rrv-gs-record] active GS VRAM snapshot failed; recording disabled\n");
        return nullptr;
    }
    return g_vramSnapshot.data();
}
} // namespace

bool rrv_gs_record_enabled() {
    // While an RRV_GS_RECORD_DELAY countdown is running the recorder must still
    // SEE presents (that is what it counts) even though it writes nothing. Both
    // hooks re-check g_enabled internally, so reporting "enabled" here during
    // the countdown costs a predictable branch and records no bytes.
    return g_enabled.load(std::memory_order_relaxed) ||
           g_delayActive.load(std::memory_order_relaxed) ||
           g_signalArmEnabled.load(std::memory_order_relaxed);
}

void rrv_gs_record_init_from_env(const uint8_t* vram, uint32_t vramSize,
                                 const uint64_t* privRegs19,
                                 VramSnapshotProvider vramProvider,
                                 void* vramProviderContext,
                                 PostInitialSnapshotWriteHook postWriteHook,
                                 void* postWriteContext) {
    if (g_inited.exchange(true)) return;
    const char* v = std::getenv("RRV_GS_RECORD");
    if (!v || !*v) { g_enabled.store(false); return; }

    std::string path = looksLikePath(v) ? std::string(v) : std::string("/tmp/rrv_gs_record/rec.gsr");

    g_armedVram = vram;
    g_armedVramSize = vramSize;
    g_armedPrivRegs = privRegs19;
    g_vramProvider = vramProvider;
    g_vramProviderContext = vramProviderContext;
    g_postWriteHook = postWriteHook;
    g_postWriteContext = postWriteContext;

    if (const char* frames = std::getenv("RRV_GS_RECORD_FRAMES"); frames && *frames) {
        g_maxFrames = std::strtoull(frames, nullptr, 10);
    }

    if (const char* delay = std::getenv("RRV_GS_RECORD_DELAY"); delay && *delay) {
        g_triggerDelay.store(std::strtoull(delay, nullptr, 10), std::memory_order_relaxed);
    }

    // Armed mode: stash everything and write nothing until triggered.
    if (const char* arm = std::getenv("RRV_GS_RECORD_ARM");
        arm && arm[0] != '\0' && arm[0] != '0') {
        g_armedPath = path;
        g_armed.store(true, std::memory_order_relaxed);
        g_enabled.store(false, std::memory_order_relaxed);
        if (const char* signal = std::getenv("RRV_GS_RECORD_SIGNAL");
            signal && std::strcmp(signal, "USR1") == 0) {
            const SignalHandler previous = std::signal(SIGUSR1, onCaptureSignal);
            if (previous != SIG_ERR) {
                g_previousSignalHandler = previous;
                g_signalHandlerInstalled.store(true, std::memory_order_relaxed);
                g_signalArmEnabled.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "[rrv-gs-record] trigger with: kill -USR1 %ld\n",
                             static_cast<long>(::getpid()));
            } else {
                std::fprintf(stderr, "[rrv-gs-record] could not install SIGUSR1 trigger\n");
                g_armed.store(false, std::memory_order_relaxed);
                return;
            }
        }
        std::fprintf(stderr,
                     "[rrv-gs-record] ARMED -> %s (waiting for trigger; frame cap %llu)\n",
                     path.c_str(), static_cast<unsigned long long>(g_maxFrames));
        return;
    }

    ensureParentDir(path);

    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) {
        std::fprintf(stderr, "[rrv-gs-record] FAILED to open %s for writing — recording disabled\n", path.c_str());
        g_enabled.store(false);
        return;
    }

    GsrFileHeader hdr{};
    hdr.magic = kGsrMagic;
    hdr.version = kGsrVersion;
    hdr.vramSize = vramSize;
    hdr.privRegsSize = static_cast<uint32_t>(19u * sizeof(uint64_t));
    if (privRegs19) {
        std::memcpy(hdr.initialPrivRegs, privRegs19, sizeof(hdr.initialPrivRegs));
    } else {
        std::memset(hdr.initialPrivRegs, 0, sizeof(hdr.initialPrivRegs));
    }
    hdr.eventCount = 0; // patched on shutdown
    hdr.renderMode = static_cast<uint32_t>(rrv::gs::captureRenderMode());
    hdr.reserved = 0;
    std::fwrite(&hdr, sizeof(hdr), 1, g_file);
    // Remember where eventCount lives so shutdown can seek back and patch it.
    g_eventCountFileOffset = static_cast<long>(offsetof(GsrFileHeader, eventCount));

    const uint8_t* snapshotVram = snapshotVramForWrite();
    if (vramProvider && vramSize && !snapshotVram) {
        std::fclose(g_file);
        g_file = nullptr;
        g_enabled.store(false);
        return;
    }
    if (snapshotVram && vramSize) {
        std::fwrite(snapshotVram, 1, vramSize, g_file);
    }

    g_eventCount = 0;
    g_enabled.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[rrv-gs-record] recording ENABLED -> %s (initial VRAM snapshot %u bytes)\n",
                 path.c_str(), vramSize);
}

void rrv_gs_record_shutdown() {
    // Restore even when arming never opened a file or the initial capture failed.
    // The exchange makes repeated shutdown calls leave the disposition alone.
    restoreCaptureSignal();
    // B8 capture-integrity diagnostic. RRV_GS_RECORD_VRAM_AT_CLOSE=<path>
    // writes the LIVE GS local memory again when the recording closes. The
    // header already carries the same buffer as it looked at trigger, so the
    // pair answers the question directly: if the framebuffers are populated
    // here but empty at trigger, the sparse snapshot is a TIMING artefact; if
    // they are empty at both ends, the live renderer never keeps the visible
    // frame in GS VRAM at all. Diagnostic only, no effect when unset.
    if (const char* p = std::getenv("RRV_GS_RECORD_VRAM_AT_CLOSE");
        p && *p && g_armedVram && g_armedVramSize) {
        const uint8_t* snapshotVram = snapshotVramForWrite();
        FILE* vf = snapshotVram ? std::fopen(p, "wb") : nullptr;
        if (vf) {
            std::fwrite(snapshotVram, 1, g_armedVramSize, vf);
            std::fclose(vf);
            std::fprintf(stderr, "[rrv-gs-record] closing VRAM snapshot -> %s (%u bytes)\n",
                         p, g_armedVramSize);
        }
    }
    std::lock_guard<std::mutex> lk(g_fileMutex);
    if (!g_file) return;
    // Patch the final event count into the header, then close.
    std::fflush(g_file);
    long endPos = std::ftell(g_file);
    if (endPos >= 0 && std::fseek(g_file, g_eventCountFileOffset, SEEK_SET) == 0) {
        uint64_t count = g_eventCount;
        std::fwrite(&count, sizeof(count), 1, g_file);
        std::fseek(g_file, endPos, SEEK_SET);
    }
    std::fclose(g_file);
    g_file = nullptr;
    g_enabled.store(false, std::memory_order_relaxed);
}

bool rrv_gs_record_armed() { return g_armed.load(std::memory_order_relaxed); }

namespace {
void openRecordingNow();
} // namespace

void rrv_gs_record_trigger() {
    // Signal-only capture owns this armed recorder until hookPresent consumes
    // the request. Ignore the legacy scene anchor while waiting for the user.
    if (g_signalArmEnabled.load(std::memory_order_relaxed)) return;
    // Only meaningful once, and only when init_from_env armed us.
    if (!g_armed.exchange(false)) return;

    const uint64_t delay = g_triggerDelay.load(std::memory_order_relaxed);
    if (delay != 0u) {
        g_delayRemaining.store(delay, std::memory_order_relaxed);
        g_delayActive.store(true, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[rrv-gs-record] anchor hit — RRV_GS_RECORD_DELAY=%llu, "
                     "holding off the recording for that many presents\n",
                     static_cast<unsigned long long>(delay));
        return;
    }

    openRecordingNow();
}

namespace {
void openRecordingNow() {
    ensureParentDir(g_armedPath);
    // The provider is a pure snapshot. Keep it before fopen so a failed or
    // throwing provider cannot truncate an existing recording at this path.
    const uint8_t* snapshotVram = snapshotVramForWrite();
    if (g_vramProvider && g_armedVramSize && !snapshotVram) {
        return;
    }
    FILE* pending = std::fopen(g_armedPath.c_str(), "wb");
    if (!pending) {
        std::fprintf(stderr,
                     "[rrv-gs-record] FAILED to open %s at trigger — recording disabled\n",
                     g_armedPath.c_str());
        return;
    }

    // Prepare and flush the complete initial image before any optional GS
    // cache action. The stream stays private and no file mutex is held while
    // the post-write hook calls back into the active backend.
    GsrFileHeader hdr{};
    hdr.magic = kGsrMagic;
    hdr.version = kGsrVersion;
    hdr.vramSize = g_armedVramSize;
    hdr.privRegsSize = static_cast<uint32_t>(19u * sizeof(uint64_t));
    hdr.renderMode = static_cast<uint32_t>(rrv::gs::captureRenderMode());
    if (g_armedPrivRegs) {
        std::memcpy(hdr.initialPrivRegs, g_armedPrivRegs, sizeof(hdr.initialPrivRegs));
    }
    bool initialWriteOk = std::fwrite(&hdr, sizeof(hdr), 1, pending) == 1;
    if (initialWriteOk && snapshotVram && g_armedVramSize) {
        initialWriteOk = std::fwrite(snapshotVram, 1, g_armedVramSize, pending) == g_armedVramSize;
    }
    if (initialWriteOk) initialWriteOk = std::fflush(pending) == 0;
    if (!initialWriteOk) {
        std::fprintf(stderr,
                     "[rrv-gs-record] FAILED to write initial snapshot to %s — recording disabled\n",
                     g_armedPath.c_str());
        std::fclose(pending);
        std::remove(g_armedPath.c_str());
        return;
    }
    if (g_postWriteHook) {
        bool postWriteOk = false;
        try {
            postWriteOk = g_postWriteHook(g_postWriteContext, snapshotVram, g_armedVramSize);
        } catch (...) {
            std::fclose(pending);
            std::remove(g_armedPath.c_str());
            throw;
        }
        if (!postWriteOk) {
            std::fprintf(stderr,
                         "[rrv-gs-record] post-snapshot action failed for %s — recording disabled\n",
                         g_armedPath.c_str());
            std::fclose(pending);
            std::remove(g_armedPath.c_str());
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_fileMutex);
        g_file = pending;
        g_eventCountFileOffset = static_cast<long>(offsetof(GsrFileHeader, eventCount));
        g_eventCount = 0;
        g_frameCount = 0;
    }
    g_enabled.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[rrv-gs-record] TRIGGERED -> %s (VRAM snapshot %u bytes)\n",
                 g_armedPath.c_str(), g_armedVramSize);
}
} // namespace

void hookGifPacket(uint8_t pathId, const uint8_t* data, uint32_t sizeBytes) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!data || sizeBytes == 0) return;
    writeEvent(EventTag::GifPacket, pathId, data, sizeBytes);
}

void hookPresent(const RecordPresent& pr) {
    // The serialized field boundary performs the VRAM snapshot, file I/O, and
    // state transition. That snapshot is already post-VSync: replay must start
    // with the next event, not present the triggering field a second time.
    if (g_signalArmEnabled.load(std::memory_order_relaxed) &&
        g_signalPending.exchange(0, std::memory_order_relaxed)) {
        g_signalArmEnabled.store(false, std::memory_order_relaxed);
        rrv_gs_record_trigger();
        return;
    }
    // RRV_GS_RECORD_DELAY countdown. Runs BEFORE the g_enabled gate because the
    // whole point is that recording is still off while it counts down.
    if (g_delayActive.load(std::memory_order_relaxed)) {
        const uint64_t left = g_delayRemaining.fetch_sub(1u, std::memory_order_relaxed);
        if (left <= 1u) {
            g_delayActive.store(false, std::memory_order_relaxed);
            openRecordingNow();
        }
        return;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    GsrPresentPayload payload{};
    payload.pmode = pr.pmode; payload.smode2 = pr.smode2;
    payload.dispfb1 = pr.dispfb1; payload.display1 = pr.display1;
    payload.dispfb2 = pr.dispfb2; payload.display2 = pr.display2;
    payload.vsyncTick = pr.vsyncTick;
    writeEvent(EventTag::Present, 0, &payload, static_cast<uint32_t>(sizeof(payload)));

    // Bounded capture: stop cleanly after the requested number of presents so
    // the file stays small enough to diff (RRV_GS_RECORD_FRAMES).
    if (g_maxFrames != 0) {
        uint64_t n;
        {
            std::lock_guard<std::mutex> lk(g_fileMutex);
            n = ++g_frameCount;
        }
        if (n >= g_maxFrames) {
            std::fprintf(stderr, "[rrv-gs-record] frame cap %llu reached — closing recording\n",
                         static_cast<unsigned long long>(g_maxFrames));
            rrv_gs_record_shutdown();
            // The frame cap ends the *recording*, not the process, so a capture
            // run otherwise sits there until someone closes the window by hand.
            // RRV_GS_RECORD_QUIT=1 makes the run self-terminating, which is what
            // an unattended/scripted capture wants. Default off: interactive
            // runs that record a prefix and then keep playing still work.
            if (const char* quit = std::getenv("RRV_GS_RECORD_QUIT");
                quit && quit[0] != '\0' && quit[0] != '0') {
                std::fprintf(stderr, "[rrv-gs-record] RRV_GS_RECORD_QUIT set — exiting\n");
                std::fflush(nullptr);
                std::_Exit(0);
            }
        }
    }
}

} // namespace rrv::gsrecord
