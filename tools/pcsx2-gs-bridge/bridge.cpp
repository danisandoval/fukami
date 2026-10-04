// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team; 2026 RRV-Recomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Provenance: lifecycle/config/window glue is adapted from
// pcsx2-gsrunner/Main.cpp at PCSX2 d5f75c9e42064a77036466f1f4594b0507888633
// (GPL-3.0+).  The exact PCSX2 GS API calls below are from pcsx2/GS/GS.h and
// pcsx2/GS/GS.cpp at the same revision.  This file is built only by the
// clean-worktree script; it never modifies the pinned, dirty PCSX2 checkout.

#define RRV_PCSX2_GS_BRIDGE_BUILD 1
#include "rrv_pcsx2_gs_bridge.h"
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
#include "canonical_receipt.h"
#endif

#include "common/FileSystem.h"
#include "common/FPControl.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Memory.h"
#include "pcsx2/VMManager.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <cstddef>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {

extern "C" void rrv_pcsx2_gs_bridge_initialize_host_console();
extern "C" int rrv_pcsx2_gs_bridge_configure_native_surface(
    uint32_t kind, uint32_t flags, void* native_view, void* native_layer,
    uint32_t width_pixels, uint32_t height_pixels, float backing_scale,
    char* error, uint32_t error_capacity);
extern "C" void rrv_pcsx2_gs_bridge_clear_native_surface();
extern "C" int rrv_pcsx2_gs_bridge_validate_macos_surface(
    void* native_view, void* native_layer, char* error, uint32_t error_capacity);
extern "C" int rrv_pcsx2_gs_bridge_is_macos_main_thread();
extern "C" int rrv_pcsx2_gs_bridge_consume_owner_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity);
#if !defined(__APPLE__)
// linux_surface_adapter.cpp: handle-shape checks for the X11/Wayland kinds.
extern "C" int rrv_pcsx2_gs_bridge_validate_linux_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity);
#endif

// The GPU renderer the bridge presents through on this platform: Metal on
// macOS, Vulkan elsewhere (Linux/Steam Deck, Gate 5). Direct presentation,
// the RR5 hardware profile and RENDERER_AUTO all resolve to it.
#if defined(__APPLE__)
constexpr GSRendererType kPlatformGpuRenderer = GSRendererType::Metal;
constexpr const char* kPlatformGpuBackendName = "Metal";
#else
constexpr GSRendererType kPlatformGpuRenderer = GSRendererType::VK;
constexpr const char* kPlatformGpuBackendName = "Vulkan";
#endif

constexpr size_t kPrivRegsBytes = 0x2000;
constexpr size_t kSlotCount = RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT;
constexpr std::array<size_t, kSlotCount> kPcsx2Offsets = {
    0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224,
    0x1000, 0x1010, 0x1040, 0x1080,
};

static_assert(kPcsx2Offsets.size() == kSlotCount);

void SetError(char* destination, uint32_t capacity, const char* format, ...)
{
    if (!destination || capacity == 0)
        return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(destination, capacity, format, args);
    va_end(args);
    destination[capacity - 1] = '\0';
}

std::string GetBridgeDirectory()
{
#if defined(_WIN32)
    return {};
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&GetBridgeDirectory), &info) == 0 || !info.dli_fname)
        return {};
    const std::string library_path = Path::Canonicalize(info.dli_fname);
    return std::string(Path::GetDirectory(library_path));
#endif
}

bool InitializePcsx2BaseSettings(std::string& error)
{
    // gsrunner uses these same in-memory settings layers.  GSopen(SW) still
    // initializes ImGui's presentation plumbing, so its font setup is required
    // even though this bridge is surfaceless and never draws an OSD.
    static bool initialized = false;
    static MemorySettingsInterface settings;
    if (initialized)
        return true;

    rrv_pcsx2_gs_bridge_initialize_host_console();

    EmuFolders::SetAppRoot();
    // The canonical package reader has already verified the loaded image. Its
    // resources live beside that image and must remain the only resource root:
    // environment, bundle, and default-library fallbacks would sever that
    // source-derived authority.
    const std::string bridge_directory = GetBridgeDirectory();
    const std::string staged_resources = bridge_directory.empty() ? std::string{} :
        Path::Combine(bridge_directory, "resources");
    if (!staged_resources.empty() && FileSystem::DirectoryExists(staged_resources.c_str()))
        EmuFolders::Resources = Path::Canonicalize(staged_resources);
    else
    {
        error = "PCSX2 resources directory is unavailable beside the verified bridge";
        return false;
    }

    if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()) ||
        !EmuFolders::SetDataDirectory(nullptr))
    {
        error = "PCSX2 resources/data directory setup failed";
        return false;
    }
    // SetDataDirectory creates writable user paths. Pin its resource override
    // slot to the verified immutable package before any PCSX2 subsystem can
    // resolve fonts or shaders through GetOverridableResourcePath().
    EmuFolders::UserResources = EmuFolders::Resources;

    const char* hardware_error = nullptr;
    if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
    {
        error = hardware_error ? hardware_error : "PCSX2 early hardware checks failed";
        return false;
    }

    Host::Internal::SetBaseSettingsLayer(&settings);
    VMManager::SetDefaultSettings(settings, true, true, true, true, true);
    VMManager::Internal::LoadStartupSettings();

    const std::string roboto_path =
        EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
    const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
    if (roboto_data.empty())
    {
        error = "PCSX2 resource font is unavailable: " + roboto_path;
        return false;
    }

    std::vector<ImGuiManager::FontInfo> fonts;
    ImGuiManager::FontInfo font{};
    font.data = roboto_data;
    fonts.push_back(font);
    ImGuiManager::SetFonts(std::move(fonts));
    initialized = true;
    return true;
}

// PCSX2's software rasterizer JIT emits into the host code reserve
// (HostMemoryMap::SWrec), which every PCSX2 frontend allocates through
// SysMemory before opening the GS. The product bridge no longer runs that
// JIT: the SW rasterizer (CPU sprite rendering) uses ahead-of-time specialised
// C functions, verified pixel-identical to the ARM64 JIT (the ARM64 C-path
// defects behind KNOWN_ISSUES #21 are fixed in pcsx2-gs-bridge-target.patch),
// so nothing is mapped. Only the diagnostic JIT build
// (RRV_PCSX2_GS_BRIDGE_SW_JIT=ON, which defines RRV_PCSX2_GS_SW_JIT) owns the
// map. The EE/IOP/VU regions it also reserves are unused by RRV; they are
// address-space reservations, not commitments.
//
// Release is mandatory, not tidiness: SharedMemoryMappingArea's destructor
// asserts "No mappings left", and reaching that assert during static
// destruction aborts the process on an already-destroyed mutex.
bool s_host_memory_allocated = false;

bool AcquireHostMemory(std::string& error)
{
    if (s_host_memory_allocated)
        return true;
#if !defined(RRV_PCSX2_GS_SW_JIT)
    (void)error;
    return true;
#endif
    if (!SysMemory::Allocate())
    {
        error = "PCSX2 host memory map allocation failed (required by the SW rasterizer JIT)";
        return false;
    }
    s_host_memory_allocated = true;
    return true;
}

void ReleaseHostMemory()
{
    if (!s_host_memory_allocated)
        return;
    SysMemory::Release();
    s_host_memory_allocated = false;
}

// AcquireHostMemory() reserves process-global PCSX2 mappings.  A failed
// bridge create must release them before returning: otherwise a later create
// observes stale ownership and the static PCSX2 teardown can assert.
class HostMemoryReleaseGuard
{
public:
    ~HostMemoryReleaseGuard()
    {
        if (m_armed)
            ReleaseHostMemory();
    }

    void disarm() { m_armed = false; }

private:
    bool m_armed = true;
};

// Env hatches for the KNOWN_ISSUES #24 differential. All default to the
// bridge's existing behaviour, so an unset environment changes nothing.
bool EnvFlag(const char* name, bool fallback)
{
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0')
        return fallback;
    return v[0] != '0';
}

// Take the merged texture at PCSX2's own internal resolution (window 0x0),
// which is exactly what `pcsx2-gsrunner`'s ScreenshotSize=InternalResolution-
// Uncorrected does. Passing a real window size instead makes PCSX2 run
// CalculateDrawDstRect (aspect correction) + a bilinear StretchRect fit, i.e. a
// different image operation from the reference, and the frontend then presents
// that already-resampled image -- two transforms where there should be none.
// That resample WAS all of KNOWN_ISSUES #24: matching it collapses direct-vs-
// MTGS from MAE 20.43 to 0.71 on A1 and 11.52 to 0.31 on A2, both inside the
// self-drift floor. RRV_PCSX2_GS_SNAPSHOT_INTERNAL=0 restores the old fit.
bool SnapshotAtInternalResolution()
{
    static const bool v = EnvFlag("RRV_PCSX2_GS_SNAPSHOT_INTERNAL", true);
    return v;
}

// RRV_PCSX2_GS_VSYNC_REGWRITTEN=0 passes registers_written=false, which is what
// GSDumpReplayer does (MTGS::PostVsyncStart(false)) on every frame. It feeds
// GSRenderer::VSync's SkipDuplicateFrames / PerformanceMetrics decision only.
bool VsyncRegistersWritten()
{
    static const bool v = EnvFlag("RRV_PCSX2_GS_VSYNC_REGWRITTEN", true);
    return v;
}

// RRV_PCSX2_GS_VSYNC_FIELD_FROM_CSR=1 stops forcing CSR.FIELD from RRV's own
// field parity and derives the GSvsync field argument from the delivered CSR
// exactly as MTGS does. `field` is the argument Merge() composes with, so this
// is the one vsync input that can change the composed image.
bool VsyncFieldFromCsr()
{
    static const bool v = EnvFlag("RRV_PCSX2_GS_VSYNC_FIELD_FROM_CSR", false);
    return v;
}

