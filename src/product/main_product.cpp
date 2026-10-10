#include "ps2_runtime.h"
#include "register_functions.h"
#include "games_database.h"
#include "patches.h"
#include "rrv_gs_record_hooks.h"
#include "rrv_gs_backend.h"
#include "rrv_m2_neutrality.h"
#include "rrv_m2_causal_trace.h"
#include "rrv_sdl_presentation.h"
#include "rrv_guest_terminal_outcome.h"
#include "rrv_elf_identity.h"
#include "fukami_app.h"
#include "fukami_settings.h"
#if defined(__linux__)
#include "rrv_thread_cpu_log.h"
#endif
#if defined(RRV_GS_PRODUCER_CONTROL)
#include "Syscalls/Interrupt.h"
#include "ps2_stubs.h"
#endif

#if defined(RRV_GS_PRODUCER_CONTROL)
#include <array>
#endif
#include <cstring>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
// The PCSX2 hardware-GS device the product presents with: Metal on macOS,
// Vulkan on Linux (Gate 5, Steam Deck).
#if defined(__APPLE__)
constexpr rrv::gsbackend::RendererKind kProductRendererKind = rrv::gsbackend::RendererKind::Metal;
#define RRV_PRODUCT_BACKEND_NAME "Metal"
#else
constexpr rrv::gsbackend::RendererKind kProductRendererKind = rrv::gsbackend::RendererKind::Vulkan;
#define RRV_PRODUCT_BACKEND_NAME "Vulkan"
#endif

void printStartup()
{
    std::cerr
        << "[rrv-product] RRV commit=" << RRV_SOURCE_COMMIT
        << " source-clean=" << RRV_SOURCE_CLEAN << '\n'
        << "[rrv-product] PS2Recomp identity=" << RRV_PRODUCER_NAME
        << " commit=" << RRV_PRODUCER_COMMIT
        << " tree=" << RRV_PRODUCER_TREE
        << " upstream-base=" << RRV_PRODUCER_BASE << '\n'
        << "[rrv-product] PCSX2 commit=" << RRV_PCSX2_COMMIT << '\n'
        << "[rrv-product] bridge ABI=5\n"
        << "[rrv-product] requested renderer=PCSX2 hardware GS backend=" RRV_PRODUCT_BACKEND_NAME
           " presentation=direct GPU\n"
        << "[rrv-product] continuous CPU readback=disabled\n"
        << "[rrv-product] diagnostic capture=available\n"
        << "[rrv-product] F7/P3 linked=no\n";
    // The typed view of the settings this process was launched with (the environment is only the transport).
    const auto launch = fukami::settings::LaunchConfig::fromEnvironment([](const char *name) { return std::getenv(name); });
    std::cerr << "[rrv-product] launch ratio=" << launch.ratio << " hud=" << launch.hud << " render=" << launch.renderMode
              << " scale=" << launch.scale << " aa1=" << launch.aa1 << " fxaa=" << launch.fxaa << " cas=" << launch.cas
              << " aniso=" << launch.aniso << " texture-filter=" << launch.textureFilter << " mipmap=" << launch.mipmap
              << " analog=" << launch.analog
              << " rumble=" << launch.rumble << " car-lod=" << launch.carLod << " draw-distance=" << launch.drawDistance
              << " fast-unpack=" << launch.fastUnpack << " native-code=" << launch.nativeCode
              << " paced=" << !launch.unpaced << " inline=" << launch.inlineExecution << " headless=" << launch.headless
              << " fullscreen=" << launch.fullscreen << " present-pacing=" << launch.presentPacing << '\n';
}

bool startupTestException()
{
    const char *value = std::getenv("RRV_PRODUCT_STARTUP_TEST");
    return value && std::strcmp(value, "1") == 0;
}

uint64_t testPresentLimit()
{
    const char *value = std::getenv("RRV_TEST_PRESENT_LIMIT");
    if (!value || value[0] == '\0')
        return 0u;
    if (value[0] < '0' || value[0] > '9')
        throw std::invalid_argument("RRV_TEST_PRESENT_LIMIT must be a positive integer");
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno == ERANGE || !end || end == value || *end != '\0' || parsed == 0u)
        throw std::invalid_argument("RRV_TEST_PRESENT_LIMIT must be a positive integer");
    return static_cast<uint64_t>(parsed);
}

// Product input is process-scoped in the reconstructed runtime so it must be
// detached before the SDL owner goes away, including on an initialization or
// run exception.  Keep this immediately after the host object in main() so
// destruction order is unambiguous.
class ExternalPadBackendReset final
{
public:
    ExternalPadBackendReset() = default;
    ExternalPadBackendReset(const ExternalPadBackendReset &) = delete;
    ExternalPadBackendReset &operator=(const ExternalPadBackendReset &) = delete;
    ~ExternalPadBackendReset() { PSPadBackend::setExternalBackendForProcess({}); }
};

bool snapshotActiveGsVram(void *context, uint8_t *out, uint32_t byteCount)
{
    auto *runtime = static_cast<PS2Runtime *>(context);
    if (!runtime || !out || byteCount != PS2_GS_VRAM_SIZE)
        return false;
    std::vector<uint8_t> snapshot;
    if (!runtime->snapshotActiveGsBackendLocalMemory(snapshot) || snapshot.size() != byteCount)
        return false;
    std::memcpy(out, snapshot.data(), byteCount);
    return true;
}

bool restoreCapturedGsVramAfterInitialWrite(void *context, const uint8_t *snapshot,
                                             uint32_t byteCount)
{
    auto *runtime = static_cast<PS2Runtime *>(context);
    if (!runtime || !snapshot || byteCount != PS2_GS_VRAM_SIZE)
        return false;
    if (!runtime->restoreActiveGsBackendLocalMemory(snapshot, byteCount))
        return false;
    std::cerr << "[rrv-product] GAME-001 diagnostic: restored written 4 MiB GS VRAM "
                 "snapshot and rebuilt PCSX2 texture caches at signal field boundary\n";
    return true;
}

#if defined(RRV_GS_PRODUCER_CONTROL)
constexpr uint32_t kGate1Csr = 0x12001000u;
constexpr uint32_t kGate1Imr = 0x12001010u;
constexpr uint32_t kGate1Siglblid = 0x12001080u;
constexpr uint32_t kGate1VifDma = 0x10009000u;
constexpr uint32_t kGate1IntcStat = 0x1000f000u;

void gate1Check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void appendGate1U32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (uint32_t shift = 0u; shift != 32u; shift += 8u)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendGate1U64(std::vector<uint8_t> &bytes, uint64_t value)
{
    for (uint32_t shift = 0u; shift != 64u; shift += 8u)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}

struct Gate1Ad
{
    uint8_t reg;
    uint64_t value;
};

std::vector<uint8_t> gate1Packet(std::initializer_list<Gate1Ad> registers)
{
    std::vector<uint8_t> bytes;
    appendGate1U64(bytes, static_cast<uint64_t>(registers.size()) | (1ull << 15u) | (1ull << 60u));
    appendGate1U64(bytes, 0xeull); // PACKED, one A+D descriptor.
    for (const Gate1Ad &entry : registers)
    {
        appendGate1U64(bytes, entry.value);
        appendGate1U64(bytes, entry.reg);
    }
    return bytes;
}

std::vector<uint8_t> gate1Direct(const std::vector<uint8_t> &gif)
{
    gate1Check((gif.size() % 16u) == 0u, "Gate-1 DIRECT fixture is not qword aligned");
    std::vector<uint8_t> bytes;
    appendGate1U32(bytes, 0x50000000u | static_cast<uint32_t>(gif.size() / 16u));
    bytes.insert(bytes.end(), gif.begin(), gif.end());
    bytes.resize((bytes.size() + 15u) & ~size_t(15u), 0u);
    return bytes;
}

