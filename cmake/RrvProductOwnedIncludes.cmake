# Source-owned product (RRV_PRODUCT_OWNED_SOURCE): include roots and the Gate-8 audio library.
#
# No build step edits or copies source. The runtime includes the live RRV sources and the game-derived
# data in place, through include directories:
#   src/guest-time                      rrv_guest_time/rrv_guest_rtc/rrv_ee_timers (.h and .cpp)
#   src/vu-aot                          rrv_vu_aot_engine.inc
#   ${RRV_GENERATED_INCLUDE_DIR}/vu     rrv_vu_aot_blocks.inc      (game-derived, private repository only)
#   ${RRV_GENERATED_INCLUDE_DIR}/native rrv_ee_native.inc          (game-derived, included by src/product/patches.cpp)
# src/guest-time also holds older variants of three headers that the runtime keeps its own, newer
# versions of in third_party/ps2recomp (rrv_guest_scheduler.h, rrv_intc.h, rrv_temporal_owner.h):
# it is always searched AFTER the runtime's include directory.
set(RRV_GENERATED_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/generated/rr5" CACHE PATH
    "Directory holding the game-derived vu/ and native/ include sources (private repository only)")
foreach(_dir IN ITEMS "${RRV_GENERATED_INCLUDE_DIR}/vu" "${RRV_GENERATED_INCLUDE_DIR}/native"
        "${CMAKE_SOURCE_DIR}/src/guest-time" "${CMAKE_SOURCE_DIR}/src/vu-aot")
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR "Source-owned product include directory is missing: ${_dir}")
    endif()
endforeach()

# Gate 8 audio library. Added here, before the project-wide USE_SSE2NEON definition and
# sse2neon include directory, exactly where the stage-era candidate added it: the SPU2 core
# is compiled without them.
if(RRV_PCSX2_SOURCE_DIR)
    set(RRV_SPU2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(RRV_SPU2_TSAN_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/pcsx2-spu2" pcsx2-spu2)
endif()
