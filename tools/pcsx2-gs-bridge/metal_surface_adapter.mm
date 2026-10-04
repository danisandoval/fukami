// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS-only validation for the portable bridge ABI. This is intentionally the
// only bridge source that validates Objective-C/Metal types; callers pass the
// SDL-created view/layer as opaque handles and retain them through bridge
// destruction. This adapter creates no application, window, or view.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rrv_pcsx2_gs_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>

namespace
{
// PCSX2 exposes one process-global GS. Keep one pending preparation receipt;
// the flag alone must not let a caller bypass main-thread validation.
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
}

extern "C" int rrv_pcsx2_gs_bridge_validate_macos_surface(
    void* native_view, void* native_layer, char* error, uint32_t error_capacity)
{
    @autoreleasepool
    {
        id view = (__bridge id)native_view;
        id layer = (__bridge id)native_layer;
        if (!view || ![view isKindOfClass:[NSView class]])
        {
            SetError(error, error_capacity, "direct presentation native_view is not an NSView");
            return 0;
        }
        if (!layer || ![layer isKindOfClass:[CAMetalLayer class]])
        {
            SetError(error, error_capacity,
                     "direct presentation native_layer is not a CAMetalLayer");
            return 0;
        }
        return 1;
    }
}

extern "C" int rrv_pcsx2_gs_bridge_is_macos_main_thread()
{
    return [NSThread isMainThread] ? 1 : 0;
}

extern "C" int rrv_pcsx2_gs_bridge_prepare_owner_surface(
    RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    @autoreleasepool
    {
        if (![NSThread isMainThread] || !surface ||
            surface->struct_size < sizeof(*surface) ||
            surface->kind != RRV_PCSX2_GS_SURFACE_MACOS_VIEW_METAL_LAYER ||
            (surface->flags & (RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES |
                               RRV_PCSX2_GS_SURFACE_FLAG_MAIN_THREAD_PREPARED)) !=
                (RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES |
                 RRV_PCSX2_GS_SURFACE_FLAG_MAIN_THREAD_PREPARED))
        {
            SetError(error, error_capacity, "GS owner surface requires SDL preparation on main");
            return 0;
        }
        if (!rrv_pcsx2_gs_bridge_validate_macos_surface(
                surface->native_view, surface->native_layer, error, error_capacity))
            return 0;
        NSView* view = (__bridge NSView*)surface->native_view;
        if ([view layer] != (__bridge CAMetalLayer*)surface->native_layer)
        {
            SetError(error, error_capacity, "GS owner surface layer is not attached to the SDL view");
            return 0;
        }
        surface->flags |= RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD;
        std::lock_guard<std::mutex> lock(s_preparation_mutex);
        s_prepared_surface = *surface;
        return 1;
    }
}

extern "C" int rrv_pcsx2_gs_bridge_consume_owner_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    std::lock_guard<std::mutex> lock(s_preparation_mutex);
    if (!s_prepared_surface ||
        s_prepared_surface->native_view != surface->native_view ||
        s_prepared_surface->native_layer != surface->native_layer ||
        s_prepared_surface->width_pixels != surface->width_pixels ||
        s_prepared_surface->height_pixels != surface->height_pixels ||
        s_prepared_surface->backing_scale != surface->backing_scale)
    {
        SetError(error, error_capacity, "GS owner surface has no matching main-thread preparation");
        return 0;
    }
    s_prepared_surface.reset();
    return 1;
}

extern "C" void rrv_pcsx2_gs_bridge_owner_command(
    RrvPcsx2GsBridgeOwnerCommand command, void* context)
{
    @autoreleasepool
    {
        if (command)
            command(context);
    }
}

extern "C" int rrv_pcsx2_gs_bridge_pump_main_thread()
{
    if (![NSThread isMainThread])
        return 0;
    @autoreleasepool
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
    }
    return 1;
}