uint64_t gate1Id(uint32_t value, uint32_t mask = ~uint32_t(0))
{
    return static_cast<uint64_t>(value) | (static_cast<uint64_t>(mask) << 32u);
}

struct Gate1IrqObservation
{
    uint64_t csr = 0u;
    uint64_t siglblid = 0u;
};

struct Gate1Receipt
{
    std::string scenario;
    std::string mode;
    uint64_t signals = 0u;
    uint64_t finishes = 0u;
    uint64_t imrWrites = 0u;
    uint64_t finishDeliveries = 0u;
    uint64_t signalAcks = 0u;
    uint64_t finishAcks = 0u;
    uint64_t irqRequests = 0u;
    uint64_t irqDispatches = 0u;
    uint64_t vifInputBytes = 0u;
    uint64_t firstSignalCsr = 0u;
    uint64_t secondSignalCsr = 0u;
    uint64_t finishCsr = 0u;
    uint64_t finalSiglblid = 0u;
    uint64_t finalCsr = 0u;
    uint64_t fieldBefore = 0u;
    uint64_t fieldAfter = 0u;
    uint64_t guestFieldFlag = 0u;
    uint64_t guestFieldTick = 0u;
    uint64_t fieldCsr = 0u;
    // Logical FIELD result: host tick values above depend on scheduling and
    // are printed only as diagnostics, never compared across modes.
    uint64_t fieldAdvanced = 0u;
    uint64_t fieldTickMatches = 0u;
    uint64_t fieldParityMatches = 0u;
    // Cleanup row: the positive control proves the dispatch path would have
    // reached a still-registered handler.
    uint64_t cleanupPositiveControl = 0u;
    uint64_t cleanupNormalRemoved = 0u;
    uint64_t cleanupEarlyExitRemoved = 0u;
};

thread_local std::vector<Gate1IrqObservation> *g_gate1IrqObservations = nullptr;

void gate1SetArgument(R5900Context &context, int reg, uint32_t value)
{
    context.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

void gate1GuestIrq(uint8_t *, R5900Context *context, PS2Runtime *runtime)
{
    gate1Check(g_gate1IrqObservations != nullptr && runtime != nullptr,
               "Gate-1 guest IRQ handler lost its product-runtime observation state");
    g_gate1IrqObservations->push_back({runtime->memory().read64(kGate1Csr),
                                       runtime->memory().read64(kGate1Siglblid)});
    runtime->memory().writeIORegister(kGate1IntcStat, 1u);
    context->pc = 0u;
}

// FIELD row: what a guest VBlank-start INTC handler sees. The VSync worker
// wakes WaitForNextVSyncTick before it writes the registered flag/tick words
// and before it advances CSR.FIELD (Interrupt.cpp signalVSyncFlag, then
// advanceActiveGsBackendField), so a read right after the wake can predate all
// three. The VBlank-start handler runs in the worker's guest scope right after
// that field's service, so it is the observation point.
struct Gate1VblankObservation
{
    uint32_t flag = 0u;
    uint64_t tick = 0u;
    uint64_t csr = 0u;
};
std::mutex g_gate1VblankMutex;
std::vector<Gate1VblankObservation> *g_gate1VblankObservations = nullptr;
uint32_t g_gate1VblankFlagAddress = 0u;
uint32_t g_gate1VblankTickAddress = 0u;

void gate1GuestVblank(uint8_t *rdram, R5900Context *context, PS2Runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_gate1VblankMutex);
    if (g_gate1VblankObservations != nullptr && runtime != nullptr)
    {
        Gate1VblankObservation observation{};
        std::memcpy(&observation.flag, rdram + g_gate1VblankFlagAddress, sizeof(observation.flag));
        std::memcpy(&observation.tick, rdram + g_gate1VblankTickAddress, sizeof(observation.tick));
        observation.csr = runtime->memory().read64(kGate1Csr);
        g_gate1VblankObservations->push_back(observation);
    }
    context->pc = 0u;
}

void gate1KickVifDma(PS2Runtime &runtime, const std::vector<uint8_t> &bytes)
{
    gate1Check((bytes.size() % 16u) == 0u, "Gate-1 VIF DMA fixture is not qword aligned");
    constexpr uint32_t source = 0x10000u;
    auto &memory = runtime.memory();
    std::memcpy(memory.getRDRAM() + source, bytes.data(), bytes.size());
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGate1VifDma + 0x10u, source);
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGate1VifDma + 0x20u,
                    static_cast<uint32_t>(bytes.size() / 16u));
    runtime.Store32(memory.getRDRAM(), &runtime.cpu(), kGate1VifDma, 0x101u);
}

