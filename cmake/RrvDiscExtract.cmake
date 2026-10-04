# First-launch disc extraction for the Fukami app (src/app/disc_extract.*).
#
# Provenance: libchdr is built here as a static library straight from the pinned
# PCSX2 2.8.2 tree (build-deps/pcsx2-2.8.2/3rdparty/libchdr, BSD-3-Clause, Copyright
# Romain Tisserand; LICENSE.txt in that directory) with its LZMA decoder taken from
# build-deps/pcsx2-2.8.2/3rdparty/lzma (public-domain 7-Zip SDK). Nothing is copied
# into this repository. The CHD sector-layout handling in disc_extract.cpp follows
# pcsx2/CDVD/ChdFileReader.cpp of the same tree (GPL-3.0+). zlib comes from the SDK
# and zstd from the static Homebrew library (libzstd.a).
set(RRV_PCSX2_282_DIR "${CMAKE_SOURCE_DIR}/build-deps/pcsx2-2.8.2" CACHE PATH "Pinned PCSX2 2.8.2 source tree")
set(_chdr "${RRV_PCSX2_282_DIR}/3rdparty/libchdr")
set(_lzma "${RRV_PCSX2_282_DIR}/3rdparty/lzma")
find_path(RRV_DISC_ZSTD_INCLUDE_DIR zstd.h)
find_library(RRV_DISC_ZSTD_STATIC NAMES libzstd.a PATHS /opt/homebrew/lib /usr/local/lib)
if(NOT RRV_DISC_ZSTD_STATIC AND UNIX AND NOT APPLE)
    # Linux (Gate 5): a shared libzstd is acceptable (it ships in lib/ with the app).
    find_library(RRV_DISC_ZSTD_STATIC NAMES zstd)
endif()
find_package(ZLIB)

if(NOT EXISTS "${_chdr}/src/libchdr_chd.c" OR NOT RRV_DISC_ZSTD_INCLUDE_DIR OR NOT RRV_DISC_ZSTD_STATIC OR NOT ZLIB_FOUND)
    message(STATUS "rrv-disc-extract: skipped (need build-deps/pcsx2-2.8.2 libchdr, static zstd, zlib)")
    return()
endif()

enable_language(C)

add_library(rrv-disc-chdr STATIC
    "${_chdr}/src/libchdr_bitstream.c"
    "${_chdr}/src/libchdr_cdrom.c"
    "${_chdr}/src/libchdr_chd.c"
    "${_chdr}/src/libchdr_flac.c"
    "${_chdr}/src/libchdr_huffman.c"
    "${_lzma}/src/LzmaDec.c"
    "${_lzma}/src/Alloc.c"
    "${_lzma}/src/CpuArch.c")
target_compile_definitions(rrv-disc-chdr PRIVATE _7ZIP_ST)
target_compile_options(rrv-disc-chdr PRIVATE -w)
target_include_directories(rrv-disc-chdr PUBLIC "${_chdr}/include")
target_include_directories(rrv-disc-chdr PRIVATE "${_lzma}/include" "${RRV_DISC_ZSTD_INCLUDE_DIR}")
target_link_libraries(rrv-disc-chdr PUBLIC ZLIB::ZLIB "${RRV_DISC_ZSTD_STATIC}")

add_library(rrv-disc-extract STATIC "${CMAKE_SOURCE_DIR}/src/app/disc_extract.cpp")
target_include_directories(rrv-disc-extract PUBLIC "${CMAKE_SOURCE_DIR}/src/app")
target_link_libraries(rrv-disc-extract PUBLIC rrv-disc-chdr)
target_compile_options(rrv-disc-extract PRIVATE -Wall -Wextra)

add_executable(disc-extract "${CMAKE_SOURCE_DIR}/tools/disc-extract/main.cpp")
target_link_libraries(disc-extract PRIVATE rrv-disc-extract)

add_executable(rrv-disc-extract-tests "${CMAKE_SOURCE_DIR}/tests/disc_extract_tests.cpp")
target_link_libraries(rrv-disc-extract-tests PRIVATE rrv-disc-extract)
add_test(NAME rrv-disc-extract-tests COMMAND rrv-disc-extract-tests)
# Registered only where build-deps/pcsx2-2.8.2 and zstd exist (not in CI).
set_tests_properties(rrv-disc-extract-tests PROPERTIES LABELS needs-build-deps)
