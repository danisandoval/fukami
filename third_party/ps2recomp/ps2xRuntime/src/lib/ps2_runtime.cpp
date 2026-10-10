#include "rrv_guest_rtc.cpp"
// RRV product SDL host overlay
#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_gs_gpu.h"
#include "rrv_gate4_owner_timeline.h" // Gate-4 S0 owner timeline (RRV_GATE4_OWNER_TIMELINE)
#include "runtime/diag_counters.h" // RRV_RUNTIME_LOG performance counters
#include "ThreadNaming.h"
#include "rrv_gs_record_hooks.h"
#if !defined(PS2X_RRV_FIELD_ONLY)
#include "rrv_ir_hooks.h"
#endif
#include "rrv_present_dump_ticks.h"
#include "rrv_snapshot_hooks.h"  // developer snapshots (docs/SNAPSHOTS.md)
#include "rrv_cadence_diag.h"    // P0-cadence (C0) frame-budget probe; default off
#include "rrv_window_title.h"    // window title: live GS backend + guest frame rate
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "rrv_sdl_audio.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>
#include <thread>
#include <exception>
#include <set>
#include <unordered_map>
#include <sstream>
#include <stdexcept>

namespace ps2_stubs
{
    void resetSifState();
}

// RRV_FLAG_A_M1_DIAG helpers live beside GIF ingress in ps2_memory.cpp.
bool rrvFlagAM1DiagEnabled();
void rrvNoteFlagAM1Present(unsigned phase, uint64_t nowNs);
void rrvReportFlagAM1Rates(double seconds);

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct HostFrameProbePoint
    {
        uint32_t x;
        uint32_t y;
    };

    constexpr HostFrameProbePoint kGhostProbePoints[] = {
        {220u, 176u},
        {260u, 208u},
        {320u, 208u},
        {260u, 240u},
        {320u, 240u},
        {260u, 272u},
        {320u, 272u},
    };

    uint32_t sampleHostFramePixel(const std::vector<uint8_t> &pixels,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t x,
                                  uint32_t y)
    {
        if (x >= width || y >= height)
        {
            return 0u;
        }

        const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
        if (offset + 4u > pixels.size())
        {
            return 0u;
        }

        return static_cast<uint32_t>(pixels[offset + 0u]) |
               (static_cast<uint32_t>(pixels[offset + 1u]) << 8) |
               (static_cast<uint32_t>(pixels[offset + 2u]) << 16) |
               (static_cast<uint32_t>(pixels[offset + 3u]) << 24);
    }

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    thread_local std::unordered_map<PS2Runtime *, uint32_t> g_guestExecutionDepths;

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectDispatchRecoveryPc(const PS2Runtime *runtime)
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return 0u;
        }

        uint32_t firstHigh = 0u;
        for (uint32_t step = 1u; step <= count; ++step)
        {
            const uint32_t idx = (h.next + h.pcs.size() - step) % static_cast<uint32_t>(h.pcs.size());
            const uint32_t pc = h.pcs[idx];
            if (pc < 0x00100000u)
            {
                continue;
            }
            if (runtime && !runtime->hasFunction(pc))
            {
                continue;
            }

            if (firstHigh == 0u)
            {
                firstHigh = pc;
                continue;
            }

            return pc;
        }

        return firstHigh;
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    uint32_t readGuestU32Wrapped(const uint8_t *rdram, uint32_t addr)
    {
        if (!rdram)
        {
            return 0;
        }

        uint32_t value = 0;
        value |= static_cast<uint32_t>(rdram[(addr + 0u) & PS2_RAM_MASK]) << 0;
        value |= static_cast<uint32_t>(rdram[(addr + 1u) & PS2_RAM_MASK]) << 8;
        value |= static_cast<uint32_t>(rdram[(addr + 2u) & PS2_RAM_MASK]) << 16;
        value |= static_cast<uint32_t>(rdram[(addr + 3u) & PS2_RAM_MASK]) << 24;
        return value;
    }

    uint64_t readGuestU64Wrapped(const uint8_t *rdram, uint32_t addr)
    {
        const uint64_t lo = readGuestU32Wrapped(rdram, addr);
        const uint64_t hi = readGuestU32Wrapped(rdram, addr + 4u);
        return lo | (hi << 32);
    }

    uint32_t selectStackRecoveryPc(const uint8_t *rdram, const R5900Context *ctx, const PS2Runtime *runtime)
    {
        if (!rdram || !ctx || !runtime)
        {
            return 0u;
        }

        const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
        constexpr uint32_t kScanBytes = 0x200u;

        for (uint32_t offset = 0u; offset < kScanBytes; offset += 8u)
        {
            const uint32_t slotAddr = sp + offset;
            const uint32_t ra32 = static_cast<uint32_t>(readGuestU64Wrapped(rdram, slotAddr));
            if (ra32 < 0x00100000u)
            {
                continue;
            }
            if (!runtime->hasFunction(ra32))
            {
                continue;
            }
            return ra32;
        }

        for (uint32_t offset = 0u; offset < kScanBytes; offset += 4u)
        {
            const uint32_t slotAddr = sp + offset;
            const uint32_t ra32 = readGuestU32Wrapped(rdram, slotAddr);
            if (ra32 < 0x00100000u)
            {
                continue;
            }
            if (!runtime->hasFunction(ra32))
            {
                continue;
            }
            return ra32;
        }

        return 0u;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

PS2Runtime::GuestExecutionScope::GuestExecutionScope(PS2Runtime *runtime)
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_runtime->enterGuestExecution();
    }
}

PS2Runtime::GuestExecutionScope::~GuestExecutionScope()
{
    if (m_runtime)
    {
        m_runtime->leaveGuestExecution();
    }
}

namespace
{
    // C0 frame budget: this scope is opened at EVERY deliberate guest block
    // (Thread.cpp sleep/wait, Sync.cpp semaphore wait, Interrupt.cpp vsync
    // wait), so timing it captures all guest idling in one place. Kept as a
    // file-local thread_local rather than a member so the header/ABI is
    // untouched.
    thread_local uint64_t g_cadenceReleaseStartNs = 0u;
    thread_local uint32_t g_cadenceReleaseDepth = 0u;
}

PS2Runtime::GuestExecutionReleaseScope::GuestExecutionReleaseScope(PS2Runtime *runtime)
    : m_runtime(runtime)
{
    if (m_runtime)
    {
        m_depth = m_runtime->releaseGuestExecution();
    }
    if (rrv::cadence::enabled() && g_cadenceReleaseDepth++ == 0u)
    {
        g_cadenceReleaseStartNs = rrv::cadence::nowNs();
    }
}

PS2Runtime::GuestExecutionReleaseScope::~GuestExecutionReleaseScope()
{
    if (rrv::cadence::enabled() && g_cadenceReleaseDepth != 0u &&
        --g_cadenceReleaseDepth == 0u)
    {
        rrv::cadence::t_blockNs += rrv::cadence::nowNs() - g_cadenceReleaseStartNs;
    }
    if (m_runtime && m_depth != 0u)
    {
        m_runtime->reacquireGuestExecution(m_depth);
    }
}

namespace
{
    thread_local uint32_t g_backEdgeYieldSuppressionDepth = 0u;


}

PS2Runtime::BackEdgeYieldSuppressionScope::BackEdgeYieldSuppressionScope() noexcept
{
    ++g_backEdgeYieldSuppressionDepth;
}

PS2Runtime::BackEdgeYieldSuppressionScope::~BackEdgeYieldSuppressionScope()
{
    if (g_backEdgeYieldSuppressionDepth != 0u)
    {
        --g_backEdgeYieldSuppressionDepth;
    }
}

void PS2Runtime::setHostPresentationConfig(const HostPresentationConfig &config)
{
    if (m_gsBackend.active())
        throw std::logic_error("host presentation must be configured before GS initialization");
    m_hostPresentation = config;
}

void PS2Runtime::dispatchGsPacket(GifPathId pathId, const uint8_t *data, uint32_t size,
                                  uint32_t vu1Pc, const GifPacketDiagnostic *diagnostic)
{
    rrv::gate4::OwnerScope gate4Owner(rrv::gate4::OwnerKind::GsPacket, size);
    // RRV owns PATH1/2/3 arbitration. The optional bridge sees precisely the
    // packet selected by that arbiter, not a PCSX2 DMA/VIF reconstruction.
    if (m_gsBackend.active())
    {
        std::string error;
        if (m_gsBackend.submit(static_cast<uint32_t>(pathId), data, size, &error))
        {
#if !defined(PS2X_RRV_FIELD_ONLY)
            // F7 FullFrame keeps PCSX2 as the guest-visible GS authority,
            // then decodes the exact same post-arbitration packet once through
            // RRV's local GS as an IR/VRAM shadow.  The shadow's decode-only
            // mode suppresses software rasterization, so it cannot contribute
            // a field snapshot or alter PCSX2 submission timing.
            if (m_gsBackend.renderMode() == rrv::gs::GsRenderMode::FullFrame)
            {
                auto cadenceShadowGs = rrv::cadence::shadowGsScope();
                m_gs.processGIFPacket(pathId, data, size, vu1Pc, diagnostic);
            }
#endif
            // Gate-5 split: the backend already queued the readback after the packet.
            if (m_gsBackend.readbackDeferred())
                return;
            uint64_t csr = 0u;
            uint64_t siglblid = 0u;
            if (!m_gsBackend.readback(csr, siglblid, &error))
            {
                throw std::runtime_error("PCSX2 GS bridge readback failed: " + error);
            }
            // Signal/finish/label side effects are guest-visible through the
            // privileged GS registers. RRV continues to own their MMIO reads
            // and interrupt delivery; the bridge supplies only GS-produced
            // state after decoding the already-arbitrated packet.
            gate4StoreReadbackV1(csr, siglblid);
            return;
        }
        throw std::runtime_error("PCSX2 GS bridge packet dispatch failed: " + error);
    }

    m_gs.processGIFPacket(pathId, data, size, vu1Pc, diagnostic);
}

namespace
{
    void copyBridgeReadback(PS2Runtime &runtime, rrv::gsbackend::Backend &backend,
                            const char *operation)
    {
        std::string error;
        uint64_t csr = 0u;
        uint64_t siglblid = 0u;
        if (!backend.readback(csr, siglblid, &error))
        {
            throw std::runtime_error(std::string("PCSX2 GS bridge ") + operation +
                                     " readback failed: " + error);
        }
        runtime.gate4StoreReadbackV1(csr, siglblid);
    }

    void recordBackendFieldBoundary(const std::array<uint64_t, 19u> &regs19,
                                    uint64_t fieldIndex)
    {
        // PCSX2 owns rasterization, so the legacy IR stream is intentionally
        // absent here. GSR presents remain backend-neutral field anchors for
        // capture/cadence; they must not be mistaken for a PCSX2 IR capture.
        if (rrv::gsrecord::rrv_gs_record_enabled())
        {
            rrv::gsrecord::RecordPresent present{};
            present.pmode = regs19[0];
            present.smode2 = regs19[2];
            present.dispfb1 = regs19[7];
            present.display1 = regs19[8];
            present.dispfb2 = regs19[9];
            present.display2 = regs19[10];
            present.vsyncTick = fieldIndex;
            rrv::gsrecord::hookPresent(present);
        }
    }
}

