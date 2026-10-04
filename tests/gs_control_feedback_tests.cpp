// Exercises actual PS2Runtime::syncCoreSubsystems/dispatchGsPacket and field
// snapshots through the real synchronous Backend and existing ABI fake. The
// fake models the pinned renderer's stale CSR/SIGLBLID snapshot, not event logic.
#include "ps2_runtime.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
using Digest = uint64_t (*)();
std::vector<uint8_t> packet(std::initializer_list<std::array<uint64_t, 2>> ad) {
    std::vector<uint8_t> bytes(16 + 16 * ad.size());
    const uint64_t tag[] = {uint64_t(ad.size()) | 0x8000 | (uint64_t(1) << 60), 0xe};
    std::memcpy(bytes.data(), tag, 16);
    size_t offset = 16;
    for (const auto& pair : ad) { std::memcpy(bytes.data() + offset, pair.data(), 16); offset += 16; }
    return bytes;
}
struct Result { uint64_t csr, siglblid, rendererDigest; };

Result exercise(const char* mode, bool producer, Digest digest, Digest operations) {
    setenv("RRV_GS_EXECUTION", mode, 1);
    setenv("RRV_GS_CONTROL", producer ? "producer" : "off", 1);
    PS2Runtime runtime;
    check(runtime.memory().initialize(), "memory initialization failed");
    PS2Runtime::HostPresentationConfig host{};
    host.backend.rendererKind = rrv::gsbackend::RendererKind::Metal;
    host.backend.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
    auto& surface = host.backend.surface;
    surface.kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
    surface.nativeView = reinterpret_cast<void*>(uintptr_t(1));
    surface.nativeLayer = reinterpret_cast<void*>(uintptr_t(2));
    surface.widthPixels = 640; surface.heightPixels = 448;
    surface.mainThreadPrepared = true;
    host.context = &surface;
    host.pumpEvents = [](void*) {};
    host.shouldClose = [](void*) { return false; };
    host.currentSurface = [](void* context, rrv::gsbackend::NativeSurface* out) {
        *out = *static_cast<rrv::gsbackend::NativeSurface*>(context); return true;
    };
    runtime.setHostPresentationConfig(host);
    // This public seam initializes the real runtime backend, callbacks and
    // producer control without loading game data, starting SDL or tick workers.
    check(runtime.syncCoreSubsystems() && runtime.activeGsBackend(), "real backend/control initialization failed");
    PS2Runtime::GuestExecutionScope guest(&runtime);
    auto& memory = runtime.memory();
    check(memory.gsControl().enabled() == producer, "startup mode did not reach actual runtime");
    const auto send = [&](const auto& bytes) {
        memory.submitGifPacket(GifPathId::Path3, bytes.data(), static_cast<uint32_t>(bytes.size()));
        runtime.gifArbiter().drain();
    };
    memory.write32(0x12001010, 0x7f00); // Mask IRQs, retaining visible latches.
    memory.write32(0x12001000, 0x1f);
    send(packet({{0xffffffff11223344ULL, 0x60}, {0xffffffff55667788ULL, 0x62}, {0, 0x61}}));
    constexpr uint64_t oldId = 0x5566778811223344ULL;
    if (producer) {
        check((memory.read64(0x12001000) & 3) == 3, "actual GIF dispatch did not latch SIGNAL and FINISH");
        check(memory.read64(0x12001080) == oldId, "actual GIF dispatch did not apply masked identifiers");
    } else {
        // Legacy lacks producer event handlers. Seed the formerly latched
        // field state solely to reproduce its subsequent stale-bank bug.
        memory.gs().csr |= 3;
        memory.gs().siglblid = oldId;
    }
    const uint64_t beforeFields = memory.read64(0x12001000);
    const auto signalAcks = memory.gsControl().counters().signalAcks;
    const auto finishAcks = memory.gsControl().counters().finishAcks;
    runtime.advanceActiveGsBackendField(1);
    check(memory.read64(0x12001000) == (beforeFields | 0x2000),
          "odd field failed to mirror FIELD or changed unrelated pending CSR state");
    runtime.advanceActiveGsBackendField(2);
    check(memory.read64(0x12001000) == (beforeFields & ~uint64_t(0x2000)),
          "even field failed to mirror FIELD or changed unrelated pending CSR state");
    check(memory.gsControl().counters().signalAcks == signalAcks &&
          memory.gsControl().counters().finishAcks == finishAcks,
          "field transition incorrectly acknowledged a guest event");
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), 0x12001000, 3);
    check((memory.read64(0x12001000) & 3) == 0, "guest CSR acknowledgement failed before submission");
    constexpr uint64_t newId = 0xdeadbeefaabbccddULL;
    memory.write64(0x12001080, newId);
    const auto before = digest();
    send(packet({{0, 0x7f}})); // Ordinary non-event packed A+D command.
    check(digest() != before, "ordinary packet never reached the real bridge dispatch");
    const Result result{memory.read64(0x12001000), memory.read64(0x12001080), digest()};
    if (producer) {
        check((result.csr & 3) == 0 && result.siglblid == newId,
              "ordinary runtime submission resurrected stale renderer CSR/SIGLBLID");
        check(memory.gsControl().counters().signals == 1 && memory.gsControl().counters().finishes == 1,
              "ordinary packet incorrectly produced another guest event");
    } else {
        check((result.csr & 3) == 3 && result.siglblid == oldId,
              "legacy control failed to reproduce the stale-bank overwrite");
    }
    if (std::strcmp(mode, "worker-sync") == 0) {
        const uint64_t used = operations();
        check((used & (uint64_t(1) << 3)) && (used & (uint64_t(1) << 5)), "worker did not consume packet/field operations");
        check(bool(used & (uint64_t(1) << 4)) == !producer,
              "producer path unexpectedly read renderer CSR or legacy readback disappeared");
    }
    return result; // Runtime destructor joins GS owner before next mode.
}
} // namespace