bool BridgeDiagEnabled()
{
    const char* value = std::getenv("RRV_PCSX2_GS_DIAG");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool BridgeDiagVerbose()
{
    const char* value = std::getenv("RRV_PCSX2_GS_DIAG");
    return value && std::strcmp(value, "verbose") == 0;
}

// Inline GS calls can arrive inside the producer's VU rounding scope. PCSX2's
// MTGS thread instead starts with GetDefault(), so isolate only when requested
// and restore the caller's FPCR before returning to guest execution.
class ScopedBridgeGsFpcr
{
public:
    explicit ScopedBridgeGsFpcr(bool isolated)
    {
        if (isolated)
            m_backup.emplace(FPControlRegister::GetDefault());
    }

private:
    std::optional<FPControlRegisterBackup> m_backup;
};

// PCSX2's GameIndex gives Ridge Racer V (SLUS-20002) textureInsideRT=1 to
// repair its post-processing feedback passes. The bridge never boots a disc,
// so it has no serial/CRC through which PCSX2 could select that profile. The
// remaining values below each have an independent, capture-backed reason:
// CPU sprite rendering fixes the car material feedback path, while the
// large-ST rewrite is limited to the reflection-map draws which need it.
// Keep this as a deliberately small, named hardware profile until each
// additional GameIndex setting has its own capture-backed justification.
constexpr const char* kMetalTextureInsideRtEnv = "RRV_PCSX2_GS_METAL_TEXTURE_INSIDE_RT";
constexpr const char* kMetalCpuSpriteBwEnv = "RRV_PCSX2_GS_CPU_SPRITE_BW";
// RR5 Metal profile: PCSX2 GPU palette conversion (paltex) is on. 8-bit
// textures stay indexed and the CLUT lookup runs in the shader, so the hash
// cache no longer keys on the CLUT. Gate-4 G4-6 KEEP (owner visual acceptance
// VIS-004, 2026-09-25): window R +28.6%, traces/digests identical, image
// differs from the CPU-expanded path in sampling detail. RRV_PCSX2_GS_PALTEX=0
// is the rollback to PCSX2's default (off); =1 is accepted and changes nothing.
constexpr const char* kMetalPaltexEnv = "RRV_PCSX2_GS_PALTEX";

const char* TextureInsideRtModeName(GSTextureInRtMode mode)
{
    switch (mode)
    {
        case GSTextureInRtMode::Disabled:
            return "Disabled";
        case GSTextureInRtMode::InsideTargets:
            return "InsideTargets";
        case GSTextureInRtMode::MergeTargets:
            return "MergeTargets";
        default:
            return "Unknown";
    }
}

bool ParseMetalTextureInsideRtOverride(GSTextureInRtMode& mode)
{
    const char* value = std::getenv(kMetalTextureInsideRtEnv);
    if (!value || value[0] == '\0')
        return false;

    if (value[0] >= '0' && value[0] <= '2' && value[1] == '\0')
    {
        mode = static_cast<GSTextureInRtMode>(value[0] - '0');
        return true;
    }

    std::fprintf(stderr,
                 "[pcsx2-gs-bridge] ignoring invalid %s=%s (expected 0=Disabled, "
                 "1=InsideTargets, or 2=MergeTargets)\n",
                 kMetalTextureInsideRtEnv, value);
    return false;
}

bool ParseMetalPaltexOverride(bool& paltex)
{
    const char* value = std::getenv(kMetalPaltexEnv);
    if (!value || value[0] == '\0')
        return false;
    if ((value[0] == '0' || value[0] == '1') && value[1] == '\0')
    {
        paltex = value[0] == '1';
        return true;
    }
    std::fprintf(stderr, "[pcsx2-gs-bridge] ignoring invalid %s=%s (expected 0 or 1)\n",
                 kMetalPaltexEnv, value);
    return false;
}

bool ParseMetalCpuSpriteBwOverride(unsigned& bw)
{
    const char* value = std::getenv(kMetalCpuSpriteBwEnv);
    if (!value || value[0] == '\0')
        return false;
    if ((value[0] == '0' || value[0] == '1') && value[1] == '\0')
    {
        bw = static_cast<unsigned>(value[0] - '0');
        return true;
    }
    std::fprintf(stderr, "[pcsx2-gs-bridge] ignoring invalid %s=%s (expected 0 or 1)\n",
                 kMetalCpuSpriteBwEnv, value);
    return false;
}

// The RR5 hardware profile. Every setting here is a PCSX2 HW-renderer option
// (GSRendererHW / GSTextureCache), not a Metal one, so the Vulkan renderer on
// Linux gets the same profile; the env names keep their historical METAL
// spelling so launchers and captures stay comparable across platforms.
void ApplyRrvMetalHardwareProfile(GSRendererType renderer, Pcsx2Config::GSOptions& options)
{
#if defined(__APPLE__)
    // GSopen(Auto) resolves to Metal on macOS (GSUtil::GetPreferredRenderer).
    // Keep the software renderer's defaults untouched.
    if (renderer != GSRendererType::Metal && renderer != GSRendererType::Auto)
        return;
#else
    // Off macOS the bridge resolves AUTO to Vulkan itself (ResolveRenderer),
    // so GSopen never sees Auto here; OpenGL is not a supported product path.
    if (renderer != GSRendererType::VK)
        return;
#endif

    GSTextureInRtMode texture_inside_rt = GSTextureInRtMode::InsideTargets;
    const bool overridden = ParseMetalTextureInsideRtOverride(texture_inside_rt);
    unsigned cpu_sprite_bw = 1;
    const bool cpu_sprite_overridden = ParseMetalCpuSpriteBwOverride(cpu_sprite_bw);
    options.UserHacks_TextureInsideRt = texture_inside_rt;
    options.UserHacks_CPUSpriteRenderBW = cpu_sprite_bw;
    options.UserHacks_CPUSpriteRenderLevel = 1;
    options.UserHacks_RewriteLargeST = true;
    bool paltex = true;
    const bool paltex_overridden = ParseMetalPaltexOverride(paltex);
    options.GPUPaletteConversion = paltex;
    std::fprintf(stderr,
                 "[pcsx2-gs-bridge] RR5 %s profile: TextureInsideRt=%u (%s)%s "
                 "CPUSpriteRenderBW=%u%s CPUSpriteRenderLevel=%u RewriteLargeST=%u "
                 "GPUPaletteConversion=%u%s\n",
#if defined(__APPLE__)
                 "Metal",
#else
                 "Vulkan",
#endif
                 static_cast<unsigned>(texture_inside_rt), TextureInsideRtModeName(texture_inside_rt),
                 overridden ? " [env override]" : "",
                 static_cast<unsigned>(options.UserHacks_CPUSpriteRenderBW),
                 cpu_sprite_overridden ? " [diagnostic override]" : "",
                 static_cast<unsigned>(options.UserHacks_CPUSpriteRenderLevel),
                 static_cast<unsigned>(options.UserHacks_RewriteLargeST),
                 static_cast<unsigned>(options.GPUPaletteConversion),
                 paltex_overridden ? " [env override]" : "");
}

bool ResolveRendererFromEnvironment(GSRendererType& renderer, const char*& name,
                                    uint32_t& kind, const char*& display_name,
                                    std::string& error)
{
    const char* requested = std::getenv("RRV_PCSX2_GS_RENDERER");
    if (!requested || requested[0] == '\0' || std::strcmp(requested, "software") == 0)
    {
        renderer = GSRendererType::SW;
        name = "software";
        kind = RRV_PCSX2_GS_RENDERER_SOFTWARE;
        display_name = "PCSX2 Software";
        return true;
    }
    if (std::strcmp(requested, "auto") == 0)
    {
#if defined(__APPLE__)
        renderer = GSRendererType::Auto;
#else
        // PCSX2's own Auto may pick OpenGL on Linux; the product is Vulkan.
        renderer = GSRendererType::VK;
#endif
        name = "auto";
        kind = RRV_PCSX2_GS_RENDERER_AUTO;
        display_name = "PCSX2 Auto";
        return true;
    }
    if (std::strcmp(requested, "metal") == 0)
    {
#if defined(__APPLE__)
        renderer = GSRendererType::Metal;
        name = "metal";
        kind = RRV_PCSX2_GS_RENDERER_METAL;
        display_name = "PCSX2 Metal";
        return true;
#else
        error = "RRV_PCSX2_GS_RENDERER=metal is only available on macOS";
        return false;
#endif
    }
    if (std::strcmp(requested, "vulkan") == 0)
    {
#if !defined(__APPLE__)
        renderer = GSRendererType::VK;
        name = "vulkan";
        kind = RRV_PCSX2_GS_RENDERER_VULKAN;
        display_name = "PCSX2 Vulkan";
        return true;
#else
        error = "RRV_PCSX2_GS_RENDERER=vulkan is not available on macOS (use metal)";
        return false;
#endif
    }

    error = "invalid RRV_PCSX2_GS_RENDERER='" + std::string(requested) +
        "' (expected software, metal, vulkan, or auto)";
    return false;
}

bool ResolveRenderer(uint32_t requested_kind, GSRendererType& renderer, const char*& name,
                     uint32_t& kind, const char*& display_name, std::string& error)
{
    if (requested_kind == RRV_PCSX2_GS_RENDERER_UNKNOWN)
        return ResolveRendererFromEnvironment(renderer, name, kind, display_name, error);

    switch (requested_kind)
    {
        case RRV_PCSX2_GS_RENDERER_SOFTWARE:
            renderer = GSRendererType::SW;
            name = "software";
            kind = requested_kind;
            display_name = "PCSX2 Software";
            return true;
        case RRV_PCSX2_GS_RENDERER_AUTO:
#if defined(__APPLE__)
            renderer = GSRendererType::Auto;
#else
            // AUTO resolves to Vulkan off macOS (bridge ABI contract).
            renderer = GSRendererType::VK;
#endif
            name = "auto";
            kind = requested_kind;
            display_name = "PCSX2 Auto";
            return true;
        case RRV_PCSX2_GS_RENDERER_METAL:
#if defined(__APPLE__)
            renderer = GSRendererType::Metal;
            name = "metal";
            kind = requested_kind;
            display_name = "PCSX2 Metal";
            return true;
#else
            error = "Metal renderer requested through the bridge ABI on a non-macOS host";
            return false;
#endif
        case RRV_PCSX2_GS_RENDERER_VULKAN:
#if !defined(__APPLE__)
            renderer = GSRendererType::VK;
            name = "vulkan";
            kind = requested_kind;
            display_name = "PCSX2 Vulkan";
            return true;
#else
            error = "Vulkan renderer requested through the bridge ABI on macOS";
            return false;
#endif
        default:
            error = "invalid renderer kind value " + std::to_string(requested_kind);
            return false;
    }
}

// Gate-4 G4-I4 GS attribution recorder. Off by default; host-only; never
// read by the guest or by any GS decision. RRV_GATE4_GS_COUNTERS=<path>
// records, per bridge VSync (one GS field service):
//   - the steady_clock ns at VSync entry, on the same clock as the G4-I1
//     start clock, so fields can be mapped to effective-start ordinals;
//   - GIF packets, bytes and host ns inside GSgifTransfer since the previous
//     VSync, and host ns inside this GSvsync;
//   - bridge CSR/SIGLBLID readback calls and the submits after which PCSX2's
//     CSR or SIGLBLID changed (SIGNAL/FINISH/LABEL results an async GS would
//     have to wait for), and local-to-host reads (count, bytes, host ns);
//   - Gate 7: GPU ms of the Metal command buffers (Vulkan: timestamp-query
//     GPU time, when the device supports timestamps) that completed since the
//     previous read (GSDevice accumulated GPU time, read and reset just
//     before and just after this GSvsync, because PCSX2's present path
//     resets it inside GSvsync). Buffers complete asynchronously, so a
//     field's value can include earlier fields' work; the sum is complete.
//     The second value is PCSX2's own per-present GPU average (0.5 s
//     metrics interval), which the first read now starves;
//   - PCSX2's own g_perfmon counters accumulated between the previous VSync
//     return and this VSync entry, and inside this GSvsync when PCSX2 did not
//     reset them there (it resets every 32 frames; then the value is -1).
// Rows are kept in a preallocated array and written once at bridge destroy or
// process exit. This is an attribution instrument: its clock pairs cost time,
// so runs that enable it are never timing runs.
//
// RRV_GATE4_GS_CAPTURE_DIR=<dir> with RRV_GATE4_GS_CAPTURE_FIELDS=<spec>
// (comma-separated N or A-B or A-B/S, in bridge field indices) writes the
// merged display output at internal resolution after each selected GSvsync as
// <dir>/field-<N>-<W>x<H>.rgba (raw RGBA8). This is a synchronous GPU readback
// for the G4-6 pixel check; capture runs are never timing runs.
struct Gate4GsObserverV1;
Gate4GsObserverV1& Gate4GsObserver();

struct Gate4GsFieldV1
{
    uint64_t field_index = 0, entry_ns = 0, packets = 0, bytes = 0, gif_ns = 0, vsync_ns = 0;
    uint64_t readbacks = 0, result_changes = 0, local_reads = 0, local_read_bytes = 0, local_read_ns = 0;
    uint32_t parity = 0;
    float gpu_ms = 0.0f, gpu_avg_ms = 0.0f;
    bool vsync_perf_valid = false;
    std::array<double, GSPerfMon::CounterLast> perf{};
    std::array<double, GSPerfMon::CounterLast> vsync_perf{};
};

struct Gate4GsObserverV1
{
    bool enabled = false, written = false;
    std::string path;
    std::vector<Gate4GsFieldV1> rows;
    uint64_t dropped = 0;
    Gate4GsFieldV1 open_field;
    std::array<double, GSPerfMon::CounterLast> last_post{};
    // Capture.
    std::string capture_dir;
    std::vector<std::array<uint64_t, 3>> capture_ranges; // first, last, step
    uint64_t captures = 0, capture_failures = 0;

    static uint64_t Now()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static std::array<double, GSPerfMon::CounterLast> Perf()
    {
        std::array<double, GSPerfMon::CounterLast> out{};
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = g_perfmon.GetCounter(static_cast<GSPerfMon::counter_t>(i));
        return out;
    }
    bool WantCapture(uint64_t field) const
    {
        for (const auto& r : capture_ranges)
            if (field >= r[0] && field <= r[1] && (field - r[0]) % r[2] == 0u)
                return true;
        return false;
    }
    void Configure()
    {
        written = false;
        enabled = false;
        capture_dir.clear();
        capture_ranges.clear();
        if (const char* v = std::getenv("RRV_GATE4_GS_COUNTERS"); v && v[0] != '\0')
        {
            enabled = true;
            path = v;
            rows.clear();
            rows.reserve(32768u);
            dropped = 0;
            open_field = {};
            last_post = Perf();
            static const bool at_exit = [] { std::atexit([] { Gate4GsObserver().Write(); }); return true; }();
            (void)at_exit;
            std::fprintf(stderr, "[gate4-gs] counters -> %s\n", path.c_str());
        }
        const char* dir = std::getenv("RRV_GATE4_GS_CAPTURE_DIR");
        const char* spec = std::getenv("RRV_GATE4_GS_CAPTURE_FIELDS");
        if (dir && dir[0] != '\0' && spec && spec[0] != '\0')
        {
            capture_dir = dir;
            capture_ranges.clear();
            const char* p = spec;
            while (*p)
            {
                char* end = nullptr;
                const uint64_t first = std::strtoull(p, &end, 10);
                uint64_t last = first, step = 1;
                if (end == p) break;
                p = end;
                if (*p == '-') { last = std::strtoull(p + 1, &end, 10); p = end; }
                if (*p == '/') { step = std::strtoull(p + 1, &end, 10); p = end; }
                if (step == 0u || last < first) break;
                capture_ranges.push_back({first, last, step});
                if (*p == ',') ++p; else break;
            }
            if (*p != '\0')
            {
                std::fprintf(stderr, "[gate4-gs] invalid RRV_GATE4_GS_CAPTURE_FIELDS=%s; capture off\n", spec);
                capture_ranges.clear();
            }
            else
                std::fprintf(stderr, "[gate4-gs] capture -> %s (%zu ranges)\n",
                             capture_dir.c_str(), capture_ranges.size());
        }
    }
    void Write()
    {
        if (written)
            return;
        written = true;
        if (!capture_dir.empty() && !capture_ranges.empty())
            std::fprintf(stderr, "[gate4-gs] captures=%llu failures=%llu\n",
                         static_cast<unsigned long long>(captures),
                         static_cast<unsigned long long>(capture_failures));
        if (!enabled)
            return;
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f)
        {
            std::fprintf(stderr, "[gate4-gs] cannot open %s\n", path.c_str());
            return;
        }
        std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
        // perf columns are PCSX2 HW names: Prim Draw DrawCalls Readbacks Swizzle
        // Unswizzle TextureCopies(Fillrate) TextureUploads(SyncPoint) Barriers
        // RenderPasses TextureCopiesROV DrawCallsROV BarriersROV.
        std::fprintf(f, "# gate4-gs v1 columns: field parity entry_ns packets bytes gif_ns vsync_ns "
                        "readbacks result_changes local_reads local_read_bytes local_read_ns "
                        "p:Prim,Draw,DrawCalls,Readbacks,Swizzle,Unswizzle,TextureCopies,TextureUploads,"
                        "Barriers,RenderPasses,TextureCopiesROV,DrawCallsROV,BarriersROV "
                        "v:<same, inside GSvsync, -1 if PCSX2 reset> g:gpu_ms,pcsx2_avg_gpu_ms\n");
        for (const Gate4GsFieldV1& r : rows)
        {
            std::fprintf(f, "%llu %u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu p:",
                static_cast<unsigned long long>(r.field_index), r.parity,
                static_cast<unsigned long long>(r.entry_ns), static_cast<unsigned long long>(r.packets),
                static_cast<unsigned long long>(r.bytes), static_cast<unsigned long long>(r.gif_ns),
                static_cast<unsigned long long>(r.vsync_ns), static_cast<unsigned long long>(r.readbacks),
                static_cast<unsigned long long>(r.result_changes), static_cast<unsigned long long>(r.local_reads),
                static_cast<unsigned long long>(r.local_read_bytes),
                static_cast<unsigned long long>(r.local_read_ns));
            for (size_t i = 0; i < r.perf.size(); ++i)
                std::fprintf(f, i ? ",%.0f" : "%.0f", r.perf[i]);
            std::fprintf(f, " v:");
            for (size_t i = 0; i < r.vsync_perf.size(); ++i)
                std::fprintf(f, i ? ",%.0f" : "%.0f", r.vsync_perf_valid ? r.vsync_perf[i] : -1.0);
            std::fprintf(f, " g:%.4f,%.4f\n", r.gpu_ms, r.gpu_avg_ms);
        }
        std::fclose(f);
        std::fprintf(stderr, "[gate4-gs] fields=%zu dropped=%llu path=%s\n", rows.size(),
                     static_cast<unsigned long long>(dropped), path.c_str());
    }
};

Gate4GsObserverV1& Gate4GsObserver()
{
    static Gate4GsObserverV1 observer;
    return observer;
}

} // namespace

struct RrvPcsx2GsBridge {
    alignas(16) std::array<uint8_t, kPrivRegsBytes> pcsx2_regs{};
    std::vector<uint32_t> pixels;
    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    uint64_t field_index = 0;
    uint64_t sequence = 0;
    uint64_t vsync_from_field = 0; // RRV_GATE3_FAST_FORWARD: FIFO switched on here
    RrvPcsx2GsBridgeStats stats{sizeof(RrvPcsx2GsBridgeStats), 0u, 0u, 0u,
                                 0u, 0u, 0u, 0u, 0u};
    bool display_enabled = false;
    bool has_successful_frame = false;
    bool gpu_snapshot_fallback = false;
    bool direct_present = false;
    bool owner_thread_surface = false;
    bool gpu_renderer = false;
    bool capture_in_progress = false;
    bool fp_isolated = false;
    bool full_frame = false;
    bool fpcr_path_logged[4]{};
    bool open = false;
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    rrv::pcsx2::receipt::Collector canonical_receipt;
    // This sideband is intentionally separate from Collector and receipt
    // schema 1. It proves where the live selector-edge arm occurred without
    // making that diagnostic state part of a field workload digest.
    bool receipt_arm_after_vsync_pending = false;
    uint64_t consumer_ack_ordinal = 0u;
    RrvPcsx2GsBridgeReceiptBoundaryOrdering receipt_boundary_ordering{
        sizeof(RrvPcsx2GsBridgeReceiptBoundaryOrdering), 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u};
#endif
};

#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
void NoteConsumerAcknowledgement(RrvPcsx2GsBridge* bridge, uint32_t event_type,
                                 bool transfer)
{
    ++bridge->consumer_ack_ordinal;
    RrvPcsx2GsBridgeReceiptBoundaryOrdering& ordering =
        bridge->receipt_boundary_ordering;
    if (ordering.armed_after_successful_vsync == 0u)
    {
        return;
    }
    if (ordering.first_post_arm_consumer_ack_ordinal == 0u)
    {
        ordering.first_post_arm_consumer_ack_ordinal = bridge->consumer_ack_ordinal;
        ordering.first_post_arm_consumer_event_type = event_type;
    }
    if (transfer && ordering.first_post_arm_transfer_ack_ordinal == 0u)
        ordering.first_post_arm_transfer_ack_ordinal = bridge->consumer_ack_ordinal;
}
#endif

extern "C" uint32_t rrv_pcsx2_gs_bridge_version(void)
{
    return RRV_PCSX2_GS_BRIDGE_ABI_VERSION;
}

extern "C" void rrv_pcsx2_gs_bridge_owner_thread_init(void)
{
    // Match MTGS::ThreadEntryPoint at fd9d310c: the GS thread does not inherit
    // the producer's EE/VU rounding and denormal policy.
    FPControlRegister::SetCurrent(FPControlRegister::GetDefault());
}

