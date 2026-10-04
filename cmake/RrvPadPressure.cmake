# Product libpad correction. Compose after the optional pad observer; retain
# its hooks and header closure while leaving the locked producer untouched.
function(rrv_pad_pressure_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;OUTPUT_DIR;TARGET" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_OUTPUT_DIR OR NOT ARG_TARGET)
        message(FATAL_ERROR "rrv_pad_pressure_generate_overlay requires RUNTIME_SOURCE_DIR, OUTPUT_DIR, and TARGET")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_target_property(_target_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_replaced 0)
    set(_output "${ARG_OUTPUT_DIR}/src/lib/Kernel/Stubs/Pad.cpp")
    foreach(_entry IN LISTS _target_sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        if(_source MATCHES "/src/lib/Kernel/Stubs/Pad\\.cpp$")
            set(_input "${_source}")
            list(APPEND _new_sources "${_output}")
            math(EXPR _replaced "${_replaced}+1")
        else()
            list(APPEND _new_sources "${_source}")
        endif()
    endforeach()
    if(NOT _replaced EQUAL 1)
        message(FATAL_ERROR "PAD-001 expected one Pad.cpp replacement, found ${_replaced}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_input}"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Pad.cpp"
        "${CMAKE_SOURCE_DIR}/scripts/pad_pressure_overlay.py"
        "${CMAKE_SOURCE_DIR}/scripts/m2p_pad_overlay.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/pad_pressure_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --source "${_input}"
            --output-dir "${ARG_OUTPUT_DIR}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${ARG_OUTPUT_DIR}/pad-pressure-overlay-manifest.json")
        message(FATAL_ERROR "PAD-001 pressure overlay generation failed: ${_stderr}${_stdout}")
    endif()
    set_source_files_properties("${_output}" TARGET_DIRECTORY ${ARG_TARGET}
        PROPERTIES INCLUDE_DIRECTORIES "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs")
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
endfunction()
