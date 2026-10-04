// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS-only G1-A test.  It intentionally creates just the AppKit view and
// CAMetalLayer which the pinned bridge authenticates as its main-thread owner
// surface.  It creates no NSApplication/NSWindow, reads no ELF/game data, and
// drives the composed PS2Runtime/GIF/VIF/DMA producer rather than bridge ABI
// calls directly.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ps2_runtime.h"
#include "Syscalls/Interrupt.h"
#include "Stubs/GS.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kCsr = 0x12001000u;
constexpr uint32_t kImr = 0x12001010u;
constexpr uint32_t kSiglblid = 0x12001080u;
constexpr uint32_t kGifDma = 0x1000a000u;
constexpr uint32_t kPsmCt32 = 0u;
constexpr uint32_t kPsmCt24 = 1u;
constexpr uint32_t kPsmCt16 = 2u;
constexpr uint32_t kPsmCt16S = 10u;
constexpr uint32_t kPsmT8 = 19u;
constexpr uint32_t kPsmT4 = 20u;
constexpr uint32_t kPsmT8H = 27u;
constexpr uint32_t kPsmT4HL = 36u;
constexpr uint32_t kPsmT4HH = 44u;
// The bridge read API takes the guest's local-to-host descriptor, whose
// source buffer width is BITBLTBUF.SBW (bits 16..21).  This is deliberately
// distinct from the host-to-local upload's DBW field.
constexpr uint64_t kReadBitbltbuf = uint64_t(1u) << 16u;

void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void put64(std::vector<uint8_t> &out, uint64_t value) {
    for (unsigned byte = 0; byte != 8; ++byte)
        out.push_back(static_cast<uint8_t>(value >> (byte * 8u)));
}
struct Ad { uint8_t reg; uint64_t value; };
std::vector<uint8_t> packed(std::initializer_list<Ad> values) {
    std::vector<uint8_t> bytes;
    put64(bytes, static_cast<uint64_t>(values.size()) | 0x8000u | (uint64_t(1) << 60u));
    put64(bytes, 0xeu);
    for (const auto value : values) {
        put64(bytes, value.value);
        put64(bytes, value.reg);
    }
    return bytes;
}
uint64_t trxreg(uint32_t width, uint32_t height = 1u) {
    return uint64_t(width) | (uint64_t(height) << 32u);
}
uint64_t bitbltbuf(uint32_t psm, bool local_to_host) {
    // SBW/DBW=1; set both PSM fields so packet construction mirrors the
    // producer descriptor even though only one side is active per direction.
    return (local_to_host ? kReadBitbltbuf : (uint64_t(1u) << 48u)) |
           (uint64_t(psm & 0x3fu) << 24u) | (uint64_t(psm & 0x3fu) << 56u);
}
std::vector<uint8_t> hostToLocalArmPacket(uint32_t width, uint32_t psm = kPsmCt32) {
    // Host->local upload at GS local address (0, 0). This is also used without
    // IMAGE data to prove that an upload arm is never a read transaction.
    return packed({
        {0x50u, bitbltbuf(psm, false)},
        {0x51u, 0u},
        {0x52u, trxreg(width)},
        {0x53u, 0u}, // host -> local
    });
}
std::vector<uint8_t> hostToLocalPacket(const std::vector<uint8_t> &payload, uint32_t width,
                                       uint32_t psm = kPsmCt32, uint32_t height = 1u) {
    std::vector<uint8_t> bytes = packed({
        {0x50u, bitbltbuf(psm, false)},
        {0x51u, 0u},
        {0x52u, trxreg(width, height)},
        {0x53u, 0u}, // host -> local
    });
    const uint64_t qwc = (payload.size() + 15u) / 16u;
    put64(bytes, qwc | 0x8000u | (uint64_t(2) << 58u) | (uint64_t(1) << 60u));
    // EOP IMAGE. NREG is encoded in bits 60..63; its zero encoding means 16,
    // so this transfer explicitly uses one IMAGE register.
    put64(bytes, 0u);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.resize(bytes.size() + static_cast<size_t>(qwc * 16u - payload.size()), 0u);
    return bytes;
}
std::vector<uint8_t> hostToLocalPacket(const std::array<uint8_t, 16> &payload) {
    return hostToLocalPacket(std::vector<uint8_t>(payload.begin(), payload.end()), 4u, kPsmCt32);
}
std::vector<uint8_t> localToHostPacket(uint32_t width, uint64_t descriptor = kReadBitbltbuf,
                                       uint64_t trxpos = 0u) {
    // sceGsExecStoreImage issues this descriptor before the FIFO read.  The
    // bridge read API consumes that already-issued local-to-host transfer; it
    // must not manufacture TRXDIR or advance guest producer state itself.
    return packed({
        {0x50u, descriptor},
        {0x51u, trxpos},
        {0x52u, trxreg(width)},
        {0x53u, 1u}, // local -> host
    });
}
bool snapshotHasCt32Row(const std::vector<uint8_t> &snapshot,
                        const std::array<uint8_t, 16> &payload) {
    // Native PSMCT32 swizzle for x=0..3, y=0 maps the first two pixels to
    // bytes 0..7 and the next two to bytes 16..23; the guest row is not a
    // contiguous span in a raw GS local-memory snapshot.
    return snapshot.size() >= 24u &&
           std::equal(payload.begin(), payload.begin() + 8u, snapshot.begin()) &&
           std::equal(payload.begin() + 8u, payload.end(), snapshot.begin() + 16u);
}
uint64_t id(uint32_t value, uint32_t mask = ~uint32_t(0)) {
    return uint64_t(value) | (uint64_t(mask) << 32u);
}
uint64_t siglblid(uint32_t signal, uint32_t label) {
    return uint64_t(signal) | (uint64_t(label) << 32u);
}

struct IrqEvent { uint64_t csr; uint64_t siglblid; };
thread_local std::vector<IrqEvent> *g_irqs = nullptr;
void guestIrq(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) {
    g_irqs->push_back({runtime->memory().read64(kCsr), runtime->memory().read64(kSiglblid)});
    runtime->memory().writeIORegister(0x1000f000u, 1u);
    ctx->pc = 0u;
}