void PS2Runtime::advanceActiveGsBackendField(uint64_t fieldIndex)
{
    rrv::gate4::OwnerScope gate4Owner(rrv::gate4::OwnerKind::FieldVsync, 19u * 8u);
    if (!m_gsBackend.active())
        return;

    // GSRegisters is explicitly 19 contiguous uint64_t fields. The bridge ABI
    // deliberately snapshots that stable, privileged-register ordering rather
    // than exposing any runtime or PCSX2 C++ type across the boundary.
    static_assert(sizeof(GSRegisters) == 19u * sizeof(uint64_t),
                  "PCSX2 GS bridge expects the GSRegisters 19-slot ABI");
    std::array<uint64_t, 19u> regs19{};
    std::memcpy(regs19.data(), &m_memory.gs(), sizeof(GSRegisters));
    constexpr uint64_t kCsrFieldBit = 1ull << 13u;
    regs19[15] = (regs19[15] & ~kCsrFieldBit) |
                  ((gate3TemporalV1().field()) != 0 ? kCsrFieldBit : 0u);

    if (m_vu1gsModeV1 != Vu1GsModeV1::Inline)
    {
        const uint32_t parity = static_cast<uint32_t>(gate3TemporalV1().field());
        m_memory.ownerPostV1([this, regs19, fieldIndex, parity]
                             { gate4AdvanceFieldOwnerV1(regs19, fieldIndex, parity); },
                             sizeof(regs19), 1u, false);
        return;
    }

    std::string error;
    if (!m_gsBackend.vsync(regs19.data(), fieldIndex,
                           static_cast<uint32_t>(gate3TemporalV1().field()), &error))
    {
        throw std::runtime_error("PCSX2 GS bridge field transition failed: " + error);
    }

    gate4FinishFieldV1(regs19, fieldIndex);
}

// The owner-side tail of a field transition (inline: the same thread).
void PS2Runtime::gate4FinishFieldV1(const std::array<uint64_t, 19u> &regs19, uint64_t fieldIndex)
{
    copyBridgeReadback(*this, m_gsBackend, "post-vsync");
    // Capture selection is part of the same serialized field transition as
    // vsync/direct present.  The SDL event thread must never query a moving
    // guest field and then attempt to retroactively tag a capture.
    if (directGpuPresentationActive())
        captureDirectPresentationIfRequested(fieldIndex);
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (m_gsBackend.renderMode() == rrv::gs::GsRenderMode::FullFrame)
    {
        // `vsync()` above synchronized PCSX2's completed list. Its packed local
        // memory is therefore the exact start state of the NEXT list. Preserve
        // it separately while the snapshot retained from the preceding
        // boundary replaces the CURRENT IR frame's decode-only-shadow seed.
        std::vector<uint8_t> nextFrameStartVram;
        std::string snapshotError;
        if (!m_gsBackend.snapshotLocalMemory(nextFrameStartVram, &snapshotError))
        {
            throw std::runtime_error("PCSX2 FullFrame boundary snapshot failed: " + snapshotError);
        }
        if (!m_liveFullFrameNextStartVram.empty() &&
            !rrv::ir::rrv_ir_set_live_frame_start_vram(
                m_liveFullFrameNextStartVram.data(),
                static_cast<uint32_t>(m_liveFullFrameNextStartVram.size())))
        {
            throw std::runtime_error("live FullFrame rejected the authoritative PCSX2 frame-start snapshot");
        }
        // Close exactly the shadow's completed packet/IR list at the same
        // vblank boundary PCSX2 observed.  Its host pixels are deliberately
        // discarded; hookPresent() publishes only the semantic frame to the
        // progressive consumer.  Re-read PCSX2 state afterwards because the
        // shadow may have decoded SIGNAL/FINISH/LABEL while closing. The
        // The interrupt worker already owns the authoritative tick. Passing it
        // into the local shadow prevents a state->vsync lock inversion while
        // preserving the guest-execution ordering around the vblank callbacks.
        m_gs.latchHostPresentationFrame(fieldIndex);
        m_liveFullFrameNextStartVram = std::move(nextFrameStartVram);
        copyBridgeReadback(*this, m_gsBackend, "post-full-frame shadow latch");
    }
#endif
    recordBackendFieldBoundary(regs19, fieldIndex);
}

bool PS2Runtime::latchActiveGsBackendPresentationFrame()
{
    // Field transitions occur in the vblank worker. The frontend's upload
    // cadence only samples the latest completed bridge frame.
    return m_gsBackend.active();
}

bool PS2Runtime::activeGsBackend() const
{
    return m_gsBackend.active();
}

bool PS2Runtime::directGpuPresentationActive() const
{
    return m_gsBackend.active() &&
           m_gsBackend.presentationMode() == rrv::gsbackend::PresentationMode::DirectGpu;
}

bool PS2Runtime::presentationStats(rrv::gsbackend::PresentationStats &stats, std::string *error)
{
    return m_gsBackend.presentationStats(stats, error);
}

#if !defined(PS2X_RRV_FIELD_ONLY)
bool PS2Runtime::activeGsBackendUsesFullFrame() const
{
    return m_gsBackend.active() &&
           m_gsBackend.renderMode() == rrv::gs::GsRenderMode::FullFrame;
}
#endif

void PS2Runtime::submitActiveGsBackendRegisterPairs(const uint64_t *pairs, uint32_t pairCount)
{
    rrv::gate4::OwnerScope gate4Owner(rrv::gate4::OwnerKind::HleRegs, 16ull + 16ull * pairCount);
    if (!m_gsBackend.active() || pairCount == 0u)
        return;
    if (!pairs || pairCount > 0x7fffu)
        throw std::runtime_error("invalid HLE GS A+D register sequence");

    // One packed A+D GIF tag followed by guest-order {value, register} pairs.
    // Direct HLE operations do not traverse GifArbiter, so path 3 is a narrow
    // provenance marker only; bytes are already final GS packet bytes.
    std::vector<uint64_t> packet(static_cast<size_t>(pairCount) * 2u + 2u);
    packet[0] = static_cast<uint64_t>(pairCount) | (1ull << 15u) | (1ull << 60u);
    packet[1] = 0xEull;
    for (uint32_t index = 0u; index < pairCount; ++index)
    {
        const uint64_t reg = pairs[static_cast<size_t>(index) * 2u + 1u];
        if ((reg & ~0xffull) != 0u)
            throw std::runtime_error("invalid HLE GS A+D register address");
        packet[static_cast<size_t>(index) * 2u + 2u] = pairs[static_cast<size_t>(index) * 2u];
        packet[static_cast<size_t>(index) * 2u + 3u] = reg;
    }

    // Direct HLE sequences bypass GifArbiter. Record and snapshot-trigger the
    // same post-arbitration bytes immediately before their PCSX2 submission,
    // preserving the order a backend actually observes.
    const uint32_t packetBytes = static_cast<uint32_t>(packet.size() * sizeof(uint64_t));
    if (rrv::gsrecord::rrv_gs_record_enabled())
        rrv::gsrecord::hookGifPacket(3u, reinterpret_cast<const uint8_t *>(packet.data()), packetBytes);
    if (rrv::snapshot::enabled())
        rrv::snapshot::hookGifPacket(3u, packetBytes);

    if (m_vu1gsModeV1 != Vu1GsModeV1::Inline)
    {
        auto owned = std::make_shared<std::vector<uint64_t>>(std::move(packet));
        m_memory.ownerPostV1([this, owned, packetBytes]
        {
            std::string ownerError;
            if (!m_gsBackend.submit(3u, reinterpret_cast<const uint8_t *>(owned->data()),
                                    packetBytes, &ownerError))
                throw std::runtime_error("PCSX2 GS bridge HLE register submission failed: " + ownerError);
            copyBridgeReadback(*this, m_gsBackend, "HLE register submission");
        }, packetBytes, 0u, false);
        return;
    }
    std::string error;
    if (!m_gsBackend.submit(3u, reinterpret_cast<const uint8_t *>(packet.data()),
                            packetBytes, &error))
    {
        throw std::runtime_error("PCSX2 GS bridge HLE register submission failed: " + error);
    }
    copyBridgeReadback(*this, m_gsBackend, "HLE register submission");
}

bool PS2Runtime::readActiveGsBackendLocalMemory(uint8_t *dst, uint32_t byteCount,
                                                uint64_t bitbltbuf, uint64_t trxpos,
                                                uint64_t trxreg)
{
    if (!m_gsBackend.active())
        return false;
    rrv::gate4::ownerBarrier(rrv::gate4::BarrierKind::LocalRead);
    gate4FenceV1();

    std::string error;
    if (!m_gsBackend.readLocalMemory(dst, byteCount, bitbltbuf, trxpos, trxreg, &error))
    {
        throw std::runtime_error("PCSX2 GS bridge local-to-host transfer failed: " + error);
    }
    return true;
}

bool PS2Runtime::snapshotActiveGsBackendLocalMemory(std::vector<uint8_t> &outBytes)
{
    GuestExecutionScope guestExecution(this);
    if (!m_gsBackend.active())
        return false;

    gate4FenceV1();
    std::string error;
    if (m_gsBackend.snapshotLocalMemory(outBytes, &error))
        return true;
    throw std::runtime_error("PCSX2 GS bridge local-memory snapshot failed: " + error);
}

bool PS2Runtime::restoreActiveGsBackendLocalMemory(const uint8_t *bytes, uint32_t byteCount)
{
    GuestExecutionScope guestExecution(this);
    if (!m_gsBackend.active())
        return false;

    gate4FenceV1();
    std::string error;
    if (m_gsBackend.restoreLocalMemory(bytes, byteCount, &error))
        return true;
    throw std::runtime_error("PCSX2 GS bridge local-memory restore failed: " + error);
}

bool PS2Runtime::copyActiveGsBackendPresentationFrame(std::vector<uint8_t> &outPixels,
                                                       uint32_t &outWidth,
                                                       uint32_t &outHeight,
                                                       uint64_t *outLiveVsyncTick,
                                                       bool *outNewLivePresent)
{
    if (outLiveVsyncTick) *outLiveVsyncTick = 0u;
    if (outNewLivePresent) *outNewLivePresent = false;
    if (!m_gsBackend.active())
    {
        return false;
    }

    std::string error;
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (m_gsBackend.renderMode() == rrv::gs::GsRenderMode::FullFrame)
    {
        // PCSX2 remains the GS timing/CSR authority in FullFrame mode, but its
        // field snapshot is never a substitute for the application-owned
        // progressive result.  Missing IR, resources, or Metal support is a
        // hard failure rather than a weave/fallback.
        // The frontend starts polling before the first vblank has closed a
        // shadow display list. Keep that startup fetch blank; it must not
        // sample PCSX2's field snapshot or turn a normal boot ordering into a
        // terminal Metal failure.
        if (rrv::ir::rrv_ir_live_full_frame_startup_pending())
            return false;
        rrv::ir::LiveFullFramePresentInfo presentInfo{};
        if (rrv::ir::rrv_ir_render_live_full_frame(&outPixels, &outWidth, &outHeight, &error,
                                                   &presentInfo))
        {
            if (outLiveVsyncTick && presentInfo.hasVsyncTick)
                *outLiveVsyncTick = presentInfo.vsyncTick;
            if (outNewLivePresent)
                *outNewLivePresent = presentInfo.newlyRendered;
            return true;
        }
        std::fprintf(stderr, "[rrv-full-frame] terminal live-present rejection: %s\n", error.c_str());
        throw std::runtime_error("live FullFrame presentation failed closed: " + error);
    }
#endif
    if (m_gsBackend.copyFrame(outPixels, outWidth, outHeight, &error))
    {
        return true;
    }

    throw std::runtime_error("PCSX2 GS bridge presentation snapshot failed: " + error);
}

