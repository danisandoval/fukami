// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linux-only (Gate 5, Steam Deck) counterpart of metal_surface_adapter.mm.
// Callers pass SDL's X11 (Display*, Window XID) or Wayland (wl_display*,
// wl_surface*) handles as opaque borrowed pointers and retain them until
// destroy returns. PCSX2's Vulkan device creates its own VkSurfaceKHR and swap
// chain from them (VKSwapChain::CreateVulkanSurface at fd9d310c); this adapter
// creates no display connection, window or surface of its own.
//
// Unlike AppKit, neither X11 nor Wayland WSI requires the platform main thread:
// Mesa/NVIDIA Xlib WSI talks to the server through the display's XCB
// connection, which is thread-safe, and SDL2 calls XInitThreads when it opens
// the display. So there is no main-thread check and pump_main_thread is a no-op.
// The owner-thread sideband (prepare -> consume) is kept with the same contract
// as macOS so one dedicated-owner caller works unchanged on both platforms.
//
// Validation is deliberately shape-only (kind, non-null handles, extent):
// probing the X server here (XGetWindowAttributes) would route a stale XID to
// Xlib's default error handler, which exits the process.

#include "rrv_pcsx2_gs_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>

namespace
{
std::mutex s_preparation_mutex;
std::optional<RrvPcsx2GsBridgeSurface> s_prepared_surface;

void SetError(char* destination, uint32_t capacity, const char* format, ...)
{
    if (!destination || capacity == 0u)
        return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(destination, capacity, format, args);
    va_end(args);
    destination[capacity - 1u] = '\0';
}
} // namespace

extern "C" int rrv_pcsx2_gs_bridge_validate_linux_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    if (!surface || surface->struct_size < sizeof(*surface))
    {
        SetError(error, error_capacity, "direct presentation surface is missing or too small");
        return 0;
    }
    if (surface->kind != RRV_PCSX2_GS_SURFACE_LINUX_X11 &&
        surface->kind != RRV_PCSX2_GS_SURFACE_LINUX_WAYLAND)
    {
        SetError(error, error_capacity,
                 "direct presentation surface kind %u is not X11 (%u) or Wayland (%u)",
                 surface->kind, static_cast<unsigned>(RRV_PCSX2_GS_SURFACE_LINUX_X11),
                 static_cast<unsigned>(RRV_PCSX2_GS_SURFACE_LINUX_WAYLAND));
        return 0;
    }
    if ((surface->flags & RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES) == 0u)
    {
        SetError(error, error_capacity, "direct presentation surface handles must be caller-owned");
        return 0;
    }
    const bool x11 = surface->kind == RRV_PCSX2_GS_SURFACE_LINUX_X11;
    if (!surface->native_view)
    {
        SetError(error, error_capacity, "direct presentation native_view is not a %s",
                 x11 ? "X11 Display*" : "wl_display*");
        return 0;
    }
    if (!surface->native_layer)
    {
        SetError(error, error_capacity, "direct presentation native_layer is not a %s",
                 x11 ? "X11 Window (non-zero XID)" : "wl_surface*");
        return 0;
    }
    if (surface->width_pixels == 0u || surface->height_pixels == 0u || !(surface->backing_scale > 0.0f))
    {
        SetError(error, error_capacity, "direct presentation surface has an empty extent or scale");
        return 0;
    }
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_prepare_owner_surface(
    RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    if (!rrv_pcsx2_gs_bridge_validate_linux_surface(surface, error, error_capacity))
        return 0;
    surface->flags |= RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD;
    std::lock_guard<std::mutex> lock(s_preparation_mutex);
    s_prepared_surface = *surface;
    return 1;
}

extern "C" int rrv_pcsx2_gs_bridge_consume_owner_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    std::lock_guard<std::mutex> lock(s_preparation_mutex);
    if (!s_prepared_surface || !surface ||
        s_prepared_surface->kind != surface->kind ||
        s_prepared_surface->native_view != surface->native_view ||
        s_prepared_surface->native_layer != surface->native_layer ||
        s_prepared_surface->width_pixels != surface->width_pixels ||
        s_prepared_surface->height_pixels != surface->height_pixels ||
        s_prepared_surface->backing_scale != surface->backing_scale)
    {
        SetError(error, error_capacity, "GS owner surface has no matching preparation");
        return 0;
    }
    s_prepared_surface.reset();
    return 1;
}

extern "C" void rrv_pcsx2_gs_bridge_owner_command(
    RrvPcsx2GsBridgeOwnerCommand command, void* context)
{
    if (command)
        command(context);
}

// PCSX2's Vulkan device never dispatches attach/detach to a platform main
// thread, so there is nothing to service. Returns 1 ("serviced") so callers
// that loop on it while awaiting owner create/destroy behave as on macOS.
extern "C" int rrv_pcsx2_gs_bridge_pump_main_thread()
{
    return 1;
}