void gate1ExpectBlockedProductResultCalls(PS2Runtime &runtime)
{
    // ABI-sized guest descriptor consumed by the normal sceGsExecStoreImage
    // HLE path: x, y, width, height, vram address, vram width, PSM.
    struct Gate1GsImage final
    {
        uint16_t x, y, width, height, vramAddress;
        uint8_t vramWidth, psm;
    };
    static_assert(sizeof(Gate1GsImage) == 12u);
    constexpr uint32_t imageAddress = 0x14000u;
    constexpr uint32_t storeDestination = 0x15000u;
    const Gate1GsImage image{0u, 0u, 2u, 2u, 0u, 1u, 0u};
    std::memcpy(runtime.memory().getRDRAM() + imageAddress, &image, sizeof(image));
    std::memset(runtime.memory().getRDRAM() + storeDestination, 0x7cu, 16u);
    std::array<uint8_t, 16u> localRead{};
    localRead.fill(0x5au);
    std::vector<uint8_t> snapshot(PS2_GS_VRAM_SIZE, 0x6bu);
    bool readRejected = false;
    bool storeImageRejected = false;
    bool snapshotRejected = false;
    bool restoreRejected = false;
    try
    {
        (void)runtime.readActiveGsBackendLocalMemory(localRead.data(), static_cast<uint32_t>(localRead.size()),
                                                     0u, 0u, 0u);
    }
    catch (const std::runtime_error &)
    {
        readRejected = true;
    }
    try
    {
        R5900Context context{};
        gate1SetArgument(context, 4, imageAddress);
        gate1SetArgument(context, 5, storeDestination);
        ps2_stubs::sceGsExecStoreImage(runtime.memory().getRDRAM(), &context, &runtime);
    }
    catch (const std::runtime_error &)
    {
        storeImageRejected = true;
    }
    try
    {
        (void)snapshotActiveGsVram(&runtime, snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    }
    catch (const std::runtime_error &)
    {
        snapshotRejected = true;
    }
    try
    {
        (void)runtime.restoreActiveGsBackendLocalMemory(nullptr, 0u);
    }
    catch (const std::runtime_error &)
    {
        restoreRejected = true;
    }
    gate1Check(readRejected && storeImageRejected && snapshotRejected && restoreRejected,
               "actual product result callers admitted held VIF producer work");
    gate1Check(localRead.front() == 0x5au && localRead.back() == 0x5au &&
                   snapshot.front() == 0x6bu && snapshot.back() == 0x6bu &&
                   runtime.memory().getRDRAM()[storeDestination] == 0x7cu &&
                   runtime.memory().getRDRAM()[storeDestination + 15u] == 0x7cu,
               "rejected actual product result caller modified its caller-owned output");
}

// Registers the guest VSync flag and a VBlank-start INTC handler, waits for
// the next field and returns what the handler saw once the worker had
// serviced it (see Gate1VblankObservation). The caller holds the guest scope.
struct Gate1FieldService
{
    uint64_t fieldBefore = 0u;
    uint64_t fieldAfter = 0u;
    Gate1VblankObservation observed{};
};

Gate1FieldService gate1ServiceNextField(PS2Runtime &runtime)
{
    auto &memory = runtime.memory();
    R5900Context setVsyncContext{};
    constexpr uint32_t kGate1VsyncFlag = 0x18000u;
    constexpr uint32_t kGate1VsyncTick = 0x18008u;
    gate1SetArgument(setVsyncContext, 4, kGate1VsyncFlag);
    gate1SetArgument(setVsyncContext, 5, kGate1VsyncTick);
    constexpr uint32_t kGate1VblankStartCause = 2u;
    constexpr uint32_t kGate1VblankHandler = 0x200100u;
    std::vector<Gate1VblankObservation> vblankObservations;
    {
        std::lock_guard<std::mutex> lock(g_gate1VblankMutex);
        g_gate1VblankFlagAddress = kGate1VsyncFlag;
        g_gate1VblankTickAddress = kGate1VsyncTick;
        g_gate1VblankObservations = &vblankObservations;
    }
    struct Gate1VblankRegistration final
    {
        PS2Runtime *runtime = nullptr;
        int32_t handlerId = -1;
        ~Gate1VblankRegistration()
        {
            if (runtime && handlerId >= 0)
            {
                R5900Context context{};
                gate1SetArgument(context, 4, kGate1VblankStartCause);
                gate1SetArgument(context, 5, static_cast<uint32_t>(handlerId));
                ps2_syscalls::RemoveIntcHandler(runtime->memory().getRDRAM(), &context, runtime);
            }
            std::lock_guard<std::mutex> lock(g_gate1VblankMutex);
            g_gate1VblankObservations = nullptr;
        }
    } vblankRegistration{&runtime};
    runtime.registerFunction(kGate1VblankHandler, gate1GuestVblank);
    R5900Context addVblankContext{};
    gate1SetArgument(addVblankContext, 4, kGate1VblankStartCause);
    gate1SetArgument(addVblankContext, 5, kGate1VblankHandler);
    gate1SetArgument(addVblankContext, 6, 0u);
    gate1SetArgument(addVblankContext, 7, 0u);
    ps2_syscalls::AddIntcHandler(memory.getRDRAM(), &addVblankContext, &runtime);
    vblankRegistration.handlerId = static_cast<int32_t>(getRegU32(&addVblankContext, 2));
    gate1Check(vblankRegistration.handlerId >= 0, "field row VBlank-start AddIntcHandler registration failed");
    R5900Context enableVblankContext{};
    gate1SetArgument(enableVblankContext, 4, kGate1VblankStartCause);
    ps2_syscalls::EnableIntc(memory.getRDRAM(), &enableVblankContext, &runtime);

    ps2_syscalls::SetVSyncFlag(memory.getRDRAM(), &setVsyncContext, &runtime);
    const uint64_t fieldBefore = ps2_syscalls::GetCurrentVSyncTick();
    const uint64_t fieldAfter = ps2_syscalls::WaitForNextVSyncTick(memory.getRDRAM(), &runtime);
    // Wait (guest execution released, bounded) until the worker has
    // serviced fieldAfter; the worker services ticks in order, one at a
    // time, so that field's observation carries tick == fieldAfter.
    std::optional<Gate1VblankObservation> observed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!observed && std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lock(g_gate1VblankMutex);
            for (const auto &observation : vblankObservations)
                if (observation.tick == fieldAfter)
                {
                    observed = observation;
                    break;
                }
        }
        if (!observed)
        {
            PS2Runtime::GuestExecutionReleaseScope release(&runtime);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    gate1Check(observed.has_value(), "no VBlank-start service observed for the awaited field");
    return {fieldBefore, fieldAfter, *observed};
}

Gate1Receipt runGate1ProducerControlCoverage(std::string_view scenario)
{
    gate1Check(scenario == "all" || scenario == "signal" || scenario == "vif-finish" || scenario == "field" || scenario == "cleanup",
               "unknown Gate-1 producer-control scenario");
    const char *const execution = std::getenv("RRV_GS_EXECUTION");
    gate1Check(execution != nullptr &&
                   (std::strcmp(execution, "inline") == 0 || std::strcmp(execution, "worker-sync") == 0),
               "Gate-1 product control requires RRV_GS_EXECUTION=inline or worker-sync");
    const char *const control = std::getenv("RRV_GS_CONTROL");
    gate1Check(control != nullptr && std::strcmp(control, "producer") == 0,
               "Gate-1 product control requires RRV_GS_CONTROL=producer");

    rrv::host::SdlPresentation host;
    std::string hostError;
    gate1Check(host.create("RRV-Recomp Gate-1 producer control", &hostError),
               ("Gate-1 SDL " RRV_PRODUCT_BACKEND_NAME " host initialization failed: " + hostError).c_str());
    rrv::gsbackend::NativeSurface surface{};
    gate1Check(host.currentSurface(&surface), "Gate-1 SDL " RRV_PRODUCT_BACKEND_NAME " host did not provide a drawable surface");

    PSPadBackend::setExternalBackendForProcess(host.padBackend());
    ExternalPadBackendReset resetExternalPadBackend;
    PS2Runtime runtime;
    gate1Check(runtime.memory().initialize(), "Gate-1 actual rrv-product memory initialization failed");
    PS2Runtime::HostPresentationConfig presentation{};
    presentation.backend.snapshotWidth = 640u;
    presentation.backend.snapshotHeight = 448u;
    presentation.backend.rendererKind = kProductRendererKind;
    presentation.backend.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
    presentation.backend.surface = surface;
    presentation.context = &host;
    presentation.pumpEvents = &rrv::host::SdlPresentation::pumpCallback;
    presentation.shouldClose = &rrv::host::SdlPresentation::closeCallback;
    presentation.currentSurface = &rrv::host::SdlPresentation::surfaceCallback;
    runtime.setHostPresentationConfig(presentation);
    gate1Check(runtime.syncCoreSubsystems() && runtime.activeGsBackend() && runtime.directGpuPresentationActive(),
               "Gate-1 actual rrv-product GS/" RRV_PRODUCT_BACKEND_NAME " initialization failed");

    std::vector<Gate1IrqObservation> irqObservations;
    g_gate1IrqObservations = &irqObservations;
    struct Gate1IrqRegistration final
    {
        PS2Runtime *runtime = nullptr;
        int32_t handlerId = -1;
        void remove()
        {
            if (!runtime || handlerId < 0)
                return;
            R5900Context context{};
            gate1SetArgument(context, 4, 0u);
            gate1SetArgument(context, 5, static_cast<uint32_t>(handlerId));
            ps2_syscalls::RemoveIntcHandler(runtime->memory().getRDRAM(), &context, runtime);
            handlerId = -1;
        }
        ~Gate1IrqRegistration()
        {
            remove();
        }
    } irqRegistration{&runtime};
    struct Gate1IrqSession final
    {
        ~Gate1IrqSession()
        {
            ps2_syscalls::stopInterruptWorker();
            g_gate1IrqObservations = nullptr;
        }
    } irqSession{};
    runtime.registerFunction(0x200000u, gate1GuestIrq);
    R5900Context addIntcContext{};
    gate1SetArgument(addIntcContext, 4, 0u);
    gate1SetArgument(addIntcContext, 5, 0x200000u);
    gate1SetArgument(addIntcContext, 6, 0u);
    gate1SetArgument(addIntcContext, 7, 0u);
    ps2_syscalls::AddIntcHandler(runtime.memory().getRDRAM(), &addIntcContext, &runtime);
    irqRegistration.handlerId = static_cast<int32_t>(getRegU32(&addIntcContext, 2));
    gate1Check(irqRegistration.handlerId >= 0, "actual AddIntcHandler registration failed");

    PS2Runtime::GuestExecutionScope guest(&runtime);
    auto &memory = runtime.memory();
    gate1Check(memory.gsControl().enabled(), "Gate-1 producer control was not enabled in actual rrv-product");
    if (scenario == "cleanup")
    {
        // Positive control first: the same dispatch call must reach the
        // registered handler, or the removal checks below would be vacuous.
        R5900Context enableContext{};
        gate1SetArgument(enableContext, 4, 0u);
        ps2_syscalls::EnableIntc(memory.getRDRAM(), &enableContext, &runtime);
        const size_t controlBefore = irqObservations.size();
        gate1Check(ps2_syscalls::dispatchGsControlInterrupt(memory.getRDRAM(), &runtime) &&
                       irqObservations.size() == controlBefore + 1u,
                   "cleanup positive control: GS INTC dispatch did not reach the registered guest handler");
        const size_t observationsBefore = irqObservations.size();
        irqRegistration.remove();
        gate1Check(ps2_syscalls::dispatchGsControlInterrupt(memory.getRDRAM(), &runtime),
                   "cleanup: GS INTC cause was disabled before the post-remove dispatch");
        gate1Check(irqObservations.size() == observationsBefore,
                   "normal RemoveIntcHandler cleanup left a guest IRQ handler registered");
        struct ControlledCleanupExit final {};
        try
        {
            Gate1IrqRegistration earlyRegistration{&runtime};
            R5900Context earlyContext{};
            gate1SetArgument(earlyContext, 4, 0u);
            gate1SetArgument(earlyContext, 5, 0x200000u);
            gate1SetArgument(earlyContext, 6, 0u);
            gate1SetArgument(earlyContext, 7, 0u);
            ps2_syscalls::AddIntcHandler(memory.getRDRAM(), &earlyContext, &runtime);
            earlyRegistration.handlerId = static_cast<int32_t>(getRegU32(&earlyContext, 2));
            gate1Check(earlyRegistration.handlerId >= 0, "early-exit AddIntcHandler registration failed");
            throw ControlledCleanupExit{};
        }
        catch (const ControlledCleanupExit &)
        {
        }
        gate1Check(ps2_syscalls::dispatchGsControlInterrupt(memory.getRDRAM(), &runtime),
                   "cleanup: GS INTC cause was disabled before the early-exit dispatch");
        gate1Check(irqObservations.size() == observationsBefore,
                   "early-exit RemoveIntcHandler cleanup left a guest IRQ handler registered");
        Gate1Receipt receipt{};
        receipt.scenario = std::string(scenario);
        receipt.mode = execution;
        receipt.cleanupPositiveControl = 1u;
        receipt.cleanupNormalRemoved = 1u;
        receipt.cleanupEarlyExitRemoved = 1u;
        return receipt;
    }
    if (scenario == "field")
    {
        const auto signalAcks = memory.gsControl().counters().signalAcks;
        const auto finishAcks = memory.gsControl().counters().finishAcks;
        const auto service = gate1ServiceNextField(runtime);
        const uint64_t fieldBefore = service.fieldBefore;
        const uint64_t fieldAfter = service.fieldAfter;
        const uint32_t guestVsyncFlag = service.observed.flag;
        const uint64_t guestVsyncTick = service.observed.tick;
        const uint64_t csr = service.observed.csr;
        gate1Check(fieldAfter > fieldBefore && guestVsyncFlag == 1u && guestVsyncTick == fieldAfter &&
                       ((csr >> 13u) & 1u) == (fieldAfter & 1u) && irqObservations.empty() &&
                       memory.gsControl().counters().signalAcks == signalAcks &&
                       memory.gsControl().counters().finishAcks == finishAcks,
                   "guest VSync FIELD service did not preserve producer-control authority");
        Gate1Receipt receipt{};
        receipt.scenario = std::string(scenario);
        receipt.mode = execution;
        receipt.fieldBefore = fieldBefore;
        receipt.fieldAfter = fieldAfter;
        receipt.guestFieldFlag = guestVsyncFlag;
        receipt.guestFieldTick = guestVsyncTick;
        receipt.fieldCsr = csr;
        receipt.fieldAdvanced = fieldAfter > fieldBefore ? 1u : 0u;
        receipt.fieldTickMatches = guestVsyncTick == fieldAfter ? 1u : 0u;
        receipt.fieldParityMatches = ((csr >> 13u) & 1u) == (fieldAfter & 1u) ? 1u : 0u;
        return receipt;
    }
    memory.write32(kGate1Imr, 0x7f00u); // Mask SIGNAL and FINISH before their requests.
    const auto firstSignal = gate1Packet({{0x60u, gate1Id(1u)}});
    const auto secondSignalAndLabel = gate1Packet({{0x60u, gate1Id(2u)}, {0x62u, gate1Id(3u)}});
    memory.submitGifPacket(GifPathId::Path3, firstSignal.data(), static_cast<uint32_t>(firstSignal.size()));
    memory.submitGifPacket(GifPathId::Path3, secondSignalAndLabel.data(),
                           static_cast<uint32_t>(secondSignalAndLabel.size()));
    gate1Check(memory.gsControl().stalled() && (memory.read64(kGate1Csr) & 1u) != 0u &&
                   irqObservations.empty(),
               "masked first SIGNAL did not stall without premature guest IRQ delivery");

    memory.write32(kGate1Imr, 0x7e00u); // Unmask SIGNAL only.
    gate1Check(irqObservations.size() == 1u && (irqObservations[0].csr & 1u) != 0u &&
                   irqObservations[0].siglblid == 1u,
               "SIGNAL unmask did not deliver the first completed request to the actual guest handler");
    memory.write32(kGate1Csr, 1u); // Acknowledge first SIGNAL; normal resume reaches the second.
    gate1Check(irqObservations.size() == 2u && (irqObservations[1].csr & 1u) != 0u &&
                   static_cast<uint32_t>(irqObservations[1].siglblid) == 2u &&
                   static_cast<uint32_t>(irqObservations[1].siglblid >> 32u) == 3u,
               "first SIGNAL acknowledgement did not resume and dispatch the second request in order");
    memory.write32(kGate1Csr, 1u); // Acknowledge second SIGNAL; normal resume reaches LABEL.
    const uint64_t resumedSiglblid = memory.read64(kGate1Siglblid);
    gate1Check(!memory.gsControl().stalled() && static_cast<uint32_t>(resumedSiglblid) == 2u &&
                   static_cast<uint32_t>(resumedSiglblid >> 32u) == 3u,
               "second SIGNAL acknowledgement did not resume the pending LABEL exactly once");
    if (scenario == "signal")
    {
        const auto &counts = memory.gsControl().counters();
        return {std::string(scenario), execution, counts.signals, counts.finishes, counts.imrWrites,
                counts.finishDeliveries, counts.signalAcks, counts.finishAcks, counts.irqRequests,
                memory.gsIrqDispatches(), 0u, irqObservations[0].csr, irqObservations[1].csr,
                0u, memory.read64(kGate1Siglblid), memory.read64(kGate1Csr)};
    }

    const auto vif = gate1Direct(gate1Packet({{0x61u, 0u}, {0x62u, gate1Id(4u)}}));
    gate1KickVifDma(runtime, vif);
    gate1Check(!memory.m_heldVif1Transfers.empty() &&
                   (static_cast<uint32_t>(memory.gsResultBoundary().work) &
                    static_cast<uint32_t>(rrv::gs::ProducerWork::HeldVif1)) != 0u,
               "actual VIF DMA did not retain producer-owned work before its guest release point");
    gate1ExpectBlockedProductResultCalls(runtime);
    R5900Context syncVContext{};
    ps2_stubs::sceGsSyncV(memory.getRDRAM(), &syncVContext, &runtime);
    constexpr uint64_t kGate1Signal2Label4 = (uint64_t{4u} << 32u) | 2u;
    gate1Check((memory.read64(kGate1Csr) & 2u) != 0u && memory.read64(kGate1Siglblid) == kGate1Signal2Label4 &&
                   memory.gsDeferredVif1Bytes() == 0u,
               "VIF DIRECT FINISH/LABEL did not complete in guest order");

    memory.write32(kGate1Imr, 0x7c00u); // Unmask the now-pending FINISH.
    gate1Check(irqObservations.size() == 3u && (irqObservations[2].csr & 2u) != 0u &&
                   irqObservations[2].siglblid == kGate1Signal2Label4,
               "FINISH unmask did not deliver after the VIF LABEL to the actual guest handler");
    memory.write32(kGate1Csr, 2u); // FINISH acknowledgement must not be supplied by FIELD service.
    gate1Check((memory.read64(kGate1Csr) & 3u) == 0u, "FINISH guest acknowledgement did not clear only FINISH");
    if (scenario == "vif-finish")
    {
        const auto &counts = memory.gsControl().counters();
        return {std::string(scenario), execution, counts.signals, counts.finishes, counts.imrWrites,
                counts.finishDeliveries, counts.signalAcks, counts.finishAcks, counts.irqRequests,
                memory.gsIrqDispatches(), static_cast<uint64_t>(vif.size()), irqObservations[0].csr,
                irqObservations[1].csr, irqObservations[2].csr, memory.read64(kGate1Siglblid),
                memory.read64(kGate1Csr)};
    }

    const auto signalAcks = memory.gsControl().counters().signalAcks;
    const auto finishAcks = memory.gsControl().counters().finishAcks;
    const auto service = gate1ServiceNextField(runtime);
    gate1Check(service.fieldAfter > service.fieldBefore && service.observed.flag == 1u &&
                   service.observed.tick == service.fieldAfter &&
                   ((service.observed.csr >> 13u) & 1u) == (service.fieldAfter & 1u) &&
                   (service.observed.csr & 3u) == 0u && irqObservations.size() == 3u &&
                   memory.gsControl().counters().signalAcks == signalAcks &&
                   memory.gsControl().counters().finishAcks == finishAcks,
               "guest VSync FIELD service did not preserve acknowledged producer-control order");

    const auto &counts = memory.gsControl().counters();
    gate1Check(counts.signals == 2u && counts.finishes == 1u && counts.imrWrites == 3u &&
                   counts.finishDeliveries == 1u && counts.signalAcks == 2u && counts.finishAcks == 1u &&
                   counts.irqRequests >= 3u && memory.gsIrqDispatches() == 3u,
               "Gate-1 producer-control receipt contains an empty or unordered VIF/FINISH/IMR/IRQ row");
    return {std::string(scenario), execution, counts.signals, counts.finishes, counts.imrWrites, counts.finishDeliveries,
            counts.signalAcks, counts.finishAcks, counts.irqRequests, memory.gsIrqDispatches(),
            static_cast<uint64_t>(vif.size()), irqObservations[0].csr, irqObservations[1].csr,
            irqObservations[2].csr, memory.read64(kGate1Siglblid), memory.read64(kGate1Csr)};
}

int runGate1ProducerControlCoverageAndPrintReceipt(std::string_view scenario)
{
    const Gate1Receipt receipt = runGate1ProducerControlCoverage(scenario);
    std::cout << "GATE1_PRODUCER_CONTROL_RECEIPT={"
              << "\"mode\":\"" << receipt.mode << "\","
              << "\"scenario\":\"" << receipt.scenario << "\","
              << "\"signals\":" << receipt.signals << ','
              << "\"finishes\":" << receipt.finishes << ','
              << "\"imr_writes\":" << receipt.imrWrites << ','
              << "\"finish_deliveries\":" << receipt.finishDeliveries << ','
              << "\"signal_acks\":" << receipt.signalAcks << ','
              << "\"finish_acks\":" << receipt.finishAcks << ','
              << "\"irq_requests\":" << receipt.irqRequests << ','
              << "\"irq_dispatches\":" << receipt.irqDispatches << ','
              << "\"vif_input_bytes\":" << receipt.vifInputBytes << ','
              << "\"first_signal_csr\":" << receipt.firstSignalCsr << ','
              << "\"second_signal_csr\":" << receipt.secondSignalCsr << ','
              << "\"finish_csr\":" << receipt.finishCsr << ','
              << "\"final_siglblid\":" << receipt.finalSiglblid << ','
              << "\"final_csr\":" << receipt.finalCsr << ','
              << "\"guest_field_flag\":" << receipt.guestFieldFlag << ','
              << "\"field_advanced\":" << receipt.fieldAdvanced << ','
              << "\"field_tick_matches\":" << receipt.fieldTickMatches << ','
              << "\"field_parity_matches\":" << receipt.fieldParityMatches << ','
              << "\"cleanup_positive_control\":" << receipt.cleanupPositiveControl << ','
              << "\"cleanup_normal_removed\":" << receipt.cleanupNormalRemoved << ','
              << "\"cleanup_early_exit_removed\":" << receipt.cleanupEarlyExitRemoved << ','
              // Host VSync tick values depend on scheduling: diagnostic only.
              << "\"diagnostic\":{"
              << "\"field_before\":" << receipt.fieldBefore << ','
              << "\"field_after\":" << receipt.fieldAfter << ','
              << "\"guest_field_tick\":" << receipt.guestFieldTick << ','
              << "\"field_csr\":" << receipt.fieldCsr << "}}\n";
    return 0;
}
#endif // RRV_GS_PRODUCER_CONTROL

void printStats(const rrv::gsbackend::PresentationStats &stats)
{
    std::cerr << "[rrv-product] presentation-stats"
              << " presented=" << stats.presentedFrames
              << " direct-gpu=" << stats.directGpuPresents
              << " captures-requested=" << stats.requestedCaptures
              << " captures-completed=" << stats.completedCaptures
              << " sync-readbacks=" << stats.synchronousCpuReadbacks
              << " unexpected-readbacks=" << stats.unexpectedReadbacks
              << " cpu-waits=" << stats.cpuWaits << '\n';
}

void printTerminalOutcome(const rrv::guestoutcome::TerminalOutcome &outcome)
{
    std::cerr << "[rrv-product] guest-terminal-outcome="
              << rrv::guestoutcome::name(outcome.kind)
              << " pc=0x" << std::hex << outcome.context.pc
              << " ra=0x" << outcome.context.ra
              << " sp=0x" << outcome.context.sp << std::dec
              << " detail=" << outcome.detail.data() << '\n';
}
} // namespace