PS2Runtime::PS2Runtime()
{
    // B-5: identify m_vu0 (VU0 micro-mode instance) to the shared interpreter
    // for the RRV_VU0_FMAND_DIAG probe / RRV_VU_FLAG_PIPELINE model, which are
    // keyed off byte PCs inside the boot VU0 microprogram specifically — never
    // the general-purpose m_vu1 instance.
    m_vu0.setIsVu0(true);

    std::memset(&m_cpuContext, 0, sizeof(m_cpuContext));

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_functionTable.clear();

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        ps2_syscalls::detachAllGuestHostThreads();
        gate4ShutdownOwnerStreamV1();
        m_gsBackend.shutdown();
        m_audioBackend.stopAll();
        rrv::audio::shutdown();
        m_audioBackend.setAudioReady(false);

        m_loadedModules.clear();

        m_functionTable.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    // Default ON (2026-07-11): frame-clear presentation eliminates the empty→
    // partial→full present-cycling (validated A/B on attract — steady complete
    // frames, no cycling, camera advances). Opt out with RRV_PRESENT_AT_FRAME_CLEAR=0.
    static const bool s_presentAtFrameClearEnabled = []()
    {
        const char *value = std::getenv("RRV_PRESENT_AT_FRAME_CLEAR");
        return !value || (value[0] && value[0] != '0');
    }();
    m_gs.setPresentAtFrameClearEnabled(s_presentAtFrameClearEnabled);
    rrv::gsbackend::InitializeOptions backendOptions = m_hostPresentation.backend;
    if (backendOptions.presentationMode != rrv::gsbackend::PresentationMode::DirectGpu)
    {
        backendOptions.snapshotWidth = FB_WIDTH;
        backendOptions.snapshotHeight = DEFAULT_DISPLAY_HEIGHT;
    }
    else if (!backendOptions.surface.mainThreadPrepared || !backendOptions.surface.nativeView ||
             !backendOptions.surface.nativeLayer || !m_hostPresentation.pumpEvents ||
             !m_hostPresentation.shouldClose || !m_hostPresentation.currentSurface)
    {
        std::cerr << "Direct GPU presentation requires a prepared platform surface and event callbacks" << std::endl;
        return false;
    }
    std::string gsBackendError;
    if (!m_gsBackend.initialize(backendOptions, &gsBackendError))
    {
        std::cerr << "Failed to initialize selected GS backend: " << gsBackendError << std::endl;
        return false;
    }
#if defined(PS2X_RRV_FIELD_ONLY)
    if (m_gsBackend.active() &&
        m_gsBackend.renderMode() != rrv::gs::GsRenderMode::Field)
    {
        std::cerr << "Field-only producer rejected a non-field GS bridge mode" << std::endl;
        m_gsBackend.shutdown();
        return false;
    }
#endif
    m_gifArbiter.setProcessPacketFn([this](GifPathId pathId, const uint8_t *data,
                                           uint32_t size, uint32_t vu1Pc,
                                           const GifPacketDiagnostic *diagnostic)
                                    { dispatchGsPacket(pathId, data, size, vu1Pc, diagnostic); });
    m_memory.setGifArbiter(&m_gifArbiter);
    gate4ConfigureOwnerStreamV1();
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t itop, uint32_t top)
                                 {
                                     // RRV_VU1_LEAN: execute() with its per-call switches resolved once.
                                     if (!m_vu1.aotLean(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                        m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                        m_gs, &m_memory, startPC, itop, 65536, top))
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, itop, 65536, top);
                                 });
    m_memory.setVu1MscntCallback([this](uint32_t itop, uint32_t top)
                                 { m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                m_gs, &m_memory, itop, 65536, top); });
    m_memory.setDmacCompletionCallback([this](uint32_t cause)
                                       { ps2_syscalls::dispatchDmacHandlersForCause(
                                             m_memory.getRDRAM(), this, cause); });
    m_iop.init(rdram);
    m_iop.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

        if (!directGpuPresentationActive())
            throw std::logic_error("product runtime requires SDL direct presentation");
        rrv::audio::initialize();
        m_audioBackend.setAudioReady(rrv::audio::ready());
        std::fprintf(stderr, "[m1-runtime] SDL direct GPU presentation enabled; "
                     "SDL audio-ready=%s\n", rrv::audio::ready() ? "yes" : "no");

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
        m_asyncCallbackStackMainBase = 0u;
        m_asyncCallbackStackMainLimit = 0u;
        m_asyncCallbackStackMainReserved = false;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    ps2_game_overrides::applyMatching(*this, elfPath, m_cpuContext.pc);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

void PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    m_functionTable[address] = func;
    if (address < PS2_RAM_SIZE && (address & 3u) == 0u)
    {
        auto &page = m_fnPages[address >> kFnPageShift];
        if (!page)
            page.reset(new RecompiledFunction[(1u << kFnPageShift) >> 2]()); // zeroed
        page[(address & ((1u << kFnPageShift) - 1u)) >> 2] = func;
    }
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    if (flatFunction(address))
        return true;
    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        return true;
    }

    return false;
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    if (const RecompiledFunction flat = flatFunction(address))
        return flat;
    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        return it->second;
    }

    std::cerr << "Warning: Function at address 0x" << std::hex << address << std::dec << " not found" << std::endl;

    static RecompiledFunction defaultFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
        const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
        const uint32_t gp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) : 0u;
        const uint32_t a0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) : 0u;
        const uint32_t a1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) : 0u;
        const uint32_t v0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0)) : 0u;
        const uint32_t v1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0)) : 0u;

        if (ctx && runtime)
        {
            thread_local uint32_t s_recoverCount = 0u;
            thread_local bool s_loggedContext = false;
            const uint32_t pc = ctx->pc;
            const bool hasPcFunction = runtime->hasFunction(pc);

            if (!hasPcFunction && s_recoverCount < 8192u)
            {
                if (!s_loggedContext)
                {
                    std::ostringstream stackDump;
                    if (rdram)
                    {
                        stackDump << " [stack]";
                        for (uint32_t off = 0u; off < 0x40u; off += 4u)
                        {
                            const uint32_t slot = readGuestU32Wrapped(rdram, sp + off);
                            stackDump << " +" << std::hex << off << "=0x" << slot;
                        }
                    }
                    std::cerr << "[dispatch:first-bad-pc] bad=0x" << std::hex << pc
                              << " ra=0x" << ra
                              << " sp=0x" << sp
                              << " gp=0x" << gp
                              << " v0=0x" << v0
                              << " v1=0x" << v1
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " trace=" << formatDispatchHistory()
                              << stackDump.str()
                              << std::dec << std::endl;
                    s_loggedContext = true;
                }

                uint32_t recoveryPc = 0u;
                if (ra != 0u && runtime->hasFunction(ra))
                {
                    recoveryPc = ra;
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }

                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 256u)
                    {
                        std::cerr << "[dispatch:recover-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }

            if (hasPcFunction)
            {
                s_recoverCount = 0u;
                s_loggedContext = false;
            }
            else if (pc < 0x00100000u && ra == pc && s_recoverCount < 4096u)
            {
                uint32_t recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }
                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 128u)
                    {
                        std::cerr << "[dispatch:recover-low-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }
        }

        std::ostringstream oss;
        oss << "Error: Called unimplemented function at address 0x" << std::hex << (ctx ? ctx->pc : 0u)
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " hostTid=" << std::this_thread::get_id()
            << " pcTrace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_defaultFnLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_defaultFnLogMutex);
            std::cerr << oss.str() << std::endl;
        }

        runtime->requestStop();
    };

    return defaultFunction;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // RRV_VU0_LEAN: a flat per-entry counter instead of a hash lookup on every
    // call (this runs once per vertex); saturating, only < 3 and > 3 are read.
    static std::unordered_map<uint32_t, int> seen;
    static int seenFlat[PS2_VU0_CODE_SIZE / 8u];
    int &count = (address < PS2_VU0_CODE_SIZE && (address & 7u) == 0u)
                     ? seenFlat[address >> 3]
                     : seen[address];
    if (count < 3)
    {
        RUNTIME_LOG("[VU0] microprogram @0x" << std::hex << address
                                             << " pc=0x" << ctx->pc
                                             << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0))
                                             << std::dec << std::endl);
        const uint8_t *code = m_memory.getVU0Code();
        uint32_t nonzero = 0u;
        uint64_t hash = 14695981039346656037ull;
        if (code)
        {
            for (uint32_t i = 0u; i < PS2_VU0_CODE_SIZE; ++i)
            {
                nonzero += code[i] != 0u ? 1u : 0u;
                hash ^= code[i];
                hash *= 1099511628211ull;
            }
        }
        std::fprintf(stderr,
                     "[vu0:execute] address=%03x code-nonzero=%u code-fnv1a64=%016llx\n",
                     address, nonzero, static_cast<unsigned long long>(hash));
    }
    if (count < 4)
        ++count;

    // RRV_VU0_DIAG=1 — uncapped census of VU0 MICRO-mode entries (default off).
    // The [vu0:execute] log above is capped at 3 per entry address, so it is a
    // boot-time sample and says nothing about a mid-attract scene. This is the
    // mirror of the scratch PCSX2 RRV_GT_FNHITS counters: the two engines' VU0
    // micro activity for one scene can then be compared at the same guest
    // coordinate. Lock-free so arming it does not perturb frame timing.
    static const bool s_vu0Diag = [] {
        const char *v = std::getenv("RRV_VU0_DIAG");
        return v && v[0] && v[0] != '0';
    }();
    if (s_vu0Diag)
    {
        static constexpr size_t kSlots = 8u;
        static std::atomic<uint32_t> s_addr[kSlots];
        static std::atomic<uint64_t> s_hits[kSlots];
        static std::atomic<uint64_t> s_total{0};
        const uint32_t key = address + 1u; // 0 marks an unused slot
        for (size_t i = 0u; i < kSlots; ++i)
        {
            uint32_t cur = s_addr[i].load(std::memory_order_relaxed);
            if (cur == 0u)
            {
                uint32_t expected = 0u;
                if (!s_addr[i].compare_exchange_strong(expected, key,
                                                       std::memory_order_relaxed))
                    cur = expected;
                else
                    cur = key;
            }
            if (cur == key)
            {
                s_hits[i].fetch_add(1u, std::memory_order_relaxed);
                break;
            }
        }
        const uint64_t n = s_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 2000u) == 0u)
        {
            std::fprintf(stderr, "[vu0:diag] total=%llu", (unsigned long long)n);
            for (size_t i = 0u; i < kSlots; ++i)
            {
                const uint32_t a = s_addr[i].load(std::memory_order_relaxed);
                if (a == 0u)
                    continue;
                std::fprintf(stderr, " %03x=%llu", a - 1u,
                             (unsigned long long)s_hits[i].load(std::memory_order_relaxed));
            }
            std::fputc('\n', stderr);
        }
    }

    auto logVf = [&](const char *phase, uint32_t reg, __m128 value)
    {
        if (count > 3)
            return;
        alignas(16) float lanes[4];
        _mm_storeu_ps(lanes, value);
        std::fprintf(stderr,
                     "[vu0:reg] entry=%03x %s vf%u=%g,%g,%g,%g\n",
                     address, phase, reg, lanes[0], lanes[1], lanes[2], lanes[3]);
    };
    logVf("in", 12u, ctx->vu0_vf[12]);
    logVf("in", 28u, ctx->vu0_vf[28]);
    logVf("in", 31u, ctx->vu0_vf[31]);

    if (!ctx || !m_memory.getVU0Code() || !m_memory.getVU0Data())
        return;

    // RRV_VU0_WHITE_DIAG (B-5, default off): boundary capture for the white Namco
    // builder sub_00222EB8. ctx->pc identifies the exact VCALLMS site inside the
    // builder, correlating each entry (0/0x10/0x20/0x30/0x40) with the white
    // path. Logs entry/exit registers (vertex I/O + persistent-constant range),
    // VU0 data-memory population, and dumps code+data once to RRV_VU0_WHITE_DIAG's
    // directory (game-derived bytes: /tmp-only, never committed).
    static const char *whiteDiagDir = std::getenv("RRV_VU0_WHITE_DIAG");
    const bool isWhiteSite =
        ctx->pc >= 0x222eb8u && ctx->pc < 0x223e98u; // sub_00222EB8 body
    static std::atomic<uint32_t> whiteHits{0u};
    uint32_t whiteHit = 0u;
    const bool whiteDiag = whiteDiagDir && isWhiteSite &&
                           (whiteHit = whiteHits.fetch_add(1u)) < 60u;
    auto dumpRegs = [&](const char *phase)
    {
        if (!whiteDiag)
            return;
        char line[1024];
        int off = std::snprintf(line, sizeof(line),
                                "[vu0white:%u] %s site=%06x entry=%03x ra=%08x",
                                whiteHit, phase, ctx->pc, address,
                                static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)));
        // Vertex I/O regs + likely persistent constants (vf13..vf26 cover the
        // range setup entries would populate; hex bit patterns, not %g, so int
        // payloads and NaN encodings stay distinguishable).
        static const uint8_t regs[] = {12u, 27u, 28u, 30u, 31u,
                                       13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u,
                                       21u, 22u, 23u, 24u, 25u, 26u};
        for (uint8_t r : regs)
        {
            alignas(16) uint32_t b[4];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(b),
                             _mm_castps_si128(ctx->vu0_vf[r]));
            off += std::snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                                 " vf%u=%08x,%08x,%08x,%08x", r, b[0], b[1], b[2], b[3]);
            if (off >= static_cast<int>(sizeof(line)) - 48)
                break;
        }
        std::fprintf(stderr, "%s\n", line);
    };
    if (whiteDiag)
    {
        const uint8_t *dmem = m_memory.getVU0Data();
        uint32_t dnz = 0u;
        uint64_t dh = 14695981039346656037ull;
        for (uint32_t i = 0u; i < PS2_VU0_DATA_SIZE; ++i)
        {
            dnz += dmem[i] != 0u ? 1u : 0u;
            dh ^= dmem[i];
            dh *= 1099511628211ull;
        }
        std::fprintf(stderr,
                     "[vu0white:%u] datamem nonzero=%u fnv=%016llx vi1=%04x vi2=%04x vi3=%04x\n",
                     whiteHit, dnz, static_cast<unsigned long long>(dh),
                     ctx->vi[1], ctx->vi[2], ctx->vi[3]);
        static std::atomic<bool> dumped{false};
        if (!dumped.exchange(true))
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/vu0_code.bin", whiteDiagDir);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(m_memory.getVU0Code(), 1, PS2_VU0_CODE_SIZE, f);
                std::fclose(f);
            }
            std::snprintf(path, sizeof(path), "%s/vu0_data.bin", whiteDiagDir);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(dmem, 1, PS2_VU0_DATA_SIZE, f);
                std::fclose(f);
            }
        }
    }
    dumpRegs("in ");

    // VCALLMS encodes an instruction index; the micro interpreter tracks a byte PC.
    // VU0 and VU1 share the same microinstruction encoding, so use the existing
    // interpreter with a dedicated state instance and synchronize the COP2-visible
    // registers around the call. The guest uploads its own microcode through the
    // modeled 0x11000000 code window; no title-specific program is embedded here.
    // The hand-over itself is vu0LoadStateV1 / vu0RunStateV1 / vu0StoreStateV1 below, shared with the
    // burst entry (vu0BurstBeginV1 ...) so both move exactly the same state.
    const uint32_t clipBefore = ctx->vu0_clip_flags;
    vu0LoadStateV1(ctx);
    vu0RunStateV1(address, ctx->vu0_itop, ctx->vu0_top);
    vu0StoreStateV1(ctx, clipBefore);
    logVf("out", 12u, ctx->vu0_vf[12]);
    logVf("out", 28u, ctx->vu0_vf[28]);
    logVf("out", 31u, ctx->vu0_vf[31]);
    dumpRegs("out");
}