struct Trace {
    std::array<uint8_t, 16> local{};
    uint64_t csr = 0u, siglblid = 0u;
    std::vector<IrqEvent> irqs;
    std::vector<uint32_t> dma;
    std::vector<uint8_t> snapshot;
    std::vector<uint8_t> drawnStoreImage;
    std::vector<uint8_t> redrawnStoreImage;
};

struct StoreImageMem {
    uint16_t x = 0u;
    uint16_t y = 0u;
    uint16_t width = 0u;
    uint16_t height = 0u;
    uint16_t vram_addr = 0u;
    uint8_t vram_width = 1u;
    uint8_t psm = 0u;
};
static_assert(sizeof(StoreImageMem) == 12u, "StoreImage ABI descriptor must be 12 bytes");

struct StoreResult {
    int32_t status = -2;
    std::vector<uint8_t> bytes;
    bool leadingSentinel = false;
    bool trailingSentinel = false;
};

void setArg(R5900Context &ctx, int reg, uint32_t value) {
    ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

struct OwnerSurface {
    NSView *view = nil;
    CAMetalLayer *layer = nil;
    OwnerSurface() {
        view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 640, 448)];
        layer = [[CAMetalLayer alloc] init];
        [view setWantsLayer:YES];
        [view setLayer:layer];
    }
    ~OwnerSurface() {
        [view release];
        [layer release];
    }
    rrv::gsbackend::NativeSurface native() const {
        rrv::gsbackend::NativeSurface surface{};
        surface.kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
        surface.nativeView = view;
        surface.nativeLayer = layer;
        surface.widthPixels = 640u;
        surface.heightPixels = 448u;
        surface.backingScale = 1.0f;
        surface.mainThreadPrepared = true;
        return surface;
    }
};

