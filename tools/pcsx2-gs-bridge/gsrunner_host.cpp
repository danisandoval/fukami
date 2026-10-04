// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile the pinned gsrunner Host implementation as a library shim. Keeping
// it in this wrapper lets the bridge initialize gsrunner's internal console
// and avoids exporting an accidental process `main` from the dylib.
#include <mutex>
#include <optional>

#include "rrv_pcsx2_gs_bridge.h"

// Keep Host's original declarations visible. Main.cpp includes this header as
// well, but its include guard then prevents the definition-renaming macros
// below from changing the declarations we replace after the import.
#include "pcsx2/Host.h"
// AcquireRenderWindow/ReleaseRenderWindow are declared by GS.h rather than
// Host.h. Include it before the macro import for the same reason.
#include "pcsx2/GS/GS.h"

// The upstream runner owns Host::AcquireRenderWindow and always returns its
// own platform window. Rename only those definitions while importing the rest
// of its minimal Host implementation, then replace them with the RRV-owned
// opaque-surface provider below.
#define AcquireRenderWindow rrv_pcsx2_gsrunner_unused_acquire_render_window
#define ReleaseRenderWindow rrv_pcsx2_gsrunner_unused_release_render_window
#define main rrv_pcsx2_gsrunner_unused_main
namespace Host
{
// Main.cpp's renamed definitions still require matching declarations. The
// original declarations were deliberately imported above without macros, so
// declare only the private renamed copies used by this translation unit.
std::optional<WindowInfo> rrv_pcsx2_gsrunner_unused_acquire_render_window(bool recreate_window);
void rrv_pcsx2_gsrunner_unused_release_render_window();
} // namespace Host
#include "pcsx2-gsrunner/Main.cpp"
#undef main
#undef ReleaseRenderWindow
#undef AcquireRenderWindow

namespace
{
std::mutex s_surface_mutex;
std::optional<WindowInfo> s_native_surface;
}

extern "C" void rrv_pcsx2_gs_bridge_initialize_host_console()
{
    static bool initialized = false;
    if (!initialized)
    {
        GSRunner::InitializeConsole();
        initialized = true;
    }
}

extern "C" int rrv_pcsx2_gs_bridge_configure_native_surface(
    uint32_t kind, uint32_t flags, void* native_view, void* native_layer,
    uint32_t width_pixels, uint32_t height_pixels, float backing_scale,
    char* error, uint32_t error_capacity)
{
#if defined(__APPLE__)
    if (kind != 1u || !native_view || !native_layer || width_pixels == 0u ||
        height_pixels == 0u || backing_scale <= 0.0f)
    {
        if (error && error_capacity)
            std::snprintf(error, error_capacity, "invalid macOS direct-present surface");
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_surface_mutex);
    WindowInfo info{};
    info.type = WindowInfo::Type::MacOS;
    info.window_handle = native_view;
    info.surface_handle = native_layer;
#else
    // Linux (Gate 5): SDL's X11 (Display*, Window XID) or Wayland
    // (wl_display*, wl_surface*) handles, in the shape PCSX2's own Qt host
    // builds (pcsx2-qt/QtUtils.h GetWindowInfoForWindow at fd9d310c) and
    // VKSwapChain::CreateVulkanSurface consumes: display_connection is the
    // display, window_handle the X11 Window / wl_surface. surface_handle
    // stays null (it is a macOS-only CAMetalLayer slot).
    const bool x11 = kind == RRV_PCSX2_GS_SURFACE_LINUX_X11;
    const bool wayland = kind == RRV_PCSX2_GS_SURFACE_LINUX_WAYLAND;
    if ((!x11 && !wayland) || !native_view || !native_layer || width_pixels == 0u ||
        height_pixels == 0u || backing_scale <= 0.0f)
    {
        if (error && error_capacity)
            std::snprintf(error, error_capacity, "invalid Linux direct-present surface");
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_surface_mutex);
    WindowInfo info{};
    info.type = x11 ? WindowInfo::Type::X11 : WindowInfo::Type::Wayland;
    info.display_connection = native_view;
    info.window_handle = native_layer;
#endif
    info.surface_width = width_pixels;
    info.surface_height = height_pixels;
    info.surface_scale = backing_scale;
    s_native_surface = info;
    (void)flags;
    return 1;
}

extern "C" void rrv_pcsx2_gs_bridge_clear_native_surface()
{
    std::lock_guard<std::mutex> lock(s_surface_mutex);
    s_native_surface.reset();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
    std::lock_guard<std::mutex> lock(s_surface_mutex);
    (void)recreate_window;
    if (s_native_surface.has_value())
        return s_native_surface;
    return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
    // The RRV surface remains caller-owned until the bridge's destroy returns.
    // Destroy clears the borrowed configuration after GSclose() has detached it.
}