// ---- VU0 micro-mode register hand-over -------------------------------------------------------
// VCALLMS encodes an instruction index; the micro interpreter tracks a byte PC. VU0 and VU1 share
// the same microinstruction encoding, so VU0 runs on the VU interpreter with its own state instance
// and the COP2-visible registers are moved in before a microprogram and out after it. The guest
// uploads its own microcode through the modeled 0x11000000 code window; no title-specific program
// is embedded here.
void PS2Runtime::vu0LoadStateV1(const R5900Context *ctx)
{
    VU1State &vu = m_vu0.state();
    // Once per vertex in the race. One block copy each way instead of 32
    // 16-byte moves: the same 512 bytes (Steam Deck profile 2026-10-02: the
    // two register copies were 46% of executeVU0Microprogram).
    static_assert(sizeof(vu.vf) == sizeof(ctx->vu0_vf), "VU0 VF file layout");
    std::memcpy(vu.vf, ctx->vu0_vf, sizeof(vu.vf));
    for (uint32_t i = 0; i < 16u; ++i)
        vu.vi[i] = static_cast<int16_t>(ctx->vi[i]);
    _mm_storeu_ps(vu.acc, ctx->vu0_acc);
    vu.q = ctx->vu0_q;
    vu.p = ctx->vu0_p;
    vu.i = ctx->vu0_i;
    vu.mac = ctx->vu0_mac_flags;
    vu.clip = ctx->vu0_clip_flags;
    vu.status = ctx->vu0_status;
}

void PS2Runtime::vu0RunStateV1(uint32_t address, uint32_t itop, uint32_t top)
{
    // `address` is already a BYTE offset into VU0 micro memory: the recompiler's
    // translateVU_VCALLMS/VCALLMSR extract the 9-bit instruction index from the
    // opcode and shift left by 3 before emitting the call (code_generator.cpp).
    // Multiplying by 8 again here double-scaled every nonzero entry (e.g. white
    // Namco builder's VCALLMS 0x20 -> byte 0x100 = the middle of another
    // routine's body instead of jump-table row i004), which skipped the per-entry
    // ITOF input conversions and produced the zeroed white vertices (B-5).
    // Entry 0 only ever worked because 0*8 == 0.
    if (!m_vu0.aotLean(m_memory.getVU0Code(), PS2_VU0_CODE_SIZE,
                       m_memory.getVU0Data(), PS2_VU0_DATA_SIZE,
                       m_gs, &m_memory, address, itop, 4096u, top))
        m_vu0.execute(m_memory.getVU0Code(), PS2_VU0_CODE_SIZE,
                      m_memory.getVU0Data(), PS2_VU0_DATA_SIZE,
                      m_gs, &m_memory, address, itop, 4096u, top);
}

void PS2Runtime::vu0StoreStateV1(R5900Context *ctx, uint32_t clipBefore)
{
    const VU1State &vu = m_vu0.state();
    std::memcpy(ctx->vu0_vf, vu.vf, sizeof(vu.vf));
    for (uint32_t i = 0; i < 16u; ++i)
        ctx->vi[i] = static_cast<uint16_t>(vu.vi[i]);
    ctx->vu0_acc = _mm_loadu_ps(vu.acc);
    ctx->vu0_q = vu.q;
    ctx->vu0_p = vu.p;
    ctx->vu0_i = vu.i;
    ctx->vu0_mac_flags = vu.mac;
    ctx->vu0_clip_flags2 = clipBefore; // the clip register as it was before the (last) microprogram
    ctx->vu0_clip_flags = vu.clip;
    ctx->vu0_status = static_cast<uint16_t>(vu.status);
    // The interpreter uses byte PCs internally, but architectural TPC/CFC2 uses
    // microinstruction indices. Keep the unit conversion at this boundary.
    ctx->vu0_tpc = vu.pc / 8u;
}

// ---- VU0 burst (native builders in src/product) ----------------------------------------------
// RR5's car builder (func_222EB8) issues one VCALLMS per vertex and touches only a few VF registers
// between two calls. executeVU0Microprogram moves the whole register file into the interpreter and
// back around every call (1 KiB per vertex). A burst does that hand-over once: Begin loads the
// state, the caller reads and writes the returned VU1State where the guest code would use LQC2 /
// SQC2 / QMFC2, Run executes one microprogram, End stores the state. Between two Runs the
// per-call round trip through R5900Context is reproduced exactly: it narrows the status word to
// 16 bits and each VI register to a sign-extended 16-bit value, and nothing else. End leaves
// R5900Context as the same sequence of executeVU0Microprogram calls would have: clip_flags2 is the
// clip register before the last microprogram, and with no Run at all only the VF file is written
// back (the caller's LQC2 stand-ins). Diagnostics that key on single calls (RRV_VU0_DIAG, the
// [vu0:execute] sample, RRV_VU0_WHITE_DIAG) do not see burst calls; callers keep to
// executeVU0Microprogram when one of them is on (vu0BurstAvailableV1).
bool PS2Runtime::vu0BurstAvailableV1() const
{
    static const bool diag = [] {
        for (const char *name : {"RRV_VU0_DIAG", "RRV_VU0_WHITE_DIAG"})
            if (const char *v = std::getenv(name); v && v[0] && v[0] != '0')
                return true;
        return false;
    }();
    return !diag && m_memory.getVU0Code() && m_memory.getVU0Data();
}

VU1State &PS2Runtime::vu0BurstBeginV1(const R5900Context *ctx)
{
    vu0LoadStateV1(ctx);
    m_vu0BurstRunsV1 = 0;
    m_vu0BurstClipV1 = ctx->vu0_clip_flags;
    return m_vu0.state();
}

