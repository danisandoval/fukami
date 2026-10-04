# Product PS2 runtime built straight from the committed third_party/ps2recomp source
# (source ownership, Phase 3). This replaces the build-local "product SDL host overlay"
# CMake file (scripts/product_host_overlay.py) together with the configure-time source
# overlays: their effect is now in the committed runtime sources themselves.
#
# Included by cmake/RrvLegacyLive.cmake when RRV_PRODUCT_OWNED_SOURCE is ON.
# Expects: RRV_PS2RECOMP_SOURCE_DIR, PS2X_RRV_FIELD_ONLY, PS2X_RRV_EXTENSION_SOURCE_DIR and the
# rrv_sdl_product_audio target.
cmake_minimum_required(VERSION 3.20)
project(RrvProductRuntime LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT PS2X_RRV_FIELD_ONLY OR NOT TARGET rrv_sdl_product_audio)
    message(FATAL_ERROR "SDL product runtime requires the field backend and product audio target")
endif()
set(RRV_RT "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime")
add_library(ps2_runtime STATIC
    "${RRV_RT}/src/lib/game_overrides.cpp"
    "${RRV_RT}/src/lib/ps2_gif_arbiter.cpp"
    "${RRV_RT}/src/lib/ps2_audio.cpp"
    "${RRV_RT}/src/lib/ps2_audio_vag.cpp"
    "${RRV_RT}/src/lib/ps2_gs_gpu.cpp"
    "${RRV_RT}/src/lib/ps2_gs_memory.cpp"
    "${RRV_RT}/src/lib/ps2_gs_rasterizer.cpp"
    "${RRV_RT}/src/lib/ps2_iop.cpp"
    "${RRV_RT}/src/lib/ps2_iop_audio.cpp"
    "${RRV_RT}/src/lib/ps2_iop_dbcman.cpp"
    "${RRV_RT}/src/lib/ps2_memory.cpp"
    "${RRV_RT}/src/lib/ps2_pad.cpp"
    "${RRV_RT}/src/lib/ps2_runtime.cpp"
    "${RRV_RT}/src/lib/ps2_vif1_interpreter.cpp"
    "${RRV_RT}/src/lib/ps2_vif0_interpreter.cpp"
    "${RRV_RT}/src/lib/ps2_vu1.cpp"
    "${RRV_RT}/src/lib/games_database.cpp"
)
file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS "${RRV_RT}/src/lib/Kernel/*.cpp")
target_sources(ps2_runtime PRIVATE ${KERNEL_SRC_FILES})
# The PS2 VU FMAC is not a fused multiply-add (docs/TESTING.md T-P7-VUSTEP).
set_source_files_properties("${RRV_RT}/src/lib/ps2_vu1.cpp" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
target_include_directories(ps2_runtime PUBLIC "${RRV_RT}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${RRV_RT}/src/lib/Kernel"
    "${RRV_RT}/src/lib/Kernel/Syscalls")
# Live RRV sources and game-derived data are included in place (cmake/RrvProductOwnedIncludes.cmake).
# src/guest-time comes last: the runtime's own copies of three headers must win.
target_include_directories(ps2_runtime PUBLIC "${CMAKE_SOURCE_DIR}/src/guest-time")
target_include_directories(ps2_runtime PRIVATE "${CMAKE_SOURCE_DIR}/src/vu-aot"
    "${RRV_GENERATED_INCLUDE_DIR}/vu")
target_compile_definitions(ps2_runtime PRIVATE PS2X_RRV_FIELD_ONLY=1)
target_include_directories(ps2_runtime PRIVATE
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/gs-backend
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/gs-record
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/snapshot
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/ir
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/host
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/diag
    ${PS2X_RRV_EXTENSION_SOURCE_DIR}/src/vu-jit
)
target_link_libraries(ps2_runtime PUBLIC rrv_sdl_product_audio)
include("${RRV_RT}/cmake/ReleaseMode.cmake")
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
endif()