// Gate-3 headless presentation (RRV_GATE3_HEADLESS=1; off by default).
// Qualification runs need no visible output: presentation totals are
// diagnostic under ADR-0007. With several windows presenting at once, macOS 27
// WindowServer panicked twice ("mismatched swapID", 2026-09-25). Headless mode
// gives the bridge an NSView and CAMetalLayer that are never placed in a
// window: the PCSX2 Metal device still renders and presents every field, but
// nothing is composited by WindowServer. The main-thread run loop is still
// serviced so the bridge's main-queue work completes. No SDL window, event or
// input exists: guest pad input comes only from the bound workload.
//
// Gate-8 L1 signal stop: SDL's SIGTERM handler only queues SDL_QUIT, which
// this surface never pumps, so a signal was lost and only SIGKILL stopped a
// headless run (at-exit files unwritten). The surface takes SIGTERM, and
// SIGINT/SIGHUP unless inherited as ignored (background job, nohup); the
// close callback then stops the run exactly like a window close. Handlers are
// one-shot: a second signal takes the default action. Host-only.
#include <atomic>
#include <csignal>
#include <dlfcn.h>

namespace {
// AppKit, QuartzCore, libobjc and CoreFoundation are already loaded through the
// SDL runtime dependency; resolve them at run time instead of adding link
// inputs to the candidate composition.
template <typename F>
F gate3HeadlessSymbolV1(const char *name)
{
    return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
}
struct Gate3RectV1 { double x, y, width, height; }; // CGRect layout (arm64 HFA)

bool gate3HeadlessRequestedV1()
{
    const char *value = std::getenv("RRV_GATE3_HEADLESS");
    return value && std::strcmp(value, "1") == 0;
}

// Product storage switch (RRV_GATE3_MC_PERSISTENT=1). It must agree with the
// bound workload's storage binding; qualification workloads bind none.
bool gate3McPersistentEnvV1()
{
    const char *value = std::getenv("RRV_GATE3_MC_PERSISTENT");
    return value && std::strcmp(value, "1") == 0;
}

class Gate3NeutralHostPadV1 final : public HostPadBackend
{
public:
    HostPadState snapshot(unsigned) override { return {}; }
    HostPadCapabilities capabilities(unsigned) const override { return {}; }
};

class Gate3HeadlessSurfaceV1
{
public:
    using Id = void *;
    ~Gate3HeadlessSurfaceV1()
    {
        if (m_view) send<void>(m_view, "release");
        if (m_layer) send<void>(m_layer, "release");
    }