void PS2Runtime::vu0BurstRunV1(const R5900Context *ctx, uint32_t address)
{
    VU1State &vu = m_vu0.state();
    // What vu0StoreStateV1 followed by vu0LoadStateV1 does to the interpreter state.
    for (uint32_t i = 0; i < 16u; ++i)
        vu.vi[i] = static_cast<int16_t>(static_cast<uint16_t>(vu.vi[i]));
    vu.status = static_cast<uint16_t>(vu.status);
    m_vu0BurstClipV1 = vu.clip;
    vu0RunStateV1(address, ctx->vu0_itop, ctx->vu0_top);
    ++m_vu0BurstRunsV1;
}

void PS2Runtime::vu0BurstEndV1(R5900Context *ctx)
{
    if (m_vu0BurstRunsV1 != 0)
    {
        vu0StoreStateV1(ctx, m_vu0BurstClipV1);
        return;
    }
    const VU1State &vu = m_vu0.state();
    std::memcpy(ctx->vu0_vf, vu.vf, sizeof(vu.vf));
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    gate3ChargeSyscallHleV1(ctx, syscallId);
    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

PS2Runtime::AsyncCallbackStackWatermarks PS2Runtime::asyncCallbackStackWatermarks() const
{
    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t snapshotTop = m_asyncCallbackStackTop;
    if (m_asyncCallbackStackMainReserved && m_asyncCallbackStackMainLimit > m_asyncCallbackStackFloor &&
        snapshotTop > m_asyncCallbackStackMainBase)
    {
        // The snapshot format carries only a top/floor pair. Cap the exported
        // top below the main stack for a fresh-runtime restore.
        snapshotTop = std::max(m_asyncCallbackStackFloor, m_asyncCallbackStackMainBase & ~(kGuestHeapDefaultAlignment - 1u));
    }
    return {snapshotTop, m_asyncCallbackStackFloor};
}

bool PS2Runtime::reserveAsyncCallbackStackMainThread(uint32_t stackBase, uint32_t stackLimit)
{
    if (stackBase >= stackLimit || stackLimit > PS2_RAM_SIZE)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    // This is deliberately a startup declaration.  Existing callback frames
    // have no relocation protocol, so never retroactively move their range.
    if (m_asyncCallbackStackMainReserved)
    {
        return m_asyncCallbackStackMainBase == stackBase && m_asyncCallbackStackMainLimit == stackLimit;
    }
    if (m_asyncCallbackStackTop != PS2_RAM_SIZE)
    {
        return false;
    }

    m_asyncCallbackStackMainBase = stackBase;
    m_asyncCallbackStackMainLimit = stackLimit;
    m_asyncCallbackStackMainReserved = true;
    return true;
}

bool PS2Runtime::restoreAsyncCallbackStackWatermarks(AsyncCallbackStackWatermarks watermarks)
{
    // reserveAsyncCallbackStack() aligns all reservations to at least 16 bytes.
    // Reject malformed snapshot data instead of broadening the allocator into
    // arbitrary restored EE RAM.
    if ((watermarks.floor & (kGuestHeapDefaultAlignment - 1u)) != 0u ||
        (watermarks.top & (kGuestHeapDefaultAlignment - 1u)) != 0u)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    if (watermarks.floor != m_asyncCallbackStackFloor ||
        watermarks.top < watermarks.floor ||
        watermarks.top > m_asyncCallbackStackTop)
    {
        return false;
    }

    m_asyncCallbackStackTop = watermarks.top;
    return true;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (m_asyncCallbackStackMainReserved &&
        base < m_asyncCallbackStackMainLimit && top > m_asyncCallbackStackMainBase)
    {
        // A single callback frame cannot straddle the main-thread stack.
        // Continue below the declared interval; space above it remains usable
        // whenever the requested frame fit there without overlap.
        top = m_asyncCallbackStackMainBase & ~(kGuestHeapDefaultAlignment - 1u);
        if (top <= allocSize)
        {
            return 0u;
        }
        base = top - allocSize;
        base &= ~(normalizedAlignment - 1u);
    }
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)
{
    const auto rrvTerminalOutcome = terminalOutcomeHandle();
    uint32_t lastPc = std::numeric_limits<uint32_t>::max();
    uint32_t samePcCount = 0;
    constexpr uint32_t kSamePcYieldInterval = 0x4000u;

    while (!isStopRequested())
    {
        // Developer snapshots (docs/SNAPSHOTS.md §3): THE safepoint. This point
        // — top of the dispatch loop, before any recompiled function is entered
        // — is the only place where this thread holds no guest host frames, so
        // it is the only place a capture can be taken that is resumable from
        // {memory, devices, ctx} alone. No-op unless armed from the command
        // line (one relaxed atomic load).
        if (rrv::snapshot::enabled())
        {
            rrv::snapshot::hookDispatchBoundary(this, ctx);
        }

        // RRV_GIF_PATH_LATENCY backstop (default on; =0 rollback). A held
        // VIF1 chain must not outlive the frame that kicked it; this is the only
        // point that is both on the guest thread and free of live guest host
        // frames, which expanding VU1 + submitting GIF packets requires. Early
        // out is one cached bool plus an empty() check.
        m_memory.flushHeldVif1AtFrameBoundary();

        const uint32_t pc = ctx->pc;

        if (pc == lastPc)
        {
            ++samePcCount;
            if ((samePcCount % kSamePcYieldInterval) == 0u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    RUNTIME_LOG("CPU is doing some work at PC 0x" << std::hex << pc << ". PC not updating.");
                });
                std::this_thread::yield();
            }
        }
        else
        {
            samePcCount = 0;
            lastPc = pc;
        }

        m_debugPc.store(pc, std::memory_order_relaxed);
        // C0 frame budget: guest-work proxy (dispatched recompiled functions per
        // guest frame) plus the live guest PC a sampler can histogram. One
        // relaxed load when RRV_CADENCE_DIAG is unset.
        rrv::cadence::noteDispatch(pc);
        m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), std::memory_order_relaxed);
        m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)), std::memory_order_relaxed);
        m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)), std::memory_order_relaxed);

        // RRV_CD_INDEX_DIAG (temporary, task_ca53a515): sub_002967B0
        // (0x2967B0, config/output/sub_002967B0_0x2967b0.cpp) computes
        // tableBase(0x1D72570)+a0*112 and writes to it with NO bounds mask,
        // unlike the read/validate side (sub_00296710) which masks the same
        // style of index to &0x3F (0-63). The pc-zero crash's bad jump target
        // (0x1d7be20) is exactly tableBase+349*112. Log every call's a0 (the
        // unmasked index) and its caller (ra) to catch the offending caller
        // and the actual index value live.
        if (pc == 0x2967B0u)
        {
            static const bool s_cdIndexDiag = [] {
                const char *v = std::getenv("RRV_CD_INDEX_DIAG");
                return v && v[0] && v[0] != '0';
            }();
            if (s_cdIndexDiag)
            {
                const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
                const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                static thread_local uint32_t s_maxA0 = 0u;
                const bool newMax = (a0 > s_maxA0);
                if (newMax)
                {
                    s_maxA0 = a0;
                }
                if (newMax || static_cast<int32_t>(a0) > 63)
                {
                    std::cerr << "[cd-index-diag] a0=" << a0 << " (0x" << std::hex << a0
                              << std::dec << ") ra=0x" << std::hex << ra << std::dec
                              << (static_cast<int32_t>(a0) > 63 ? " OUT-OF-RANGE" : "")
                              << std::endl;
                }
            }
        }

        // Product qualification records a rejected target before the producer
        // fallback can attempt diagnostic recovery.  Registered indirect/re-entry
        // targets keep their existing dispatch behavior and create no outcome.
        if (!hasFunction(pc))
        {
            rrv::guestoutcome::recordFailure(
                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::RejectedMainDispatch,
                {pc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),
                 static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0))},
                "unregistered main dispatch target");
        }
        RecompiledFunction fn = lookupFunction(pc);
        const uint32_t dispatchedPc = pc;
        const uint32_t dispatchedRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));

        // Fine-grained indirect-jump tracing (aggressive-logs only): a bare `jr $reg`
        // sets ctx->pc to the register's value and returns straight here. If a *real*
        // function/label just handed control to an address with no registered function,
        // that resume entry (dispatchedPc) is the precise source of a bad indirect jump
        // — pinpointing the offending jr/jalr that the function-level dispatch history
        // can only bracket. Guarded so the extra hasFunction() lookups stay out of the
        // normal hot path.
        [[maybe_unused]] bool dispatchedPcWasReal = false;
        PS2_IF_AGRESSIVE_LOGS({ dispatchedPcWasReal = hasFunction(dispatchedPc); });

        {
            GuestExecutionScope guestExecution(this);
            fn(rdram, ctx, this);
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (dispatchedPcWasReal && ctx->pc != 0u && ctx->pc != dispatchedPc && !hasFunction(ctx->pc))
            {
                static thread_local uint32_t s_indirectBadLogged = 0u;
                if (s_indirectBadLogged < 16u)
                {
                    ++s_indirectBadLogged;
                    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
                    std::cerr << "[dispatch:indirect-to-bad] from=0x" << std::hex << dispatchedPc
                              << " badTarget=0x" << ctx->pc
                              << " ra=0x" << ra
                              << " sp=0x" << sp
                              << " trace=" << formatDispatchHistory()
                              << std::dec << std::endl;
                }
            }
        });

        if (ctx->pc == 0u)
        {
            rrv::guestoutcome::recordMainCompletion(
                *rrvTerminalOutcome,
                {ctx->pc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),
                 static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0))});
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            std::cerr << "[dispatch:pc-zero] from=0x" << std::hex << dispatchedPc
                      << " fromRa=0x" << dispatchedRa
                      << " ra=0x" << ra
                      << " sp=0x" << sp
                      << " gp=0x" << gp
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;

            // PC=0 means this guest thread returned (usually via jr $ra with RA=0).
            // Do not request a global runtime stop here: other guest threads may still run.
            break;
        }
    }
}

// Compiled by the effective producer runtime and by the asset-free TU harness.
void PS2Runtime::enterGuestExecution()
{
    auto& depth = g_guestExecutionDepths[this];
    if (!depth) gate3Scheduler.enter(rrv::guest_time::context_id);
    m_guestExecutionMutex.lock();
    ++depth;
}
void PS2Runtime::leaveGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || !it->second) return;
    const bool last = --it->second == 0;
    m_guestExecutionMutex.unlock();
    if (last) {
        g_guestExecutionDepths.erase(it);
        gate3Scheduler.release(rrv::guest_time::context_id);
    }
}
uint32_t PS2Runtime::releaseGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || !it->second)
        throw std::logic_error("Gate3 park without guest ownership");
    const uint32_t depth = it->second;
    for (uint32_t i = 0; i < depth; ++i) m_guestExecutionMutex.unlock();
    g_guestExecutionDepths.erase(it);
    gate3Scheduler.release(rrv::guest_time::context_id);
    return depth;
}
void PS2Runtime::reacquireGuestExecution(uint32_t depth)
{
    for (uint32_t i = 0; i < depth; ++i) enterGuestExecution();
}
void PS2Runtime::gate3CheckpointSchedulerV1(int rotate)
{
    const int id = rrv::guest_time::context_id;
    // Gate-4: an unchanged scheduler gives the same stay/live answer again.
    if (rotate < 0 && gate3Scheduler.quiet(id)) return;
    const uint64_t version = gate3Scheduler.version();
    const bool yield = gate3Scheduler.checkpoint(id, rotate);
    if (yield) {
        GuestExecutionReleaseScope release(this);
    }
    if (gate3Scheduler.terminated(id))
        throw rrv::guest_time::ThreadExit();
    if (rotate < 0 && !yield) gate3Scheduler.markQuiet(id, version);
}
bool PS2Runtime::shouldPreemptGuestExecution()
{
    gate3CheckpointSchedulerV1();
    return false; // Same-stack handoff: never abandon a generated continuation.
}


