# Built by scripts/build_pcsx2_gs_bridge.sh inside a clean detached PCSX2
# worktree.  `pcsx2-gsrunner/Main.cpp` supplies the deliberately minimal Host
# implementation required by PCSX2's GS/device code; it is upstream GPL-3.0+.
add_library(rrv-pcsx2-gs-bridge SHARED
    bridge.cpp
    gsrunner_host.cpp
)

option(RRV_PCSX2_GS_CANONICAL_RECEIPTS
    "Compile bounded M2 consumer-side receipt support into the diagnostic bridge"
    OFF)
if(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    target_sources(rrv-pcsx2-gs-bridge PRIVATE canonical_receipt.cpp)
    target_compile_definitions(rrv-pcsx2-gs-bridge PRIVATE
        RRV_PCSX2_GS_CANONICAL_RECEIPTS=1)
endif()

if(APPLE)
    target_sources(rrv-pcsx2-gs-bridge PRIVATE metal_surface_adapter.mm)
    # PCSX2's target PCH is generated as C++; Clang cannot consume it while
    # compiling Objective-C++. This small adapter is the only ObjC++ source.
    set_source_files_properties(metal_surface_adapter.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
else()
    # Linux (Gate 5): X11/Wayland surface checks + owner-thread sideband for
    # PCSX2's Vulkan renderer. Plain C++; no windowing headers or libraries.
    target_sources(rrv-pcsx2-gs-bridge PRIVATE linux_surface_adapter.cpp)
    # PCSX2 sends every Linux library to <build>/bin; keep the bridge (and its
    # staged resources) at <build>/pcsx2-gs-bridge as on macOS, where
    # scripts/build_pcsx2_gs_bridge.sh and the manifest expect it.
    set_target_properties(rrv-pcsx2-gs-bridge PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

target_include_directories(rrv-pcsx2-gs-bridge
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE
        "${CMAKE_BINARY_DIR}/common/include"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(rrv-pcsx2-gs-bridge PRIVATE PCSX2_FLAGS PCSX2)
target_compile_definitions(rrv-pcsx2-gs-bridge PRIVATE RRV_PCSX2_GS_BRIDGE_BUILD=1)
set_target_properties(rrv-pcsx2-gs-bridge PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# GSopen(SW) still initializes PCSX2's font/shader presentation plumbing.
# Stage the pinned redistributable PCSX2 resources beside the optional bridge;
# runtime discovery is relative to the loaded library, not the RRV executable.
add_custom_command(TARGET rrv-pcsx2-gs-bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/bin/resources"
            "$<TARGET_FILE_DIR:rrv-pcsx2-gs-bridge>/resources"
    VERBATIM
)

if(APPLE)
    foreach(metal_library IN ITEMS default.metallib Metal22.metallib Metal23.metallib)
        if(NOT EXISTS "${RRV_PCSX2_GS_METAL_RESOURCE_DIR}/${metal_library}")
            message(FATAL_ERROR
                "Missing ${metal_library}; set RRV_PCSX2_GS_METAL_RESOURCE_DIR "
                "to the Contents/Resources directory of the pinned PCSX2.app build")
        endif()
        add_custom_command(TARGET rrv-pcsx2-gs-bridge POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${RRV_PCSX2_GS_METAL_RESOURCE_DIR}/${metal_library}"
                    "$<TARGET_FILE_DIR:rrv-pcsx2-gs-bridge>/resources/${metal_library}"
            VERBATIM
        )
    endforeach()
endif()
