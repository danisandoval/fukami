# Compiler front-end neutral spellings of the flags this repository names directly.
#
# The macOS and Linux builds use clang or GCC (GNU-style options). The Windows build uses clang-cl: the MSVC command
# line, the MSVC ABI and CRT, and clang's code generator. clang-cl takes a GNU-style option only through the
# `/clang:` prefix, so the same codegen policy (x86-64-v3, no FMA contraction where the VU FMAC must round twice) is
# spelled differently there. Call these helpers instead of writing the GNU spelling.
#
# Included once from the root CMakeLists.txt and from the standalone tools/pcsx2-spu2 project.
include_guard(GLOBAL)

set(RRV_CLANG_CL FALSE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(RRV_CLANG_CL TRUE)
endif()
if(WIN32 AND NOT RRV_CLANG_CL)
    message(FATAL_ERROR
        "Windows builds use clang-cl (the MSVC ABI and CRT with clang's code generator); found "
        "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER_FRONTEND_VARIANT} front end) at ${CMAKE_CXX_COMPILER}. "
        "Use the m0-windows-x86_64 preset from a shell that has run vcvars64 (see BUILDING.md).")
endif()

# Each helper stores a list in <out>; an empty list means "nothing to pass on this compiler".
function(rrv_flag_march_x86_64_v3 out)
    if(RRV_CLANG_CL)
        set(${out} "/clang:-march=x86-64-v3" PARENT_SCOPE)
    else()
        set(${out} "-march=x86-64-v3" PARENT_SCOPE)
    endif()
endfunction()

# FMA contraction off: the PS2 VU FMAC is not a fused multiply-add (docs/TESTING.md T-P7-VUSTEP).
function(rrv_flag_fp_contract_off out)
    if(RRV_CLANG_CL)
        set(${out} "/clang:-ffp-contract=off" PARENT_SCOPE)
    else()
        set(${out} "-ffp-contract=off" PARENT_SCOPE)
    endif()
endfunction()

function(rrv_flag_no_strict_aliasing out)
    if(RRV_CLANG_CL)
        set(${out} "/clang:-fno-strict-aliasing" PARENT_SCOPE)
    else()
        set(${out} "-fno-strict-aliasing" PARENT_SCOPE)
    endif()
endfunction()

# Third-party C code that is compiled as is: no diagnostics from it.
function(rrv_flag_no_warnings out)
    if(RRV_CLANG_CL)
        set(${out} "/w" PARENT_SCOPE)
    else()
        set(${out} "-w" PARENT_SCOPE)
    endif()
endfunction()

# -Wall -Wextra. clang-cl's own /Wall means -Weverything, so the GNU options go through /clang:.
function(rrv_flag_warnings_all out)
    if(RRV_CLANG_CL)
        set(${out} "/clang:-Wall" "/clang:-Wextra" PARENT_SCOPE)
    else()
        set(${out} -Wall -Wextra PARENT_SCOPE)
    endif()
endfunction()

# Force-include <header> into every translation unit of a target (GNU -include, MSVC /FI).
function(rrv_flag_force_include out header)
    if(RRV_CLANG_CL)
        set(${out} "/FI${header}" PARENT_SCOPE)
    else()
        set(${out} "SHELL:-include \"${header}\"" PARENT_SCOPE)
    endif()
endfunction()

# Add <dir> to the quote-include search path only (GNU -iquote). MSVC has no equivalent option; clang-cl passes it on.
function(rrv_flag_iquote out dir)
    if(RRV_CLANG_CL)
        set(${out} "/clang:-iquote${dir}" PARENT_SCOPE)
    else()
        set(${out} "-iquote${dir}" PARENT_SCOPE)
    endif()
endfunction()

# Sanitizers are a Unix-only configuration here (the Gate-9 presets); the standalone SPU2 project asks before using one.
function(rrv_sanitizers_available out)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

if(WIN32)
    # One set for every target. Windows.h without min/max macros and without the rarely used APIs; the CRT's
    # "unsafe" function warnings stay off (the sources use the portable C library functions); sources are UTF-8
    # (paths and menu strings); the generated game code and the PCSX2 translation units exceed the default COFF
    # section limit. _USE_MATH_DEFINES: M_PI and friends, which the MSVC math.h hides otherwise. SDL_MAIN_HANDLED: SDL.h would otherwise rename main() to SDL_main and ask for SDL2main.lib; the
    # product keeps its own main() (the GUI-subsystem executable enters through /ENTRY:mainCRTStartup).
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED _USE_MATH_DEFINES)
    add_compile_options(/utf-8 /bigobj)
    # A Windows executable reserves 1 MiB of stack by default, for its main thread and for every thread created without
    # an explicit size (std::thread). macOS and Linux give 8 MiB, which the runtime and its tests were written for
    # (the gs-backend tests overflow 1 MiB). The reservation is address space, not committed memory.
    string(APPEND CMAKE_EXE_LINKER_FLAGS " /STACK:8388608")

    # The MSVC runtime has no compiler-rt helpers: clang emits calls to them for 128-bit integer division
    # (rrv_temporal_owner's __uint128_t deadline arithmetic: __udivti3) that the GNU toolchains find in libgcc/compiler-rt
    # automatically. clang-cl names its own copy; link it into every target (it only supplies symbols nothing else does).
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE _rrv_clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rrv_builtins_result)
    file(TO_CMAKE_PATH "${_rrv_clang_resource_dir}" _rrv_clang_resource_dir)
    set(RRV_CLANG_BUILTINS_LIB "${_rrv_clang_resource_dir}/lib/windows/clang_rt.builtins-x86_64.lib")
    if(NOT _rrv_builtins_result EQUAL 0 OR NOT EXISTS "${RRV_CLANG_BUILTINS_LIB}")
        message(FATAL_ERROR "clang-cl has no compiler-rt builtins library at '${RRV_CLANG_BUILTINS_LIB}'; "
            "install the LLVM/clang-cl component of the Visual Studio Build Tools")
    endif()
    link_libraries("${RRV_CLANG_BUILTINS_LIB}")
endif()
