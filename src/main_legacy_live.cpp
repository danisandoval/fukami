#include "ps2_runtime.h"
#include "register_functions.h"
#include "games_database.h"
#include "patches.h"
#include "rrv_gs_record_hooks.h"
#include "rrv_gs_backend.h"
#include "rrv_m2_neutrality.h"
#include "rrv_m2_causal_trace.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <vector>

namespace
{
void printProvenance()
{
    std::cerr
        << "[m0r-provenance] target=rrv-legacy-live\n"
        << "[m0r-provenance] RRV commit=" << RRV_SOURCE_COMMIT
        << " source-clean=" << RRV_SOURCE_CLEAN << "\n"
        << "[m0r-provenance] PS2Recomp name=" << RRV_PRODUCER_NAME << "\n"
        << "[m0r-provenance] PS2Recomp commit=" << RRV_PRODUCER_COMMIT
        << " tree=" << RRV_PRODUCER_TREE
        << " upstream-base=" << RRV_PRODUCER_BASE << "\n"
        << "[m0r-provenance] PCSX2 commit=" << RRV_PCSX2_COMMIT
        << " bridge-patch-sha256=" << RRV_BRIDGE_PATCH_SHA256 << "\n"
        << "[m0r-provenance] bridge-ABI=5 render-mode=field\n"
        << "[m0r-provenance] generation RRV commit=" << RRV_GENERATION_RRV_COMMIT
        << " producer-commit=" << RRV_GENERATION_PRODUCER_COMMIT
        << " manifest-sha256=" << RRV_GENERATION_MANIFEST_SHA256 << "\n"
        << "[m0r-provenance] hook-fingerprint=" << RRV_GENERATION_HOOK_FINGERPRINT << "\n"
        << "[m0r-provenance] bridge-manifest-sha256=" << RRV_BRIDGE_MANIFEST_SHA256
        << " library-sha256=" << RRV_BRIDGE_LIBRARY_SHA256 << "\n"
        << "[m0r-provenance] Metal-source-manifest-sha256="
        << RRV_METAL_SOURCE_MANIFEST_SHA256 << "\n"
        << "[m0r-provenance] presentation=diagnostic CPU RGBA snapshot\n"
        << "[m0r-provenance] F7/P3 linked=no\n";
}

bool snapshotActiveGsVram(void *context, uint8_t *out, uint32_t byteCount)
{
    auto *runtime = static_cast<PS2Runtime *>(context);
    if (!runtime || !out || byteCount != PS2_GS_VRAM_SIZE)
        return false;
    std::vector<uint8_t> snapshot;
    if (!runtime->snapshotActiveGsBackendLocalMemory(snapshot) ||
        snapshot.size() != byteCount)
        return false;
    std::memcpy(out, snapshot.data(), byteCount);
    return true;
}
}

int main(int argc, char **argv)
{
    if (argc != 2 || std::string(argv[1]) == "--help")
    {
        std::cerr << "usage: " << argv[0] << " <user-owned-boot-elf>\n";
        return argc == 2 ? 0 : 2;
    }

    printProvenance();
    try
    {
        if (!rrv::m2causal::initializeFromEnvironment())
            throw std::runtime_error("M2 causal trace initialization failed");
        const std::filesystem::path elf = std::filesystem::absolute(argv[1]);
        const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
        std::filesystem::current_path(executable.parent_path());
        PS2Runtime::IoPaths paths;
        paths.elfDirectory = elf.parent_path();
        paths.cdRoot = elf.parent_path();
        PS2Runtime::setIoPaths(paths);

        const std::string elfName = elf.filename().string();
        const char *gameName = getGameName(elfName.c_str());
        std::string title = "RRV legacy live | ";
        title += gameName ? gameName : elfName;

        PS2Runtime runtime;
        if (!runtime.initialize(title.c_str()))
        {
            std::cerr << "[rrv-legacy-live] runtime initialization failed\n";
            return 1;
        }
        if (!runtime.activeGsBackend())
        {
            std::cerr << "[rrv-legacy-live] PCSX2 GS bridge is not active\n";
            return 1;
        }
#if defined(RRV_LEGACY_REQUIRE_METAL)
        if (rrv::gsbackend::activeRendererKind() !=
            rrv::gsbackend::RendererKind::Metal)
        {
            std::cerr << "[rrv-legacy-live] actual post-GSopen renderer is not Metal\n";
            return 1;
        }
#endif

        registerAllFunctions(runtime);
        registerPatches(runtime);
        const uint64_t *privRegs19 =
            reinterpret_cast<const uint64_t *>(&runtime.memory().gs());
        rrv::gsrecord::rrv_gs_record_init_from_env(
            runtime.memory().getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE),
            privRegs19, snapshotActiveGsVram, &runtime);

        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "[rrv-legacy-live] failed to load user ELF\n";
            return 1;
        }
        std::cerr << "[m0r-provenance] verified-generated-compatible-code=executing\n";
        bool m2NeutralityOk = true;
        if (rrv::m2neutral::enabled())
        {
            std::cerr << "[m2-test] semantic-edge bounded legacy execution enabled\n";
            std::jthread boundedTest([&runtime](std::stop_token stop) {
                while (!stop.stop_requested())
                {
                    if (rrv::m2neutral::intervalComplete())
                    {
                        runtime.requestStop();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            runtime.run();
            boundedTest.request_stop();
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
        else
        {
            runtime.run();
        }
        rrv::gsrecord::rrv_gs_record_shutdown();
        if (!m2NeutralityOk)
            return 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << "[rrv-legacy-live] fatal: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