int main() {
    try {
        unsetenv("RRV_PCSX2_GS_BRIDGE"); // Production path override rejection stays enabled.
        setenv("RRV_GS_BACKEND", "pcsx2", 1);
        setenv("RRV_GS_RENDER_MODE", "field", 1);
        setenv("RRV_TEST_FAKE_GS_BRIDGE_DIRECT", "1", 1);
        setenv("RRV_GIF_PATH_LATENCY", "0", 1);
        setenv("RRV_GIF_PATH_INTERLEAVE", "0", 1);
        void* fake = dlopen(RRV_TEST_FAKE_GS_BRIDGE, RTLD_NOW | RTLD_LOCAL);
        check(fake != nullptr, "fake bridge fixture could not load");
        auto digest = reinterpret_cast<Digest>(dlsym(fake, "rrv_test_fake_pcsx2_gs_bridge_event_digest"));
        auto operations = reinterpret_cast<Digest>(dlsym(fake, "rrv_test_fake_pcsx2_gs_bridge_owner_operations"));
        auto violations = reinterpret_cast<Digest>(dlsym(fake, "rrv_test_fake_pcsx2_gs_bridge_owner_violations"));
        check(digest && operations && violations, "fake instrumentation symbols missing");
        exercise("inline", false, digest, operations);
        exercise("worker-sync", false, digest, operations);
        check(violations() == 0, "legacy worker lifecycle owner violation");
        const auto inlineResult = exercise("inline", true, digest, operations);
        const auto workerResult = exercise("worker-sync", true, digest, operations);
        check(violations() == 0, "producer worker lifecycle owner violation");
        check(inlineResult.csr == workerResult.csr && inlineResult.siglblid == workerResult.siglblid &&
              inlineResult.rendererDigest == workerResult.rendererDigest, "corrected modes differ on equivalent input");
        dlclose(fake);
        std::cout << "Actual runtime stale-bank regression passed: legacy reproduced; producer inline/worker-sync preserved acknowledgement\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
