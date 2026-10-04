# Opt-in GAME-001 IOP heap storage and explicit CD/SIF route overlay.
function(rrv_iop_heap_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;CALLBACK_SOURCE_DIR;OUTPUT_DIR;ALLOWED_OUTPUT_ROOT;TARGET" "" ${ARGN})
    foreach(_required IN ITEMS RUNTIME_SOURCE_DIR CALLBACK_SOURCE_DIR OUTPUT_DIR ALLOWED_OUTPUT_ROOT TARGET)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "rrv_iop_heap_generate_overlay requires ${_required}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_manifest "${ARG_OUTPUT_DIR}/iop-heap-overlay-manifest.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ARG_CALLBACK_SOURCE_DIR}/src/lib/Kernel/Stubs/CD.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/SIF.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Common.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Helpers/Support.h"
        "${CMAKE_SOURCE_DIR}/scripts/iop_heap_overlay.py"
        "${CMAKE_SOURCE_DIR}/tools/patches/ps2recomp-runtime-iop-heap-isolation.patch")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/iop_heap_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}"
            --callback-source "${ARG_CALLBACK_SOURCE_DIR}"
            --output-dir "${ARG_OUTPUT_DIR}"
            --allowed-output-root "${ARG_ALLOWED_OUTPUT_ROOT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "IOP heap overlay generation failed: ${_stderr}${_stdout}")
    endif()
    get_target_property(_target_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_replaced_cd 0)
    set(_replaced_sif 0)
    foreach(_entry IN LISTS _target_sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        if(_source MATCHES "/src/lib/Kernel/Stubs/CD.cpp$")
            list(APPEND _new_sources "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Stubs/CD.cpp")
            math(EXPR _replaced_cd "${_replaced_cd}+1")
        elseif(_source MATCHES "/src/lib/Kernel/Stubs/SIF.cpp$")
            list(APPEND _new_sources "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp")
            math(EXPR _replaced_sif "${_replaced_sif}+1")
        else()
            list(APPEND _new_sources "${_source}")
        endif()
    endforeach()
    if(NOT _replaced_cd EQUAL 1 OR NOT _replaced_sif EQUAL 1)
        message(FATAL_ERROR "IOP heap overlay expected one CD and one SIF replacement")
    endif()
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
    # Thread.cpp resolves its unqualified Common.h from callback Syscalls.
    # Keep that directory ahead of staged Stubs, which must precede pinned Stubs.
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC
        "${ARG_CALLBACK_SOURCE_DIR}/src/lib/Kernel/Syscalls"
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Stubs")
    set(RRV_IOP_HEAP_MANIFEST "${_manifest}" PARENT_SCOPE)
endfunction()