// Gate-4 store path (scripts/gate4_store_path_overlay.py): scratchpad fast path
// switch, read once (RRV_GATE4_SPR_FAST=0 disables it for A/B runs).
static const bool g_rrvSprFastV1 = [] {
    const char *v = std::getenv("RRV_GATE4_SPR_FAST");
    return !(v && v[0] == '0');
}();
static inline uint8_t *rrvSprFastV1(PS2Memory &memory, uint32_t vaddr, uint32_t size)
{
    const uint32_t offset = vaddr - PS2_SCRATCHPAD_BASE;
    if (!g_rrvSprFastV1 || g_ps2PathWatchArmed || offset >= PS2_SCRATCHPAD_SIZE || (vaddr & (size - 1u)) != 0u)
        return nullptr;
    uint8_t *spr = memory.getScratchpad();
    return spr ? spr + offset : nullptr;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 1u))
    {
        uint8_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 2u))
    {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 4u))
    {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 8u))
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 16u))
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 1u))
    {
        std::memcpy(p, &value, sizeof(value));
        return;
    }
    if (g_ps2PathWatchArmed) ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 2u))
    {
        std::memcpy(p, &value, sizeof(value));
        return;
    }
    if (g_ps2PathWatchArmed) ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 4u))
    {
        std::memcpy(p, &value, sizeof(value));
        return;
    }
    if (g_ps2PathWatchArmed) ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    // B3 (docs/TESTING.md §B3): name the GUEST PC that starts each DMA channel.
    // This is the only place a channel-control store still carries the guest
    // context, so it is the one point where a kick can be attributed to the
    // code that issued it. Metadata only -- never payload bytes. Default off.
    {
        static const bool kickProbe = []
        {
            const char *v = std::getenv("RRV_DMA_KICK_PC");
            return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }();
        // D_CHCR for every channel is 0x1000n000; bit 8 (STR) starts it.
        if (kickProbe && ctx && (vaddr & 0xFFFF0FFFu) == 0x10000000u &&
            (vaddr & 0x0000F000u) <= 0x0000A000u && (value & 0x100u) != 0u)
        {
            // Also read the guest-side submission state machine that drives
            // sub_0021FE88: [0x346E84] = state index, [0x346E80] = pending
            // channel mask, and the chain-root slot table at 0x003470C0.
            uint32_t st = 0, mask = 0, madr = 0, tadr = 0;
            if (rdram)
            {
                std::memcpy(&st, rdram + 0x346E84u, 4);
                std::memcpy(&mask, rdram + 0x346E80u, 4);
            }
            madr = m_memory.readIORegister((vaddr & 0xFFFFF000u) | 0x010u);
            tadr = m_memory.readIORegister((vaddr & 0xFFFFF000u) | 0x030u);
            std::fprintf(stderr,
                         "[dma-kick] chcr=%08x val=%08x guest-pc=%08x ra=%08x "
                         "state=%u mask=%u madr=%08x tadr=%08x\n",
                         vaddr, value, ctx->pc,
                         static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),
                         st, mask, madr, tadr);
        }
    }
    try
    {
        m_memory.write32(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 8u))
    {
        std::memcpy(p, &value, sizeof(value));
        return;
    }
    if (g_ps2PathWatchArmed) ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    if (uint8_t *p = rrvSprFastV1(m_memory, vaddr, 16u))
    {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(p), value);
        return;
    }
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    if (g_ps2PathWatchArmed) ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    gate3Scheduler.stop(-418);
    ps2_syscalls::notifyRuntimeStop();
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

// RRV_RUNTIME_LOG: definitions for the perf counters (declared in diag_counters.h)
namespace ps2_diag
{
    std::atomic<uint64_t> g_ticksCredited{0};
    std::atomic<uint64_t> g_framesPresented{0};
    std::atomic<uint64_t> g_triCount{0};
    std::atomic<uint64_t> g_rasterNs{0};
    std::atomic<uint64_t> g_pixelCount{0};
    std::atomic<uint64_t> g_primPixBucket[7] = {};
    std::atomic<uint64_t> g_parPixels{0};
    std::atomic<uint64_t> g_primCount{0};
    std::atomic<uint64_t> g_uploadEvents{0};
    std::atomic<uint64_t> g_batchBuckets[7] = {};
    std::atomic<uint64_t> g_vu1Ns{0};
    std::atomic<uint64_t> g_vu1Instrs{0};
    std::atomic<uint64_t> g_vu1Mscals{0};
    std::atomic<uint64_t> g_vu1MpgUploads{0};
}

void PS2Runtime::serviceDirectPresentation()
{
    // This executes on the product's SDL main thread. It neither acquires a
    // drawable nor waits for a Metal command buffer: PCSX2 does both inside
    // its GS field transition. The callback only owns SDL lifecycle events.
    m_hostPresentation.pumpEvents(m_hostPresentation.context);
    if (m_hostPresentation.shouldClose(m_hostPresentation.context))
    {
        requestStop();
        return;
    }

    rrv::gsbackend::NativeSurface updated{};
    if (m_hostPresentation.currentSurface(m_hostPresentation.context, &updated))
    {
        const auto &current = m_hostPresentation.backend.surface;
        if (updated.widthPixels != current.widthPixels ||
            updated.heightPixels != current.heightPixels ||
            std::fabs(updated.backingScale - current.backingScale) > 0.001f)
        {
            std::string error;
            if (!m_gsBackend.resize(updated.widthPixels, updated.heightPixels,
                                    updated.backingScale, &error))
            {
                throw std::runtime_error("direct SDL presentation resize failed: " + error);
            }
            m_hostPresentation.backend.surface = updated;
            std::fprintf(stderr, "[m1-runtime] direct surface resized pixels=%ux%u scale=%.3f\n",
                         updated.widthPixels, updated.heightPixels, updated.backingScale);
        }
    }
    // Event servicing only. Guest field pacing remains the existing interrupt
    // worker's 60000/1001 policy; this never adapts it to presentation speed.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void PS2Runtime::captureDirectPresentationIfRequested(uint64_t fieldIndex)
{
    static const char *dumpDirectory = std::getenv("RRV_PRESENT_DUMP");
    static const std::set<uint64_t> requestedTicks =
        rrv::present_dump::parseRequestedTicks(std::getenv("RRV_PRESENT_DUMP_TICKS"));
    static std::set<uint64_t> completedTicks;
    if (!dumpDirectory || !dumpDirectory[0] || requestedTicks.empty())
        return;

    if (!requestedTicks.contains(fieldIndex) || completedTicks.contains(fieldIndex))
        return;

    rrv::gsbackend::PresentationStats stats{};
    std::string error;
    if (!m_gsBackend.presentationStats(stats, &error))
        throw std::runtime_error("direct capture stats failed: " + error);
    // A selected capture tick is an explicit diagnostic contract. Never drop
    // it merely because the direct-present surface has no drawable: there is
    // no later field to which this tag can safely be reassigned.
    if (stats.directGpuPresents == 0u)
    {
        throw std::runtime_error("explicit direct capture requested at field " +
                                 std::to_string(fieldIndex) +
                                 " before any direct GPU present");
    }

    rrv::gsbackend::CaptureResult capture{};
    const rrv::gsbackend::CaptureTag tag{fieldIndex, fieldIndex,
                                         stats.directGpuPresents};
    if (!m_gsBackend.capture(tag, capture, &error))
        throw std::runtime_error("explicit direct presentation capture failed: " + error);

    char path[512];
    const int pathLength = std::snprintf(path, sizeof(path), "%s/t%llu-f%llu-p%llu.ppm", dumpDirectory,
                                         static_cast<unsigned long long>(capture.tag.guestTick),
                                         static_cast<unsigned long long>(capture.tag.fieldIndex),
                                         static_cast<unsigned long long>(capture.tag.presentIndex));
    if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(path))
        throw std::runtime_error("explicit direct capture path is too long");
    if (FILE *file = std::fopen(path, "wb"))
    {
        bool written = std::fprintf(file, "P6\n%u %u\n255\n", capture.width, capture.height) > 0;
        for (uint32_t y = 0u; y < capture.height; ++y)
        {
            const uint8_t *row = capture.rgba.data() + static_cast<size_t>(y) * capture.width * 4u;
            for (uint32_t x = 0u; x < capture.width; ++x)
                written = std::fwrite(row + x * 4u, 1u, 3u, file) == 3u && written;
        }
        const int closeResult = std::fclose(file);
        if (!written || closeResult != 0)
            throw std::runtime_error(std::string("cannot write complete explicit direct capture: ") + path);
    }
    else
    {
        throw std::runtime_error(std::string("cannot write explicit direct capture: ") + path);
    }
    completedTicks.insert(fieldIndex);
    std::fprintf(stderr,
                 "[m1-capture] guest-tick=%llu field=%llu present=%llu sequence=%llu "
                 "size=%ux%u renderer=PCSX2-hardware-GS backend=Metal presentation=direct-GPU\n",
                 static_cast<unsigned long long>(capture.tag.guestTick),
                 static_cast<unsigned long long>(capture.tag.fieldIndex),
                 static_cast<unsigned long long>(capture.tag.presentIndex),
                 static_cast<unsigned long long>(capture.sequence), capture.width, capture.height);
}

struct ThreadInfo; // global type, Kernel/Syscalls/Helpers/State.h
namespace ps2_syscalls { std::shared_ptr<::ThreadInfo> gate3InitializeMainThread(R5900Context*); void gate3CompleteMainThread(uint8_t*, R5900Context*, PS2Runtime*, const std::shared_ptr<::ThreadInfo>&); }

