#ifndef RRV_SDL_PRESENTATION_H
#define RRV_SDL_PRESENTATION_H

#include <memory>
#include <string>

#include "rrv_gs_backend.h"

class HostPadBackend;

namespace rrv::host
{
// SDL is RRV's sole product-platform shell. This header remains portable: the
// bridge sees only opaque SDL_MetalView and Metal-layer handles below.
class SdlPresentation
{
public:
    SdlPresentation();
    ~SdlPresentation();

    SdlPresentation(const SdlPresentation &) = delete;
    SdlPresentation &operator=(const SdlPresentation &) = delete;

    bool create(const char *title, std::string *error);
    void destroy();

    void pumpEvents();
    bool shouldClose() const;
    bool currentSurface(rrv::gsbackend::NativeSurface *surface) const;
    std::shared_ptr<HostPadBackend> padBackend() const;
    bool lifecycleTestEnabled() const;
    bool lifecycleTestComplete(std::string *error) const;

    // Function-pointer forms keep PS2Runtime free of SDL and platform types.
    static void pumpCallback(void *context);
    static bool closeCallback(void *context);
    static bool surfaceCallback(void *context,
                                rrv::gsbackend::NativeSurface *surface);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace rrv::host

#endif // RRV_SDL_PRESENTATION_H