extern "C" RrvPcsx2GsBridge* rrv_pcsx2_gs_bridge_create(
    const RrvPcsx2GsBridgeConfig* config,
    RrvPcsx2GsBridgeCapabilities* capabilities,
    char* error, uint32_t error_capacity)
{
    static_assert(offsetof(RrvPcsx2GsBridgeConfig, presentation_mode) == 20u,
                  "bridge config ABI v5 presentation offset changed");
    static_assert(offsetof(RrvPcsx2GsBridgeConfig, renderer_kind) == 24u,
                  "bridge config ABI v5 renderer offset changed");
    static_assert(alignof(RrvPcsx2GsBridgeSurface) == alignof(void*),
                  "bridge surface ABI v5 alignment changed");
    static_assert(alignof(RrvPcsx2GsBridgeConfig) == alignof(void*),
                  "bridge config ABI v5 alignment changed");
    static_assert(offsetof(RrvPcsx2GsBridgeSurface, native_view) % alignof(void*) == 0u,
                  "bridge surface native-handle alignment changed");
    static_assert(offsetof(RrvPcsx2GsBridgeCapabilities, renderer_name) % alignof(void*) == 0u,
                  "bridge capabilities renderer-name alignment changed");
    if (!capabilities || capabilities->struct_size < sizeof(RrvPcsx2GsBridgeCapabilities))
    {
        SetError(error, error_capacity, "bridge capabilities output is too small");
        return nullptr;
    }
    if (config && config->struct_size < sizeof(RrvPcsx2GsBridgeConfig))
    {
        SetError(error, error_capacity, "bridge config is too small (%u, expected %zu)",
            config->struct_size, sizeof(RrvPcsx2GsBridgeConfig));
        return nullptr;
    }

    GSRendererType renderer = GSRendererType::SW;
    const char* renderer_name = nullptr;
    uint32_t renderer_kind = RRV_PCSX2_GS_RENDERER_UNKNOWN;
    const char* renderer_display_name = "PCSX2";
    std::string startup_error;
    const uint32_t requested_renderer = config ? config->renderer_kind :
        RRV_PCSX2_GS_RENDERER_UNKNOWN;
    if (!ResolveRenderer(requested_renderer, renderer, renderer_name, renderer_kind,
                         renderer_display_name, startup_error))
    {
        SetError(error, error_capacity, "%s", startup_error.c_str());
        return nullptr;
    }

    // Gate 6: full-frame mode is a hardware-renderer mode (the texture cache
    // keeps render targets as scaled host textures). The software renderer
    // rasterizes into native GS memory only, so it offers field mode alone.
    capabilities->supported_render_modes = RRV_PCSX2_GS_RENDER_MODE_CAP_FIELD |
        (renderer != GSRendererType::SW ? RRV_PCSX2_GS_RENDER_MODE_CAP_FULL_FRAME : 0u);
    capabilities->renderer_kind = renderer_kind;
    capabilities->capability_flags = RRV_PCSX2_GS_CAP_ON_DEMAND_CAPTURE |
        RRV_PCSX2_GS_CAP_LEGACY_CPU_SNAPSHOT;
    capabilities->renderer_name = renderer_display_name;

    const uint32_t presentation_mode = config ? config->presentation_mode :
        RRV_PCSX2_GS_PRESENTATION_LEGACY_CPU_SNAPSHOT;
    if (presentation_mode > RRV_PCSX2_GS_PRESENTATION_DIRECT_GPU)
    {
        SetError(error, error_capacity, "invalid presentation mode value %u", presentation_mode);
        return nullptr;
    }
    const bool direct_requested = presentation_mode == RRV_PCSX2_GS_PRESENTATION_DIRECT_GPU;
#if defined(__APPLE__)
    if (direct_requested && (!config ||
        config->surface.struct_size < sizeof(RrvPcsx2GsBridgeSurface) ||
        config->surface.kind != RRV_PCSX2_GS_SURFACE_MACOS_VIEW_METAL_LAYER ||
        (config->surface.flags & (RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES |
                                  RRV_PCSX2_GS_SURFACE_FLAG_MAIN_THREAD_PREPARED)) !=
            (RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES |
             RRV_PCSX2_GS_SURFACE_FLAG_MAIN_THREAD_PREPARED) ||
        !config->surface.native_view || !config->surface.native_layer ||
        config->surface.width_pixels == 0u || config->surface.height_pixels == 0u ||
        config->surface.backing_scale <= 0.0f))
    {
        SetError(error, error_capacity,
                 "direct presentation requires a main-thread-prepared macOS view/layer "
                 "surface retained by the caller for the bridge lifetime");
        return nullptr;
    }
#else
    // Linux (Gate 5): an SDL X11 (Display*, Window) or Wayland (wl_display*,
    // wl_surface*) surface that the caller retains for the bridge lifetime.
    // PCSX2's Vulkan device creates its own VkSurfaceKHR/swap chain on it and
    // never marshals to a platform main thread, so MAIN_THREAD_PREPARED is
    // accepted but not required here.
    if (direct_requested && (!config ||
        config->surface.struct_size < sizeof(RrvPcsx2GsBridgeSurface) ||
        (config->surface.kind != RRV_PCSX2_GS_SURFACE_LINUX_X11 &&
         config->surface.kind != RRV_PCSX2_GS_SURFACE_LINUX_WAYLAND) ||
        (config->surface.flags & RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES) == 0u ||
        !config->surface.native_view || !config->surface.native_layer ||
        config->surface.width_pixels == 0u || config->surface.height_pixels == 0u ||
        config->surface.backing_scale <= 0.0f))
    {
        SetError(error, error_capacity,
                 "direct presentation requires an X11 or Wayland surface retained by the "
                 "caller for the bridge lifetime");
        return nullptr;
    }
#endif

    const uint32_t requested_mode = config ? config->gs_render_mode :
        RRV_PCSX2_GS_RENDER_MODE_FIELD;
    if (requested_mode > RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME)
    {
        SetError(error, error_capacity, "invalid GS render mode value %u", requested_mode);
        return nullptr;
    }
    const uint32_t requested_mode_bit = 1u << requested_mode;
    if ((capabilities->supported_render_modes & requested_mode_bit) == 0u)
    {
        const char* mode_name = requested_mode == RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME ? "full" : "field";
        SetError(error, error_capacity,
                 "RRV_GS_RENDER_MODE=%s is not implemented by %s backend",
                 mode_name, renderer_display_name);
        return nullptr;
    }

    if (!InitializePcsx2BaseSettings(startup_error))
    {
        SetError(error, error_capacity, "%s", startup_error.c_str());
        return nullptr;
    }

    if (!AcquireHostMemory(startup_error))
    {
        SetError(error, error_capacity, "%s", startup_error.c_str());
        return nullptr;
    }
    HostMemoryReleaseGuard host_memory_guard;

    auto bridge = std::make_unique<RrvPcsx2GsBridge>();
    if (const char* value = std::getenv("RRV_PCSX2_GS_FP_ISOLATED"); value && value[0] != '\0')
    {
        bridge->fp_isolated = std::strcmp(value, "1") == 0;
        if (!bridge->fp_isolated)
            std::fprintf(stderr, "[pcsx2-gs-bridge:fpcr] ignoring invalid RRV_PCSX2_GS_FP_ISOLATED=%s (expected 1)\n", value);
    }
    std::fprintf(stderr, "[pcsx2-gs-bridge:fpcr] isolated=%u default=0x%llx\n",
                 bridge->fp_isolated ? 1u : 0u,
                 static_cast<unsigned long long>(FPControlRegister::GetDefault().bitmask));
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    if (!bridge->canonical_receipt.initializeFromEnvironment(&startup_error))
    {
        SetError(error, error_capacity, "canonical consumer receipt initialization failed: %s",
                 startup_error.c_str());
        return nullptr;
    }
#endif
    if (config)
    {
        bridge->snapshot_width = config->snapshot_width;
        bridge->snapshot_height = config->snapshot_height;
    }

    std::fprintf(stderr, "[pcsx2-gs-bridge] renderer=%s (%s)\n", renderer_name,
                 Pcsx2Config::GSOptions::GetRendererName(renderer));

    Pcsx2Config::GSOptions options{};
    options.Renderer = renderer;
    options.UpscaleMultiplier = 1.0f;
    options.VsyncEnable = false;
    options.AutoFlushSW = true;
    ApplyRrvMetalHardwareProfile(renderer, options);
    const bool full_frame = requested_mode == RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME;
    if (full_frame)
    {
        // Gate 6 full-frame mode (docs/evidence/GATE6_FULL_FRAME_PLAN_2026-09-26.md).
        // RR5 draws each field's complete display list into a 640x224 target.
        // A 2x hardware scale makes PCSX2 rasterize those draws at 448 rows
        // (1280x448 host targets): real full-height geometry, with PCSX2's
        // texture cache keeping feedback, aliases, uploads and readbacks in
        // native GS addresses. The field half-line (odd fields draw with
        // XYOFFSET.OFY 0x7908, even with 0x7900) becomes exactly one host row
        // at 2x and is removed by GSRrvSetFieldOfyCanonical below, so both
        // parities land on one 448-row grid. No deinterlacer runs: each
        // field's target already is the progressive frame.
        options.UpscaleMultiplier = 2.0f;
        options.InterlaceMode = GSInterlaceMode::Off;
        // RRV_PCSX2_GS_FULL_SCALE=1..8: internal scale (default 2). The
        // parity-dependent OFY is canonicalised away (below), so odd scales
        // keep both parities on one grid too; 1 has no extra progressive rows.
        // RRV_PCSX2_GS_FULL_HWMIPMAP=0|1: PCSX2's GS mipmap emulation (PCSX2
        // default on). Launcher option `mipmap` (owner decision 2026-09-29);
        // 0 departs from hardware (the guest's MIPMAP/TEX1 levels are ignored).
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_SCALE"); value && value[0] != '\0')
        {
            if (value[0] < '1' || value[0] > '8' || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_SCALE=%s (expected 1..8)", value);
                return nullptr;
            }
            options.UpscaleMultiplier = static_cast<float>(value[0] - '0');
            std::fprintf(stderr, "[pcsx2-gs-bridge] render mode=full: UpscaleMultiplier=%s%s\n", value,
                         value[0] == '1' ? " [diagnostic override]" : "");
        }
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_HWMIPMAP"); value && value[0] != '\0')
        {
            if ((value[0] != '0' && value[0] != '1') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_HWMIPMAP=%s (expected 0 or 1)", value);
                return nullptr;
            }
            options.HWMipmap = value[0] == '1';
            std::fprintf(stderr, "[pcsx2-gs-bridge] render mode=full: HWMipmap=%s [override]\n", value);
        }
        // PCSX2's AA1 emulation (edge coverage for primitives the game draws
        // with PRIM.AA1) is on in full mode (owner decision 2026-09-26). RR5
        // sets AA1 only on untextured blended lines/strips (census 2026-09-26);
        // visibly the race rev-counter needle. RRV_PCSX2_GS_FULL_HWAA1=0 turns
        // it off.
        options.HWAA1 = true;
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_HWAA1"); value && value[0] != '\0')
        {
            if ((value[0] != '0' && value[0] != '1') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_HWAA1=%s (expected 0 or 1)", value);
                return nullptr;
            }
            options.HWAA1 = value[0] == '1';
            std::fprintf(stderr, "[pcsx2-gs-bridge] render mode=full: HWAA1=%s [override]\n", value);
        }
        if (const char* interlace = std::getenv("RRV_PCSX2_GS_INTERLACE");
            interlace && interlace[0] != '\0')
        {
            SetError(error, error_capacity,
                     "RRV_PCSX2_GS_INTERLACE is a field-mode option; full-frame mode never deinterlaces");
            return nullptr;
        }
        // RR5-specific rule (Gate 6, G6-1 census): 0x7908 is the only
        // parity-dependent OFY; every other half-pixel OFY (the FBP 210
        // downsample pass at OFY 8) is constant across fields and is kept.
        // RRV_GS_FULL_OFY_CANONICAL=0 turns the rule off for the Gate-6
        // G6-2 A/B only (it re-introduces the one-row parity bob).
        bool ofy_canonical = true;
        if (const char* value = std::getenv("RRV_GS_FULL_OFY_CANONICAL"); value && value[0] != '\0')
        {
            if ((value[0] != '0' && value[0] != '1') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_GS_FULL_OFY_CANONICAL=%s (expected 0 or 1)", value);
                return nullptr;
            }
            ofy_canonical = value[0] == '1';
        }
        GSRrvSetFieldOfyCanonical(ofy_canonical ? 0x7908u : 0u, ofy_canonical ? 0x7900u : 0u);
        if (!ofy_canonical)
            std::fprintf(stderr, "[pcsx2-gs-bridge] render mode=full: field OFY canonicalization OFF [A/B override]\n");
        // PCSX2's own GameDB entry for SLUS-20002 carries two upscaling fixes
        // that only act above 1x ("halfPixelOffset: 2 # Fixes title screen and
        // some intro post-processing alignment", "roundSprite: 1 # Fixes ui
        // and hud alignment"). The bridge does not run PCSX2's GameDB loader,
        // so full mode applies them the way RR5's Metal profile applies the
        // other GameDB entries. RRV_PCSX2_GS_FULL_HPO=<0..5> and
        // RRV_PCSX2_GS_FULL_ROUND_SPRITE=<0..2> override them for the Gate-6
        // stock-setting matrix only.
        options.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Special;
        options.UserHacks_RoundSprite = 1;
        bool fixes_overridden = false;
        // Gate-6 owner review (2026-09-26): with RoundSprite rounding both
        // axes, HUD/menu glyphs lose alternate rows. RR5 fonts are full-height
        // textures drawn into half-height field sprites, each field sampling
        // alternate texel rows; rounding V to the native field texel grid keeps
        // only one field's rows in every progressive frame. So vertically
        // minified sprites (the fonts) round V on the progressive grid (twice
        // the field rows; skipping it bled one texel row at 4x); U is always rounded (a
        // stray column follows glyph strings without it) and 1:1 sprites keep
        // V rounding (without it the post-process page strips show seam rows).
        // RRV_GS_FULL_ROUND_SPRITE_U_ONLY=0 restores stock both-axis rounding (A/B).
        bool round_sprite_u_only = true;
        if (const char* value = std::getenv("RRV_GS_FULL_ROUND_SPRITE_U_ONLY"); value && value[0] != '\0')
        {
            if ((value[0] != '0' && value[0] != '1') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_GS_FULL_ROUND_SPRITE_U_ONLY=%s (expected 0 or 1)", value);
                return nullptr;
            }
            round_sprite_u_only = value[0] == '1';
            fixes_overridden = true;
        }
        GSRrvSetRoundSpriteUOnly(round_sprite_u_only);
        // Gate-6 owner review round 2: far rival cars turned red-brown at 2x.
        // Measured (race field 4559 draw dumps, 1x vs 2x): 197 of 201 red
        // pixels come from car env-map triangles with degenerate Q whose ST
        // the large-ST rewrite replaced; those triangles add nothing (black
        // texel) at 1x but sample red at 2x. Full mode therefore drops the
        // triangles it had to rewrite (mode 2). Race captures move closer to
        // field mode in every changed field. RRV_PCSX2_GS_FULL_LARGE_ST=0 (no
        // rewrite) / 1 (rewrite only) / 2 (default) for A/B.
        unsigned large_st = options.UserHacks_RewriteLargeST ? 2u : 0u;
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_LARGE_ST"); value && value[0] != '\0')
        {
            if ((value[0] < '0' || value[0] > '2') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_LARGE_ST=%s (expected 0..2)", value);
                return nullptr;
            }
            large_st = static_cast<unsigned>(value[0] - '0');
            fixes_overridden = true;
        }
        options.UserHacks_RewriteLargeST = large_st != 0u;
        GSRrvSetDropLargeSTTriangles(large_st == 2u);
        std::fprintf(stderr, "[pcsx2-gs-bridge] render mode=full: large-ST mode=%u\n", large_st);
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_HPO"); value && value[0] != '\0')
        {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' ||
                parsed >= static_cast<unsigned long>(GSHalfPixelOffset::MaxCount))
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_HPO=%s (expected 0..%u)", value,
                         static_cast<unsigned>(GSHalfPixelOffset::MaxCount) - 1u);
                return nullptr;
            }
            options.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(parsed);
            fixes_overridden = true;
        }
        if (const char* value = std::getenv("RRV_PCSX2_GS_FULL_ROUND_SPRITE"); value && value[0] != '\0')
        {
            if ((value[0] < '0' || value[0] > '2') || value[1] != '\0')
            {
                SetError(error, error_capacity, "invalid RRV_PCSX2_GS_FULL_ROUND_SPRITE=%s (expected 0..2)", value);
                return nullptr;
            }
            options.UserHacks_RoundSprite = static_cast<s8>(value[0] - '0');
            fixes_overridden = true;
        }
        // Further stock PCSX2 upscaling fixes, default off (PCSX2 defaults),
        // selectable for the Gate-6 matrix only.
        auto small_override = [&](const char* name, unsigned max_value, unsigned& out) -> bool {
            const char* value = std::getenv(name);
            if (!value || value[0] == '\0')
                return true;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > max_value)
            {
                SetError(error, error_capacity, "invalid %s=%s (expected 0..%u)", name, value, max_value);
                return false;
            }
            out = static_cast<unsigned>(parsed);
            fixes_overridden = true;
            return true;
        };
        unsigned native_scaling = static_cast<unsigned>(options.UserHacks_NativeScaling);
        unsigned align_sprite = options.UserHacks_AlignSpriteX ? 1u : 0u;
        unsigned merge_sprite = options.UserHacks_MergePPSprite ? 1u : 0u;
        unsigned bilinear = static_cast<unsigned>(options.UserHacks_BilinearHack);
        if (!small_override("RRV_PCSX2_GS_FULL_NATIVE_SCALING",
                            static_cast<unsigned>(GSNativeScaling::MaxCount) - 1u, native_scaling) ||
            !small_override("RRV_PCSX2_GS_FULL_ALIGN_SPRITE", 1u, align_sprite) ||
            !small_override("RRV_PCSX2_GS_FULL_MERGE_SPRITE", 1u, merge_sprite) ||
            !small_override("RRV_PCSX2_GS_FULL_BILINEAR",
                            static_cast<unsigned>(GSBilinearDirtyMode::MaxCount) - 1u, bilinear))
            return nullptr;
        options.UserHacks_NativeScaling = static_cast<GSNativeScaling>(native_scaling);
        options.UserHacks_AlignSpriteX = align_sprite != 0u;
        options.UserHacks_MergePPSprite = merge_sprite != 0u;
        options.UserHacks_BilinearHack = static_cast<GSBilinearDirtyMode>(bilinear);
        std::fprintf(stderr,
                     "[pcsx2-gs-bridge] render mode=full: NativeScaling=%u AlignSprite=%u MergeSprite=%u "
                     "BilinearHack=%u\n", native_scaling, align_sprite, merge_sprite, bilinear);
        std::fprintf(stderr,
                     "[pcsx2-gs-bridge] render mode=full: UpscaleMultiplier=%g InterlaceMode=Off "
                     "field OFY 0x7908->0x7900 HalfPixelOffset=%u RoundSprite=%d%s%s\n",
                     static_cast<double>(options.UpscaleMultiplier),
                     static_cast<unsigned>(options.UserHacks_HalfPixelOffset),
                     static_cast<int>(options.UserHacks_RoundSprite),
                     round_sprite_u_only ? " (minified sprites: V on progressive grid)" : " (U and V)",
                     fixes_overridden ? " [matrix override]" : "");
    }
    else
    {
        GSRrvSetFieldOfyCanonical(0u, 0u);
        GSRrvSetRoundSpriteUOnly(false);
        GSRrvSetDropLargeSTTriangles(false);
    }
    // RRV_PCSX2_GS_INTERLACE=<n> selects Pcsx2Config::GSOptions::InterlaceMode
    // (0=Automatic .. 9=AdaptiveBFF, see PCSX2 Config.h GSInterlaceMode).
    // Default is unchanged (PCSX2's own Automatic) so this is observation-only
    // until a value is proven. RR5 renders a complete display list per field
    // into a HALF-HEIGHT 640x224 buffer with SMODE2 INT=1 FFMD=1, and the live
    // output is currently two different 224-line images stacked into the 448-line
    // scan-out, so the deinterlace/field-composition policy is a prime suspect.
    if (const char* interlace = std::getenv("RRV_PCSX2_GS_INTERLACE");
        interlace && interlace[0] != '\0')
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(interlace, &end, 10);
        if (end != interlace && *end == '\0' &&
            parsed < static_cast<unsigned long>(GSInterlaceMode::Count))
        {
            options.InterlaceMode = static_cast<GSInterlaceMode>(parsed);
            std::fprintf(stderr, "[pcsx2-gs-bridge] InterlaceMode=%lu\n", parsed);
        }
        else
        {
            std::fprintf(stderr,
                         "[pcsx2-gs-bridge] ignoring invalid RRV_PCSX2_GS_INTERLACE=%s\n",
                         interlace);
        }
    }
    // RRV_PCSX2_GS_DRAWDUMP=<dir> mirrors `pcsx2-gsrunner -dump i`: PCSX2's own
    // per-draw context dump, written from inside GSState::DumpDrawInfo and keyed
    // on the global draw counter s_n. The point is not the dump itself -- it is
    // that the direct-call consumer and the MTGS consumer then carry the SAME
    // instrumentation at the SAME code point, so a draw-level diff between them
    // is a measurement rather than an inference. Default off; observation only.
    // RRV_PCSX2_GS_DRAWDUMP_FRAMES=<start>[,<count>] narrows the frame range
    // (PCSX2 defaults to every frame, which is far too much data here).
    if (const char* dump_dir = std::getenv("RRV_PCSX2_GS_DRAWDUMP");
        dump_dir && dump_dir[0] != '\0')
    {
        options.DumpGSData = true;
        options.SaveInfo = true;
        options.SWDumpDirectory = dump_dir;
        options.HWDumpDirectory = dump_dir;
        options.SaveFrameStart = 0;
        options.SaveFrameCount = -1;
        options.SaveFrameBy = 1;
        // PCSX2 also caps dumps at the first 5000 draws of the run (s_n is
        // global); the frame range is the only limit here.
        options.SaveDrawCount = -1;
        // RRV_PCSX2_GS_DRAWDUMP_TEX=1 also writes each draw's source texture
        // and render target images (large; use a narrow frame range).
        if (const char* tex = std::getenv("RRV_PCSX2_GS_DRAWDUMP_TEX"); tex && std::strcmp(tex, "1") == 0)
        {
            options.SaveTexture = true;
            options.SaveRT = true;
        }
        if (const char* range = std::getenv("RRV_PCSX2_GS_DRAWDUMP_FRAMES");
            range && range[0] != '\0')
        {
            char* end = nullptr;
            const long start = std::strtol(range, &end, 10);
            options.SaveFrameStart = static_cast<int>(start);
            if (end && *end == ',')
                options.SaveFrameCount = static_cast<int>(std::strtol(end + 1, nullptr, 10));
        }
        std::fprintf(stderr,
                     "[pcsx2-gs-bridge] draw dump -> %s (frames %d count %d)\n",
                     dump_dir, options.SaveFrameStart, options.SaveFrameCount);
    }
    if (config)
    {
        if (config->sw_threads > UINT16_MAX)
        {
            SetError(error, error_capacity, "sw_threads %u exceeds PCSX2's uint16 range", config->sw_threads);
            return nullptr;
        }
        // The caller owns this policy: the software rasterizer's worker count is
        // the single biggest lever on how much of a frame the SUBMITTING thread
        // pays for (measured: ~9 ms -> ~3.7 ms per guest frame on the 3D attract
        // phases). 0 means "rasterize on the submitting thread", which is the
        // single-threaded rollback, NOT "use PCSX2's default".
        options.SWExtraThreads = static_cast<uint16_t>(config->sw_threads);
        std::fprintf(stderr, "[pcsx2-gs-bridge] SWExtraThreads=%u\n",
                     static_cast<unsigned>(options.SWExtraThreads));
    }

    if (direct_requested)
    {
#if defined(__APPLE__)
        const bool owner_surface =
            (config->surface.flags & RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD) != 0u;
        if (!owner_surface && !rrv_pcsx2_gs_bridge_is_macos_main_thread())
        {
            SetError(error, error_capacity,
                     "direct presentation bridge creation must run on AppKit's main thread");
            return nullptr;
        }
        if (owner_surface && !rrv_pcsx2_gs_bridge_consume_owner_surface(
                &config->surface, error, error_capacity))
            return nullptr;
        if ((!owner_surface && !rrv_pcsx2_gs_bridge_validate_macos_surface(config->surface.native_view,
                                                          config->surface.native_layer,
                                                          error, error_capacity)) ||
            !rrv_pcsx2_gs_bridge_configure_native_surface(
                config->surface.kind, config->surface.flags, config->surface.native_view,
                config->surface.native_layer, config->surface.width_pixels,
                config->surface.height_pixels, config->surface.backing_scale,
                error, error_capacity))
        {
            return nullptr;
        }
        bridge->owner_thread_surface = owner_surface;
        GSRrvResetDirectPresentCount();
#else
        // Linux: no AppKit-style main-thread rule. The owner sideband is still
        // honoured so a dedicated-owner caller uses the same protocol on both
        // platforms (prepare on the SDL thread, consume here on the owner).
        const bool owner_surface =
            (config->surface.flags & RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD) != 0u;
        if (owner_surface && !rrv_pcsx2_gs_bridge_consume_owner_surface(
                &config->surface, error, error_capacity))
            return nullptr;
        if (!rrv_pcsx2_gs_bridge_validate_linux_surface(&config->surface, error, error_capacity) ||
            !rrv_pcsx2_gs_bridge_configure_native_surface(
                config->surface.kind, config->surface.flags, config->surface.native_view,
                config->surface.native_layer, config->surface.width_pixels,
                config->surface.height_pixels, config->surface.backing_scale,
                error, error_capacity))
        {
            return nullptr;
        }
        bridge->owner_thread_surface = owner_surface;
        GSRrvResetDirectPresentCount();
#endif
    }

    // RRV_PCSX2_GS_VSYNC=1 (the windowed, real-time product launcher sets it):
    // present on the display's vertical sync (Metal displaySyncEnabled +
    // presentDrawable; Vulkan VK_PRESENT_MODE_FIFO_KHR), so a frame never
    // replaces another mid-scan (no tearing). Host presentation only; guest
    // time is unaffected. Off by default and never with a detached (headless)
    // surface. The Metal-only atTime present pacer (pcsx2-gs-bridge-target
    // .patch, RrvPresentPacer) has no Vulkan equivalent: core Vulkan has no
    // present-at-time, so on Linux RRV_PCSX2_GS_PRESENT_* are ignored and
    // FIFO alone paces presentation.
    const bool vsync = direct_requested && EnvFlag("RRV_PCSX2_GS_VSYNC", false);
    // RRV_GATE3_FAST_FORWARD=N (replay debugging): the runtime runs the first N
    // starts unpaced, so vsync would throttle them to the display rate; present
    // without it and switch to FIFO at field N.
    uint64_t vsync_from_field = 0;
    if (vsync)
        if (const char* ff = std::getenv("RRV_GATE3_FAST_FORWARD"); ff && *ff)
            vsync_from_field = std::strtoull(ff, nullptr, 10);
    options.VsyncEnable = vsync && !vsync_from_field;
    if (vsync && vsync_from_field)
        std::fprintf(stderr, "[pcsx2-gs-bridge] presentation vsync=FIFO from field %llu (fast-forward)\n",
                     static_cast<unsigned long long>(vsync_from_field));
    else if (vsync)
        std::fprintf(stderr, "[pcsx2-gs-bridge] presentation vsync=FIFO (display sync)\n");
    // Gate 7 aspect/UI: how the displayed image is fitted into the window.
    // RRV_PCSX2_GS_ASPECT=auto|4:3|16:9|16:10|21:9|stretch (default auto =
    // PCSX2's Auto 4:3/3:2, i.e. 4:3 for RR5's interlaced NTSC output). 16:10
    // and 21:9 use Auto with PCSX2's custom ratio (the slot patches use).
    // Wider ratios widen the same picture unless the game renders wider too
    // (RR5 enhancement RRV_RR5_WIDESCREEN).
    // PCSX2's CalculateDrawDstRect reads EmuConfig.CurrentAspectRatio, not
    // GSConfig, so both are set. RRV_PCSX2_GS_INTEGER_SCALING=1 snaps the
    // image to whole multiples of the output size. Presentation only: the
    // headless snapshot path takes the internal-resolution texture (window
    // 0x0) and never runs this fit.
    float custom_aspect = 0.0f;
    if (const char* aspect = std::getenv("RRV_PCSX2_GS_ASPECT"); aspect && aspect[0] != '\0')
    {
        const std::string_view value(aspect);
        if (value == "auto")
            options.AspectRatio = AspectRatioType::RAuto4_3_3_2;
        else if (value == "4:3")
            options.AspectRatio = AspectRatioType::R4_3;
        else if (value == "16:9")
            options.AspectRatio = AspectRatioType::R16_9;
        else if (value == "16:10" || value == "21:9")
        {
            options.AspectRatio = AspectRatioType::RAuto4_3_3_2;
            custom_aspect = value == "16:10" ? 16.0f / 10.0f : 21.0f / 9.0f;
        }
        else if (value == "stretch")
            options.AspectRatio = AspectRatioType::Stretch;
        else
        {
            SetError(error, error_capacity, "RRV_PCSX2_GS_ASPECT must be auto, 4:3, 16:9, 16:10, 21:9 or stretch, got %s",
                     aspect);
            return nullptr;
        }
    }
    options.IntegerScaling = EnvFlag("RRV_PCSX2_GS_INTEGER_SCALING", false);
    // Optional presentation post-processing, both off by default (owner
    // decision 2026-09-29). RRV_PCSX2_GS_FXAA=1 runs PCSX2's FXAA pass on the
    // merged frame (GSRenderer::Merge -> GSDevice::FXAA). RRV_PCSX2_GS_CAS=
    // 1..100 runs AMD FidelityFX CAS at that sharpness (GSRenderer::VSync ->
    // GSDevice::CAS); PCSX2 sharpens only when the internal resolution exceeds
    // the window and sharpens+resizes when it is smaller. 0 = off.
    // Direct presentation only: FXAA runs inside Merge(), so enabling it on
    // the headless path would change the merged snapshot/readback image that
    // captures and oracle comparisons read.
    options.FXAA = false;
    options.CASMode = GSCASMode::Disabled;
    // RRV_PCSX2_GS_ANISO=0|2|4|8|16: PCSX2's shader anisotropic filtering
    // (ps.sw_aniso, triangles with automatic LOD only). Off by default (owner
    // decision 2026-09-29). A host enhancement: it changes rendered pixels,
    // including headless captures, so it is never set for oracle work.
    options.MaxAnisotropy = 0;
    if (const char* aniso = std::getenv("RRV_PCSX2_GS_ANISO"); aniso && aniso[0] != '\0')
    {
        const std::string_view value(aniso);
        if (value != "0" && value != "2" && value != "4" && value != "8" && value != "16")
        {
            SetError(error, error_capacity, "RRV_PCSX2_GS_ANISO must be 0, 2, 4, 8 or 16, got %s", aniso);
            return nullptr;
        }
        options.MaxAnisotropy = static_cast<u8>(std::atoi(aniso));
        std::fprintf(stderr, "[pcsx2-gs-bridge] anisotropic filtering=%ux\n",
                     static_cast<unsigned>(options.MaxAnisotropy));
    }
    if (direct_requested)
    {
        options.FXAA = EnvFlag("RRV_PCSX2_GS_FXAA", false);
        if (const char* cas = std::getenv("RRV_PCSX2_GS_CAS"); cas && cas[0] != '\0')
        {
            char* end = nullptr;
            const long sharpness = std::strtol(cas, &end, 10);
            if (end == cas || *end != '\0' || sharpness < 0 || sharpness > 100)
            {
                SetError(error, error_capacity, "RRV_PCSX2_GS_CAS must be 0 to 100, got %s", cas);
                return nullptr;
            }
            if (sharpness > 0)
            {
                options.CASMode = GSCASMode::SharpenAndResize;
                options.CAS_Sharpness = static_cast<u8>(sharpness);
            }
        }
        std::fprintf(stderr, "[pcsx2-gs-bridge] presentation fxaa=%u cas=%u\n", options.FXAA ? 1u : 0u,
                     options.CASMode == GSCASMode::Disabled ? 0u : static_cast<unsigned>(options.CAS_Sharpness));
    }
    EmuConfig.GS.AspectRatio = options.AspectRatio;
    EmuConfig.CurrentAspectRatio = options.AspectRatio;
    EmuConfig.CurrentCustomAspectRatio = custom_aspect;
    if (direct_requested)
        std::fprintf(stderr, "[pcsx2-gs-bridge] presentation aspect=%s custom=%.4f integer-scaling=%u\n",
                     Pcsx2Config::GSOptions::AspectRatioNames[static_cast<size_t>(options.AspectRatio)],
                     static_cast<double>(custom_aspect), options.IntegerScaling ? 1u : 0u);
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (!GSopen(options, renderer, bridge->pcsx2_regs.data(),
                options.VsyncEnable ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false))
    {
        if (direct_requested)
            rrv_pcsx2_gs_bridge_clear_native_surface();
        SetError(error, error_capacity, "PCSX2 GSopen(%s) failed; see PCSX2 console output", renderer_name);
        return nullptr;
    }

    // GSopen may resolve Auto or select a fallback. Report the instantiated
    // renderer from PCSX2's live GS state, after open, and overwrite the
    // pre-open request metadata returned through the ABI. This is the M0R
    // proof point: callers must never infer Metal execution from an option or
    // a library name.
    const GSRendererType actual_renderer = GSGetCurrentRenderer();
    const bool actual_hardware = GSIsHardwareRenderer();
    switch (actual_renderer)
    {
        case GSRendererType::Metal:
            capabilities->renderer_kind = RRV_PCSX2_GS_RENDERER_METAL;
            capabilities->renderer_name = "PCSX2 Metal";
            break;
        case GSRendererType::VK:
            capabilities->renderer_kind = RRV_PCSX2_GS_RENDERER_VULKAN;
            capabilities->renderer_name = "PCSX2 Vulkan";
            break;
        case GSRendererType::SW:
            capabilities->renderer_kind = RRV_PCSX2_GS_RENDERER_SOFTWARE;
            capabilities->renderer_name = "PCSX2 Software";
            break;
        default:
            capabilities->renderer_kind = RRV_PCSX2_GS_RENDERER_UNKNOWN;
            capabilities->renderer_name = "PCSX2 Other";
            break;
    }
    std::fprintf(stderr,
                 "[pcsx2-gs-bridge] instantiated renderer=%s hardware=%s backend=%s\n",
                 Pcsx2Config::GSOptions::GetRendererName(actual_renderer),
                 actual_hardware ? "yes" : "no",
                 actual_renderer == GSRendererType::Metal ? "Metal" :
                 actual_renderer == GSRendererType::VK ? "Vulkan" : "non-Metal");

    if (full_frame && !actual_hardware)
    {
        GSclose();
        if (direct_requested)
            rrv_pcsx2_gs_bridge_clear_native_surface();
        SetError(error, error_capacity,
                 "RRV_GS_RENDER_MODE=full requires a hardware renderer, but PCSX2 instantiated %s",
                 Pcsx2Config::GSOptions::GetRendererName(actual_renderer));
        return nullptr;
    }
    if (actual_hardware)
        capabilities->capability_flags |= RRV_PCSX2_GS_CAP_HARDWARE_GS;
    if (actual_renderer == GSRendererType::Metal)
        capabilities->capability_flags |= RRV_PCSX2_GS_CAP_METAL_BACKEND;
    if (actual_renderer == GSRendererType::VK)
        capabilities->capability_flags |= RRV_PCSX2_GS_CAP_VULKAN_BACKEND;
    if (direct_requested && actual_renderer != kPlatformGpuRenderer)
    {
        GSclose();
        rrv_pcsx2_gs_bridge_clear_native_surface();
        SetError(error, error_capacity,
                 "direct presentation requested but PCSX2 did not instantiate %s",
                 kPlatformGpuBackendName);
        return nullptr;
    }
    if (direct_requested)
        capabilities->capability_flags |= RRV_PCSX2_GS_CAP_DIRECT_PRESENT;

    bridge->full_frame = full_frame;
    // The platform GPU renderer (Metal or Vulkan) can expose PMODE before it
    // has a snapshotable target, and every CPU snapshot waits on the GPU.
    bridge->gpu_snapshot_fallback = (actual_renderer == kPlatformGpuRenderer);
    bridge->gpu_renderer = (actual_renderer == kPlatformGpuRenderer);
    bridge->direct_present = direct_requested;
    bridge->vsync_from_field = vsync_from_field;
    bridge->open = true;
    Gate4GsObserver().Configure();
    host_memory_guard.disarm();
    return bridge.release();
}