    bool create(std::string *error)
    {
        const auto getClass = gate3HeadlessSymbolV1<Id (*)(const char *)>("objc_getClass");
        s_selector = gate3HeadlessSymbolV1<Id (*)(const char *)>("sel_registerName");
        s_msgSend = gate3HeadlessSymbolV1<void *>("objc_msgSend");
        s_runLoop = gate3HeadlessSymbolV1<int32_t (*)(const void *, double, unsigned char)>("CFRunLoopRunInMode");
        const auto mode = gate3HeadlessSymbolV1<const void *const *>("kCFRunLoopDefaultMode");
        const Id viewClass = getClass ? getClass("NSView") : nullptr;
        const Id layerClass = getClass ? getClass("CAMetalLayer") : nullptr;
        if (!s_selector || !s_msgSend || !s_runLoop || !mode || !viewClass || !layerClass)
        {
            if (error) *error = "headless: Objective-C/AppKit/QuartzCore/CoreFoundation symbols unavailable";
            return false;
        }
        s_mode = *mode;
        m_layer = send<Id>(send<Id>(layerClass, "layer"), "retain");
        const Gate3RectV1 frame{0.0, 0.0, double(kWidth), double(kHeight)};
        m_view = reinterpret_cast<Id (*)(Id, Id, Gate3RectV1)>(s_msgSend)(
            send<Id>(viewClass, "alloc"), s_selector("initWithFrame:"), frame);
        if (!m_layer || !m_view)
        {
            if (error) *error = "headless: NSView/CAMetalLayer creation failed";
            return false;
        }
        reinterpret_cast<void (*)(Id, Id, signed char)>(s_msgSend)(m_view, s_selector("setWantsLayer:"), 1);
        reinterpret_cast<void (*)(Id, Id, Id)>(s_msgSend)(m_view, s_selector("setLayer:"), m_layer);
        std::cerr << "[gate3] headless presentation: detached NSView/CAMetalLayer, no window\n";
        installStopSignals();
        return true;
    }

