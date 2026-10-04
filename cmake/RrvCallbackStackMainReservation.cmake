# Opt-in GAME-001 repair. This materializes a complete callback-stack source
# closure after product-host-final while retaining the canonical producer clean.
function(rrv_callback_stack_main_reservation_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;HEADER_INPUT;RUNTIME_INPUT;OUTPUT_DIR;ALLOWED_OUTPUT_ROOT;TARGET" "" ${ARGN})
    foreach(_required IN ITEMS RUNTIME_SOURCE_DIR HEADER_INPUT RUNTIME_INPUT OUTPUT_DIR ALLOWED_OUTPUT_ROOT TARGET)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "rrv_callback_stack_main_reservation_generate_overlay requires ${_required}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_paths src/lib/ps2_runtime.cpp src/lib/Kernel/Syscalls/System.cpp
        src/lib/Kernel/Syscalls/Interrupt.cpp src/lib/Kernel/Syscalls/Sync.cpp src/lib/Kernel/Stubs/CD.cpp
        src/lib/Kernel/Stubs/GS.cpp)
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
    if(NOT _replaced EQUAL 6)
        message(FATAL_ERROR "callback-stack reservation expected six runtime source replacements, found ${_replaced}")
    endif()
    set(_manifest "${ARG_OUTPUT_DIR}/callback-stack-main-reservation-overlay-manifest.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ARG_HEADER_INPUT}" "${ARG_RUNTIME_INPUT}"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/System.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Interrupt.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Sync.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Common.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Helpers/Runtime.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/CD.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/GS.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Common.h"
        "${CMAKE_SOURCE_DIR}/scripts/callback_stack_main_reservation_overlay.py"
        "${CMAKE_SOURCE_DIR}/tools/patches/ps2recomp-runtime-callback-stack-main-reservation.patch")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/callback_stack_main_reservation_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}"
            --header-input "${ARG_HEADER_INPUT}" --runtime-input "${ARG_RUNTIME_INPUT}"
            --output-dir "${ARG_OUTPUT_DIR}" --allowed-output-root "${ARG_ALLOWED_OUTPUT_ROOT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "callback-stack reservation overlay generation failed: ${_stderr}${_stdout}")
    endif()
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC
        "${ARG_OUTPUT_DIR}/ps2xRuntime/include"
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls"
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs")
    include_directories(BEFORE "${ARG_OUTPUT_DIR}/ps2xRuntime/include"
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls"
        "${ARG_OUTPUT_DIR}/ps2xRuntime/src/lib/Kernel"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs")
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
    set(RRV_CALLBACK_STACK_MAIN_RESERVATION_MANIFEST "${_manifest}" PARENT_SCOPE)
endfunction()
