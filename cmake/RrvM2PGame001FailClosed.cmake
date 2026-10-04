# Immutable Phase-0 GAME-001 diagnostic overlay.  It owns one producer source
# copy and stops before lookupFunction() can invoke its default recovery body.
function(rrv_m2p_game001_fail_closed_add_library)
    if(TARGET rrv_m2p_game001_fail_closed)
        return()
    endif()
    add_library(rrv_m2p_game001_fail_closed STATIC
        "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2p_game001_fail_closed.cpp")
    target_include_directories(rrv_m2p_game001_fail_closed PUBLIC
        "${CMAKE_SOURCE_DIR}/src/diag"
        "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/include")
    target_include_directories(rrv_m2p_game001_fail_closed PRIVATE
        "${CMAKE_SOURCE_DIR}/src/gs-backend")
    target_compile_features(rrv_m2p_game001_fail_closed PUBLIC cxx_std_20)
    target_compile_definitions(rrv_m2p_game001_fail_closed PUBLIC
        RRV_M2P_GAME001_FAIL_CLOSED=1)
endfunction()

function(rrv_m2p_game001_fail_closed_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;GENERATED_SOURCE;REENTRY_SOURCE;OVERLAP_SOURCE;OUTPUT_DIR;TARGET" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_GENERATED_SOURCE OR NOT ARG_REENTRY_SOURCE OR NOT ARG_OUTPUT_DIR OR NOT ARG_TARGET)
        message(FATAL_ERROR "rrv_m2p_game001_fail_closed_generate_overlay requires RUNTIME_SOURCE_DIR, GENERATED_SOURCE, REENTRY_SOURCE, OUTPUT_DIR, and TARGET")
    endif()
    if(NOT ARG_OVERLAP_SOURCE)
        get_filename_component(_generated_directory "${ARG_GENERATED_SOURCE}" DIRECTORY)
        set(ARG_OVERLAP_SOURCE "${_generated_directory}/sub_002D2C40_0x2d2c40.cpp")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_source "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_runtime.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/m2p_game001_overlay.py" "${_source}"
        "${ARG_GENERATED_SOURCE}" "${ARG_REENTRY_SOURCE}" "${ARG_OVERLAP_SOURCE}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/m2p_game001_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}"
            --generated-source "${ARG_GENERATED_SOURCE}"
            --reentry-source "${ARG_REENTRY_SOURCE}"
            --overlap-source "${ARG_OVERLAP_SOURCE}"
            --output-dir "${ARG_OUTPUT_DIR}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    set(_manifest "${ARG_OUTPUT_DIR}/m2p-game001-fail-closed-overlay-manifest.json")
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "M2P GAME-001 fail-closed overlay generation failed: ${_stderr}${_stdout}")
    endif()

    get_target_property(_target_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_replaced 0)
    foreach(_entry IN LISTS _target_sources)
        get_filename_component(_source_entry "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        file(RELATIVE_PATH _relative "${ARG_RUNTIME_SOURCE_DIR}" "${_source_entry}")
        if(_relative STREQUAL "src/lib/ps2_runtime.cpp")
            list(APPEND _new_sources "${ARG_OUTPUT_DIR}/${_relative}")
            get_filename_component(_parent "${_source_entry}" DIRECTORY)
            set_source_files_properties("${ARG_OUTPUT_DIR}/${_relative}" TARGET_DIRECTORY ${ARG_TARGET}
                PROPERTIES INCLUDE_DIRECTORIES "${_parent}")
            math(EXPR _replaced "${_replaced}+1")
        else()
            list(APPEND _new_sources "${_source_entry}")
        endif()
    endforeach()
    if(NOT _replaced EQUAL 1)
        message(FATAL_ERROR "M2P GAME-001 fail-closed expected one ps2_runtime.cpp replacement, found ${_replaced}")
    endif()
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
    target_include_directories(${ARG_TARGET} PUBLIC "${CMAKE_SOURCE_DIR}/src/diag")
    target_compile_definitions(${ARG_TARGET} PUBLIC RRV_M2P_GAME001_FAIL_CLOSED=1)
    target_link_libraries(${ARG_TARGET} PUBLIC rrv_m2p_game001_fail_closed)
    get_filename_component(_generated_name "${ARG_GENERATED_SOURCE}" NAME)
    get_filename_component(_reentry_name "${ARG_REENTRY_SOURCE}" NAME)
    set(RRV_M2P_GAME001_GENERATED_OWNER_OVERLAY
        "${ARG_OUTPUT_DIR}/generated/${_generated_name}" PARENT_SCOPE)
    set(RRV_M2P_GAME001_REENTRY_OVERLAY
        "${ARG_OUTPUT_DIR}/generated/${_reentry_name}" PARENT_SCOPE)
    get_filename_component(_overlap_name "${ARG_OVERLAP_SOURCE}" NAME)
    set(RRV_M2P_GAME001_OVERLAP_OVERLAY
        "${ARG_OUTPUT_DIR}/generated/${_overlap_name}" PARENT_SCOPE)
endfunction()
