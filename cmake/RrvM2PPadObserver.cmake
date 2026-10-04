# Optional M2P input receipt.  It copies two SHA-pinned producer files at
# configure time; it never mutates the local producer checkout.
function(rrv_m2p_pad_observer_add_library)
    if(TARGET rrv_m2p_pad_observer)
        return()
    endif()
    add_library(rrv_m2p_pad_observer STATIC
        "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2p_pad_observer.cpp")
    target_include_directories(rrv_m2p_pad_observer PUBLIC "${CMAKE_SOURCE_DIR}/src/diag")
    target_compile_features(rrv_m2p_pad_observer PUBLIC cxx_std_20)
    target_compile_definitions(rrv_m2p_pad_observer PUBLIC RRV_M2P_PAD_OBSERVER=1)
endfunction()

function(rrv_m2p_pad_observer_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;OUTPUT_DIR;TARGET" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_OUTPUT_DIR OR NOT ARG_TARGET)
        message(FATAL_ERROR "rrv_m2p_pad_observer_generate_overlay requires RUNTIME_SOURCE_DIR, OUTPUT_DIR, and TARGET")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_sources
        "${ARG_RUNTIME_SOURCE_DIR}/include/runtime/host_pad.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Pad.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/m2p_pad_overlay.py" ${_sources})
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/m2p_pad_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --output-dir "${ARG_OUTPUT_DIR}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${ARG_OUTPUT_DIR}/m2p-pad-overlay-manifest.json")
        message(FATAL_ERROR "M2P pad observer overlay generation failed: ${_stderr}${_stdout}")
    endif()

    get_target_property(_target_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_replaced 0)
    foreach(_entry IN LISTS _target_sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        file(RELATIVE_PATH _relative "${ARG_RUNTIME_SOURCE_DIR}" "${_source}")
        if(_relative STREQUAL "src/lib/Kernel/Stubs/Pad.cpp")
            list(APPEND _new_sources "${ARG_OUTPUT_DIR}/${_relative}")
            get_filename_component(_parent "${_source}" DIRECTORY)
            set_source_files_properties("${ARG_OUTPUT_DIR}/${_relative}" TARGET_DIRECTORY ${ARG_TARGET}
                PROPERTIES INCLUDE_DIRECTORIES "${_parent}")
            math(EXPR _replaced "${_replaced}+1")
        else()
            list(APPEND _new_sources "${_source}")
        endif()
    endforeach()
    if(NOT _replaced EQUAL 1)
        message(FATAL_ERROR "M2P pad observer expected one Pad.cpp producer replacement, found ${_replaced}")
    endif()
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC "${ARG_OUTPUT_DIR}/include")
    # HostPadState gains a diagnostic-only correlation field in this overlay.
    # Parent RRV/game targets commonly add the pinned runtime include directly,
    # so propagate the same header closure at directory scope before those
    # later target-local include lists can select the old ABI.
    include_directories(BEFORE "${ARG_OUTPUT_DIR}/include")
    target_compile_definitions(${ARG_TARGET} PUBLIC RRV_M2P_PAD_OBSERVER=1)
    target_link_libraries(${ARG_TARGET} PUBLIC rrv_m2p_pad_observer)
endfunction()