Trace exercise(const char *execution, const OwnerSurface &owner) {
    setenv("RRV_GS_EXECUTION", execution, 1);
    setenv("RRV_GS_CONTROL", "producer", 1);
    setenv("RRV_GS_BACKEND", "pcsx2", 1);
    setenv("RRV_GS_RENDER_MODE", "field", 1);
    // Exercise the production-default VIF1 path latency rather than forcing
    // the test-only synchronous override.
    unsetenv("RRV_GIF_PATH_LATENCY");
    unsetenv("RRV_GIF_PATH_INTERLEAVE");

    PS2Runtime runtime;
    check(runtime.memory().initialize(), "real bridge memory initialization failed");
    PS2Runtime::HostPresentationConfig host{};
    host.backend.rendererKind = rrv::gsbackend::RendererKind::Metal;
    host.backend.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
    host.backend.surface = owner.native();
    host.context = const_cast<OwnerSurface *>(&owner);
    host.pumpEvents = [](void *) {};
    host.shouldClose = [](void *) { return false; };
    host.currentSurface = [](void *context, rrv::gsbackend::NativeSurface *out) {
        *out = static_cast<OwnerSurface *>(context)->native();
        return true;
    };
    runtime.setHostPresentationConfig(host);
    check(runtime.syncCoreSubsystems() && runtime.activeGsBackend(),
          "actual pinned PCSX2 GS did not initialize");
    check(runtime.memory().gsControl().enabled(), "composed producer control was not enabled");

    PS2Runtime::GuestExecutionScope guest(&runtime);
    Trace trace;
    g_irqs = &trace.irqs;
    runtime.registerFunction(0x200000u, guestIrq);
    ps2_syscalls::IrqHandlerSnapshotCounters counters{};
    counters.enabledIntcMask = 1u;
    counters.nextIntcHandlerId = 2u;
    counters.nextDmacHandlerId = 1u;
    ps2_syscalls::importIrqHandlerState({{0u, 1u, 0u, 0x200000u, 0u, 0u, 0u, 1u, 0u}}, counters);
    auto &memory = runtime.memory();
    memory.setDmacCompletionCallback([&](uint32_t cause) { trace.dma.push_back(cause); });
    const auto submit = [&](const std::vector<uint8_t> &packet) {
        memory.submitGifPacket(GifPathId::Path3, packet.data(), static_cast<uint32_t>(packet.size()));
        runtime.gifArbiter().drain();
    };

    // Known real-GS upload/read and exact native-order snapshot/restore.
    const std::array<uint8_t, 16> expected{{0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xbau, 0xdcu, 0xfeu,
                                             0x11u, 0x33u, 0x55u, 0x77u, 0x99u, 0xbbu, 0xddu, 0xffu}};
    submit(hostToLocalPacket(expected));
    runtime.advanceActiveGsBackendField(0u);
    std::vector<uint8_t> uploadedSnapshot;
    check(runtime.snapshotActiveGsBackendLocalMemory(uploadedSnapshot) &&
          snapshotHasCt32Row(uploadedSnapshot, expected),
          "actual PCSX2 GS native snapshot does not contain the host-to-local upload");
    submit(localToHostPacket(4u));
    std::array<uint8_t, 16> zeroRead{};
    zeroRead.fill(0xa5u);
    bool zeroReadRejected = false;
    try {
        zeroReadRejected = !runtime.readActiveGsBackendLocalMemory(
            zeroRead.data(), 0u, kReadBitbltbuf, 0u, uint64_t(1u) << 32u | 4u);
    } catch (const std::runtime_error &) {
        zeroReadRejected = true;
    }
    check(zeroReadRejected &&
              std::all_of(zeroRead.begin(), zeroRead.end(), [](uint8_t value) { return value == 0xa5u; }),
          "zero-byte local-memory read did not fail closed before consuming the native download cursor");
    check(runtime.readActiveGsBackendLocalMemory(trace.local.data(), trace.local.size(),
                                                 kReadBitbltbuf, 0u, uint64_t(1u) << 32u | 4u),
          "valid read after zero-byte rejection did not retain the native local-to-host cursor");
    check(trace.local == expected, "actual PCSX2 GS local-memory bytes differ from upload");
    check(runtime.snapshotActiveGsBackendLocalMemory(trace.snapshot) && trace.snapshot.size() == 4u * 1024u * 1024u,
          "actual PCSX2 GS local-memory snapshot failed");
    const auto baseline = trace.snapshot;
    trace.snapshot[0] ^= 0xffu;
    check(runtime.restoreActiveGsBackendLocalMemory(trace.snapshot.data(), static_cast<uint32_t>(trace.snapshot.size())),
          "actual PCSX2 GS local-memory restore failed");
    std::vector<uint8_t> restored;
    check(runtime.snapshotActiveGsBackendLocalMemory(restored) && restored == trace.snapshot,
          "actual PCSX2 GS local-memory snapshot/restore is not exact");
    check(runtime.restoreActiveGsBackendLocalMemory(baseline.data(), static_cast<uint32_t>(baseline.size())),
          "actual PCSX2 GS baseline restore failed");

    // G1-B: exercise the real bridge's native local-to-host cursor, not a
    // bridge-side decoder. Rejections must leave caller sentinels intact; a
    // subsequent valid request proves they preflight before consumption.
    const auto readLocal = [&](uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf,
                               uint64_t trxpos, uint64_t reg) {
        try {
            return runtime.readActiveGsBackendLocalMemory(dst, bytes, bitbltbuf, trxpos, reg);
        } catch (const std::runtime_error &) {
            return false;
        }
    };
    const auto expectRejectedRead = [&](uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos,
                                        uint64_t reg, const char *message) {
        std::vector<uint8_t> destination(std::max<uint32_t>(bytes, 1u), 0xa5u);
        check(!readLocal(destination.data(), bytes, bitbltbuf, trxpos, reg) &&
              std::all_of(destination.begin(), destination.end(), [](uint8_t byte) { return byte == 0xa5u; }),
              message);
    };
    const auto expectRead = [&](const std::vector<uint8_t> &expected, uint32_t bytes,
                                uint64_t descriptor, uint64_t reg, const char *message) {
        std::vector<uint8_t> destination(bytes, 0xa5u);
        check(readLocal(destination.data(), bytes, descriptor, 0u, reg) &&
              destination == expected, message);
    };

    // A completed transfer is inactive; a host-to-local arm is likewise never
    // admitted as a download, even though both are ordinary real GIF traffic.
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(4u),
                       "no-active native local-memory read changed its destination");
    expectRejectedRead(0u, kReadBitbltbuf, 0u, trxreg(4u),
                       "no-active zero-byte local-memory read changed its destination");
    submit(hostToLocalArmPacket(4u));
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(4u),
                       "host-to-local transfer was accepted as local-to-host");

    const std::vector<uint8_t> split32{{0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
                                        0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du, 0x0eu, 0x0fu,
                                        0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u,
                                        0x88u, 0x89u, 0x8au, 0x8bu, 0x8cu, 0x8du, 0x8eu, 0x8fu}};
    submit(hostToLocalPacket(split32, 8u));
    submit(localToHostPacket(8u));
    expectRejectedRead(16u, kReadBitbltbuf, 1u, trxreg(8u),
                       "descriptor mismatch consumed a native local-to-host cursor");
    expectRead(std::vector<uint8_t>(split32.begin(), split32.begin() + 16u), 16u, kReadBitbltbuf, trxreg(8u),
               "first aligned native local-to-host split differs");
    expectRead(std::vector<uint8_t>(split32.begin() + 16u, split32.end()), 16u, kReadBitbltbuf, trxreg(8u),
               "second aligned native local-to-host split differs");
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(8u),
                       "completed local-to-host transfer was accepted twice");

    // Intermediate reads must be exact QWC units. Oversize and non-QWC
    // requests are rejected without consuming the transfer; normal aligned
    // continuation remains possible afterwards.
    submit(hostToLocalPacket(split32, 8u));
    submit(localToHostPacket(8u));
    expectRejectedRead(20u, kReadBitbltbuf, 0u, trxreg(8u),
                       "non-QWC intermediate read consumed native transfer state");
    expectRejectedRead(48u, kReadBitbltbuf, 0u, trxreg(8u),
                       "oversized local-to-host read consumed native transfer state");
    expectRead(std::vector<uint8_t>(split32.begin(), split32.begin() + 16u), 16u, kReadBitbltbuf, trxreg(8u),
               "valid continuation after rejected local-to-host read differs");
    expectRead(std::vector<uint8_t>(split32.begin() + 16u, split32.end()), 16u, kReadBitbltbuf, trxreg(8u),
               "final continuation after rejected local-to-host read differs");

    // The bridge must cap before it constructs its QWC-padded FIFO.  Use a
    // non-null one-byte destination so UINT32_MAX exercises the public ABI
    // without inviting the test itself to allocate a malformed request.
    submit(hostToLocalPacket(split32, 8u));
    submit(localToHostPacket(8u));
    std::array<uint8_t, 1> hugeReadSentinel{{0xa5u}};
    check(!readLocal(hugeReadSentinel.data(), UINT32_MAX, kReadBitbltbuf, 0u, trxreg(8u)) &&
          hugeReadSentinel[0] == 0xa5u,
          "oversized preallocation request changed its small destination or reached native consumption");
    expectRead(std::vector<uint8_t>(split32.begin(), split32.begin() + 16u), 16u, kReadBitbltbuf, trxreg(8u),
               "valid continuation after oversized preallocation request differs");
    expectRead(std::vector<uint8_t>(split32.begin() + 16u, split32.end()), 16u, kReadBitbltbuf, trxreg(8u),
               "final continuation after oversized preallocation request differs");

    // These are direct, synthetic GIF transfers through the composed native
    // bridge—not broader alternate-PSM HLE support. The existing HLE helper
    // computes 4 bytes/pixel for CT24, T8H, and T4H. PCSX2's native trbpp is
    // 24, 8, and 4 respectively, so those producer-style sizes must fail
    // closed before consumption; an exact native continuation must still
    // return packed bytes. CT16/T8 demonstrate native packing and tails where
    // their ordinary producer byte counts agree. Pinned PCSX2's PSMT4 reader
    // is explicitly unsupported below because it cannot fill a deterministic
    // native FIFO.
    const auto nativeFormat = [&](uint32_t psm, uint32_t width, const std::vector<uint8_t> &payload,
                                  uint32_t incompatible_bytes, const char *name) {
        const uint64_t descriptor = bitbltbuf(psm, true);
        submit(hostToLocalPacket(payload, width, psm));
        submit(localToHostPacket(width, descriptor));
        if (incompatible_bytes != 0u)
            expectRejectedRead(incompatible_bytes, descriptor, 0u, trxreg(width), name);
        size_t offset = 0u;
        while (payload.size() - offset > 16u)
        {
            expectRead(std::vector<uint8_t>(payload.begin() + offset, payload.begin() + offset + 16u),
                       16u, descriptor, trxreg(width), name);
            offset += 16u;
        }
        const uint32_t tail = static_cast<uint32_t>(payload.size() - offset);
        check(tail != 0u, "native format fixture unexpectedly has no bytes");
        expectRead(std::vector<uint8_t>(payload.begin() + offset, payload.end()), tail,
                   descriptor, trxreg(width), name);
    };
    const auto pattern = [](uint32_t bytes, uint8_t seed) {
        std::vector<uint8_t> result(bytes);
        for (uint32_t index = 0; index != bytes; ++index)
            result[index] = static_cast<uint8_t>(seed + index);
        return result;
    };
    nativeFormat(kPsmCt24, 8u, pattern(24u, 0x41u), 32u,
                 "CT24 producer-style size was accepted or native continuation differed");
    nativeFormat(kPsmCt16, 9u, pattern(18u, 0x61u), 0u,
                 "CT16 native packed tail differs");
    nativeFormat(kPsmCt16S, 9u, pattern(18u, 0x69u), 0u,
                 "CT16S native packed tail differs");
    nativeFormat(kPsmT8, 17u, pattern(17u, 0x71u), 0u,
                 "T8 native packed tail differs");
    nativeFormat(kPsmT8H, 17u, pattern(17u, 0x81u), 68u,
                 "T8H producer-style size was accepted or native continuation differed");
    const uint64_t t4_descriptor = bitbltbuf(kPsmT4, true);
    submit(hostToLocalPacket(pattern(17u, 0x91u), 34u, kPsmT4));
    submit(localToHostPacket(34u, t4_descriptor));
    expectRejectedRead(17u, t4_descriptor, 0u, trxreg(34u),
                       "unsupported native T4 read did not fail closed");
    nativeFormat(kPsmT4HL, 34u, pattern(17u, 0xa1u), 136u,
                 "T4H producer-style size was accepted or native continuation differed");

    // G1-D: call the linked producer HLE directly.  Each case supplies an
    // ordinary host-to-local upload, then StoreImage itself emits BITBLTBUF /
    // TRXPOS / TRXREG / TRXDIR and processes its own GIF DMA.  No generated
    // game dispatch, asset, or pre-armed native download is involved.
    const auto invokeStore = [&](StoreImageMem image, uint32_t capacity) {
        const uint32_t imageAddr = runtime.guestMalloc(sizeof(image), 4u);
        const uint32_t allocation = runtime.guestMalloc(capacity + 32u, 16u);
        check(imageAddr != 0u && allocation != 0u, "StoreImage guest fixture allocation failed");
        uint8_t *const imageBytes = memory.getRDRAM() + imageAddr;
        uint8_t *const allocationBytes = memory.getRDRAM() + allocation;
        std::memcpy(imageBytes, &image, sizeof(image));
        std::fill(allocationBytes, allocationBytes + capacity + 32u, 0xa5u);
        const uint32_t destination = allocation + 16u;
        R5900Context ctx{};
        setArg(ctx, 4, imageAddr);
        setArg(ctx, 5, destination);
        try {
            ps2_stubs::sceGsExecStoreImage(memory.getRDRAM(), &ctx, &runtime);
        } catch (...) {
            runtime.guestFree(allocation);
            runtime.guestFree(imageAddr);
            throw;
        }
        StoreResult result;
        result.status = static_cast<int32_t>(getRegU32(&ctx, 2));
        result.bytes.assign(allocationBytes + 16u, allocationBytes + 16u + capacity);
        result.leadingSentinel = std::all_of(allocationBytes, allocationBytes + 16u,
                                             [](uint8_t value) { return value == 0xa5u; });
        result.trailingSentinel = std::all_of(allocationBytes + 16u + capacity,
                                              allocationBytes + 32u + capacity,
                                              [](uint8_t value) { return value == 0xa5u; });
        runtime.guestFree(allocation);
        runtime.guestFree(imageAddr);
        return result;
    };
    const auto hleSuccess = [&](uint32_t psm, uint32_t width, uint32_t height,
                                const std::vector<uint8_t> &payload, const char *message) {
        const size_t dmaBefore = trace.dma.size();
        submit(hostToLocalPacket(payload, width, psm, height));
        const StoreResult result = invokeStore({0u, 0u, static_cast<uint16_t>(width),
                                                static_cast<uint16_t>(height), 0u, 1u,
                                                static_cast<uint8_t>(psm)},
                                               static_cast<uint32_t>(payload.size()));
        check(result.status == 0 && result.bytes == payload && result.leadingSentinel && result.trailingSentinel,
              message);
        check(trace.dma.size() == dmaBefore + 1u && trace.dma.back() == 2u,
              "StoreImage did not append exactly one GIF DMA completion");
    };
    hleSuccess(kPsmCt32, 4u, 1u, pattern(16u, 0xb1u),
               "StoreImage CT32 did not return exact native bytes and sentinels");
    hleSuccess(kPsmCt24, 8u, 1u, pattern(24u, 0xc1u),
               "StoreImage CT24 did not use packed three-byte pixels");
    hleSuccess(kPsmCt16, 9u, 1u, pattern(18u, 0xd1u),
               "StoreImage CT16 did not preserve its packed tail");
    hleSuccess(kPsmCt16S, 9u, 1u, pattern(18u, 0xd9u),
               "StoreImage CT16S did not preserve its packed tail");
    hleSuccess(kPsmT8, 17u, 1u, pattern(17u, 0xe1u),
               "StoreImage T8 did not preserve its packed tail");
    hleSuccess(kPsmT8H, 17u, 1u, pattern(17u, 0xf1u),
               "StoreImage T8H did not use one byte per pixel");
    hleSuccess(kPsmT4HL, 34u, 1u, pattern(17u, 0x31u),
               "StoreImage T4HL did not use packed four-bit pixels");
    hleSuccess(kPsmT4HH, 34u, 1u, pattern(17u, 0x51u),
               "StoreImage T4HH did not use packed four-bit pixels");
    // The odd three-by-three transfer proves rounding happens once for the
    // whole transfer (ceil(9 / 2) == 5), not once per row.
    hleSuccess(kPsmT4HL, 3u, 3u, pattern(5u, 0x71u),
               "StoreImage T4HL rounded an odd multirow transfer per row");

    const std::vector<uint8_t> repeatedStore{{0x61u, 0x62u, 0x63u, 0x64u,
                                               0x65u, 0x66u, 0x67u, 0x68u,
                                               0x69u, 0x6au, 0x6bu, 0x6cu,
                                               0x6du, 0x6eu, 0x6fu, 0x70u}};
    hleSuccess(kPsmCt32, 4u, 1u, repeatedStore,
               "first repeated StoreImage call did not independently arm a native download");
    hleSuccess(kPsmCt32, 4u, 1u, repeatedStore,
               "second repeated StoreImage call reused or lost the native download state");

    // G1-E: hardware draw -> the actual linked StoreImage HLE. This deliberately
    // leaves the target resident in PCSX2 GS local memory: no upload, snapshot,
    // capture, VSync, or restore occurs between either draw and its StoreImage.
    // FBW=2 makes the target 128 pixels wide and falls outside the pinned
    // Metal profile's CPUSpriteRenderBW=1 threshold (only FBW <= 1 qualifies).
    constexpr uint32_t kDrawWidth = 128u;
    constexpr uint32_t kDrawHeight = 64u;
    constexpr uint32_t kDrawFbw = 2u;
    constexpr uint32_t kDrawZbp = 64u;
    constexpr uint64_t kDrawFrame = uint64_t(kDrawFbw) << 16u;
    constexpr uint64_t kDrawZbuf = uint64_t(kDrawZbp) | (uint64_t(1u) << 32u);
    constexpr uint64_t kDrawScissor = uint64_t(0u) | (uint64_t(kDrawWidth - 1u) << 16u) |
                                      (uint64_t(0u) << 32u) | (uint64_t(kDrawHeight - 1u) << 48u);
    constexpr uint64_t kDrawTest = (uint64_t(1u) << 16u) | (uint64_t(1u) << 17u);
    constexpr uint64_t kDrawPrim = 6u; // sprite, flat, untextured, unblended, context 1
    check(kDrawWidth == kDrawFbw * 64u && kDrawFbw > 1u,
          "G1-E fixture must remain outside the configured CPU sprite shortcut basis");
    check((kDrawFrame & 0x1ffu) == 0u && ((kDrawFrame >> 16u) & 0x3fu) == kDrawFbw &&
          ((kDrawFrame >> 24u) & 0x3fu) == kPsmCt32 && (kDrawFrame >> 32u) == 0u &&
          (kDrawZbuf & 0x1ffu) == kDrawZbp && ((kDrawZbuf >> 24u) & 0xfu) == 0u &&
          ((kDrawZbuf >> 32u) & 1u) == 1u && ((kDrawTest >> 0u) & 1u) == 0u &&
          ((kDrawTest >> 14u) & 1u) == 0u && ((kDrawTest >> 15u) & 1u) == 0u &&
          ((kDrawTest >> 16u) & 1u) == 1u && ((kDrawTest >> 17u) & 3u) == 1u,
          "G1-E decoded FRAME/ZBUF/TEST fields no longer express the hardware draw contract");

    const auto rgbaq = [](const std::array<uint8_t, 4> &rgba) {
        constexpr uint32_t kQOne = 0x3f800000u;
        return uint64_t(rgba[0]) | (uint64_t(rgba[1]) << 8u) |
               (uint64_t(rgba[2]) << 16u) | (uint64_t(rgba[3]) << 24u) |
               (uint64_t(kQOne) << 32u);
    };
    const auto xyz2 = [](uint32_t x, uint32_t y) {
        return uint64_t(x << 4u) | (uint64_t(y << 4u) << 16u);
    };
    const auto drawStatePacket = [&] {
        return packed({
            {0x1au, 1u},                 // PRMODECONT.AC=1
            {0x4cu, kDrawFrame},         // FRAME_1: FBP=0, FBW=2, PSMCT32, mask=0
            {0x4eu, kDrawZbuf},          // ZBUF_1: nonoverlap ZBP=64, ZMSK=1
            {0x18u, 0u},                 // XYOFFSET_1=0
            {0x40u, kDrawScissor},       // SCISSOR_1: [0,128) x [0,64)
            {0x47u, kDrawTest},          // alpha/date off; ZTE=1, ZTST=ALWAYS
            {0x4au, 0u},                 // FBA_1=0
            {0x46u, 1u},                 // COLCLAMP=1
            {0x45u, 0u},                 // DTHE=0
            {0x00u, kDrawPrim},          // flat untextured/unblended sprite
        });
    };
    const auto spritePacket = [&](const std::array<uint8_t, 4> &rgba,
                                  uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        return packed({
            {0x01u, rgbaq(rgba)},
            {0x05u, xyz2(x0, y0)},
            {0x05u, xyz2(x1, y1)},
        });
    };
    const std::array<uint8_t, 4> background{{0x19u, 0x37u, 0x5bu, 0xffu}};
    const std::array<uint8_t, 4> inner{{0xd1u, 0x63u, 0x27u, 0xffu}};
    const std::array<uint8_t, 4> redraw{{0x24u, 0xc8u, 0x8eu, 0xffu}};
    const auto statePacket = drawStatePacket();
    const auto backgroundPacket = spritePacket(background, 0u, 0u, kDrawWidth, kDrawHeight);
    const auto innerPacket = spritePacket(inner, 16u, 16u, 48u, 40u);
    const auto redrawPacket = spritePacket(redraw, 24u, 20u, 40u, 32u);
    const auto expectedDraw = [&](bool includeRedraw) {
        std::vector<uint8_t> result(kDrawWidth * kDrawHeight * 4u);
        const auto putPixel = [&](uint32_t x, uint32_t y, const std::array<uint8_t, 4> &rgba) {
            const size_t offset = (static_cast<size_t>(y) * kDrawWidth + x) * 4u;
            std::copy(rgba.begin(), rgba.end(), result.begin() + offset);
        };
        for (uint32_t y = 0u; y != kDrawHeight; ++y)
            for (uint32_t x = 0u; x != kDrawWidth; ++x)
                putPixel(x, y, background);
        for (uint32_t y = 16u; y != 40u; ++y)
            for (uint32_t x = 16u; x != 48u; ++x)
                putPixel(x, y, inner);
        if (includeRedraw)
            for (uint32_t y = 20u; y != 32u; ++y)
                for (uint32_t x = 24u; x != 40u; ++x)
                    putPixel(x, y, redraw);
        return result;
    };
    const auto packetHex = [](const std::vector<uint8_t> &packet) {
        std::ostringstream text;
        text << std::hex;
        for (uint8_t byte : packet)
        {
            static constexpr char kHex[] = "0123456789abcdef";
            text << kHex[byte >> 4u] << kHex[byte & 0x0fu];
        }
        return text.str();
    };
    const auto assertDrawStore = [&](const StoreResult &result, const std::vector<uint8_t> &expectedPixels,
                                     const char *phase) {
        if (result.status != 0 || !result.leadingSentinel || !result.trailingSentinel ||
            result.bytes.size() != expectedPixels.size())
        {
            std::ostringstream text;
            text << "G1-E " << phase << " StoreImage status/sentinel/size failure"
                 << " status=" << result.status << " bytes=" << result.bytes.size()
                 << " expected_bytes=" << expectedPixels.size()
                 << " frame=0x" << std::hex << kDrawFrame << " zbuf=0x" << kDrawZbuf
                 << " scissor=0x" << kDrawScissor << " test=0x" << kDrawTest
                 << " prim=0x" << kDrawPrim
                 << " packets={state:" << packetHex(statePacket)
                 << ",background:" << packetHex(backgroundPacket)
                 << ",inner:" << packetHex(innerPacket)
                 << ",redraw:" << packetHex(redrawPacket) << '}';
            throw std::runtime_error(text.str());
        }
        for (size_t index = 0u; index != expectedPixels.size(); ++index)
        {
            if (result.bytes[index] == expectedPixels[index])
                continue;
            const uint32_t pixel = static_cast<uint32_t>(index / 4u);
            std::ostringstream text;
            text << "G1-E " << phase << " first pixel mismatch at ("
                 << (pixel % kDrawWidth) << ',' << (pixel / kDrawWidth) << ") byte="
                 << (index % 4u) << " actual=0x" << std::hex << unsigned(result.bytes[index])
                 << " expected=0x" << unsigned(expectedPixels[index])
                 << " frame=0x" << kDrawFrame << " zbuf=0x" << kDrawZbuf
                 << " scissor=0x" << kDrawScissor << " test=0x" << kDrawTest
                 << " prim=0x" << kDrawPrim
                 << " packets={state:" << packetHex(statePacket)
                 << ",background:" << packetHex(backgroundPacket)
                 << ",inner:" << packetHex(innerPacket)
                 << ",redraw:" << packetHex(redrawPacket) << '}';
            throw std::runtime_error(text.str());
        }
    };
    submit(statePacket);
    submit(backgroundPacket); // own EOP packet
    submit(innerPacket);      // own EOP packet
    const size_t dmaBeforeFirstDrawStore = trace.dma.size();
    const StoreResult firstDrawStore = invokeStore({0u, 0u, static_cast<uint16_t>(kDrawWidth),
                                                    static_cast<uint16_t>(kDrawHeight), 0u, kDrawFbw,
                                                    static_cast<uint8_t>(kPsmCt32)},
                                                   kDrawWidth * kDrawHeight * 4u);
    assertDrawStore(firstDrawStore, expectedDraw(false), "first");
    check(trace.dma.size() == dmaBeforeFirstDrawStore + 1u && trace.dma.back() == 2u,
          "G1-E first StoreImage did not append exactly one GIF DMA completion");
    trace.drawnStoreImage = firstDrawStore.bytes;

    submit(redrawPacket); // second draw is a new own-EOP packet over interior pixels.
    const size_t dmaBeforeSecondDrawStore = trace.dma.size();
    const StoreResult secondDrawStore = invokeStore({0u, 0u, static_cast<uint16_t>(kDrawWidth),
                                                     static_cast<uint16_t>(kDrawHeight), 0u, kDrawFbw,
                                                     static_cast<uint8_t>(kPsmCt32)},
                                                    kDrawWidth * kDrawHeight * 4u);
    assertDrawStore(secondDrawStore, expectedDraw(true), "second");
    check(trace.dma.size() == dmaBeforeSecondDrawStore + 1u && trace.dma.back() == 2u &&
          secondDrawStore.bytes != firstDrawStore.bytes,
          "G1-E second StoreImage did not materialize the fresh drawn target");
    trace.redrawnStoreImage = secondDrawStore.bytes;

    // Reinitialize the active draw state before later independent transfer and
    // control cases in this shared synthetic runtime fixture.
    submit(drawStatePacket());

    const auto rejectStore = [&](StoreImageMem image, const char *message) {
        const auto beforeCsr = memory.read64(kCsr);
        const auto beforeId = memory.read64(kSiglblid);
        const auto beforeDma = trace.dma;
        const StoreResult result = invokeStore(image, 16u);
        check(result.status == -1 &&
              std::all_of(result.bytes.begin(), result.bytes.end(), [](uint8_t value) { return value == 0xa5u; }) &&
              result.leadingSentinel && result.trailingSentinel && memory.read64(kCsr) == beforeCsr &&
              memory.read64(kSiglblid) == beforeId && trace.dma == beforeDma,
              message);
    };
    rejectStore({0u, 0u, 4u, 1u, 0u, 1u, static_cast<uint8_t>(kPsmT4)},
                "StoreImage T4 reached DMA or changed its destination");
    rejectStore({0u, 0u, 4u, 1u, 0u, 1u, 63u},
                "StoreImage unknown PSM reached DMA or changed its destination");
    rejectStore({0u, 0u, 0u, 1u, 0u, 1u, static_cast<uint8_t>(kPsmCt32)},
                "StoreImage zero width reached DMA or changed its destination");
    rejectStore({0u, 0u, 4095u, 4095u, 0u, 1u, static_cast<uint8_t>(kPsmCt32)},
                "StoreImage excessive count reached DMA or changed its destination");

    const std::vector<uint8_t> tail20{{0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u,
                                       0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu, 0x30u,
                                       0x31u, 0x32u, 0x33u, 0x34u}};
    submit(hostToLocalPacket(tail20, 5u));
    submit(localToHostPacket(5u));
    expectRead(std::vector<uint8_t>(tail20.begin(), tail20.begin() + 16u), 16u, kReadBitbltbuf, trxreg(5u),
               "CT32 20-byte transfer first QWC differs");
    expectRead(std::vector<uint8_t>(tail20.begin() + 16u, tail20.end()), 4u, kReadBitbltbuf, trxreg(5u),
               "CT32 20-byte transfer exact tail differs");

    // Reset after a partial native download abandons that old cursor. A new
    // guest-issued transaction is still independently valid.
    submit(hostToLocalPacket(split32, 8u));
    submit(localToHostPacket(8u));
    expectRead(std::vector<uint8_t>(split32.begin(), split32.begin() + 16u), 16u, kReadBitbltbuf, trxreg(8u),
               "reset fixture first native local-to-host split differs");
    memory.write32(kCsr, 0x200u);
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(8u),
                       "GS reset retained a stale partial local-to-host cursor");

    const std::vector<uint8_t> fresh16{{0xdeu, 0xadu, 0xbeu, 0xefu, 0x01u, 0x23u, 0x45u, 0x67u,
                                         0x89u, 0xabu, 0xcdu, 0xefu, 0x10u, 0x32u, 0x54u, 0x76u}};
    submit(hostToLocalPacket(fresh16, 4u));
    submit(localToHostPacket(4u));
    expectRead(fresh16, 16u, kReadBitbltbuf, trxreg(4u), "consecutive fresh local-to-host download differs");

    submit(hostToLocalPacket(split32, 8u));
    submit(localToHostPacket(8u));
    submit(packed({{0x50u, kReadBitbltbuf | (uint64_t(2u) << 16u)}}));
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(8u),
                       "descriptor rewrite during active download was accepted");
    submit(hostToLocalPacket(fresh16, 4u));
    submit(localToHostPacket(4u));
    expectRead(fresh16, 16u, kReadBitbltbuf, trxreg(4u),
               "fresh download after rejected descriptor rewrite differs");

    // Duplicate SIGNAL holds a suffix.  Result calls must not acknowledge it,
    // mutate their destinations, or consume the retained producer work.
    submit(packed({{0x60u, id(1u)}, {0x60u, id(2u)}, {0x62u, id(3u)}}));
    check(memory.gsControl().stalled() && memory.read64(kSiglblid) == siglblid(1u, 0u),
          "duplicate SIGNAL did not retain its actual composed suffix");
    std::array<uint8_t, 16> rejected{};
    rejected.fill(0xa5u);
    const auto beforeCsr = memory.read64(kCsr);
    const auto beforeId = memory.read64(kSiglblid);
    const auto beforeBlockedBytes = memory.gsBlockedGifBytes();
    const auto beforeBlockedCount = memory.gsBlockedGifTransferCount();
    const auto beforeBlockedDma = trace.dma;
    bool rejectedRead = false, rejectedSnapshot = false, rejectedRestore = false;
    try { (void)runtime.readActiveGsBackendLocalMemory(rejected.data(), rejected.size(), kReadBitbltbuf, 0u, uint64_t(1u) << 32u | 4u); }
    catch (const std::runtime_error &) { rejectedRead = true; }
    std::vector<uint8_t> rejectedSnapshotBytes{0x7au};
    try { (void)runtime.snapshotActiveGsBackendLocalMemory(rejectedSnapshotBytes); }
    catch (const std::runtime_error &) { rejectedSnapshot = true; }
    try { (void)runtime.restoreActiveGsBackendLocalMemory(nullptr, 0u); }
    catch (const std::runtime_error &) { rejectedRestore = true; }
    // The actual linked StoreImage HLE must follow the same fail-closed result
    // boundary.  Its own descriptor/GIF-DMA submission cannot acknowledge the
    // retained SIGNAL suffix or write into the caller's guest destination.
    const uint32_t blockedImageAddr = runtime.guestMalloc(sizeof(StoreImageMem), 4u);
    const uint32_t blockedAllocation = runtime.guestMalloc(48u, 16u);
    check(blockedImageAddr != 0u && blockedAllocation != 0u,
          "blocked StoreImage fixture allocation failed");
    StoreImageMem blockedImage{0u, 0u, 4u, 1u, 0u, 1u, static_cast<uint8_t>(kPsmCt32)};
    std::memcpy(memory.getRDRAM() + blockedImageAddr, &blockedImage, sizeof(blockedImage));
    std::fill(memory.getRDRAM() + blockedAllocation, memory.getRDRAM() + blockedAllocation + 48u, 0xa5u);
    R5900Context blockedCtx{};
    setArg(blockedCtx, 4, blockedImageAddr);
    setArg(blockedCtx, 5, blockedAllocation + 16u);
    bool blockedStoreRejected = false;
    try { ps2_stubs::sceGsExecStoreImage(memory.getRDRAM(), &blockedCtx, &runtime); }
    catch (const std::runtime_error &) { blockedStoreRejected = true; }
    check(blockedStoreRejected &&
          std::all_of(memory.getRDRAM() + blockedAllocation + 16u,
                      memory.getRDRAM() + blockedAllocation + 32u,
                      [](uint8_t value) { return value == 0xa5u; }) &&
          trace.dma == beforeBlockedDma && memory.gsBlockedGifBytes() == beforeBlockedBytes &&
          memory.gsBlockedGifTransferCount() == beforeBlockedCount && memory.read64(kCsr) == beforeCsr &&
          memory.read64(kSiglblid) == beforeId,
          "blocked StoreImage submitted DMA, queued a retained descriptor, or changed guest control/destination");
    runtime.guestFree(blockedAllocation);
    runtime.guestFree(blockedImageAddr);
    check(rejectedRead && rejectedSnapshot && rejectedRestore &&
          std::all_of(rejected.begin(), rejected.end(), [](uint8_t value) { return value == 0xa5u; }) &&
          rejectedSnapshotBytes == std::vector<uint8_t>({0x7au}) && memory.read64(kCsr) == beforeCsr &&
          memory.read64(kSiglblid) == beforeId && memory.gsControl().stalled(),
          "blocked result operation changed guest-visible producer state");
    memory.write32(kCsr, 1u);
    check(!memory.gsControl().stalled() && memory.read64(kSiglblid) == siglblid(2u, 3u),
          "CSR acknowledgement did not resume retained suffix in order");
    check(memory.gsBlockedGifBytes() == beforeBlockedBytes &&
          memory.gsBlockedGifTransferCount() == beforeBlockedCount && trace.dma == beforeBlockedDma,
          "CSR acknowledgement replayed a rejected StoreImage descriptor or armed a native transaction");
    expectRejectedRead(16u, kReadBitbltbuf, 0u, trxreg(4u),
                       "acknowledged blocked StoreImage left a native local-to-host transaction armed");
    hleSuccess(kPsmCt32, 4u, 1u, repeatedStore,
               "fresh StoreImage after blocked preflight rejection did not independently succeed");

    // CSR RESET must reset renderer/parser state but preserve the already owned
    // producer suffix.  The subsequent drain is explicit guest control, not a
    // result-operation drain.
    submit(packed({{0x60u, id(4u)}, {0x60u, id(5u)}, {0x62u, id(6u)}}));
    check(memory.gsControl().stalled(), "reset continuation fixture did not stall");
    memory.write32(kCsr, 0x200u);
    check(!memory.gsControl().stalled() && runtime.gifArbiter().controlPending(),
          "CSR RESET destroyed retained producer suffix");
    runtime.gifArbiter().drain();
    // RESET clears SIGLBLID itself, but it does not discard the staged second
    // SIGNAL: that retained command becomes the first post-reset SIGNAL before
    // the retained LABEL is consumed.
    check(memory.read64(kSiglblid) == siglblid(5u, 6u),
          "CSR RESET continuation did not preserve LABEL suffix");

    // StoreImage above has already contributed its own real GIF DMA
    // completions. Preserve that composed HLE trace for the inline/worker
    // comparison, then require this final direct GIF DMA to append exactly
    // one additional completion rather than assuming the old G1-A singleton.
    const size_t dmaBeforeFinalDirect = trace.dma.size();
    const auto dmaPacket = packed({{0x62u, id(7u)}});
    constexpr uint32_t address = 0x10000u;
    std::memcpy(memory.getRDRAM() + address, dmaPacket.data(), dmaPacket.size());
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGifDma + 0x10u, address);
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGifDma + 0x20u,
                    static_cast<uint32_t>(dmaPacket.size() / 16u));
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGifDma, 0x101u);
    // LABEL changes only the upper bank; the retained post-RESET SIGNAL
    // remains observable while the GIF-DMA completion supplies LABEL=7.
    check(memory.read64(kSiglblid) == siglblid(5u, 7u),
          "actual GIF DMA did not reach composed PCSX2 GS producer");
    check(trace.dma.size() == dmaBeforeFinalDirect + 1u && trace.dma.back() == 2u,
          "actual GIF DMA completion trace differs");
    trace.csr = memory.read64(kCsr);
    trace.siglblid = memory.read64(kSiglblid);
    ps2_syscalls::importIrqHandlerState({}, {});
    g_irqs = nullptr;
    return trace;
}