extern "C" void rrv_pcsx2_gs_bridge_destroy(RrvPcsx2GsBridge* bridge)
{
    if (!bridge)
        return;
    Gate4GsObserver().Write();
    if (bridge->full_frame)
        std::fprintf(stderr, "[pcsx2-gs-bridge] full-frame: field OFY canonicalized in %llu XYOFFSET writes\n",
                     static_cast<unsigned long long>(GSRrvGetFieldOfyCanonicalCount()));
    if (bridge->full_frame)
        std::fprintf(stderr, "[pcsx2-gs-bridge] full-frame: large-ST triangles dropped %llu\n",
                     static_cast<unsigned long long>(GSRrvGetDroppedLargeSTTriangles()));
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (bridge->open)
        GSclose();
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    std::string receipt_error;
    if (!bridge->canonical_receipt.dumpDeferred(&receipt_error))
        std::fprintf(stderr, "[pcsx2-gs-bridge] %s\n", receipt_error.c_str());
#endif
    if (bridge->direct_present)
        rrv_pcsx2_gs_bridge_clear_native_surface();
    delete bridge;
    // After GSclose(): the SW JIT's generated code lives in the code reserve.
    ReleaseHostMemory();
}

// Gate 7 HUD survey/transform: a GIF-stream walker that keeps per-path tag
// state across submits (a tag can span transfers) and visits every vertex X
// (XYZ2/XYZ3/XYZF2/XYZF3 in PACKED, A+D and REGLIST form) with the current
// PRIM. RRV_PCSX2_GS_HUDSTATS=<file> writes, per field and path, vertex counts
// and X ranges per primitive type. Observation only; off by default.
namespace gifwalk
{
struct PathState
{
    uint32_t nloop = 0, nreg = 0, reg = 0, flg = 0;
    uint64_t regs = 0;
    uint32_t prim = 0; // last PRIM register value (low 11 bits)
    bool in_tag = false;
    uint64_t tag_index = 0;
    uint8_t* uv = nullptr; // last UV register in the current buffer (U = low 14 bits, 10.4)
};

// Visitor: (path, prim, ptr, kind, tag_index) where ptr points at the vertex
// data: kind 0 = PACKED XYZ2/XYZ3 qword, 1 = PACKED XYZF2/XYZF3 qword,
// 2 = 64-bit register (A+D data or REGLIST) XYZ2/XYZ3, 3 = 64-bit XYZF2/XYZF3.
// X is always the low 16 bits (12.4 fixed point, GS window coordinates).
struct NoRegVisit
{
    void operator()(uint32_t, uint32_t, uint8_t*) const {}
};

// RegVisit: (path, register address, ptr to the 64-bit data) for A+D writes
// and REGLIST registers 0x6..0xD are not reported (only A+D), which is where
// RR5 sets SCISSOR/XYOFFSET.
template <typename Visit, typename RegVisit = NoRegVisit>
void Walk(PathState& st, uint32_t path, uint8_t* data, uint32_t qwc, Visit&& visit, RegVisit&& reg_visit = RegVisit{})
{
    uint32_t q = 0;
    st.uv = nullptr;
    while (q < qwc)
    {
        uint8_t* qw = data + q * 16u;
        if (!st.in_tag)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, qw, 8);
            std::memcpy(&hi, qw + 8, 8);
            st.nloop = static_cast<uint32_t>(lo & 0x7fffu);
            const bool pre = ((lo >> 46) & 1u) != 0;
            if (pre)
                st.prim = static_cast<uint32_t>((lo >> 47) & 0x7ffu);
            st.flg = static_cast<uint32_t>((lo >> 58) & 3u);
            st.nreg = static_cast<uint32_t>((lo >> 60) & 0xfu);
            if (st.nreg == 0)
                st.nreg = 16;
            st.regs = hi;
            st.reg = 0;
            st.in_tag = st.nloop != 0;
            ++st.tag_index;
            ++q;
            continue;
        }
        if (st.flg >= 2) // IMAGE: nloop qwords of data
        {
            const uint32_t take = std::min(st.nloop, qwc - q);
            q += take;
            st.nloop -= take;
            st.in_tag = st.nloop != 0;
            continue;
        }
        if (st.flg == 0) // PACKED: one qword per register
        {
            const uint32_t r = static_cast<uint32_t>((st.regs >> (4u * st.reg)) & 0xfu);
            uint64_t lo;
            std::memcpy(&lo, qw, 8);
            if (r == 0x0)
                st.prim = static_cast<uint32_t>(lo & 0x7ffu);
            else if (r == 0x3)
                st.uv = qw;
            else if (r == 0x4 || r == 0x5 || r == 0xc || r == 0xd)
                visit(path, st.prim, qw, (r == 0x4 || r == 0xc) ? 1u : 0u, st.tag_index, st.uv);
            else if (r == 0xe)
            {
                uint64_t addr;
                std::memcpy(&addr, qw + 8, 8);
                addr &= 0xffu;
                if (addr == 0x00)
                    st.prim = static_cast<uint32_t>(lo & 0x7ffu);
                else if (addr == 0x03)
                    st.uv = qw;
                else if (addr == 0x04 || addr == 0x05 || addr == 0x0c || addr == 0x0d)
                    visit(path, st.prim, qw, (addr == 0x04 || addr == 0x0c) ? 3u : 2u, st.tag_index, st.uv);
                else
                    reg_visit(path, static_cast<uint32_t>(addr), qw);
            }
            if (++st.reg == st.nreg)
            {
                st.reg = 0;
                if (--st.nloop == 0)
                    st.in_tag = false;
            }
            ++q;
            continue;
        }
        // REGLIST: two 64-bit registers per qword.
        for (uint32_t half = 0; half < 2u && st.in_tag; ++half)
        {
            const uint32_t r = static_cast<uint32_t>((st.regs >> (4u * st.reg)) & 0xfu);
            uint8_t* d = qw + half * 8u;
            uint64_t v;
            std::memcpy(&v, d, 8);
            if (r == 0x0)
                st.prim = static_cast<uint32_t>(v & 0x7ffu);
            else if (r == 0x3)
                st.uv = d;
            else if (r == 0x4 || r == 0x5 || r == 0xc || r == 0xd)
                visit(path, st.prim, d, (r == 0x4 || r == 0xc) ? 3u : 2u, st.tag_index, st.uv);
            if (++st.reg == st.nreg)
            {
                st.reg = 0;
                if (--st.nloop == 0)
                    st.in_tag = false;
            }
        }
        ++q;
    }
}
} // namespace gifwalk