    bool currentSurface(rrv::gsbackend::NativeSurface *surface) const
    {
        if (!surface || !m_view || !m_layer)
            return false;
        *surface = {};
        surface->kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
        surface->nativeView = m_view;
        surface->nativeLayer = m_layer;
        surface->widthPixels = kWidth;
        surface->heightPixels = kHeight;
        surface->backingScale = 1.0f;
        surface->mainThreadPrepared = true;
        return true;
    }

    std::shared_ptr<HostPadBackend> pad = std::make_shared<Gate3NeutralHostPadV1>();

    // Service main-queue/run-loop work without blocking (host-only; guest time
    // is owned by the scheduler and never read here).
    static void pumpCallback(void *) { s_runLoop(s_mode, 0.0, 1); }
    static bool closeCallback(void *)
    {
        const int signal = s_stopSignal.load(std::memory_order_relaxed);
        if (signal != 0 && !s_stopLogged)
        {
            s_stopLogged = true;
            std::cerr << "[gate3] headless stop requested by signal " << signal << '\n';
        }
        return signal != 0;
    }
    static bool surfaceCallback(void *context, rrv::gsbackend::NativeSurface *surface)
    {
        return static_cast<Gate3HeadlessSurfaceV1 *>(context)->currentSurface(surface);
    }

private:
    static void onStopSignal(int signal) { s_stopSignal.store(signal, std::memory_order_relaxed); }
    static void installStopSignals()
    {
        for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        {
            struct sigaction previous{};
            if (sigaction(signal, nullptr, &previous) != 0)
                continue;
            if (signal != SIGTERM && previous.sa_handler == SIG_IGN)
                continue; // inherited ignore (background job, nohup) stays
            struct sigaction action{};
            action.sa_handler = &onStopSignal;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESETHAND; // a second signal takes the default action
            sigaction(signal, &action, nullptr);
        }
    }
    static inline std::atomic<int> s_stopSignal{0};
    static inline bool s_stopLogged = false; // main thread only
    static constexpr uint32_t kWidth = 640u, kHeight = 448u;
    static inline Id (*s_selector)(const char *) = nullptr;
    static inline void *s_msgSend = nullptr;
    static inline int32_t (*s_runLoop)(const void *, double, unsigned char) = nullptr;
    static inline const void *s_mode = nullptr;
    template <typename R>
    static R send(Id target, const char *selector)
    {
        return reinterpret_cast<R (*)(Id, Id)>(s_msgSend)(target, s_selector(selector));
    }
    Id m_view = nullptr;
    Id m_layer = nullptr;
};
} // namespace

int main(int argc, char **argv)
{
    // The developer launcher's single translation of rrv.ini and flags (see fukami_settings.h). It runs before
    // anything else: no ELF, no window, no game state.
    // From inside an app (Fukami.app, the Linux package) the answer includes the app's bundled defaults, as its
    // launch does.
    if (argc >= 2 && std::strncmp(argv[1], "--print-launch-", 15) == 0)
        return fukami::settings::launchEnvMain(argc, argv, fukami::app::defaultIniPath());
#if defined(__linux__)
    // Per-thread CPU load in the session log: which thread saturates on a
    // Steam Deck (Gate 5).
    rrv::host::startThreadCpuLog();
#endif
#if defined(RRV_GS_PRODUCER_CONTROL)
    const std::string_view gate1Prefix = "--gate1-producer-control";
    const std::string_view requested = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    const bool gate1ProducerControl = requested.rfind(gate1Prefix, 0) == 0;
    if (argc != 2 || requested == "--help")
    {
        std::cerr << "usage: " << argv[0] << " <user-owned-boot-elf>|--gate1-producer-control[=all]\n";
        return argc == 2 ? 0 : 2;
    }
#else
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <user-owned-boot-elf>\n";
        return 2;
    }
