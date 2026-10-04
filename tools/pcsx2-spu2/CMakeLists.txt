# SPDX-License-Identifier: GPL-3.0+
# rrv_pcsx2_spu2: PCSX2 2.8.2 SPU2 core (compiled unmodified from the pinned
# checkout) + RRV shims + timestamped C ABI. Standalone project:
#
#   cmake -S tools/pcsx2-spu2 -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DRRV_PCSX2_SOURCE_DIR=<repo>/build-deps/pcsx2-2.8.2
#   cmake --build <build> && ctest --test-dir <build>
cmake_minimum_required(VERSION 3.20)
project(rrv_pcsx2_spu2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Default: <repo>/build-deps/pcsx2-2.8.2 relative to a main checkout.
get_filename_component(_rrv_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RRV_PCSX2_SOURCE_DIR "${_rrv_repo}/build-deps/pcsx2-2.8.2" CACHE PATH
    "Pinned PCSX2 2.8.2 checkout (fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), read-only")
if(NOT EXISTS "${RRV_PCSX2_SOURCE_DIR}/pcsx2/SPU2/spu2sys.cpp")
    message(FATAL_ERROR "RRV_PCSX2_SOURCE_DIR does not contain pcsx2/SPU2: ${RRV_PCSX2_SOURCE_DIR}")
endif()

option(RRV_SPU2_BUILD_TESTS "Build spu2_selftest" ON)
option(RRV_SPU2_TSAN_TESTS "Also build a ThreadSanitizer variant of the library + selftest" ON)

set(_spu2 "${RRV_PCSX2_SOURCE_DIR}/pcsx2/SPU2")
# spu2.cpp (AudioStream/VMManager/GSCapture glue) and Debug.cpp/Wavedump_wav.cpp
# (devbuild only) are intentionally not compiled; rrv_pcsx2_spu2.cpp replaces spu2.cpp.
set(RRV_SPU2_PCSX2_SOURCES
    ${_spu2}/ADSR.cpp
    ${_spu2}/Dma.cpp
    ${_spu2}/Mixer.cpp
    ${_spu2}/ReadInput.cpp
    ${_spu2}/RegTable.cpp
    ${_spu2}/Reverb.cpp
    ${_spu2}/ReverbResample.cpp
    ${_spu2}/spu2sys.cpp
    ${_spu2}/spu2freeze.cpp
)

function(rrv_spu2_library name)
    add_library(${name} STATIC
        ${RRV_SPU2_PCSX2_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/rrv_pcsx2_spu2.cpp)
    # Order matters: shim/ first so it wins over same-named PCSX2 headers;
    # pcsx2/ only provides SPU2/*.h.
    target_include_directories(${name}
        PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shim" "${RRV_PCSX2_SOURCE_DIR}/pcsx2")
    target_compile_options(${name} PRIVATE
        "SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/shim/rrv_spu2_prelude.h\""
        # As upstream pcsx2/CMakeLists.txt.
        -fno-strict-aliasing
        # Upstream uses -ffp-contract=fast; the only float math here is the
        # host-side output stage (DC filter), kept FMA-free so every host
        # produces identical output samples.
        -ffp-contract=off)
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PUBLIC Threads::Threads)
endfunction()

rrv_spu2_library(rrv_pcsx2_spu2)

if(RRV_SPU2_BUILD_TESTS)
    enable_testing()
    add_executable(spu2_selftest test/spu2_selftest.cpp)
    target_link_libraries(spu2_selftest PRIVATE rrv_pcsx2_spu2)
    add_test(NAME spu2_tone COMMAND spu2_selftest tone)
    add_test(NAME spu2_determinism COMMAND spu2_selftest determinism)
    add_test(NAME spu2_threading COMMAND spu2_selftest threading)

    # Host RSPU2DRV driver (src/audio/rspu2_driver.cpp) tests; optional.
    set(_rspu2_src "${CMAKE_CURRENT_LIST_DIR}/../../src/audio/rspu2_driver.cpp")
    option(RRV_RSPU2_DRIVER_TESTS "Build the rspu2 driver tests" ON)
    set(_rspu2_tests OFF)
    if(RRV_RSPU2_DRIVER_TESTS AND EXISTS "${_rspu2_src}")
        set(_rspu2_tests ON)
        add_executable(rspu2_driver_test test/rspu2_driver_test.cpp "${_rspu2_src}")
        target_include_directories(rspu2_driver_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/audio")
        target_link_libraries(rspu2_driver_test PRIVATE rrv_pcsx2_spu2)
        add_test(NAME rspu2_unit COMMAND rspu2_driver_test unit)
        add_test(NAME rspu2_stream COMMAND rspu2_driver_test stream)
        add_test(NAME rspu2_determinism COMMAND rspu2_driver_test determinism)
    endif()

    if(RRV_SPU2_TSAN_TESTS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        if(_rspu2_tests)
            add_executable(rspu2_driver_test_tsan test/rspu2_driver_test.cpp "${_rspu2_src}")
            target_include_directories(rspu2_driver_test_tsan PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/audio")
            target_compile_options(rspu2_driver_test_tsan PRIVATE -fsanitize=thread -g)
            target_link_libraries(rspu2_driver_test_tsan PRIVATE rrv_pcsx2_spu2_tsan)
            add_test(NAME rspu2_determinism_tsan COMMAND rspu2_driver_test_tsan determinism)
            set_tests_properties(rspu2_determinism_tsan PROPERTIES
                ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:abort_on_error=1"
                FAIL_REGULAR_EXPRESSION "ThreadSanitizer")
        endif()
        rrv_spu2_library(rrv_pcsx2_spu2_tsan)
        target_compile_options(rrv_pcsx2_spu2_tsan PRIVATE -fsanitize=thread -g)
        target_link_options(rrv_pcsx2_spu2_tsan PUBLIC -fsanitize=thread)
        add_executable(spu2_selftest_tsan test/spu2_selftest.cpp)
        target_compile_options(spu2_selftest_tsan PRIVATE -fsanitize=thread -g)
        target_link_libraries(spu2_selftest_tsan PRIVATE rrv_pcsx2_spu2_tsan)
        add_test(NAME spu2_threading_tsan COMMAND spu2_selftest_tsan threading)
        set_tests_properties(spu2_threading_tsan PROPERTIES
            ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:abort_on_error=1"
            FAIL_REGULAR_EXPRESSION "ThreadSanitizer")
    endif()
endif()