struct HudStatsV1
{
    bool enabled = false;
    std::FILE* out = nullptr;
    gifwalk::PathState state[4];
    // [path][prim type 0..7]: count, min X, max X (pixels relative to 2048), textured count
    uint32_t count[4][8] = {};
    int32_t minx[4][8], maxx[4][8];
    uint32_t tme[4][8] = {};
    uint64_t field = 0;
    int64_t dump_field = -1; // RRV_PCSX2_GS_HUDSTATS_FIELD: per-tag records for this field
    struct Tag { uint32_t path, prim, n; int32_t x0, x1, y0, y1; uint32_t z0, z1; uint64_t id; uint32_t reg; uint64_t val; };
    std::vector<Tag> tags;
    HudStatsV1() { Reset(); }
    void Reset()
    {
        for (auto& p : count) for (auto& c : p) c = 0;
        for (auto& p : tme) for (auto& c : p) c = 0;
        for (auto& p : minx) for (auto& c : p) c = INT32_MAX;
        for (auto& p : maxx) for (auto& c : p) c = INT32_MIN;
    }
};

HudStatsV1& HudStats()
{
    static HudStatsV1 s = [] {
        HudStatsV1 h;
        if (const char* path = std::getenv("RRV_PCSX2_GS_HUDSTATS"); path && path[0])
        {
            h.out = std::fopen(path, "w");
            h.enabled = h.out != nullptr;
            if (h.out)
                std::fprintf(h.out, "# field path prim count textured minx maxx (pixels from GS x=2048)\n");
            if (const char* f = std::getenv("RRV_PCSX2_GS_HUDSTATS_FIELD"); f && f[0])
                h.dump_field = std::strtoll(f, nullptr, 10);
        }
        return h;
    }();
    return s;
}

void HudStatsObserve(uint32_t path, const uint8_t* bytes, uint32_t size_bytes)
{
    HudStatsV1& h = HudStats();
    if (!h.enabled || size_bytes == 0u)
        return;
    gifwalk::Walk(h.state[path], path, const_cast<uint8_t*>(bytes), size_bytes / 16u,
                  [&](uint32_t p, uint32_t prim, const uint8_t* x, uint32_t kind, uint64_t tag, uint8_t*) {
                      uint16_t raw;
                      std::memcpy(&raw, x, 2);
                      if (static_cast<int64_t>(h.field) == h.dump_field)
                      {
                          uint16_t ry;
                          uint32_t z;
                          if (kind <= 1u)
                          {
                              std::memcpy(&ry, x + 4, 2);
                              uint32_t zz;
                              std::memcpy(&zz, x + 8, 4);
                              z = kind == 0u ? zz : ((zz >> 4) & 0xffffffu);
                          }
                          else
                          {
                              std::memcpy(&ry, x + 2, 2);
                              uint32_t zz;
                              std::memcpy(&zz, x + 4, 4);
                              z = kind == 2u ? zz : (zz & 0xffffffu);
                          }
                          const int32_t px = static_cast<int32_t>(raw >> 4) - 2048;
                          const int32_t py = static_cast<int32_t>(ry >> 4) - 2048;
                          if (h.tags.empty() || h.tags.back().id != tag || h.tags.back().path != p ||
                              h.tags.back().prim != prim)
                              h.tags.push_back({p, prim, 0, px, px, py, py, z, z, tag, 0, 0});
                          auto& t = h.tags.back();
                          ++t.n;
                          t.x0 = std::min(t.x0, px); t.x1 = std::max(t.x1, px);
                          t.y0 = std::min(t.y0, py); t.y1 = std::max(t.y1, py);
                          t.z0 = std::min(t.z0, z); t.z1 = std::max(t.z1, z);
                      }
                      const uint32_t type = prim & 7u;
                      const int32_t px = static_cast<int32_t>(raw >> 4) - 2048;
                      ++h.count[p][type];
                      h.tme[p][type] += (prim >> 4) & 1u;
                      h.minx[p][type] = std::min(h.minx[p][type], px);
                      h.maxx[p][type] = std::max(h.maxx[p][type], px);
                  },
                  [&](uint32_t p, uint32_t reg, uint8_t* d) {
                      if (static_cast<int64_t>(h.field) != h.dump_field)
                          return;
                      if (reg == 0x40 || reg == 0x41 || reg == 0x18 || reg == 0x19 || reg == 0x4c || reg == 0x4d)
                      {
                          uint64_t v;
                          std::memcpy(&v, d, 8);
                          h.tags.push_back({p, 0, 0, 0, 0, 0, 0, 0, 0, ~0ull, reg, v});
                      }
                  });
}

void HudStatsFieldEnd()
{
    HudStatsV1& h = HudStats();
    if (!h.enabled)
        return;
    for (uint32_t p = 1; p < 4u; ++p)
        for (uint32_t t = 0; t < 8u; ++t)
            if (h.count[p][t])
                std::fprintf(h.out, "%llu %u %u %u %u %d %d\n", static_cast<unsigned long long>(h.field), p, t,
                             h.count[p][t], h.tme[p][t], h.minx[p][t], h.maxx[p][t]);
    for (const auto& t : h.tags)
        if (t.id == ~0ull)
            std::fprintf(h.out, "R path=%u reg=0x%02x val=0x%016llx lo=%llu hi=%llu\n", t.path, t.reg,
                         static_cast<unsigned long long>(t.val),
                         static_cast<unsigned long long>(t.val & 0x7ff),
                         static_cast<unsigned long long>((t.val >> 16) & 0x7ff));
        else
        std::fprintf(h.out, "T %llu path=%u prim=0x%03x type=%u tme=%u fst=%u abe=%u n=%u x=%d..%d y=%d..%d z=%u..%u\n",
                     static_cast<unsigned long long>(t.id), t.path, t.prim, t.prim & 7u, (t.prim >> 4) & 1u,
                     (t.prim >> 8) & 1u, (t.prim >> 6) & 1u, t.n, t.x0, t.x1, t.y0, t.y1, t.z0, t.z1);
    h.tags.clear();
    std::fflush(h.out);
    h.Reset();
    ++h.field;
}

// Gate 7 HUD un-stretch (RR5 widescreen companion). RRV_PCSX2_GS_HUD_SCALE=<f>
// (0 < f < 1; the launcher passes (4/3)/ratio with --ratio) gives every 2D
// element back its 4:3 proportions and pins it to its screen edge. RR5 draws
// all of its 2D (HUD sprites 0x356, panels, minimap fans/lines, rev needle
// strips, menu text) at the far depth plane, Z 0xFFFFF5..0xFFFFFF, through
// PATH3 (measured 2026-09-27 on race field 4700 and car select 2600); 3D
// arrives with smaller Z, and the few far-plane 3D strips come through PATH1,
// which is left alone. A vertex is 2D when Z >= 0xFFFE28 (RRV_PCSX2_GS_HUD_ZMIN
// overrides) and |X - 2048| <= 400 px. The 2D vertices of each GIF tag form
// a piece. Pieces that touch (x gap <= 24 px, y gap <= 8 px) form an element,
// so a word, a card or the rev dial moves as one; RR5 sends many letters as
// separate transfers, so elements are clustered over a whole field at VSync
// and applied to the next field (a piece takes the anchor of the previous
// field's element it overlaps most; a piece with no match is grouped within
// its own transfer). Each element keeps its size ratio and is anchored by
// its centre cx (px from the screen centre): |cx| < 107 stays centred
// (x' = x f), otherwise it keeps its distance to the nearer edge
// (x' = +-320 + (x -+ 320) f). An element that spans the whole width
// (full-screen covers, fades, backgrounds) is left alone. RRV_PCSX2_GS_HUD_MODE=race
// applies all of this only while a race HUD is on screen: a recent field drew
// the rev-counter needle, the far-plane AA1 strip 0x2cc in the bottom-right
// quarter (measured at (222,66) in races; menus draw AA1 0x284 anywhere and
// 0x2cc near the centre); menus then stay stretched. RRV_PCSX2_GS_HUD_ANCHOR=center puts everything in a centred 4:3 box
// instead. Host-side and presentation-only: guest data is unchanged. Off
// unless set.
struct HudScaleV1
{
    float factor = 0.0f;
    uint32_t zmin = 0xFFFE28u;
    bool edges = true;
    bool race_only = false;   // RRV_PCSX2_GS_HUD_MODE=race
    bool race_seen = false;   // this field drew the AA1 rev needle at the far plane
    bool race_active = false; // a recent field did (held for 60 fields)
    uint32_t race_quiet = 0;  // fields since the needle was last drawn
    uint64_t fields = 0;
    uint32_t race_prim = 0;
    int32_t race_x = 0, race_y = 0;
    gifwalk::PathState state[4];
    std::vector<uint8_t> scratch;
    struct Piece { uint64_t tag; uint32_t prim; int32_t x0, x1, y0, y1; uint32_t first, count; };
    std::vector<Piece> pieces;
    struct Box { int32_t x0, x1, y0, y1; };
    struct Element { Box box; int32_t anchor; bool full_width; uint32_t pieces; bool pin; };
    std::vector<Box> field_pieces;      // this field's pieces (original coordinates)
    std::vector<Element> prev_elements; // clustered at the previous VSync
    std::vector<uint8_t*> verts;
    std::vector<uint8_t*> uvs;
};

HudScaleV1& HudScale()
{
    static HudScaleV1 h = [] {
        HudScaleV1 v;
        if (const char* f = std::getenv("RRV_PCSX2_GS_HUD_SCALE"); f && f[0])
        {
            const float parsed = std::strtof(f, nullptr);
            if (parsed > 0.0f && parsed < 1.0f)
                v.factor = parsed;
        }
        if (const char* z = std::getenv("RRV_PCSX2_GS_HUD_ZMIN"); z && z[0])
            v.zmin = static_cast<uint32_t>(std::strtoul(z, nullptr, 0));
        if (const char* m = std::getenv("RRV_PCSX2_GS_HUD_ANCHOR"); m && std::strcmp(m, "center") == 0)
            v.edges = false;
        if (const char* m = std::getenv("RRV_PCSX2_GS_HUD_MODE"); m && std::strcmp(m, "race") == 0)
            v.race_only = true;
        if (v.factor > 0.0f)
            std::fprintf(stderr, "[pcsx2-gs-bridge] HUD un-stretch: far-plane PATH2/3 2D x%.4f, anchored to %s, %s "
                                 "(Z >= 0x%X)\n", static_cast<double>(v.factor), v.edges ? "edges" : "centre",
                         v.race_only ? "races only" : "always", v.zmin);
        return v;
    }();
    return h;
}

// VSync: cluster this field's 2D pieces into elements for the next field.
void HudScaleFieldEnd()
{
    HudScaleV1& h = HudScale();
    if (h.factor <= 0.0f)
        return;
    // Race HUD turns on with the first needle and off after 60 fields without
    // it, so short gaps (fades, the start sequence) do not make the HUD jump.
    h.race_quiet = h.race_seen ? 0u : h.race_quiet + 1u;
    const bool active = h.race_seen || (h.race_active && h.race_quiet < 60u);
    if (h.race_only && active != h.race_active)
        std::fprintf(stderr, "[pcsx2-gs-bridge] HUD un-stretch: race HUD %s at field %llu (AA1 prim 0x%03x at %d,%d)\n",
                     active ? "on" : "off", static_cast<unsigned long long>(h.fields), h.race_prim, h.race_x, h.race_y);
    ++h.fields;
    h.race_active = active;
    h.race_seen = false;
    constexpr int32_t kGapX = 24 << 4, kGapY = 8 << 4, kEdge = 312 << 4, kHalf = 320 << 4, kMiddle = 107 << 4;
    const size_t n = h.field_pieces.size();
    std::vector<uint32_t> parent(n);
    for (size_t k = 0; k < n; ++k)
        parent[k] = static_cast<uint32_t>(k);
    const auto find = [&](uint32_t k) {
        while (parent[k] != k)
            k = parent[k] = parent[parent[k]];
        return k;
    };
    // A full-width piece (menu bar, background line) is its own element and
    // never links others: measured on the team-colour menu, such bars chained
    // the whole screen into one full-width element that was then left alone.
    const auto wide = [&](const HudScaleV1::Box& b) { return b.x0 <= -kEdge && b.x1 >= kEdge; };
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
        {
            const auto& p = h.field_pieces[a];
            const auto& q = h.field_pieces[b];
            if (wide(p) || wide(q))
                continue;
            if (q.x0 - p.x1 <= kGapX && p.x0 - q.x1 <= kGapX && q.y0 - p.y1 <= kGapY && p.y0 - q.y1 <= kGapY)
                parent[find(static_cast<uint32_t>(a))] = find(static_cast<uint32_t>(b));
        }
    std::vector<int32_t> index(n, -1);
    h.prev_elements.clear();
    for (size_t k = 0; k < n; ++k)
    {
        const uint32_t r = find(static_cast<uint32_t>(k));
        const auto& p = h.field_pieces[k];
        if (index[r] < 0)
        {
            index[r] = static_cast<int32_t>(h.prev_elements.size());
            h.prev_elements.push_back({{p.x0, p.x1, p.y0, p.y1}, 0, false, 0, false});
        }
        ++h.prev_elements[static_cast<size_t>(index[r])].pieces;
        auto& b = h.prev_elements[static_cast<size_t>(index[r])].box;
        b.x0 = std::min(b.x0, p.x0); b.x1 = std::max(b.x1, p.x1);
        b.y0 = std::min(b.y0, p.y0); b.y1 = std::max(b.y1, p.y1);
    }
    for (auto& el : h.prev_elements)
    {
        // One full-width shape (cover, fade, background bar) stays as drawn; a
        // chain of pieces wider than the screen (the team-colour card
        // carousel) is one composition and keeps its proportions about the
        // centre.
        const bool spans = el.box.x0 <= -kEdge && el.box.x1 >= kEdge;
        el.full_width = spans && el.pieces == 1u;
        el.pin = spans;
        const int32_t cx = (el.box.x0 + el.box.x1) / 2;
        el.anchor = (spans || !h.edges || (cx > -kMiddle && cx < kMiddle)) ? 0 : (cx < 0 ? -kHalf : kHalf);
    }
    HudStatsV1& st = HudStats();
    if (st.enabled && static_cast<int64_t>(st.field) == st.dump_field)
        for (const auto& el : h.prev_elements)
            std::fprintf(st.out, "E x=%d..%d y=%d..%d anchor=%s\n", el.box.x0 >> 4, el.box.x1 >> 4, el.box.y0 >> 4,
                         el.box.y1 >> 4, el.full_width ? "none" : el.anchor == 0 ? "centre" : el.anchor < 0 ? "left" : "right");
    h.field_pieces.clear();
}