void PS2Runtime::run()
{
    // Gate3 candidate activated (owner authorization 2026-09-24).
    if (!m_gate3TemporalV1 || !m_gate3GuestWorkloadV1)
        throw std::logic_error("Gate3 candidate run requires a bound workload");
    const auto rrvTerminalOutcome = terminalOutcomeHandle();
    rrv::guestoutcome::begin(*rrvTerminalOutcome);
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    ps2_syscalls::resetSoundDriverRpcState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetGsSyncVCallbackState();
    ps2_stubs::resetMpegStubState();
    ps2_syscalls::initializeGuestKernelState(m_memory.getRDRAM());
    // ── argc / argv, as the EE kernel hands them to a disc-booted ELF ────────
    // We load the ELF directly, so nothing plays EELOAD's part. Real hardware
    // (and PCSX2, via eeloadHook/ParseArgumentString in pcsx2/R5900.cpp @
    // d5f75c9e4, GPL-3.0+) reaches the game's main() with argc = 1 and
    // argv[0] = the disc boot path. Starting at argc = 0 is not "no arguments";
    // it is a state the game is never in on hardware, and it makes the guest
    // skip storing its boot path — measured on RR5: hardware holds argc at
    // [0x00335100] = 1, argv at [0x00335104] -> "cdrom0:\SLUS_200.02;1", and we
    // held all three at zero.
    //
    // The block lives in the EELOAD region (PCSX2's EELOAD_START, R5900.h),
    // which is exactly where hardware's argv strings sit: below every game ELF
    // segment and above the kernel's syscall tables, so nothing else claims it.
    // RRV_BOOT_ARGV0 overrides the path; RRV_BOOT_ARGV0="" restores argc = 0.
    {
        constexpr uint32_t kArgBlock = 0x00082000u; // PCSX2 EELOAD_START
        const char *argv0 = std::getenv("RRV_BOOT_ARGV0");
        if (!argv0)
            argv0 = "cdrom0:\\SLUS_200.02;1";
        const size_t len = std::strlen(argv0);
        if (len != 0 && len < 0xF0u)
        {
            uint8_t *ram = m_memory.getRDRAM();
            const uint32_t strAddr = kArgBlock + 0x10u;
            std::memcpy(ram + strAddr, argv0, len + 1);
            const uint32_t ptr = strAddr;
            std::memcpy(ram + kArgBlock, &ptr, 4); // argv[0]
            m_cpuContext.r[4] = _mm_set_epi64x(0, 1);         // argc
            m_cpuContext.r[5] = _mm_set_epi64x(0, kArgBlock); // argv
            // The path that actually matters: RR5's crt0 ignores $a0/$a1 and
            // takes argc/argv from the block the kernel fills in SetupThread.
            ps2_syscalls::setBootArguments({std::string(argv0)});
        }
        else
        {
            m_cpuContext.r[4] = _mm_setzero_si128();
            m_cpuContext.r[5] = _mm_setzero_si128();
            ps2_syscalls::setBootArguments({});
        }
    }
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));

    // Developer snapshots (docs/SNAPSHOTS.md §3): the restore point. The guest
    // image is loaded, the HLE kernel has been reset and the default entry
    // context is set, but no guest thread is running yet — so a restore can
    // overwrite guest memory, device state and m_cpuContext with no
    // synchronisation at all. No-op unless --snapshot was passed.
    if (rrv::snapshot::enabled())
    {
        rrv::snapshot::hookRunStart(this);
    }

    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    if (!directGpuPresentationActive())
        throw std::logic_error("product runtime requires SDL direct presentation");

    g_activeThreads.store(1, std::memory_order_relaxed);
    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&, rrvTerminalOutcome]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        rrv::guest_time::context_id = 1;
        rrv::guest_time::QuietBump();
        GuestExecutionScope gate3MainLifetime(this);
        const auto gate3MainInfo = ps2_syscalls::gate3InitializeMainThread(&m_cpuContext);
        try
        {
            dispatchLoop(m_memory.getRDRAM(), &m_cpuContext);
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const rrv::guest_time::ThreadExit&) {}
        catch (const rrv::guest_time::TemporalCut&) { requestStop(); }
        catch (const std::exception &e)
        {
            rrv::guestoutcome::recordFailure(
                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::MainGuestException,
                {m_cpuContext.pc, static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)),
                 static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0))}, e.what());
            requestStop();
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            rrv::guestoutcome::recordFailure(
                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::MainGuestException,
                {m_cpuContext.pc, static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)),
                 static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0))}, "unknown main dispatch exception");
            requestStop();
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        ps2_syscalls::gate3CompleteMainThread(m_memory.getRDRAM(), &m_cpuContext, this, gate3MainInfo);
        g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
        gameThreadFinished.store(true, std::memory_order_release); });

    ps2_syscalls::EnsureVSyncWorkerRunning(m_memory.getRDRAM(), this);

    uint64_t tick = 0;
#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: sim-rate + camera-advancement rate — one summary per
    // wall-second, reported as [perf:t1]/[perf:hist]/[perf:cam]/[perf:vu1]/
    // [perf:batch]. Enable with -DRRV_RUNTIME_LOG=ON (see HANDOFF.md sec.4).
    auto perfLastReport = std::chrono::steady_clock::now();
    uint64_t perfTicksBase = ps2_diag::g_ticksCredited.load(std::memory_order_relaxed);
    uint64_t perfFramesBase = ps2_diag::g_framesPresented.load(std::memory_order_relaxed);
    uint64_t perfTriBase = ps2_diag::g_triCount.load(std::memory_order_relaxed);
    uint64_t perfRasterBase = ps2_diag::g_rasterNs.load(std::memory_order_relaxed);
    uint64_t perfPixBase = ps2_diag::g_pixelCount.load(std::memory_order_relaxed);
    uint64_t perfPrimsBase = ps2_diag::g_primCount.load(std::memory_order_relaxed);
    uint64_t perfUpsBase = ps2_diag::g_uploadEvents.load(std::memory_order_relaxed);
    // Camera eye pos = view-inverse (0x01e24e80) translation column q3 (m[12..14]).
    uint8_t *const perfRdram = m_memory.getRDRAM();
    auto perfReadF32 = [&](uint32_t a) -> float
    { float f; std::memcpy(&f, perfRdram + (a & PS2_RAM_MASK), 4); return f; };
    const uint32_t PERF_EYE = 0x01e24e80u + 0x30u;
    float perfPrevEye[3]{}; bool perfHaveEye = false;
    double perfPathAccum = 0.0; uint32_t perfCamSamples = 0;
#endif
    std::exception_ptr hostLoopFailure;
    try
    {
    while (!isStopRequested() && g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        serviceDirectPresentation();
    }
    }
    catch (...)
    {
        // UploadFrame()/host presentation can reject a completed FullFrame.
        // Do not unwind over a joinable gameThread: std::thread's destructor
        // would call terminate and hide the actual renderer/provenance error.
        hostLoopFailure = std::current_exception();
        rrv::guestoutcome::recordFailure(
            *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::HostRuntimeException,
            {},
            "host presentation/runtime exception");
        requestStop();
    }

    requestStop();

    const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gameThreadFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < joinDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (gameThread.joinable())
    {
        if (gameThreadFinished.load(std::memory_order_acquire))
        {
            gameThread.join();
        }
        else
        {
            rrv::guestoutcome::recordFailure(
                *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::ShutdownTimeout,
                {},
                "main guest thread shutdown timeout");
            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;
            gameThread.detach();
        }
    }

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < workerDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        requestStop();
        const auto finalWorkerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
               std::chrono::steady_clock::now() < finalWorkerDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (g_activeThreads.load(std::memory_order_relaxed) == 0)
    {
        ps2_syscalls::joinAllGuestHostThreads();
    }
    else
    {
        rrv::guestoutcome::recordFailure(
            *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::ShutdownTimeout,
            {},
            "guest worker shutdown timeout");
        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"
                  << std::endl;
        ps2_syscalls::detachAllGuestHostThreads();
    }

    const int remainingThreads = g_activeThreads.load(std::memory_order_relaxed);
    RUNTIME_LOG("[run] exiting loop, activeThreads=" << remainingThreads);
    if (remainingThreads > 0)
    {
        std::cerr << "[run] warning: " << remainingThreads
                  << " guest worker thread(s) still active during shutdown." << std::endl;
    }
    rrv::guestoutcome::markRunFinished(*rrvTerminalOutcome);

    // `gameThread` was joined or detached above, so this preserves the first
    // host-loop failure rather than converting it into std::terminate.
    if (hostLoopFailure)
        std::rethrow_exception(hostLoopFailure);
}

#include "rrv_guest_time.cpp"

#include "rrv_ee_timers.cpp"

#include "gate3_rrv_intc.inc"

#include "gate3_rrv_temporal_owner.inc"

#include "gate3_temporal_runtime.inc"

#include "gate3_guest_admission_runtime.inc"
#include "gate3_guest_admission_loader.inc"

// ---------------------------------------------------------------------------
// Gate-4 VU1+GS owner stream: PS2Runtime side
// (docs/evidence/GATE4_VU1_GS_WORKER_DESIGN_2026-09-25.md §4, S1-S3).
//
// RRV_VU1GS_EXECUTION selects where owner work runs (default inline):
//   inline         the original code, unchanged; no stream is installed
//   stream-inline  S1: the command stream, each command run at once on the EE
//   owner-sync     S2: each command runs on the GS owner thread; the EE waits
//   owner-async    S3: the EE waits only at barriers and at the queue caps
// owner-* need the GS worker (RRV_GS_EXECUTION=worker-sync).
// RRV_VU1GS_STRESS=<seed> (owner-* only) delays commands at random on the
// owner, so a missed barrier becomes a visible difference.
//
// Bridge-owned CSR/SIGLBLID results are kept in an owner copy and published
// to the guest bank only when the owner is idle (after each command in
// stream-inline/owner-sync, at barriers in owner-async). An EE write to CSR or
// SIGLBLID is itself a barrier and invalidates the owner copy, so the bank sees
// the same last-writer order as inline.
// ---------------------------------------------------------------------------

namespace
{
#if defined(__aarch64__)
    inline uint64_t gate4ReadFpcrV1() { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
    inline void gate4WriteFpcrV1(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
#else
    inline uint64_t gate4ReadFpcrV1() { return _mm_getcsr(); }
    inline void gate4WriteFpcrV1(uint64_t v) { _mm_setcsr(static_cast<unsigned>(v)); }
#endif
    // The inline path runs VU1 and GS work under the EE thread's floating-point
    // control; the GS owner thread otherwise runs PCSX2's default.
    struct Gate4ScopedFpcrV1
    {
        uint64_t saved;
        explicit Gate4ScopedFpcrV1(uint64_t value) : saved(gate4ReadFpcrV1())
        {
            if (value != saved)
                gate4WriteFpcrV1(value);
        }
        ~Gate4ScopedFpcrV1()
        {
            if (gate4ReadFpcrV1() != saved)
                gate4WriteFpcrV1(saved);
        }
    };
}

#if defined(__linux__) && defined(__x86_64__)
// Gate 5 (Steam Deck speed work), Linux x86-64 only; other builds keep the
// plain path. Each owner command is timed for the [cpu] log's owner split
// (src/host/rrv_thread_cpu_log.cpp, weak so tests link without it), and the
// MXCSR denormal-operand flag (DE, bit 1) is cleared before and read after,
// which counts commands that hit x86 denormal assists. MXCSR status flags do
// not change results; the saved MXCSR is restored after the command.
// RRV_OWNER_DAZ_FTZ=1 is a DIAGNOSTIC (default off, NOT equivalent: it treats
// denormals as zero on the owner thread, which IEEE ARM64/macOS does not): it
// shows how much of the owner time denormal assists cost on real hardware.
extern "C" void rrv_cpu_log_add_owner(unsigned long long ns, int denormal) __attribute__((weak));
extern "C" void rrv_cpu_log_set_split(int split) __attribute__((weak));
namespace
{
    const bool kGate5OwnerDazFtzV1 = []
    {
        const char *v = std::getenv("RRV_OWNER_DAZ_FTZ");
        const bool on = v && v[0] == '1' && v[1] == '\0';
        if (on)
            std::fprintf(stderr, "[vu1gs] RRV_OWNER_DAZ_FTZ=1: owner thread treats denormals as zero (diagnostic, not equivalent)\n");
        return on;
    }();
    void gate5OwnerCommandV1(const std::function<void()> &command, uint64_t fpcr)
    {
        const uint64_t value = (fpcr & ~uint64_t(0x2u)) | (kGate5OwnerDazFtzV1 ? uint64_t(0x8040u) : 0u);
        const auto start = std::chrono::steady_clock::now();
        bool denormal;
        {
            Gate4ScopedFpcrV1 scoped(value);
            command();
            denormal = (gate4ReadFpcrV1() & 0x2u) != 0u;
        }
        if (rrv_cpu_log_add_owner)
            rrv_cpu_log_add_owner(static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start).count()),
                                  denormal ? 1 : 0);
    }
}
#endif

