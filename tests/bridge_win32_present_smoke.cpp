// Manual smoke test for the Windows PCSX2 GS bridge: opens a plain SDL window, creates a direct-present Vulkan bridge on
// its HWND (the surface the product passes), drives a few field transitions and reports the renderer and present count.
//
//   bridge_win32_present_smoke.exe <path to rrv-pcsx2-gs-bridge.dll> [seconds]
//
// It loads the DLL the way the product's package reader does (LoadLibraryEx with LOAD_WITH_ALTERED_SEARCH_PATH, so the
// staged dependency DLLs beside it resolve). Needs a GPU with a Vulkan driver and an interactive desktop; it is not a
// CTest test. Exit codes: 0 ok, 1 usage or load failure, 2 bridge creation failed, 3 no present was counted.
#include "rrv_pcsx2_gs_bridge.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

template <typename Function>
Function resolve(HMODULE module, const char* name)
{
    const auto address = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!address)
        std::fprintf(stderr, "bridge export missing: %s\n", name);
    return address;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <rrv-pcsx2-gs-bridge.dll> [seconds]\n", argv[0]);
        return 1;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 3.0;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("bridge smoke", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
    {
        std::fprintf(stderr, "SDL_GetWindowWMInfo: %s\n", SDL_GetError());
        return 1;
    }

    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    std::wstring path(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path.data(), wide_length);
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        std::fprintf(stderr, "LoadLibraryEx failed: %lu\n", GetLastError());
        return 1;
    }
    const auto version = resolve<uint32_t (*)()>(module, "rrv_pcsx2_gs_bridge_version");
    const auto create = resolve<RrvPcsx2GsBridge* (*)(const RrvPcsx2GsBridgeConfig*, RrvPcsx2GsBridgeCapabilities*,
                                                      char*, uint32_t)>(module, "rrv_pcsx2_gs_bridge_create");
    const auto destroy = resolve<void (*)(RrvPcsx2GsBridge*)>(module, "rrv_pcsx2_gs_bridge_destroy");
    const auto vsync = resolve<int (*)(RrvPcsx2GsBridge*, const uint64_t*, uint64_t, uint32_t, char*, uint32_t)>(
        module, "rrv_pcsx2_gs_bridge_vsync");
    const auto stats = resolve<int (*)(RrvPcsx2GsBridge*, RrvPcsx2GsBridgeStats*, char*, uint32_t)>(
        module, "rrv_pcsx2_gs_bridge_get_stats");
    if (!version || !create || !destroy || !vsync || !stats || version() != RRV_PCSX2_GS_BRIDGE_ABI_VERSION)
        return 1;

    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    RrvPcsx2GsBridgeConfig config{};
    config.struct_size = sizeof(config);
    config.gs_render_mode = RRV_PCSX2_GS_RENDER_MODE_FIELD;
    config.presentation_mode = RRV_PCSX2_GS_PRESENTATION_DIRECT_GPU;
    config.renderer_kind = RRV_PCSX2_GS_RENDERER_VULKAN;
    config.surface.struct_size = sizeof(config.surface);
    config.surface.kind = RRV_PCSX2_GS_SURFACE_WIN32;
    config.surface.flags = RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES;
    config.surface.native_view = info.info.win.window;
    config.surface.native_layer = info.info.win.hinstance;
    config.surface.width_pixels = static_cast<uint32_t>(width);
    config.surface.height_pixels = static_cast<uint32_t>(height);
    config.surface.backing_scale = 1.0f;
    RrvPcsx2GsBridgeCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    std::array<char, 1024> error{};
    RrvPcsx2GsBridge* bridge = create(&config, &capabilities, error.data(), static_cast<uint32_t>(error.size()));
    if (!bridge)
    {
        std::fprintf(stderr, "bridge create failed: %s\n", error.data());
        return 2;
    }
    std::printf("renderer=%s direct_present_capable=%d\n", capabilities.renderer_name ? capabilities.renderer_name : "?",
                (capabilities.capability_flags & RRV_PCSX2_GS_CAP_DIRECT_PRESENT) != 0);

    RrvPcsx2GsPrivRegs regs{};  // no display circuits enabled: a blank field
    const auto start = std::chrono::steady_clock::now();
    uint64_t field = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
        }
        if (!vsync(bridge, regs, field, static_cast<uint32_t>(field & 1u), error.data(),
                   static_cast<uint32_t>(error.size())))
        {
            std::fprintf(stderr, "vsync failed: %s\n", error.data());
            destroy(bridge);
            return 2;
        }
        ++field;
        SDL_Delay(16);
    }
    RrvPcsx2GsBridgeStats result{};
    result.struct_size = sizeof(result);
    stats(bridge, &result, error.data(), static_cast<uint32_t>(error.size()));
    std::printf("fields=%llu presented_frames=%llu direct_gpu_presents=%llu\n", static_cast<unsigned long long>(field),
                static_cast<unsigned long long>(result.presented_frames),
                static_cast<unsigned long long>(result.direct_gpu_presents));
    destroy(bridge);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result.direct_gpu_presents > 0 ? 0 : 3;
}