// Returns the bytes to submit: the original, or a transformed copy.
const uint8_t* HudScaleApply(uint32_t path, const uint8_t* bytes, uint32_t size_bytes)
{
    HudScaleV1& h = HudScale();
    if (h.factor <= 0.0f || size_bytes == 0u || path == 1u)
        return bytes;
    h.scratch.assign(bytes, bytes + size_bytes);
    h.pieces.clear();
    h.verts.clear();
    h.uvs.clear();
    constexpr int32_t kCentre = 2048 << 4;
    gifwalk::Walk(h.state[path], path, h.scratch.data(), size_bytes / 16u,
                  [&](uint32_t, uint32_t prim, uint8_t* v, uint32_t kind, uint64_t tag, uint8_t* uv) {
                      uint32_t zz, z;
                      uint16_t x, y;
                      std::memcpy(&x, v, 2);
                      if (kind <= 1u)
                      {
                          std::memcpy(&y, v + 4, 2);
                          std::memcpy(&zz, v + 8, 4);
                          z = kind == 0u ? zz : ((zz >> 4) & 0xffffffu);
                      }
                      else
                      {
                          std::memcpy(&y, v + 2, 2);
                          std::memcpy(&zz, v + 4, 4);
                          z = kind == 2u ? zz : (zz & 0xffffffu);
                      }
                      const int32_t rx = static_cast<int32_t>(x) - kCentre;
                      if (z < h.zmin || rx < -(400 << 4) || rx > (400 << 4))
                          return;
                      const int32_t ry = static_cast<int32_t>(y) - kCentre;
                      if (h.pieces.empty() || h.pieces.back().tag != tag || h.pieces.back().prim != prim)
                          h.pieces.push_back({tag, prim, rx, rx, ry, ry, static_cast<uint32_t>(h.verts.size()), 0});
                      auto& pc = h.pieces.back();
                      pc.x0 = std::min(pc.x0, rx); pc.x1 = std::max(pc.x1, rx);
                      pc.y0 = std::min(pc.y0, ry); pc.y1 = std::max(pc.y1, ry);
                      ++pc.count;
                      h.verts.push_back(v);
                      h.uvs.push_back(uv);
                      // The rev needle: AA1 alpha-blended untextured strip 0x2cc in
                      // the dial's quarter (measured at (222,66)); menus draw
                      // AA1 0x284 anywhere and 0x2cc near the centre.
                      if (prim == 0x2ccu && (rx >> 4) >= 100 && (ry >> 4) >= 0)
                      {
                          h.race_seen = true;
                          h.race_prim = prim;
                          h.race_x = rx >> 4;
                          h.race_y = ry >> 4;
                      }
                  });
    constexpr int32_t kGapX = 24 << 4, kGapY = 8 << 4, kEdge = 312 << 4, kHalf = 320 << 4, kMiddle = 107 << 4;
    const auto anchor_of = [&](int32_t x0, int32_t x1) {
        const int32_t cx = (x0 + x1) / 2;
        return (!h.edges || (cx > -kMiddle && cx < kMiddle)) ? 0 : (cx < 0 ? -kHalf : kHalf);
    };
    // pin: inside a composition wider than the screen, an untextured panel's
    // vertex on or past the screen edge stays there (menu backgrounds, info
    // boxes and bars keep running edge to edge; measured on the entry,
    // team-colour and class-select menus); textured pictures keep their shape.
    const auto transform_piece = [&](const HudScaleV1::Piece& pc, int32_t anchor, bool pin) {
        // A textured sprite counts as a panel when it stretches its texture
        // (more than 1.5 px per texel across): the grey menu info box is one;
        // pictures such as the team-colour cards map about 1:1 and keep shape.
        bool pin_this = pin && ((pc.prim >> 4) & 1u) == 0;
        if (pin && !pin_this && (pc.prim & 7u) == 6u && ((pc.prim >> 8) & 1u) != 0 && pc.count >= 2)
        {
            uint8_t* ua = h.uvs[pc.first];
            uint8_t* ub = h.uvs[pc.first + 1];
            if (ua && ub && ua != ub)
            {
                uint32_t wa, wb;
                std::memcpy(&wa, ua, 4);
                std::memcpy(&wb, ub, 4);
                const int32_t du = std::abs(static_cast<int32_t>(wb & 0x3fffu) - static_cast<int32_t>(wa & 0x3fffu));
                pin_this = (pc.x1 - pc.x0) * 2 > du * 3;
            }
        }
        for (uint32_t n = 0; n < pc.count; ++n)
        {
            uint8_t* v = h.verts[pc.first + n];
            uint16_t x;
            std::memcpy(&x, v, 2);
            const int32_t rx = static_cast<int32_t>(x) - kCentre;
            if (pin_this && (rx <= -kEdge || rx >= kEdge))
                continue; // panels keep running off the edge; pictures keep their shape
            const int32_t nx = kCentre + anchor + static_cast<int32_t>(std::lround((rx - anchor) * h.factor));
            const uint16_t out = static_cast<uint16_t>(nx);
            std::memcpy(v, &out, 2);
        }
        // A squeezed textured sprite samples between texels; pull its U range
        // in by half a texel at each end so the filter never reaches the
        // neighbouring texels of the texture sheet (measured: 1-pixel slivers
        // at the right edge of digits, dial and minimap).
        if (pin_this || (pc.prim & 7u) != 6u || ((pc.prim >> 4) & 1u) == 0 || ((pc.prim >> 8) & 1u) == 0)
            return;
        for (uint32_t n = 0; n + 1 < pc.count; n += 2)
        {
            uint8_t* ua = h.uvs[pc.first + n];
            uint8_t* ub = h.uvs[pc.first + n + 1];
            if (!ua || !ub || ua == ub)
                continue;
            uint32_t wa, wb;
            std::memcpy(&wa, ua, 4);
            std::memcpy(&wb, ub, 4);
            int32_t ta = static_cast<int32_t>(wa & 0x3fffu), tb = static_cast<int32_t>(wb & 0x3fffu);
            if (std::abs(tb - ta) <= 16)
                continue;
            const int32_t d = ta < tb ? 8 : -8;
            ta += d;
            tb -= d;
            wa = (wa & ~0x3fffu) | static_cast<uint32_t>(ta & 0x3fff);
            wb = (wb & ~0x3fffu) | static_cast<uint32_t>(tb & 0x3fff);
            std::memcpy(ua, &wa, 4);
            std::memcpy(ub, &wb, 4);
        }
    };
    if (h.race_only && !h.race_active)
    {
        for (const auto& pc : h.pieces)
            h.field_pieces.push_back({pc.x0, pc.x1, pc.y0, pc.y1});
        return bytes; // menus stay stretched in race-only mode
    }
    // Pieces matched to the previous field's elements.
    std::vector<int32_t> match(h.pieces.size(), -1);
    for (size_t k = 0; k < h.pieces.size(); ++k)
    {
        const auto& pc = h.pieces[k];
        h.field_pieces.push_back({pc.x0, pc.x1, pc.y0, pc.y1});
        int64_t best = 0;
        for (size_t e = 0; e < h.prev_elements.size(); ++e)
        {
            const auto& b = h.prev_elements[e].box;
            const int64_t ox = std::min(pc.x1, b.x1) - std::max(pc.x0, b.x0) + (2 << 4);
            const int64_t oy = std::min(pc.y1, b.y1) - std::max(pc.y0, b.y0) + (2 << 4);
            if (ox > 0 && oy > 0 && ox * oy > best)
            {
                best = ox * oy;
                match[k] = static_cast<int32_t>(e);
            }
        }
    }
    size_t i = 0;
    while (i < h.pieces.size())
    {
        if (match[i] >= 0)
        {
            const auto& el = h.prev_elements[static_cast<size_t>(match[i])];
            if (!el.full_width)
                transform_piece(h.pieces[i], el.anchor, el.pin);
            ++i;
            continue;
        }
        // No previous element: group the unmatched pieces of this transfer.
        int32_t x0 = h.pieces[i].x0, x1 = h.pieces[i].x1, y0 = h.pieces[i].y0, y1 = h.pieces[i].y1;
        size_t j = i + 1;
        const bool first_wide = x0 <= -kEdge && x1 >= kEdge;
        for (; !first_wide && j < h.pieces.size() && match[j] < 0; ++j)
        {
            const auto& pc = h.pieces[j];
            if (pc.x0 <= -kEdge && pc.x1 >= kEdge)
                break;
            if (pc.x0 - x1 > kGapX || x0 - pc.x1 > kGapX || pc.y0 - y1 > kGapY || y0 - pc.y1 > kGapY)
                break;
            x0 = std::min(x0, pc.x0); x1 = std::max(x1, pc.x1);
            y0 = std::min(y0, pc.y0); y1 = std::max(y1, pc.y1);
        }
        const bool spans = x0 <= -kEdge && x1 >= kEdge;
        if (!spans || j - i > 1)
            for (size_t k = i; k < j; ++k)
                transform_piece(h.pieces[k], spans ? 0 : anchor_of(x0, x1), spans);
        i = j;
    }
    return h.scratch.data();
}

extern "C" int rrv_pcsx2_gs_bridge_submit(
    RrvPcsx2GsBridge* bridge, uint32_t path_id, const uint8_t* gif_bytes,
    uint32_t size_bytes, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not initialized");
        return 0;
    }
    if (path_id < 1 || path_id > 3)
    {
        SetError(error, error_capacity, "invalid post-arbitration GIF path %u", path_id);
        return 0;
    }
    if ((size_bytes != 0 && !gif_bytes) || (size_bytes & 0x0f) != 0)
    {
        SetError(error, error_capacity, "GIF packet must be non-null and 16-byte aligned in size (got %u)", size_bytes);
        return 0;
    }

    // GSDumpReplayer at the pinned revision dispatches staged GSPackets through
    // Transfer<3>; GSgifTransfer is that exported API. Its size is QWC, so the
    // ABI's byte length is divided only after the validated 16-byte alignment.
    // Re-selecting Transfer 1/2/3 here would incorrectly treat post-arbitrated
    // bytes as DMA input.
    if (size_bytes != 0 && (path_id == 1u || path_id == 3u) &&
        !bridge->fpcr_path_logged[path_id])
    {
        bridge->fpcr_path_logged[path_id] = true;
        const FPControlRegister ambient = FPControlRegister::GetCurrent();
        std::fprintf(stderr,
                     "[pcsx2-gs-bridge:fpcr] first-submit path=%u ambient=0x%llx "
                     "round=%u ftz=%u isolated=%u\n",
                     path_id, static_cast<unsigned long long>(ambient.bitmask),
                     static_cast<unsigned>(ambient.GetRoundMode()),
                     ambient.GetFlushToZero() ? 1u : 0u, bridge->fp_isolated ? 1u : 0u);
    }
    HudStatsObserve(path_id, gif_bytes, size_bytes);
    // Canonical receipts below still acknowledge the caller's original bytes.
    const uint8_t* const original_gif_bytes = gif_bytes;
    gif_bytes = HudScaleApply(path_id, gif_bytes, size_bytes);
    (void)original_gif_bytes;
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (Gate4GsObserverV1& obs = Gate4GsObserver(); obs.enabled)
    {
        uint64_t before[2], after[2];
        std::memcpy(&before[0], bridge->pcsx2_regs.data() + kPcsx2Offsets[15], sizeof(uint64_t));
        std::memcpy(&before[1], bridge->pcsx2_regs.data() + kPcsx2Offsets[18], sizeof(uint64_t));
        const uint64_t t0 = Gate4GsObserverV1::Now();
        if (size_bytes != 0)
            GSgifTransfer(gif_bytes, size_bytes / 16u);
        obs.open_field.gif_ns += Gate4GsObserverV1::Now() - t0;
        ++obs.open_field.packets;
        obs.open_field.bytes += size_bytes;
        std::memcpy(&after[0], bridge->pcsx2_regs.data() + kPcsx2Offsets[15], sizeof(uint64_t));
        std::memcpy(&after[1], bridge->pcsx2_regs.data() + kPcsx2Offsets[18], sizeof(uint64_t));
        obs.open_field.result_changes += (before[0] != after[0] || before[1] != after[1]) ? 1u : 0u;
    }
    else if (size_bytes != 0)
        GSgifTransfer(gif_bytes, size_bytes / 16u);
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    // GSgifTransfer directly executes GSState::Transfer<3> in this bridge; no
    // MTGS queue exists on this call path. Record only after that synchronous
    // consumer returned, never merely after bridge ingress.
    if (size_bytes != 0u &&
        !bridge->canonical_receipt.acknowledgeTransfer(path_id, original_gif_bytes, size_bytes))
    {
        SetError(error, error_capacity,
                 "canonical consumer receipt overflowed or rejected a consumed transfer");
        return 0;
    }
    if (size_bytes != 0u && bridge->receipt_boundary_ordering.arm_scheduled != 0u)
        NoteConsumerAcknowledgement(bridge,
                                    static_cast<uint32_t>(rrv::pcsx2::receipt::EventType::Transfer),
                                    true);
#endif
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_reset_gs(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not open for reset");
        return 0;
    }
    // fd9d310c: GS.cpp::csrWrite calls MTGS::ResetGS(false); the consumer
    // calls GSreset(false). This resets GSState's parser and draw environment
    // but does not reset producer GIF progression or clear GS local memory.
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    GSreset(false);
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_readback(
    RrvPcsx2GsBridge* bridge, uint64_t* csr, uint64_t* siglblid,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !csr || !siglblid)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge or readback outputs are null");
        return 0;
    }
    std::memcpy(csr, bridge->pcsx2_regs.data() + kPcsx2Offsets[15], sizeof(*csr));
    std::memcpy(siglblid, bridge->pcsx2_regs.data() + kPcsx2Offsets[18], sizeof(*siglblid));
    if (Gate4GsObserverV1& obs = Gate4GsObserver(); obs.enabled)
        ++obs.open_field.readbacks;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_read_local_memory(
    RrvPcsx2GsBridge* bridge, uint8_t* dst, uint32_t byte_count,
    uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not open");
        return 0;
    }
    if (byte_count == 0u)
    {
        // A zero-byte result request has no guest transaction meaning.  In
        // particular, it must not report success before checking the native
        // local-to-host cursor or be mistaken for an acknowledged no-op.
        SetError(error, error_capacity, "local-memory read requires a non-zero byte count");
        return 0;
    }
    if (!dst)
    {
        SetError(error, error_capacity, "PCSX2 GS local-memory destination is null");
        return 0;
    }

    // Keep the bridge allocation within the same fixed local-memory maximum
    // enforced by GSState::RrvReadLocalMemoryChecked.  This check must happen
    // before vector construction: the bridge is built without exceptions, so
    // an attacker-controlled UINT32_MAX request must not reach allocation and
    // terminate the process before the native adapter can reject it.
    constexpr uint64_t kNativeLocalMemoryBytes = RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES;
    if (static_cast<uint64_t>(byte_count) > kNativeLocalMemoryBytes)
    {
        SetError(error, error_capacity, "local-memory read size %u exceeds native GS local memory", byte_count);
        return 0;
    }
    const uint64_t qwc64 = (static_cast<uint64_t>(byte_count) + 15u) / 16u;
    if (qwc64 == 0u || qwc64 > UINT32_MAX || qwc64 > static_cast<uint64_t>(SIZE_MAX) / 16u)
    {
        SetError(error, error_capacity, "local-memory read size %u is not representable", byte_count);
        return 0;
    }
    const size_t fifo_bytes = static_cast<size_t>(qwc64 * 16u);

    // PCSX2 owns the cursor created by the preceding guest TRXDIR=1. The
    // maintained GS adapter compares these ABI provenance descriptors against
    // both that cursor and the live GS environment, rejects no-active/upload/
    // rewritten/inconsistent transfers before consuming anything, then uses
    // PCSX2's native InitReadFIFO/ReadFIFO continuation path. It also retires
    // rasterizer work before the local-memory operation. The bridge keeps no
    // duplicate transaction state and never falls back to the stale-state
    // ReadLocalMemoryUnsync helper.
    Gate4GsObserverV1& obs = Gate4GsObserver();
    const uint64_t local_read_t0 = obs.enabled ? Gate4GsObserverV1::Now() : 0u;
    std::vector<uint8_t> fifo(fifo_bytes);
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    const bool local_read_ok = GSReadLocalMemoryChecked(fifo.data(), byte_count, bitbltbuf, trxpos, trxreg);
    if (obs.enabled)
    {
        ++obs.open_field.local_reads;
        obs.open_field.local_read_bytes += byte_count;
        obs.open_field.local_read_ns += Gate4GsObserverV1::Now() - local_read_t0;
    }
    if (!local_read_ok)
    {
        SetError(error, error_capacity,
                 "PCSX2 rejected local-to-host transfer state or descriptor before FIFO consumption");
        return 0;
    }
    std::memcpy(dst, fifo.data(), byte_count);
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_snapshot_local_memory(
    RrvPcsx2GsBridge* bridge, uint8_t* dst, uint32_t byte_count,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !dst ||
        byte_count != RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge local-memory snapshot requires a %u-byte destination",
                 RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
        return 0;
    }
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (!GSSnapshotLocalMemory(dst, byte_count))
    {
        SetError(error, error_capacity, "PCSX2 GS local-memory snapshot failed");
        return 0;
    }
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_restore_local_memory(
    RrvPcsx2GsBridge* bridge, const uint8_t* src, uint32_t byte_count,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !src ||
        byte_count != RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge local-memory restore requires a %u-byte source",
                 RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
        return 0;
    }
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (!GSRestoreLocalMemory(src, byte_count))
    {
        SetError(error, error_capacity, "PCSX2 GS local-memory restore failed");
        return 0;
    }
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    // Restore mutates GS local memory and texture-cache state, so it is an
    // image-affecting consumer event even though it is not a GIF transfer.
    if (!bridge->canonical_receipt.acknowledgeLocalMemoryRestore(src, byte_count))
    {
        SetError(error, error_capacity,
                 "canonical consumer receipt overflowed after local-memory restore");
        return 0;
    }
    if (bridge->receipt_boundary_ordering.arm_scheduled != 0u)
    {
        NoteConsumerAcknowledgement(
            bridge, static_cast<uint32_t>(rrv::pcsx2::receipt::EventType::LocalMemoryRestore),
            false);
    }