void PS2Runtime::gate4StoreReadbackV1(uint64_t csr, uint64_t siglblid)
{
    if (m_vu1gsModeV1 == Vu1GsModeV1::Inline)
    {
        rrv::guest_time::QuietBump();
        m_memory.gs().csr = (csr & ~uint64_t(0x2008)) | (m_memory.gs().csr & 0x2008);
        m_memory.gs().siglblid = siglblid;
        return;
    }
    m_ownerCsrV1 = csr;
    m_ownerSiglblidV1 = siglblid;
    m_ownerCsrValidV1 = m_ownerSiglblidValidV1 = true;
}

void PS2Runtime::gate4PublishOwnerV1()
{
    auto &gs = m_memory.gs();
    if (m_ownerCsrValidV1)
        rrv::guest_time::QuietBump();
        gs.csr = (m_ownerCsrV1 & ~uint64_t(0x2008)) | (gs.csr & 0x2008);
    if (m_ownerSiglblidValidV1)
        gs.siglblid = m_ownerSiglblidV1;
}

void PS2Runtime::gate4FenceV1()
{
    if (m_vu1gsModeV1 == Vu1GsModeV1::Inline)
        return;
    if (m_vu1gsModeV1 == Vu1GsModeV1::OwnerAsync)
    {
        ++m_vu1gsFencesV1;
        m_gsBackend.ownerFence();
    }
    gate4PublishOwnerV1();
}

void PS2Runtime::gate4GuestGsWriteV1(uint32_t physical)
{
    if (m_vu1gsModeV1 == Vu1GsModeV1::Inline)
        return;
    gate4FenceV1();
    if (physical >= 0x12001000u && physical < 0x12001008u)
        m_ownerCsrValidV1 = false;
    if (physical >= 0x12001080u && physical < 0x12001088u)
        m_ownerSiglblidValidV1 = false;
}

void PS2Runtime::gate4ConfigureOwnerStreamV1()
{
    const char *mode = std::getenv("RRV_VU1GS_EXECUTION");
    if (!mode || !*mode || std::strcmp(mode, "inline") == 0)
    {
        m_vu1gsModeV1 = Vu1GsModeV1::Inline;
        return;
    }
    if (std::strcmp(mode, "stream-inline") == 0)
        m_vu1gsModeV1 = Vu1GsModeV1::StreamInline;
    else if (std::strcmp(mode, "owner-sync") == 0)
        m_vu1gsModeV1 = Vu1GsModeV1::OwnerSync;
    else if (std::strcmp(mode, "owner-async") == 0)
        m_vu1gsModeV1 = Vu1GsModeV1::OwnerAsync;
    else
        throw std::runtime_error("RRV_VU1GS_EXECUTION must be inline, stream-inline, owner-sync or owner-async");
    if (!m_gsBackend.active())
        throw std::runtime_error("RRV_VU1GS_EXECUTION requires the PCSX2 GS backend");
    const bool owner = m_vu1gsModeV1 != Vu1GsModeV1::StreamInline;
    if (owner && !m_gsBackend.ownerStreamAvailable())
        throw std::runtime_error("RRV_VU1GS_EXECUTION=" + std::string(mode) + " requires RRV_GS_EXECUTION=worker-sync");
    uint64_t stressSeed = 0u;
    if (const char *stress = std::getenv("RRV_VU1GS_STRESS"); stress && *stress)
    {
        if (!owner)
            throw std::runtime_error("RRV_VU1GS_STRESS needs an owner-* execution mode");
        stressSeed = std::strtoull(stress, nullptr, 0) | 1u;
    }
    m_vu1gsStressV1 = stressSeed;

    PS2Memory::OwnerStreamHooksV1 hooks;
    hooks.post = [this](std::function<void()> command, size_t bytes, size_t fields, bool sync)
    {
        if (m_vu1gsModeV1 == Vu1GsModeV1::StreamInline)
        {
            command();
            gate4PublishOwnerV1();
            return;
        }
        const uint64_t fpcr = gate4ReadFpcrV1();
        uint32_t delayUs = 0u;
        if (m_vu1gsStressV1)
        {
            uint64_t &x = m_vu1gsStressV1; // xorshift64, EE-side, deterministic per run
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            if ((x & 3u) == 0u)
                delayUs = static_cast<uint32_t>((x >> 8) % 200u);
        }
        const uint64_t sequence = m_gsBackend.ownerPost(
            [command = std::move(command), fpcr, delayUs]
            {
                if (delayUs)
                    std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
#if defined(__linux__) && defined(__x86_64__)
                gate5OwnerCommandV1(command, fpcr);
#else
                Gate4ScopedFpcrV1 scoped(fpcr);
                command();
#endif
            },
            bytes, fields);
        if (m_vu1gsModeV1 == Vu1GsModeV1::OwnerSync || sync)
        {
            if (m_vu1gsModeV1 == Vu1GsModeV1::OwnerAsync)
                ++m_vu1gsSyncWaitsV1;
            m_gsBackend.ownerWait(sequence);
            gate4PublishOwnerV1();
        }
    };
    hooks.fence = [this] { gate4FenceV1(); };
    // Gate 5: RRV_VU1GS_SPLIT=1 (owner-async only) runs VIF1+VU1 on rrv-vu1 and
    // only the bridge on rrv-gs-owner (Backend::startVuSplit). Owner readbacks
    // then arrive on rrv-gs-owner, after the packet that produced them.
    bool split = false;
    if (const char *v = std::getenv("RRV_VU1GS_SPLIT"); v && v[0] == '1' && v[1] == '\0')
    {
        if (m_vu1gsModeV1 != Vu1GsModeV1::OwnerAsync)
            throw std::runtime_error("RRV_VU1GS_SPLIT=1 requires RRV_VU1GS_EXECUTION=owner-async");
        std::string error;
        if (!m_gsBackend.startVuSplit([this](uint64_t csr, uint64_t siglblid) { gate4StoreReadbackV1(csr, siglblid); },
                                      &error))
            throw std::runtime_error(error);
        split = true;
        const char *defer = std::getenv("RRV_VU1GS_DEFER_FIELD");
        if (defer && *defer && std::strcmp(defer, "0") != 0 && std::strcmp(defer, "1") != 0)
            throw std::runtime_error("RRV_VU1GS_DEFER_FIELD must be 0 or 1");
        m_vu1gsDeferFieldV1 = !(defer && defer[0] == '0');
#if defined(__linux__) && defined(__x86_64__)
        if (rrv_cpu_log_set_split)
            rrv_cpu_log_set_split(1);
#endif
    }
    m_memory.setOwnerStreamV1(std::move(hooks));
    std::fprintf(stderr, "[vu1gs] execution=%s stress=%s split=%s defer_field=%s\n", mode,
                 stressSeed ? std::getenv("RRV_VU1GS_STRESS") : "off", split ? "on" : "off",
                 m_vu1gsDeferFieldV1 ? "on" : "off");
}

void PS2Runtime::gate4ShutdownOwnerStreamV1()
{
    if (m_vu1gsModeV1 == Vu1GsModeV1::Inline)
        return;
    try { gate4FenceV1(); } catch (...) {}
    std::fprintf(stderr, "[vu1gs] commands=%llu sync_commands=%llu async_sync_waits=%llu fences=%llu deferred_fields=%llu\n",
                 static_cast<unsigned long long>(m_memory.ownerStreamCommandsV1()),
                 static_cast<unsigned long long>(m_memory.ownerStreamSyncCommandsV1()),
                 static_cast<unsigned long long>(m_vu1gsSyncWaitsV1),
                 static_cast<unsigned long long>(m_vu1gsFencesV1),
                 static_cast<unsigned long long>(m_vu1gsDeferredFieldsV1));
    m_vu1gsDeferFieldV1 = false;
    m_memory.setOwnerStreamV1({});
    m_vu1gsModeV1 = Vu1GsModeV1::Inline;
}

bool PS2Runtime::gate4FieldDeferrableV1()
{
    static const bool presentDump = []
    {
        const char *directory = std::getenv("RRV_PRESENT_DUMP");
        return directory && directory[0];
    }();
    if (presentDump || rrv::gsrecord::rrv_gs_record_enabled())
        return false;
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (m_gsBackend.renderMode() == rrv::gs::GsRenderMode::FullFrame)
        return false;
#endif
    return m_gsBackend.fieldDeferrable();
}

// advanceActiveGsBackendField(): the EE takes the privileged-register snapshot
// and field parity at the inline point; the owner forwards them.
void PS2Runtime::gate4AdvanceFieldOwnerV1(std::array<uint64_t, 19u> regs19, uint64_t fieldIndex,
                                          uint32_t fieldParity)
{
    // The inline snapshot's bridge-owned CSR/SIGLBLID bits are the last owner
    // readback unless the guest wrote them since (which invalidates the copy).
    const auto ownerReadback = [this](uint64_t *regs)
    {
        if (m_ownerCsrValidV1)
            regs[15] = (m_ownerCsrV1 & ~uint64_t(0x2008)) | (regs[15] & 0x2008);
        if (m_ownerSiglblidValidV1)
            regs[18] = m_ownerSiglblidV1;
    };
    std::string error;
    // Split, deferred (the default with the split; RRV_VU1GS_DEFER_FIELD=0
    // restores the wait): rrv-vu1 queues the transition and starts the next
    // field. rrv-gs-owner applies ownerReadback after the field's last packet,
    // where the wait used to read it, then runs vsync and the post-vsync
    // readback. The guest writes the valid flags only behind a fence, with
    // both threads idle. Diagnostics that need the field boundary on this
    // thread (present dump, GS record, FullFrame shadow) keep the wait.
    if (m_vu1gsDeferFieldV1 && gate4FieldDeferrableV1())
    {
        if (!m_gsBackend.vsyncDeferred(regs19.data(), fieldIndex, fieldParity, ownerReadback, &error))
            throw std::runtime_error("PCSX2 GS bridge field transition failed: " + error);
        ++m_vu1gsDeferredFieldsV1;
        return;
    }
    // Split: every earlier packet (and its readback) must have run first, as
    // on one owner thread. vsync() below waits for rrv-gs-owner anyway.
    m_gsBackend.drainGsFromVu();
    ownerReadback(regs19.data());
    if (!m_gsBackend.vsync(regs19.data(), fieldIndex, fieldParity, &error))
        throw std::runtime_error("PCSX2 GS bridge field transition failed: " + error);
    gate4FinishFieldV1(regs19, fieldIndex);
}

Gate8AudioHooksV1 g_gate8AudioHooksV1{};
