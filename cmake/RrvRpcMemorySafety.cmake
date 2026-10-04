# Opt-in GAME-001 RPC memory-safety source closure after callback and IOP.
function(rrv_rpc_memory_safety_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;CALLBACK_SOURCE_DIR;OUTPUT_DIR;ALLOWED_OUTPUT_ROOT;TARGET" "" ${ARGN})
    foreach(_required IN ITEMS RUNTIME_SOURCE_DIR CALLBACK_SOURCE_DIR OUTPUT_DIR ALLOWED_OUTPUT_ROOT TARGET)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "rrv_rpc_memory_safety_generate_overlay requires ${_required}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_manifest "${ARG_OUTPUT_DIR}/rpc-memory-safety-overlay-manifest.json")
    set(_paths src/lib/Kernel/Syscalls/RPC.cpp
        src/lib/Kernel/Syscalls/System.cpp
        src/lib/Kernel/Syscalls/Thread.cpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/RPC.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.cpp"
        "${ARG_CALLBACK_SOURCE_DIR}/src/lib/Kernel/Syscalls/System.cpp"
        "${ARG_CALLBACK_SOURCE_DIR}/src/lib/Kernel/Syscalls/Common.h"
        "${ARG_CALLBACK_SOURCE_DIR}/src/lib/Kernel/Syscalls/Helpers/Runtime.h"
        "${CMAKE_SOURCE_DIR}/scripts/rpc_memory_safety_overlay.py"
        "${CMAKE_SOURCE_DIR}/tools/patches/ps2recomp-runtime-rpc-memory-safety.patch")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/rpc_memory_safety_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}"
            --callback-source "${ARG_CALLBACK_SOURCE_DIR}"
            --output-dir "${ARG_OUTPUT_DIR}"
            --allowed-output-root "${ARG_ALLOWED_OUTPUT_ROOT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "RPC memory-safety overlay generation failed: ${_stderr}${_stdout}")
    endif()
    get_target_property(_target_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_replaced 0)
    foreach(_entry IN LISTS _target_sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        set(_replacement "")
        foreach(_path IN LISTS _paths)
            if(_source MATCHES "/${_path}$")
                set(_replacement "${ARG_OUTPUT_DIR}/ps2xRuntime/${_path}")
                math(EXPR _replaced "${_replaced}+1")
                break()
            endif()
        endforeach()
        if(_replacement)
            list(APPEND _new_sources "${_replacement}")
        else()
            list(APPEND _new_sources "${_source}")
        endif()
    endforeach()
    if(NOT _replaced EQUAL 3)
        message(FATAL_ERROR "RPC memory-safety overlay expected RPC/System/Thread replacements, found ${_replaced}")
    endif()
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
    # Quoted Common.h -> Helpers/Runtime.h must resolve inside this exact closure.
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls")
    set(RRV_RPC_MEMORY_SAFETY_MANIFEST "${_manifest}" PARENT_SCOPE)
endfunction()