#endif
    return 1;
}

// ---- Host overlay (the Fukami in-game menu) --------------------------------
//
// PCSX2's present path already runs a Dear ImGui frame for its OSD:
// GSRenderer::VSync -> EndPresentFrame() renders it (GSDeviceMTL::EndPresent ->
// RenderImGui) and immediately opens the next one (ImGuiManager::NewFrame), so
// between two GSvsync calls the ImGui frame is always open on this owner. The
// host callback runs right before GSvsync and only adds windows/input to that
// frame. ImGui is compiled into this library with hidden visibility, so the
// host reaches it through the RrvPcsx2GsBridgeUi table rather than linking it.
// Adapted usage: the ImGuiKey mapping, the ClearInput calls and the IO event
// calls follow pcsx2/ImGui/ImGuiManager.cpp (ProcessHostKeyEvent,
// ProcessPointerButtonEvent, ProcessGenericInputEvent) at PCSX2 2.8.2
// (fd9d310ccbb6, GPL-3.0+), called directly because this bridge has no MTGS
// thread to marshal onto.
namespace {

std::mutex s_overlay_mutex; // held across a frame callback
RrvPcsx2GsBridgeOverlayFrame s_overlay_frame = nullptr;
void* s_overlay_context = nullptr;
std::atomic<bool> s_overlay_registered{false};
bool s_overlay_font_pushed = false;

float OverlayScale()
{
    return std::max(ImGuiManager::GetGlobalScale(), 0.5f);
}

ImGuiKey OverlayImGuiKey(uint32_t key)
{
    switch (key)
    {
        case RRV_PCSX2_GS_UI_KEY_TAB: return ImGuiKey_Tab;
        case RRV_PCSX2_GS_UI_KEY_LEFT: return ImGuiKey_LeftArrow;
        case RRV_PCSX2_GS_UI_KEY_RIGHT: return ImGuiKey_RightArrow;
        case RRV_PCSX2_GS_UI_KEY_UP: return ImGuiKey_UpArrow;
        case RRV_PCSX2_GS_UI_KEY_DOWN: return ImGuiKey_DownArrow;
        case RRV_PCSX2_GS_UI_KEY_PAGE_UP: return ImGuiKey_PageUp;
        case RRV_PCSX2_GS_UI_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
        case RRV_PCSX2_GS_UI_KEY_HOME: return ImGuiKey_Home;
        case RRV_PCSX2_GS_UI_KEY_END: return ImGuiKey_End;
        case RRV_PCSX2_GS_UI_KEY_SPACE: return ImGuiKey_Space;
        case RRV_PCSX2_GS_UI_KEY_ENTER: return ImGuiKey_Enter;
        case RRV_PCSX2_GS_UI_KEY_ESCAPE: return ImGuiKey_Escape;
        case RRV_PCSX2_GS_UI_KEY_SHIFT: return ImGuiKey_LeftShift;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_UP: return ImGuiKey_GamepadDpadUp;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_DOWN: return ImGuiKey_GamepadFaceDown;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT: return ImGuiKey_GamepadFaceRight;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_LEFT: return ImGuiKey_GamepadFaceLeft;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_UP: return ImGuiKey_GamepadFaceUp;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_L1: return ImGuiKey_GamepadL1;
        case RRV_PCSX2_GS_UI_KEY_GAMEPAD_R1: return ImGuiKey_GamepadR1;
        default: return ImGuiKey_None;
    }
}

void OverlayAddKey(uint32_t key, int down)
{
    const ImGuiKey imkey = OverlayImGuiKey(key);
    if (imkey == ImGuiKey_None)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(imkey, down != 0);
    if (key == RRV_PCSX2_GS_UI_KEY_SHIFT)
        io.AddKeyEvent(ImGuiMod_Shift, down != 0);
}

void OverlayAddMousePosition(float x, float y)
{
    ImGui::GetIO().AddMousePosEvent(x, y);
}

void OverlayAddMouseButton(uint32_t button, int down)
{
    if (button < 3u)
        ImGui::GetIO().AddMouseButtonEvent(static_cast<int>(button), down != 0);
}

void OverlayAddMouseWheel(float x, float y)
{
    ImGui::GetIO().AddMouseWheelEvent(x, y);
}

void OverlayClearInput()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

int OverlayConsumesCancel()
{
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                   ImGui::IsAnyItemActive()
               ? 1
               : 0;
}

int OverlayBeginMenu(const char* title, int appearing)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float scale = OverlayScale();
    const ImVec2 display = io.DisplaySize;
    // Dim the game picture behind the menu (drawn under every ImGui window).
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 150));
    const float width = std::min(display.x * 0.94f, 600.0f * scale);
    const float height = std::min(display.y * 0.92f, 760.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (appearing)
        ImGui::SetNextWindowFocus();
    ImGui::PushFont(ImGuiManager::GetStandardFont(), std::ceil(15.0f * scale));
    s_overlay_font_pushed = true;
    return ImGui::Begin(title ? title : "Menu", nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings) ? 1 : 0;
}

void OverlayEndMenu()
{
    ImGui::End();
    if (s_overlay_font_pushed)
    {
        ImGui::PopFont();
        s_overlay_font_pushed = false;
    }
}

void OverlaySection(const char* label)
{
    ImGui::Spacing();
    ImGui::SeparatorText(label ? label : "");
}

void OverlayText(const char* text) { ImGui::TextUnformatted(text ? text : ""); }
void OverlayTextDisabled(const char* text) { ImGui::TextDisabled("%s", text ? text : ""); }
void OverlayTextWrapped(const char* text) { ImGui::TextWrapped("%s", text ? text : ""); }
void OverlayTextWarning(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.28f, 1.0f));
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopStyleColor();
}
void OverlaySeparator() { ImGui::Separator(); }
void OverlaySameLine() { ImGui::SameLine(); }
void OverlaySpacing() { ImGui::Spacing(); }
void OverlayPushId(int id) { ImGui::PushID(id); }
void OverlayPopId() { ImGui::PopID(); }
void OverlayBeginDisabled(int disabled) { ImGui::BeginDisabled(disabled != 0); }
void OverlayEndDisabled() { ImGui::EndDisabled(); }
void OverlayTooltip(const char* text)
{
    if (text && text[0] != '\0')
        ImGui::SetItemTooltip("%s", text);
}
void OverlayDefaultFocus() { ImGui::SetItemDefaultFocus(); }

// Label in the left column, control filling the right one. The control's
// ImGui ID is "##value" inside the caller's push_id() scope, never the label:
// the host may change a label (a restart marker) while the control is active.
void OverlayRowLabel(const char* label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label ? label : "");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

int OverlayCheckbox(const char* label, int* value)
{
    if (!value)
        return 0;
    OverlayRowLabel(label);
    bool checked = *value != 0;
    const bool changed = ImGui::Checkbox("##value", &checked);
    if (changed)
        *value = checked ? 1 : 0;
    return changed ? 1 : 0;
}

int OverlayCombo(const char* label, int* index, const char* const* items, int count)
{
    if (!index || !items || count <= 0)
        return 0;
    OverlayRowLabel(label);
    const bool changed = ImGui::Combo("##value", index, items, count);
    return changed ? 1 : 0;
}

int OverlaySliderInt(const char* label, int* value, int min, int max)
{
    if (!value)
        return 0;
    OverlayRowLabel(label);
    // No Ctrl+click text entry: the menu forwards no text input.
    const bool changed = ImGui::SliderInt("##value", value, min, max, "%d",
                                          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    return changed ? 1 : 0;
}

int OverlaySliderFloat(const char* label, float* value, float min, float max, const char* format)
{
    if (!value)
        return 0;
    OverlayRowLabel(label);
    const bool changed = ImGui::SliderFloat("##value", value, min, max, format ? format : "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    return changed ? 1 : 0;
}

int OverlayButton(const char* label)
{
    const float scale = OverlayScale();
    return ImGui::Button(label ? label : "", ImVec2(std::max(120.0f * scale, 0.0f), 0.0f)) ? 1 : 0;
}

// Presentation options: the same GSConfig/EmuConfig fields rrv_pcsx2_gs_bridge_create
// sets from RRV_PCSX2_GS_ASPECT, RRV_PCSX2_GS_INTEGER_SCALING, RRV_PCSX2_GS_FXAA
// and RRV_PCSX2_GS_CAS. PCSX2 reads them on this owner at the next present:
// CalculateDrawDstRect (aspect, integer scaling), GSRenderer::Merge -> FXAA,
// GSRenderer::VSync -> CAS. GSDeviceMTL creates the FXAA and both CAS
// pipelines unconditionally at device creation, so enabling them later needs
// no device work. None of them feeds back into GS emulation.
int OverlaySetAspect(const char* value)
{
    if (!value)
        return 0;
    const std::string_view v(value);
    AspectRatioType type;
    float custom = 0.0f;
    if (v == "auto")
        type = AspectRatioType::RAuto4_3_3_2;
    else if (v == "4:3")
        type = AspectRatioType::R4_3;
    else if (v == "16:9")
        type = AspectRatioType::R16_9;
    else if (v == "16:10" || v == "21:9")
    {
        type = AspectRatioType::RAuto4_3_3_2;
        custom = v == "16:10" ? 16.0f / 10.0f : 21.0f / 9.0f;
    }
    else if (v == "stretch")
        type = AspectRatioType::Stretch;
    else
        return 0;
    GSConfig.AspectRatio = type;
    EmuConfig.GS.AspectRatio = type;
    EmuConfig.CurrentAspectRatio = type;
    EmuConfig.CurrentCustomAspectRatio = custom;
    std::fprintf(stderr, "[pcsx2-gs-bridge] menu: presentation aspect=%s\n", value);
    return 1;
}

void OverlaySetIntegerScaling(int enabled)
{
    GSConfig.IntegerScaling = enabled != 0;
    EmuConfig.GS.IntegerScaling = enabled != 0;
    std::fprintf(stderr, "[pcsx2-gs-bridge] menu: integer-scaling=%d\n", enabled != 0 ? 1 : 0);
}

void OverlaySetFxaa(int enabled)
{
    GSConfig.FXAA = enabled != 0;
    EmuConfig.GS.FXAA = enabled != 0;
    std::fprintf(stderr, "[pcsx2-gs-bridge] menu: fxaa=%d\n", enabled != 0 ? 1 : 0);
}

int OverlaySetCas(int sharpness)
{
    if (sharpness < 0 || sharpness > 100)
        return 0;
    const GSCASMode mode = sharpness > 0 ? GSCASMode::SharpenAndResize : GSCASMode::Disabled;
    GSConfig.CASMode = mode;
    EmuConfig.GS.CASMode = mode;
    if (sharpness > 0)
    {
        GSConfig.CAS_Sharpness = static_cast<u8>(sharpness);
        EmuConfig.GS.CAS_Sharpness = static_cast<u8>(sharpness);
    }
    std::fprintf(stderr, "[pcsx2-gs-bridge] menu: cas=%d\n", sharpness);
    return 1;
}

// Runs the host overlay, if one is registered, inside the open ImGui frame.
// Direct presentation only: the snapshot path presents nothing.
void RunHostOverlay()
{
    if (!s_overlay_registered.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    if (!s_overlay_frame || !ImGui::GetCurrentContext())
        return;
    const ImGuiIO& io = ImGui::GetIO();
    RrvPcsx2GsBridgeUi ui{};
    ui.struct_size = sizeof(ui);
    ui.display_width = io.DisplaySize.x;
    ui.display_height = io.DisplaySize.y;
    ui.scale = OverlayScale();
    ui.add_key = OverlayAddKey;
    ui.add_mouse_position = OverlayAddMousePosition;
    ui.add_mouse_button = OverlayAddMouseButton;
    ui.add_mouse_wheel = OverlayAddMouseWheel;
    ui.clear_input = OverlayClearInput;
    ui.consumes_cancel = OverlayConsumesCancel;
    ui.begin_menu = OverlayBeginMenu;
    ui.end_menu = OverlayEndMenu;
    ui.section = OverlaySection;
    ui.text = OverlayText;
    ui.text_disabled = OverlayTextDisabled;
    ui.text_wrapped = OverlayTextWrapped;
    ui.text_warning = OverlayTextWarning;
    ui.separator = OverlaySeparator;
    ui.same_line = OverlaySameLine;
    ui.spacing = OverlaySpacing;
    ui.push_id = OverlayPushId;
    ui.pop_id = OverlayPopId;
    ui.begin_disabled = OverlayBeginDisabled;
    ui.end_disabled = OverlayEndDisabled;
    ui.tooltip = OverlayTooltip;
    ui.default_focus = OverlayDefaultFocus;
    ui.checkbox = OverlayCheckbox;
    ui.combo = OverlayCombo;
    ui.slider_int = OverlaySliderInt;
    ui.slider_float = OverlaySliderFloat;
    ui.button = OverlayButton;
    ui.set_aspect = OverlaySetAspect;
    ui.set_integer_scaling = OverlaySetIntegerScaling;
    ui.set_fxaa = OverlaySetFxaa;
    ui.set_cas = OverlaySetCas;
    s_overlay_frame(s_overlay_context, &ui);
    if (s_overlay_font_pushed)
    {
        // The host skipped end_menu(); keep ImGui's font stack balanced.
        ImGui::PopFont();
        s_overlay_font_pushed = false;
    }
}

} // namespace

extern "C" void rrv_pcsx2_gs_bridge_set_overlay(RrvPcsx2GsBridgeOverlayFrame frame, void* context)
{
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    s_overlay_frame = frame;
    s_overlay_context = frame ? context : nullptr;
    s_overlay_registered.store(frame != nullptr, std::memory_order_release);
    std::fprintf(stderr, "[pcsx2-gs-bridge] host overlay %s\n", frame ? "registered" : "cleared");
}

extern "C" int rrv_pcsx2_gs_bridge_vsync(
    RrvPcsx2GsBridge* bridge, const RrvPcsx2GsPrivRegs regs,
    uint64_t field_index, uint32_t field_parity, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !regs)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge or privileged registers are null");
        return 0;
    }

    if (field_parity > 1u)
    {
        SetError(error, error_capacity, "field parity must be 0 or 1 (got %u)", field_parity);
        return 0;
    }

    const bool field_from_csr = VsyncFieldFromCsr();
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    bool armed_after_vsync_this_call = false;
#endif
    std::array<uint64_t, kSlotCount> consumed_regs{};
    for (size_t index = 0; index < kSlotCount; ++index)
    {
        uint64_t value = regs[index];
        if (index == 15 && !field_from_csr)
            value = (value & ~(uint64_t{1} << 13)) | (static_cast<uint64_t>(field_parity) << 13);
        consumed_regs[index] = value;
        std::memcpy(bridge->pcsx2_regs.data() + kPcsx2Offsets[index], &value, sizeof(value));
    }

    // Pinned PCSX2 MTGS.cpp passes inverse CSR.FIELD to GSvsync. The caller's
    // authoritative field has just been mirrored into the PCSX2 register bank.
    uint64_t csr = 0;
    std::memcpy(&csr, bridge->pcsx2_regs.data() + kPcsx2Offsets[15], sizeof(csr));
    const uint32_t pcsx2_field = (static_cast<uint32_t>(csr) & 0x2000u) ? 0u : 1u;
    const bool verbose = BridgeDiagVerbose();
    if (verbose)
        std::fprintf(stderr, "[pcsx2-gs-bridge:vsync] GSvsync begin field=%u\n", pcsx2_field);
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    // Host overlay (in-game menu): adds ImGui windows to the frame GSvsync is
    // about to present. No callback registered (headless) -> one atomic load.
    if (bridge->direct_present)
        RunHostOverlay();
    Gate4GsObserverV1& gate4_obs = Gate4GsObserver();
    uint64_t gate4_vsync_t0 = 0u;
    std::array<double, GSPerfMon::CounterLast> gate4_pre{};
    if (gate4_obs.enabled)
    {
        gate4_obs.open_field.entry_ns = Gate4GsObserverV1::Now();
        gate4_pre = Gate4GsObserverV1::Perf();
        // GSvsync's present path reads and resets the accumulated GPU time
        // (PerformanceMetrics); take what completed before it first.
        gate4_obs.open_field.gpu_ms = g_gs_device ? g_gs_device->GetAndResetAccumulatedGPUTime() : -1.0f;
        gate4_vsync_t0 = Gate4GsObserverV1::Now();
    }
    if (bridge->vsync_from_field && field_index >= bridge->vsync_from_field)
    {
        GSSetVSyncMode(GSVSyncMode::FIFO, false);
        std::fprintf(stderr, "[pcsx2-gs-bridge] fast-forward done at field %llu: presentation vsync=FIFO\n",
                     static_cast<unsigned long long>(field_index));
        bridge->vsync_from_field = 0;
    }
    GSvsync(pcsx2_field, VsyncRegistersWritten());
    HudScaleFieldEnd();
    HudStatsFieldEnd();
    if (gate4_obs.enabled)
    {
        Gate4GsFieldV1& row = gate4_obs.open_field;
        row.vsync_ns = Gate4GsObserverV1::Now() - gate4_vsync_t0;
        row.field_index = field_index;
        row.parity = field_parity;
        if (g_gs_device)
            row.gpu_ms += g_gs_device->GetAndResetAccumulatedGPUTime();
        row.gpu_avg_ms = PerformanceMetrics::GetGPUAverageTime();
        const auto post = Gate4GsObserverV1::Perf();
        row.vsync_perf_valid = true;
        for (size_t i = 0; i < post.size(); ++i)
        {
            row.perf[i] = gate4_pre[i] - gate4_obs.last_post[i];
            row.vsync_perf[i] = post[i] - gate4_pre[i];
            row.vsync_perf_valid &= post[i] >= gate4_pre[i];
        }
        gate4_obs.last_post = post;
        if (gate4_obs.rows.size() < gate4_obs.rows.capacity())
            gate4_obs.rows.push_back(row);
        else
            ++gate4_obs.dropped;
        row = {};
    }
    if (verbose)
        std::fprintf(stderr, "[pcsx2-gs-bridge:vsync] GSvsync end\n");
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    // GSvsync synchronously flushes the open GS state and executes
    // GSRenderer::VSync. Metal command execution/presentation remains
    // asynchronous and is deliberately outside the consumed workload.
    if (!bridge->canonical_receipt.acknowledgeVsync(
            consumed_regs.data(), pcsx2_field, VsyncRegistersWritten(), field_parity))
    {
        SetError(error, error_capacity,
                 "canonical consumer receipt overflowed or rejected a consumed VSync");
        return 0;
    }
    if (bridge->receipt_boundary_ordering.arm_scheduled != 0u)
    {
        NoteConsumerAcknowledgement(bridge,
                                    static_cast<uint32_t>(rrv::pcsx2::receipt::EventType::Vsync),
                                    false);
    }
    if (bridge->receipt_arm_after_vsync_pending)
    {
        // The actual GSvsync and Collector's successful VSync acknowledgement
        // above have closed the mixed field. Arm the preallocated collector
        // before this synchronous ABI function returns. No GS transfer can
        // interleave: Backend holds its existing serialized GS mutex across
        // this entire bridge call.
        bridge->receipt_boundary_ordering.mixed_vsync_consumer_ack_ordinal =
            bridge->consumer_ack_ordinal;
        if (!bridge->canonical_receipt.arm())
        {
            SetError(error, error_capacity,
                     "canonical consumer receipt cannot arm after successful VSync");
            return 0;
        }
        bridge->receipt_arm_after_vsync_pending = false;
        bridge->receipt_boundary_ordering.armed_after_successful_vsync = 1u;
        bridge->receipt_boundary_ordering.arm_consumer_ack_ordinal =
            bridge->consumer_ack_ordinal;
        armed_after_vsync_this_call = true;
    }
#endif

    // RRV_PCSX2_GS_VSYNC_TRACE=1 prints one line per GSvsync with every input
    // the CRTC composition depends on, in a format directly comparable with the
    // MTGS side (KNOWN_ISSUES #24). CSR is read back AFTER the call so a field
    // flip performed inside the GS is visible.
    if (std::getenv("RRV_PCSX2_GS_VSYNC_TRACE"))
    {
        uint64_t csr_after = 0;
        std::memcpy(&csr_after, bridge->pcsx2_regs.data() + kPcsx2Offsets[15], sizeof(csr_after));
        std::fprintf(stderr,
            "[vsync] n=%llu field=%u regw=%d csr_before=%016llx csr_after=%016llx "
            "pmode=%016llx smode1=%016llx smode2=%016llx syncv=%016llx "
            "dispfb1=%016llx display1=%016llx dispfb2=%016llx display2=%016llx\n",
            static_cast<unsigned long long>(bridge->field_index), pcsx2_field,
            VsyncRegistersWritten() ? 1 : 0,
            static_cast<unsigned long long>(csr),
            static_cast<unsigned long long>(csr_after),
            static_cast<unsigned long long>(regs[0]), static_cast<unsigned long long>(regs[1]),
            static_cast<unsigned long long>(regs[2]), static_cast<unsigned long long>(regs[6]),
            static_cast<unsigned long long>(regs[7]), static_cast<unsigned long long>(regs[8]),
            static_cast<unsigned long long>(regs[9]), static_cast<unsigned long long>(regs[10]));
    }

    // GSvsync is field-accurate and intentionally does no host presentation
    // work. The frontend samples via snapshot at its own cadence.
    bridge->display_enabled = (regs[0] & 0x3u) != 0;
    bridge->field_index = field_index;
    if (bridge->display_enabled && !gate4_obs.capture_ranges.empty() &&
        gate4_obs.WantCapture(field_index))
    {
        // G4-6 pixel check only (see Gate4GsObserverV1). Internal resolution,
        // no fit or stretch: the merged output of the field just serviced.
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> pixels;
        FILE* f = nullptr;
        if (GSSaveSnapshotToMemory(0u, 0u, false, true, &width, &height, &pixels))
        {
            const std::string name = gate4_obs.capture_dir + "/field-" + std::to_string(field_index) +
                                     "-" + std::to_string(width) + "x" + std::to_string(height) + ".rgba";
            f = std::fopen(name.c_str(), "wb");
        }
        if (f && std::fwrite(pixels.data(), sizeof(uint32_t), pixels.size(), f) == pixels.size())
            ++gate4_obs.captures;
        else
            ++gate4_obs.capture_failures;
        if (f)
            std::fclose(f);
    }
    if (bridge->direct_present)
    {
        // The renderer increments this only after it obtained a drawable and
        // scheduled its Metal presentation (Vulkan: after vkQueuePresentKHR
        // accepted the swap-chain image, pcsx2-gs-bridge-linux.patch).
        // Nil-drawable/occluded/skipped fields do not inflate either counter.
        bridge->stats.direct_gpu_presents = GSRrvGetDirectPresentCount();
        bridge->stats.presented_frames = bridge->stats.direct_gpu_presents;
    }
    else if (bridge->display_enabled)
    {
        // Legacy has no native present callback; this remains a field attempt
        // count for diagnostic snapshot consumers only.
        ++bridge->stats.presented_frames;
    }
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    if (armed_after_vsync_this_call)
    {
        // The fixed ordering sample is intentionally made at the actual ABI
        // return edge, after the wrapper's remaining host-only bookkeeping.
        // It must equal arm_consumer_ack_ordinal; any larger value exposes an
        // acknowledged GS consumer event between arm and return.
        bridge->receipt_boundary_ordering.bridge_vsync_return_consumer_ack_ordinal =
            bridge->consumer_ack_ordinal;
    }
#endif
    return 1;
}

