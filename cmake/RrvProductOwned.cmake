# Source-owned product (RRV_PRODUCT_OWNED_SOURCE): the shipping executable.
#
# `Fukami` (the process name the user sees; `Fukami` until 2026-10-02) is written to
# candidate-bin/, the directory the packaging scripts read. It is rrv-product plus the Gate-8 audio sources, built
# from the committed src/product, third_party/ps2recomp and generated/rr5 sources: there is no
# stage directory, no Python overlay and no per-translation-unit swap.
if(NOT TARGET rrv-product OR NOT TARGET ps2_runtime)
    message(FATAL_ERROR "Source-owned product requires the normal product graph")
endif()

# Every consumer of the runtime headers must see the one PS2Runtime layout (the runtime's own
# include tree, then the build-local copies of live sources); mirrors what the stage-era
# candidate did with its stage include directories.
include_directories(BEFORE
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls")
# Targets that include ps2_runtime.h without linking ps2_runtime still need the live guest-time
# headers it names; appended, so the runtime's own headers above always win.
include_directories(AFTER "${CMAKE_SOURCE_DIR}/src/guest-time")
target_include_directories(rrv_legacy_game_funcs BEFORE PUBLIC
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/include")
target_include_directories(rrv_legacy_game_funcs BEFORE PUBLIC "${RRV_GENERATED_SOURCE_DIR}")
target_include_directories(rrv-product BEFORE PRIVATE "${RRV_GENERATED_SOURCE_DIR}")
target_include_directories(rrv-product PRIVATE "${RRV_GENERATED_INCLUDE_DIR}/native")

get_target_property(_rrv_product_sources rrv-product SOURCES)
add_executable(Fukami ${_rrv_product_sources})
foreach(_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS
        COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
    get_target_property(_value rrv-product ${_property})
    if(_value)
        set_property(TARGET Fukami PROPERTY ${_property} "${_value}")
    endif()
endforeach()
set_target_properties(Fukami PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/candidate-bin")
if(WIN32)
    # The GUI subsystem is a target property, not one of the copied lists above (cmake/RrvFukamiApp.cmake sets it).
    get_target_property(_rrv_product_gui rrv-product WIN32_EXECUTABLE)
    if(_rrv_product_gui)
        set_target_properties(Fukami PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    # Windows finds SDL2.dll only beside the executable (or on PATH): stage it in candidate-bin, where the product runs
    # from the build tree. The packaged app ships the same file beside Fukami.exe (scripts/package_fukami_windows.py).
    add_custom_command(TARGET Fukami POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:Fukami>"
        VERBATIM)
    # rrv-disc-extract links libchdr against the dependency prefix's zlib and zstd DLLs (cmake/RrvDiscExtract.cmake).
    add_custom_command(TARGET Fukami POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RRV_DISC_RUNTIME_DLLS} "$<TARGET_FILE_DIR:Fukami>"
        VERBATIM)
endif()
add_dependencies(Fukami rrv-resource-package-producer)

# Gate 8 audio: PCSX2 SPU2 core (tools/pcsx2-spu2) and the host rewrite of RR5's IOP sound
# driver (src/audio). Only when the build names the pinned PCSX2 source.
if(RRV_PCSX2_SOURCE_DIR)
    target_sources(Fukami PRIVATE "${CMAKE_SOURCE_DIR}/src/audio/rspu2_driver.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio/rspu2_glue.cpp")
    target_link_libraries(Fukami PRIVATE rrv_pcsx2_spu2)
    target_compile_definitions(Fukami PRIVATE RRV_GATE8_SPU2=1)
    message(STATUS "Gate8 audio: PCSX2 SPU2 from ${RRV_PCSX2_SOURCE_DIR}")
endif()
message(STATUS "Source-owned product: runtime ${RRV_PS2RECOMP_SOURCE_DIR}, game code ${RRV_GENERATED_SOURCE_DIR}")