void compare(const Trace &inlineTrace, const Trace &workerTrace) {
    check(inlineTrace.local == workerTrace.local && inlineTrace.snapshot == workerTrace.snapshot &&
          inlineTrace.drawnStoreImage == workerTrace.drawnStoreImage &&
          inlineTrace.redrawnStoreImage == workerTrace.redrawnStoreImage &&
          inlineTrace.csr == workerTrace.csr && inlineTrace.siglblid == workerTrace.siglblid &&
          inlineTrace.dma == workerTrace.dma && inlineTrace.irqs.size() == workerTrace.irqs.size(),
          "inline and worker-sync real-GS result/register/DMA traces differ");
    for (size_t index = 0; index != inlineTrace.irqs.size(); ++index)
        check(inlineTrace.irqs[index].csr == workerTrace.irqs[index].csr &&
              inlineTrace.irqs[index].siglblid == workerTrace.irqs[index].siglblid,
              "inline and worker-sync real-GS IRQ trace differs");
}
}

int main() {
    @autoreleasepool {
        try {
            OwnerSurface owner;
            const Trace inlineTrace = exercise("inline", owner);
            const Trace workerTrace = exercise("worker-sync", owner);
            compare(inlineTrace, workerTrace);
            std::cout << "PASS: composed real PCSX2 GS producer result boundary inline=worker-sync\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
}
