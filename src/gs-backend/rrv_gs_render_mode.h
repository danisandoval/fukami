#ifndef RRV_GS_RENDER_MODE_H
#define RRV_GS_RENDER_MODE_H

// Renderer-independent GS presentation policy.  This deliberately contains no
// PCSX2, Metal, Vulkan, or host-window types: mode selection belongs above any
// individual renderer implementation.

#include <atomic>
#include <cstdint>

namespace rrv::gs
{
    enum class GsRenderMode : uint32_t
    {
        Field = 0u,
        FullFrame = 1u,
    };

    constexpr uint32_t renderModeCapability(GsRenderMode mode)
    {
        return 1u << static_cast<uint32_t>(mode);
    }

    constexpr const char *renderModeName(GsRenderMode mode)
    {
        switch (mode)
        {
            case GsRenderMode::Field: return "field";
            case GsRenderMode::FullFrame: return "full";
        }
        return "unknown";
    }

    // Capture is initialized after the GS backend.  Keep the already-parsed
    // choice here so .gsr/.gstrace writers do not read RRV_GS_RENDER_MODE a
    // second time. Zero is Field, which also keeps older reserved header bytes
    // backward-compatible.
    inline std::atomic<uint32_t> g_captureRenderMode{
        static_cast<uint32_t>(GsRenderMode::Field)};

    inline void publishCaptureRenderMode(GsRenderMode mode)
    {
        g_captureRenderMode.store(static_cast<uint32_t>(mode), std::memory_order_release);
    }

    inline GsRenderMode captureRenderMode()
    {
        const uint32_t value = g_captureRenderMode.load(std::memory_order_acquire);
        return value == static_cast<uint32_t>(GsRenderMode::FullFrame)
            ? GsRenderMode::FullFrame
            : GsRenderMode::Field;
    }
} // namespace rrv::gs

#endif // RRV_GS_RENDER_MODE_H