#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
extern "C" int rrv_pcsx2_gs_bridge_receipt_arm(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not initialized");
        return 0;
    }
    if (!bridge->canonical_receipt.arm())
    {
        SetError(error, error_capacity, "canonical consumer receipt cannot be armed");
        return 0;
    }
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_receipt_arm_after_vsync(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not initialized");
        return 0;
    }
    const rrv::pcsx2::receipt::Status receipt_status = bridge->canonical_receipt.status();
    if (!receipt_status.enabled || !receipt_status.armed || receipt_status.collecting ||
        receipt_status.intervalComplete || receipt_status.overflowed)
    {
        SetError(error, error_capacity,
                 "post-VSync receipt arm requires an enabled, pre-armed empty collector");
        return 0;
    }
    if (bridge->receipt_arm_after_vsync_pending)
        return 1;

    bridge->receipt_boundary_ordering = {
        sizeof(RrvPcsx2GsBridgeReceiptBoundaryOrdering), 1u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u};
    // Start the sideband counter at the requested boundary. Pre-edge consumer
    // events are deliberately outside this proof and cannot be mistaken for
    // relative epoch 0; the mixed closing VSync receives ordinal 1.
    bridge->consumer_ack_ordinal = 0u;
    bridge->receipt_arm_after_vsync_pending = true;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_receipt_boundary_ordering(
    RrvPcsx2GsBridge* bridge,
    RrvPcsx2GsBridgeReceiptBoundaryOrdering* ordering,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !ordering ||
        ordering->struct_size < sizeof(RrvPcsx2GsBridgeReceiptBoundaryOrdering))
    {
        SetError(error, error_capacity,
                 "PCSX2 GS bridge or receipt boundary ordering output is invalid");
        return 0;
    }
    *ordering = bridge->receipt_boundary_ordering;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_receipt_set_epoch_identity(
    RrvPcsx2GsBridge* bridge, uint64_t guest_field_id, uint64_t gs_field_epoch_id,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open)
    {
        SetError(error, error_capacity, "PCSX2 GS bridge is not initialized");
        return 0;
    }
    if (!bridge->canonical_receipt.setEpochIdentity(guest_field_id, gs_field_epoch_id))
    {
        SetError(error, error_capacity, "canonical consumer receipt rejected epoch identity");
        return 0;
    }
    return 1;
}
#endif

extern "C" int rrv_pcsx2_gs_bridge_resize(
    RrvPcsx2GsBridge* bridge, uint32_t width_pixels, uint32_t height_pixels,
    float backing_scale, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !bridge->direct_present || width_pixels == 0u ||
        height_pixels == 0u || backing_scale <= 0.0f)
    {
        SetError(error, error_capacity,
                 "direct presentation resize requires an active surface and non-zero dimensions");
        return 0;
    }
#if defined(__APPLE__)
    // Default ABI callers keep their original main-thread contract. The owner
    // path uses pinned GSDeviceMTL::ResizeWindow, which only updates owner GS
    // dimensions and CAMetalLayer.drawableSize; it never touches NSView or
    // dispatches to main. SDL prepares its own view on main before submission.
    if (!bridge->owner_thread_surface && !rrv_pcsx2_gs_bridge_is_macos_main_thread())
    {
        SetError(error, error_capacity,
                 "direct presentation resize must run on AppKit's main thread");
        return 0;
    }
#else
    // Linux: GSDeviceVK::ResizeWindow only waits for the GPU and recreates the
    // swap chain for the new extent on this (serialized GS owner) thread; the
    // SDL window itself was already resized by the caller. No thread rule.
#endif
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    GSResizeDisplayWindow(width_pixels, height_pixels, backing_scale);
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_snapshot(
    RrvPcsx2GsBridge* bridge, RrvPcsx2GsBridgeFrame* frame,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !frame || frame->struct_size < sizeof(RrvPcsx2GsBridgeFrame))
    {
        SetError(error, error_capacity, "bridge/frame is null or frame struct is too small");
        return 0;
    }

    const bool diag = BridgeDiagEnabled();
    const bool verbose = BridgeDiagVerbose();

    // RRV can ask before the first display-enabled field. PCSX2's screenshot
    // path assumes a display surface, so return a deterministic blank instead.
    if (!bridge->display_enabled)
    {
        const uint32_t width = bridge->snapshot_width != 0 ? bridge->snapshot_width : 640u;
        const uint32_t height = bridge->snapshot_height != 0 ? bridge->snapshot_height : 448u;
        bridge->pixels.assign(static_cast<size_t>(width) * height, 0u);
        bridge->snapshot_width = width;
        bridge->snapshot_height = height;
        ++bridge->sequence;
        frame->width = width;
        frame->height = height;
        frame->stride_bytes = width * sizeof(uint32_t);
        frame->rgba = reinterpret_cast<const uint8_t*>(bridge->pixels.data());
        frame->sequence = bridge->sequence;
        return 1;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> snapshot_pixels;
    if (verbose)
        std::fprintf(stderr, "[pcsx2-gs-bridge:vsync] snapshot begin %ux%u\n",
                     bridge->snapshot_width, bridge->snapshot_height);
    // GSSaveSnapshotToMemory is the actual CPU-visible GPU readback point.
    // Do not charge the deterministic pre-display blank above: it allocates
    // CPU pixels only and does not submit Metal work or wait for a command
    // buffer. A raw snapshot in direct mode remains an unexpected readback;
    // capture_in_progress is set only by the explicit capture API below.
    ++bridge->stats.synchronous_cpu_readbacks;
    if (bridge->gpu_renderer)
        ++bridge->stats.cpu_waits;
    if (bridge->direct_present && !bridge->capture_in_progress)
        ++bridge->stats.unexpected_readbacks;
    // Window 0x0 + crop makes this byte-for-byte the operation
    // `pcsx2-gsrunner -dumpdir` performs under ScreenshotSize=2: the merged
    // texture at internal resolution, with no CalculateDrawDstRect fit and no
    // bilinear StretchRect. Required for any pixel comparison against it.
    const bool snap_internal = SnapshotAtInternalResolution();
    const ScopedBridgeGsFpcr fpcr_guard(bridge->fp_isolated);
    if (!GSSaveSnapshotToMemory(snap_internal ? 0u : bridge->snapshot_width,
            snap_internal ? 0u : bridge->snapshot_height,
            false, snap_internal, &width, &height, &snapshot_pixels))
    {
        if (!bridge->gpu_snapshot_fallback || bridge->has_successful_frame)
        {
            SetError(error, error_capacity, "PCSX2 failed to snapshot the completed GS field");
            return 0;
        }

        // Metal can expose PMODE before it has produced a snapshotable target.
        // Do not turn that normal startup transition into a frontend exception.
        const uint32_t blank_width = bridge->snapshot_width != 0 ? bridge->snapshot_width : 640u;
        const uint32_t blank_height = bridge->snapshot_height != 0 ? bridge->snapshot_height : 448u;
        bridge->pixels.assign(static_cast<size_t>(blank_width) * blank_height, 0u);
        bridge->snapshot_width = blank_width;
        bridge->snapshot_height = blank_height;
        ++bridge->sequence;
        if (diag)
            std::fprintf(stderr, "[pcsx2-gs-bridge:snapshot] fallback=blank reason=no-target field=%llu\n",
                         static_cast<unsigned long long>(bridge->field_index));

        frame->width = bridge->snapshot_width;
        frame->height = bridge->snapshot_height;
        frame->stride_bytes = bridge->snapshot_width * sizeof(uint32_t);
        frame->rgba = reinterpret_cast<const uint8_t*>(bridge->pixels.data());
        frame->sequence = bridge->sequence;
        return 1;
    }
    if (verbose)
        std::fprintf(stderr, "[pcsx2-gs-bridge:vsync] snapshot end %ux%u pixels=%zu\n",
                     width, height, snapshot_pixels.size());
    bridge->snapshot_width = width;
    bridge->snapshot_height = height;
    bridge->pixels = std::move(snapshot_pixels);
    bridge->has_successful_frame = true;
    ++bridge->sequence;
    if (diag && (bridge->sequence <= 2u || (bridge->sequence % 60u) == 0u))
    {
        size_t rgb_nonzero = 0;
        uint64_t hash = 1469598103934665603ull;
        for (const uint32_t pixel : bridge->pixels)
        {
            rgb_nonzero += (pixel & 0x00ffffffu) != 0u;
            hash ^= pixel;
            hash *= 1099511628211ull;
        }
        std::fprintf(stderr,
            "[pcsx2-gs-bridge:frame] seq=%llu field=%llu size=%ux%u rgb-nonzero=%zu hash=%016llx\n",
            static_cast<unsigned long long>(bridge->sequence),
            static_cast<unsigned long long>(bridge->field_index), width, height, rgb_nonzero,
            static_cast<unsigned long long>(hash));
    }
    frame->width = bridge->snapshot_width;
    frame->height = bridge->snapshot_height;
    frame->stride_bytes = bridge->snapshot_width * sizeof(uint32_t);
    frame->rgba = reinterpret_cast<const uint8_t*>(bridge->pixels.data());
    frame->sequence = bridge->sequence;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_capture(
    RrvPcsx2GsBridge* bridge, const RrvPcsx2GsBridgeCaptureRequest* request,
    RrvPcsx2GsBridgeCaptureResult* result, char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !request || !result ||
        request->struct_size < sizeof(RrvPcsx2GsBridgeCaptureRequest) ||
        result->struct_size < sizeof(RrvPcsx2GsBridgeCaptureResult))
    {
        SetError(error, error_capacity, "bridge/capture request/result is null or too small");
        return 0;
    }
    ++bridge->stats.requested_captures;

    // Capture is synchronous and observes exactly the field which was last
    // submitted to GS. Accepting an arbitrary historical/future tag would
    // make regression hashes look correlated while reading a different field.
    // For direct presentation, use PCSX2's renderer-side counter, which only
    // advances once a drawable was acquired and scheduled for presentation.
    if (bridge->direct_present)
    {
        bridge->stats.direct_gpu_presents = GSRrvGetDirectPresentCount();
        bridge->stats.presented_frames = bridge->stats.direct_gpu_presents;
    }
    const uint64_t observed_present_index = bridge->stats.presented_frames;
    if (request->field_index != bridge->field_index ||
        request->present_index != observed_present_index)
    {
        SetError(error, error_capacity,
                 "diagnostic capture tag is stale: requested field=%llu present=%llu, "
                 "bridge observes field=%llu present=%llu",
                 static_cast<unsigned long long>(request->field_index),
                 static_cast<unsigned long long>(request->present_index),
                 static_cast<unsigned long long>(bridge->field_index),
                 static_cast<unsigned long long>(observed_present_index));
        return 0;
    }
    RrvPcsx2GsBridgeFrame frame{sizeof(RrvPcsx2GsBridgeFrame), 0u, 0u, 0u, nullptr, 0u};
    struct CaptureInProgressGuard
    {
        bool& value;
        explicit CaptureInProgressGuard(bool& capture_in_progress) : value(capture_in_progress)
        {
            value = true;
        }
        ~CaptureInProgressGuard() { value = false; }
    } capture_in_progress{bridge->capture_in_progress};
    if (!rrv_pcsx2_gs_bridge_snapshot(bridge, &frame, error, error_capacity))
    {
        return 0;
    }
    result->flags = 0u;
    result->guest_tick = request->guest_tick;
    result->field_index = bridge->field_index;
    result->present_index = observed_present_index;
    result->frame = frame;
    ++bridge->stats.completed_captures;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_get_stats(
    RrvPcsx2GsBridge* bridge, RrvPcsx2GsBridgeStats* stats,
    char* error, uint32_t error_capacity)
{
    if (!bridge || !bridge->open || !stats || stats->struct_size < sizeof(RrvPcsx2GsBridgeStats))
    {
        SetError(error, error_capacity, "bridge/stats is null or stats struct is too small");
        return 0;
    }
    *stats = bridge->stats;
    return 1;
}
