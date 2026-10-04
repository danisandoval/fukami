// SPDX-License-Identifier: MIT
#include "rrv_m2_neutrality.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
void setEnvironment(const char *name, const std::string &value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void storeLe64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index != 8u; ++index)
        bytes[index] = static_cast<uint8_t>(value >> (index * 8u));
}

std::array<uint8_t, 32u> packedVertex(uint16_t x, uint16_t y, uint32_t z)
{
    std::array<uint8_t, 32u> packet{};
    storeLe64(packet.data(), 1u | (1ull << 60u));
    storeLe64(packet.data() + 8u, 0x05u);
    storeLe64(packet.data() + 16u, static_cast<uint64_t>(x) |
                                      (static_cast<uint64_t>(y) << 32u));
    storeLe64(packet.data() + 24u, z);
    return packet;
}

} // namespace

int main(int argc, char **argv)
{
    const bool failedCaptureControl = argc == 2 &&
        std::string(argv[1]) == "--failed-capture";
    if (argc != 1 && !failedCaptureControl)
        return 1;
    const std::filesystem::path output =
        std::filesystem::current_path() / "m2-neutrality-control-test.json";
    const std::filesystem::path captureOutput =
        std::filesystem::current_path() / "m2-neutrality-control-test.ppm";
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    std::filesystem::remove(captureOutput, ignored);
    setEnvironment("RRV_M2_NEUTRALITY_MANIFEST", output.string());
    setEnvironment("RRV_M2_NEUTRALITY_ANCHOR", "39:1:2");
    setEnvironment("RRV_M2_NEUTRALITY_EDGE", "selector-20005");
    setEnvironment("RRV_M2_NEUTRALITY_FIELDS", "2");
    if (failedCaptureControl)
        setEnvironment("RRV_M2_NEUTRALITY_CAPTURE", captureOutput.string());
    if (!rrv::m2neutral::enabled())
        return 1;

    // An initial target selector and a broad expected metadata level both
    // remain insufficient: only a later selector edge can arm this control.
    rrv::m2neutral::observeSemanticState(7u, 9u, 11u, 0x00020005u, 90u);
    std::string error;
    if (rrv::m2neutral::dumpDeferred(&error) || std::filesystem::exists(output))
        return 1;
    rrv::m2neutral::observeSemanticState(39u, 1u, 2u, 0x00020005u, 91u);
    const uint64_t emptyPaths[3]{};
    const auto priorField = packedVertex(0x0a0au, 0x0b0bu, 0x0c0c0c0cu);
    rrv::m2neutral::noteSubmittedPacket(1u, priorField.data(), priorField.size());
    rrv::m2neutral::noteCompletedField(99u, 7u, 1u, emptyPaths, true, 10u, nullptr);

    // Metadata remains descriptive rather than an arm authority. This field
    // has transfers on both sides of the selector edge and is deliberately
    // discarded at its successful boundary VSync.
    rrv::m2neutral::observeSemanticState(7u, 9u, 11u, 0x00000000u, 92u);
    const auto originPreEdge = packedVertex(4u, 5u, 6u);
    rrv::m2neutral::noteSubmittedPacket(
        1u, originPreEdge.data(), originPreEdge.size());
    rrv::m2neutral::observeSemanticState(7u, 9u, 11u, 0x00020005u, 93u);
    const auto discarded = packedVertex(1u, 2u, 3u);
    rrv::m2neutral::noteSubmittedPacket(1u, discarded.data(), discarded.size());
    rrv::m2neutral::noteCompletedField(100u, 8u, 0u, emptyPaths, true, 11u, nullptr);
    error.clear();
    if (rrv::m2neutral::dumpDeferred(&error) || std::filesystem::exists(output))
        return 1;

    // A second edge after collection started must not replace the first origin.
    rrv::m2neutral::observeSemanticState(39u, 1u, 2u, 0u, 94u);
    rrv::m2neutral::observeSemanticState(39u, 1u, 2u, 0x00020005u, 95u);

    const auto firstComplete = packedVertex(0x1234u, 0x5678u, 0x89abcdefu);
    rrv::m2neutral::noteSubmittedPacket(1u, firstComplete.data(), firstComplete.size());
    const uint64_t firstPaths[3]{1u, 0u, 0u};
    rrv::m2neutral::noteCompletedField(101u, 9u, 1u, firstPaths, true, 12u, nullptr);
    const auto secondComplete = packedVertex(0x2222u, 0x3333u, 0x44444444u);
    rrv::m2neutral::noteSubmittedPacket(1u, secondComplete.data(), secondComplete.size());
    rrv::m2neutral::noteCompletedField(102u, 10u, 0u, firstPaths, true, 13u, nullptr);
    if (!rrv::m2neutral::intervalComplete() ||
        rrv::m2neutral::lastGuestFieldId() != 102u)
    {
        return 1;
    }
    if (!rrv::m2neutral::dumpDeferred(&error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::ifstream stream(output);
    const std::string json((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    std::filesystem::remove(output, ignored);
    std::filesystem::remove(captureOutput, ignored);
    const bool valid = json.find("\"field_count\": 2") != std::string::npos &&
        json.find("\"schema\": 3") != std::string::npos &&
        json.find("\"previous_selector\": \"00000000\"") != std::string::npos &&
        json.find("\"current_selector\": \"00020005\"") != std::string::npos &&
        json.find("\"observation_guest_tick_id\": 93") != std::string::npos &&
        json.find("\"origin_boundary_vsync\": {\"observed\": true, \"mixed_epoch_discarded\": true, \"guest_field_id\": 100") != std::string::npos &&
        json.find("\"ordinal\": 0") != std::string::npos &&
        json.find("\"relative_guest_field_id\": 0") != std::string::npos &&
        json.find("\"relative_guest_field_id\": 1") != std::string::npos &&
        json.find("\"guest_field_id\": 101") != std::string::npos &&
        json.find("\"guest_tick_id\": 101") != std::string::npos &&
        json.find("\"geometry_vertex_count\": 1") != std::string::npos &&
        json.find("\"geometry_vertex_count\": 2") == std::string::npos &&
        json.find("\"consumer_boundary_ordering\": {\"available\": false") != std::string::npos &&
        json.find(failedCaptureControl ?
            "\"deferred_capture\": {\"requested\": true, \"succeeded\": false" :
            "\"deferred_capture\": {\"requested\": false, \"succeeded\": true") != std::string::npos &&
        json.find("\"complete\": true") != std::string::npos;
    return valid ? 0 : 1;
}