#endif

    printStartup();
    try
    {
        if (!rrv::m2causal::initializeFromEnvironment())
            throw std::runtime_error("M2 causal trace initialization failed");
#if defined(RRV_GS_PRODUCER_CONTROL)
        if (gate1ProducerControl)
        {
            const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
            std::filesystem::current_path(executable.parent_path());
            constexpr std::string_view assignment = "--gate1-producer-control=";
            const std::string_view scenario = requested == gate1Prefix ? "all" : requested.substr(assignment.size());
            return runGate1ProducerControlCoverageAndPrintReceipt(scenario);
        }
#endif
        const std::filesystem::path elf = std::filesystem::absolute(argv[1]);
#ifndef RRV_GAME_ELF_SHA256
#error "RRV_GAME_ELF_SHA256 must come from generated/rr5/source-manifest.json (RRV_PRODUCT_OWNED_SOURCE build)"
#endif
        if (!rrv::product::verifyGameElfIdentity(elf, RRV_GAME_ELF_SHA256, std::cerr))
            return 3;
        const bool m2NeutralityEnabled = rrv::m2neutral::enabled();
        const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
        std::filesystem::current_path(executable.parent_path());
        const char *gameName = getGameName(elf.filename().string().c_str());
        std::string title = "Fukami | ";
        title += gameName ? gameName : elf.filename().string();

        const bool gate3Headless = gate3HeadlessRequestedV1();
        rrv::host::SdlPresentation host;
        Gate3HeadlessSurfaceV1 headless;
        std::string hostError;
        if (gate3Headless ? !headless.create(&hostError) : !host.create(title.c_str(), &hostError))
        {
            std::cerr << "[rrv-product] SDL " RRV_PRODUCT_BACKEND_NAME " host initialization failed: " << hostError << '\n';
            return 1;
        }

        rrv::gsbackend::NativeSurface initialSurface{};
        if (!(gate3Headless ? headless.currentSurface(&initialSurface) : host.currentSurface(&initialSurface)))
        {
            std::cerr << "[rrv-product] SDL " RRV_PRODUCT_BACKEND_NAME " host did not provide a drawable surface\n";
            return 1;
        }

        // Set before PS2Runtime construction so product input never constructs
        // the legacy raylib/GameController host backends. Neutral-pad mode is
        // still checked first by PSPadBackend and overrides this physical host.
        PSPadBackend::setExternalBackendForProcess(gate3Headless ? headless.pad : host.padBackend());
        ExternalPadBackendReset resetExternalPadBackend;
        std::cerr << "[rrv-product] input/event owner=SDL 2.32.10 "
                     "(keyboard and GameController APIs)\n";
        int result = 1;
        {
            PS2Runtime runtime;
            PS2Runtime::HostPresentationConfig presentation{};
            presentation.backend.snapshotWidth = 640u;
            presentation.backend.snapshotHeight = 448u;
            presentation.backend.rendererKind = kProductRendererKind;
            presentation.backend.presentationMode = rrv::gsbackend::PresentationMode::DirectGpu;
            presentation.backend.surface = initialSurface;
            presentation.context = gate3Headless ? static_cast<void*>(&headless) : static_cast<void*>(&host);
            presentation.pumpEvents = gate3Headless ? &Gate3HeadlessSurfaceV1::pumpCallback : &rrv::host::SdlPresentation::pumpCallback;
            presentation.shouldClose = gate3Headless ? &Gate3HeadlessSurfaceV1::closeCallback : &rrv::host::SdlPresentation::closeCallback;
            presentation.currentSurface = gate3Headless ? &Gate3HeadlessSurfaceV1::surfaceCallback : &rrv::host::SdlPresentation::surfaceCallback;
            runtime.setHostPresentationConfig(presentation);

            PS2Runtime::IoPaths paths;
            paths.elfDirectory = elf.parent_path();
            paths.cdRoot = elf.parent_path();
            {
                const char* mc = std::getenv("RRV_GATE3_MC_ROOT");
                if (!mc || !*mc)
                    throw std::runtime_error("Gate3 candidate requires RRV_GATE3_MC_ROOT");
                std::error_code ec;
                // Product storage (RRV_GATE3_MC_PERSISTENT=1, only with a workload
                // bound to `storage persistent_user_card`, checked after load) keeps
                // the user's memory card; qualification requires a fresh one.
                if (!gate3McPersistentEnvV1() &&
                    std::filesystem::exists(mc, ec) && !std::filesystem::is_empty(mc, ec))
                    throw std::runtime_error("Gate3 fresh_zeroed storage requires an empty RRV_GATE3_MC_ROOT");
                paths.mcRoot = std::filesystem::path(mc) / "mc0"; // port 1 is its sibling mc1
            }
            PS2Runtime::setIoPaths(paths);
            if (!runtime.initialize(title.c_str()))
            {
                std::cerr << "[rrv-product] runtime initialization failed\n";
            }
            else if (!runtime.activeGsBackend() || !runtime.directGpuPresentationActive() ||
                     rrv::gsbackend::activeRendererKind() != kProductRendererKind)
            {
                std::cerr << "[rrv-product] direct PCSX2 hardware-GS " RRV_PRODUCT_BACKEND_NAME
                             " presentation was not activated\n";
            }
            else
            {
                std::cerr << "[rrv-product] actual renderer=PCSX2 hardware GS backend=" RRV_PRODUCT_BACKEND_NAME " "
                             "presentation=direct GPU\n";
                registerAllFunctions(runtime);
                registerPatches(runtime);
                const uint64_t *privRegs19 =
                    reinterpret_cast<const uint64_t *>(&runtime.memory().gs());
                const char *cacheRebuild = std::getenv("RRV_GS_CACHE_REBUILD_ON_CAPTURE");
                const bool rebuildOnCapture = cacheRebuild && std::strcmp(cacheRebuild, "1") == 0;
                rrv::gsrecord::rrv_gs_record_init_from_env(
                    runtime.memory().getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE),
                    privRegs19, snapshotActiveGsVram, &runtime,
                    rebuildOnCapture ? restoreCapturedGsVramAfterInitialWrite : nullptr,
                    rebuildOnCapture ? &runtime : nullptr);
                struct GsRecordShutdown
                {
                    ~GsRecordShutdown() { rrv::gsrecord::rrv_gs_record_shutdown(); }
                } recordShutdown;

                if (!runtime.loadELF(elf.string()))
                {
                    std::cerr << "[rrv-product] failed to load user ELF\n";
                }
                else
                {
                    {
                        // loadELF re-derives every I/O root from the ELF path; the
                        // memory card must stay in the fresh run root, never the disc tree.
                        auto gate3Paths = PS2Runtime::getIoPaths();
                        gate3Paths.mcRoot = std::filesystem::path(std::getenv("RRV_GATE3_MC_ROOT")) / "mc0";
                        PS2Runtime::setIoPaths(gate3Paths);
                    }
                    {
                        const char* bound = std::getenv("RRV_GATE3_BOUND_WORKLOAD");
                        if (!bound || !*bound)
                            throw std::runtime_error("Gate3 candidate requires RRV_GATE3_BOUND_WORKLOAD");
                        std::cerr << "[gate3] bound workload manifest sha256="
                                  << runtime.gate3LoadBoundWorkloadV1(bound) << "\n";
                        if (runtime.gate3PersistentStorageV1() != gate3McPersistentEnvV1())
                            throw std::runtime_error("Gate3 RRV_GATE3_MC_PERSISTENT must match the workload storage binding");
                    }
                    const uint64_t presentLimit = testPresentLimit();
                    if (m2NeutralityEnabled && presentLimit != 0u)
                        throw std::invalid_argument(
                            "RRV_TEST_PRESENT_LIMIT cannot be combined with the M2 semantic bound");
                    bool m2NeutralityOk = true;
                    const auto terminalOutcomeState = runtime.terminalOutcomeHandle();
                    {
                        std::jthread boundedTest;
                        if (presentLimit != 0u)
                        {
                            std::cerr << "[m1-test] RRV_TEST_PRESENT_LIMIT=" << presentLimit
                                      << ": bounded product execution enabled\n";
                            boundedTest = std::jthread([&runtime, presentLimit, terminalOutcomeState](std::stop_token stop) {
                                while (!stop.stop_requested())
                                {
                                    // presentationStats() is a GS-thread round trip under the
                                    // producer mutex: asked every millisecond it held rrv-vu1
                                    // behind the GS and cost 4% on the Steam Deck (T-DECK-FIXES).
                                    // A field presents at most once, so ask only once enough
                                    // fields have been handed over.
                                    if (runtime.gsFieldTransitionCount() < presentLimit)
                                    {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                        continue;
                                    }
                                    rrv::gsbackend::PresentationStats observed{};
                                    std::string ignored;
                                    if (runtime.presentationStats(observed, &ignored) &&
                                        observed.directGpuPresents >= presentLimit)
                                    {
                                        rrv::guestoutcome::markControlledStop(
                                            *terminalOutcomeState,
                                            rrv::guestoutcome::ControlledStopKind::BoundedPresentLimit);
                                        runtime.requestStop();
                                        return;
                                    }
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                            });
                        }
                        else if (m2NeutralityEnabled)
                        {
                            std::cerr << "[m2-test] semantic-anchor bounded execution enabled\n";
                            boundedTest = std::jthread([&runtime, terminalOutcomeState](std::stop_token stop) {
                                while (!stop.stop_requested())
                                {
                                    if (rrv::m2neutral::intervalComplete())
                                    {
                                        rrv::guestoutcome::markControlledStop(
                                            *terminalOutcomeState,
                                            rrv::guestoutcome::ControlledStopKind::BoundedSemanticInterval);
                                        runtime.requestStop();
                                        return;
                                    }
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                            });
                        }
                        try
                        {
                            runtime.run();
                        }
                        catch (const std::exception &error)
                        {
                            // The runtime records host-loop failure before its cleanup
                            // path can alter guest state. This second call only covers a
                            // future throw site that has not yet been instrumented.
                            rrv::guestoutcome::recordFailure(
                                *terminalOutcomeState,
                                rrv::guestoutcome::TerminalKind::HostRuntimeException,
                                {}, error.what());
                            std::cerr << "[rrv-product] runtime run exception: " << error.what() << '\n';
                        }
                        boundedTest.request_stop();
                    }
                    if (m2NeutralityEnabled)
                    {
                        std::string neutralityError;
                        m2NeutralityOk = rrv::m2neutral::dumpDeferred(&neutralityError);
                        if (!rrv::m2causal::dumpDeferred())
                        {
                            m2NeutralityOk = false;
                            neutralityError = "M2 causal trace incomplete, overflowed, or export failed";
                        }
                        if (!m2NeutralityOk)
                            std::cerr << "[m2-test] fatal: " << neutralityError << '\n';
                    }
                    const auto terminalOutcome = rrv::guestoutcome::snapshot(*terminalOutcomeState);
                    printTerminalOutcome(terminalOutcome);
                    const bool gate3CutReached = runtime.gate3CutSnapshotV1().has_value() &&
                        runtime.gate3CutSnapshotV1()->effective_starts == runtime.gate3TemporalV1().terminal_starts() &&
                        terminalOutcome.kind == rrv::guestoutcome::TerminalKind::StopRequested;
                    if (gate3CutReached)
                        std::cerr << "[gate3] terminal outcome=temporal-cut effective_starts="
                                  << runtime.gate3CutSnapshotV1()->effective_starts << "\n";
                    // Product sessions (persistent_user_card storage) end when the
                    // user closes the window: a host stop request with no recorded
                    // guest failure is a clean exit there. Qualification workloads
                    // bind no storage line and keep requiring the temporal cut.
                    const bool gate3ProductUserStop = runtime.gate3PersistentStorageV1() &&
                        terminalOutcome.kind == rrv::guestoutcome::TerminalKind::StopRequested;
                    if (gate3ProductUserStop && !gate3CutReached)
                        std::cerr << "[gate3] terminal outcome=product-user-stop effective_starts="
                                  << runtime.gate3TemporalV1().starts() << "\n";
                    const bool terminalOutcomeOk = gate3CutReached || gate3ProductUserStop ||
                        rrv::guestoutcome::permitsQualificationSuccess(terminalOutcome);
                    rrv::gsbackend::PresentationStats stats{};
                    std::string statsError;
                    if (!runtime.presentationStats(stats, &statsError))
                    {
                        std::cerr << "[rrv-product] failed to read presentation stats: " << statsError << '\n';
                    }
                    else
                    {
                        printStats(stats);
                        const bool capturesMatch =
                            stats.requestedCaptures == stats.completedCaptures;
                        const bool captureReadbacksMatch =
                            stats.synchronousCpuReadbacks == stats.completedCaptures &&
                            stats.cpuWaits == stats.completedCaptures;
                        if (!terminalOutcomeOk)
                        {
                            std::cerr << "[rrv-product] fatal: guest terminal outcome is not an explicit bounded control\n";
                        }
                        else if (stats.unexpectedReadbacks != 0u)
                        {
                            std::cerr << "[rrv-product] fatal: unexpected direct-presentation CPU readback\n";
                        }
                        else if (!capturesMatch)
                        {
                            std::cerr << "[rrv-product] fatal: explicit capture request did not complete exactly once\n";
                        }
                        else if (!captureReadbacksMatch)
                        {
                            std::cerr << "[rrv-product] fatal: CPU readback/wait was not exactly capture-attributed\n";
                        }
                        else if (stats.directGpuPresents == 0u && !startupTestException())
                        {
                            std::cerr << "[rrv-product] fatal: run ended without an actual direct GPU present\n";
                        }
                        else if (host.lifecycleTestEnabled())
                        {
                            std::string lifecycleError;
                            if (!host.lifecycleTestComplete(&lifecycleError))
                                std::cerr << "[rrv-product] fatal: " << lifecycleError << '\n';
                            else
                                result = (m2NeutralityOk && terminalOutcomeOk) ? 0 : 1;
                        }
                        else
                        {
                            if (stats.directGpuPresents == 0u)
                                std::cerr << "[rrv-product] startup-test exception: no drawable present required\n";
                            result = (m2NeutralityOk && terminalOutcomeOk) ? 0 : 1;
                        }
                    }
                }
            }
        } // Backend/GSclose runs before SDL destroys the borrowed surface.
        return result;
    }
    catch (const std::exception &error)
    {
        std::cerr << "[rrv-product] fatal: " << error.what() << '\n';
        return 1;
    }
}
