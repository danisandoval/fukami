#include "rrv_pcsx2_gs_bridge.h"
#include "rrv_resource_package.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

template <typename Function>
Function required(rrv::resource_package::Package& package, const char* name)
{
    return reinterpret_cast<Function>(package.symbol(name));
}

int fail(const std::string& message)
{
    std::cerr << "ADR0006_METAL_SMOKE_FAIL " << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    try
    {
        auto package = rrv::resource_package::Package::Open(
            rrv::resource_package::rrv_resource_package_binding);
        package.open_bridge();
        const auto version = required<uint32_t (*)()>(package, "rrv_pcsx2_gs_bridge_version");
        const auto create = required<RrvPcsx2GsBridge* (*)(
            const RrvPcsx2GsBridgeConfig*, RrvPcsx2GsBridgeCapabilities*, char*, uint32_t)>(
                package, "rrv_pcsx2_gs_bridge_create");
        const auto destroy = required<void (*)(RrvPcsx2GsBridge*)>(
            package, "rrv_pcsx2_gs_bridge_destroy");
        if (version() != RRV_PCSX2_GS_BRIDGE_ABI_VERSION)
            return fail("unexpected bridge ABI");

        const auto bridge = package.bridge_path();
        const auto loaded = package.loaded_image_path();
        if (!std::filesystem::equivalent(bridge, loaded))
            return fail("dladdr image is outside the verified package bridge");
        for (const char* name : {"resources/default.metallib", "resources/Metal22.metallib", "resources/Metal23.metallib"})
        {
            if (!std::filesystem::is_regular_file(package.resource_path(name)))
                return fail(std::string("verified package omits ") + name);
        }

        RrvPcsx2GsBridgeConfig config{};
        config.struct_size = sizeof(config);
        config.snapshot_width = 64u;
        config.snapshot_height = 64u;
        config.gs_render_mode = RRV_PCSX2_GS_RENDER_MODE_FIELD;
        config.presentation_mode = RRV_PCSX2_GS_PRESENTATION_LEGACY_CPU_SNAPSHOT;
        config.renderer_kind = RRV_PCSX2_GS_RENDERER_METAL;
        RrvPcsx2GsBridgeCapabilities capabilities{};
        capabilities.struct_size = sizeof(capabilities);
        std::array<char, 1024> error{};
        RrvPcsx2GsBridge* instance = create(
            &config, &capabilities, error.data(), static_cast<uint32_t>(error.size()));
        if (!instance)
            return fail(std::string("Metal bridge initialization failed: ") + error.data());
        destroy(instance);
        std::cout << "ADR0006_METAL_SMOKE package_bridge=" << bridge << '\n'
                  << "ADR0006_METAL_SMOKE dladdr_image=" << loaded << '\n'
                  << "ADR0006_METAL_SMOKE renderer="
                  << (capabilities.renderer_name ? capabilities.renderer_name : "<none>") << '\n';
        return 0;
    }
    catch (const std::exception& failure)
    {
        return fail(failure.what());
    }
}
