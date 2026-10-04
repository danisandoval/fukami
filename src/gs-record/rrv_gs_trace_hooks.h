// rrv_gs_trace_hooks.h — write-only recorder for the GS consumption boundary.
//
// Default off. `RRV_GS_TRACE=<path>` enables it; `RRV_GS_TRACE_ARM=1` holds it
// closed until `trigger()` fires (the scene anchor in src/patches.cpp), which is
// what makes a bounded capture of a scene deep in attract possible at all.
// `RRV_GS_TRACE_FRAMES=N` stops after N Vsync events; `RRV_GS_TRACE_QUIT=1`
// exits the process at that point.
//
// Everything here is called from inside Backend's mutex, so no additional
// locking is needed and the recorded order IS the delivered order.
#ifndef RRV_GS_TRACE_HOOKS_H
#define RRV_GS_TRACE_HOOKS_H

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "rrv_gs_trace.h"
#include "rrv_gs_render_mode.h"

namespace rrv::gstrace {

struct State {
    std::FILE *file = nullptr;
    bool enabled = false;
    bool armed = false;      // waiting for trigger()
    bool open = false;       // header written, recording
    bool finished = false;
    uint64_t sequence = 0;
    uint64_t events = 0;
    uint64_t vsyncs = 0;
    uint64_t frameCap = 0;
    bool quitAtCap = false;
    std::string path;
    TraceFileHeader header{};   // kept, NOT re-read: the file is write-only
};

inline State &state() {
    static State s;
    return s;
}

inline std::atomic<bool> &liveFlag() {
    static std::atomic<bool> f{false};
    return f;
}

// Cheap predicate for the hot path: one relaxed load when tracing is off.
inline bool recording() { return liveFlag().load(std::memory_order_relaxed); }

inline void configure() {
    State &s = state();
    static bool once = false;
    if (once) return;
    once = true;
    const char *p = std::getenv("RRV_GS_TRACE");
    if (!p || p[0] == '\0') return;
    s.path = p;
    s.enabled = true;
    const char *arm = std::getenv("RRV_GS_TRACE_ARM");
    s.armed = arm && arm[0] != '\0' && arm[0] != '0';
    if (const char *n = std::getenv("RRV_GS_TRACE_FRAMES"))
        s.frameCap = std::strtoull(n, nullptr, 10);
    const char *q = std::getenv("RRV_GS_TRACE_QUIT");
    s.quitAtCap = q && q[0] != '\0' && q[0] != '0';
    std::fprintf(stderr, "[gs-trace] enabled -> %s (%s; frame cap %llu)\n",
                 s.path.c_str(), s.armed ? "armed, waiting for trigger" : "immediate",
                 static_cast<unsigned long long>(s.frameCap));
    if (!s.armed) {
        // Opening is deferred to the first begin() so the initial VRAM snapshot
        // can be taken through the live bridge.
        liveFlag().store(true, std::memory_order_relaxed);
    }
}

inline bool wantsOpen() {
    State &s = state();
    return s.enabled && !s.open && !s.finished && !s.armed;
}

// Called once, from Backend, with the initial local memory and register block.
inline void openWith(const std::vector<uint8_t> &vram, const uint64_t *regs19) {
    State &s = state();
    if (!s.enabled || s.open || s.finished) return;
    s.file = std::fopen(s.path.c_str(), "wb");
    if (!s.file) {
        std::fprintf(stderr, "[gs-trace] cannot open %s\n", s.path.c_str());
        s.enabled = false;
        liveFlag().store(false, std::memory_order_relaxed);
        return;
    }
    TraceFileHeader h{};
    h.magic = kTraceMagic;
    h.version = kTraceVersion;
    h.vramSize = static_cast<uint32_t>(vram.size());
    h.privRegsCount = 19;
    h.renderMode = static_cast<uint32_t>(rrv::gs::captureRenderMode());
    if (regs19) std::memcpy(h.initialPrivRegs, regs19, sizeof(h.initialPrivRegs));
    s.header = h;
    std::fwrite(&h, sizeof(h), 1, s.file);
    if (!vram.empty()) std::fwrite(vram.data(), 1, vram.size(), s.file);
    s.open = true;
    liveFlag().store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[gs-trace] OPEN %s (initial VRAM %zu bytes)\n",
                 s.path.c_str(), vram.size());
}

inline void trigger() {
    State &s = state();
    if (!s.enabled || !s.armed || s.open || s.finished) return;
    s.armed = false;   // wantsOpen() now returns true; Backend opens on next call
    std::fprintf(stderr, "[gs-trace] TRIGGERED\n");
}

inline void finish() {
    State &s = state();
    if (!s.open || s.finished) return;
    s.finished = true;
    liveFlag().store(false, std::memory_order_relaxed);
    // Rewrite from the retained header. Re-reading it here would fail on a
    // write-only stream and silently stamp a zeroed header over the file.
    s.header.eventCount = s.events;
    std::fseek(s.file, 0, SEEK_SET);
    std::fwrite(&s.header, sizeof(s.header), 1, s.file);
    std::fclose(s.file);
    s.file = nullptr;
    std::fprintf(stderr, "[gs-trace] CLOSED %s (%llu events, %llu vsyncs)\n",
                 s.path.c_str(), static_cast<unsigned long long>(s.events),
                 static_cast<unsigned long long>(s.vsyncs));
    if (s.quitAtCap) {
        std::fprintf(stderr, "[gs-trace] RRV_GS_TRACE_QUIT set — exiting\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
}

// Stamp the guest's attract coordinates into the stream. Called from the EE
// patch thread, which is NOT inside Backend's mutex, so it is gated on the
// same `open` flag and tolerates being dropped before the header exists.
inline void sceneMark(uint32_t phase, uint32_t script, uint32_t step);

inline void write(EventTag tag, uint8_t pathId, const void *payload,
                  uint32_t payloadSize) {
    State &s = state();
    if (!s.open || s.finished) return;
    EventHeader h{};
    h.tag = static_cast<uint8_t>(tag);
    h.pathId = pathId;
    h.payloadSize = payloadSize;
    h.sequence = s.sequence++;
    std::fwrite(&h, sizeof(h), 1, s.file);
    if (payloadSize && payload) std::fwrite(payload, 1, payloadSize, s.file);
    ++s.events;
    if (tag == EventTag::Vsync) {
        ++s.vsyncs;
        if (s.frameCap && s.vsyncs >= s.frameCap) finish();
    }
}

inline void sceneMark(uint32_t phase, uint32_t script, uint32_t step) {
    State &s = state();
    if (!s.open || s.finished) return;
    static std::mutex m;   // the EE thread races Backend's recorded calls
    std::lock_guard<std::mutex> lock(m);
    if (!s.open || s.finished) return;
    SceneMarkPayload p{};
    p.phase = phase;
    p.script = script;
    p.step = step;
    p.vsyncIndex = s.vsyncs;
    EventHeader h{};
    h.tag = static_cast<uint8_t>(EventTag::SceneMark);
    h.payloadSize = sizeof(p);
    h.sequence = s.sequence++;
    std::fwrite(&h, sizeof(h), 1, s.file);
    std::fwrite(&p, sizeof(p), 1, s.file);
    ++s.events;
    std::fprintf(stderr, "[gs-trace] scene phase=%u script=%d step=%d @vsync=%llu\n",
                 phase, static_cast<int>(script), static_cast<int>(step),
                 static_cast<unsigned long long>(s.vsyncs));
}

}  // namespace rrv::gstrace

#endif  // RRV_GS_TRACE_HOOKS_H
