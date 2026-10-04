# Opt-in producer control semantics. Compose only after SDL host finalization;
# retain original producer identities and all Stage-1 transport boundaries.
function(rrv_gs_control_generate_overlay)
    cmake_parse_arguments(ARG "CALLBACK_STACK_MAIN_RESERVATION" "RUNTIME_SOURCE_DIR;INCLUDE_INPUT;OUTPUT_DIR;TARGET" "" ${ARGN})
    foreach(_required IN ITEMS RUNTIME_SOURCE_DIR INCLUDE_INPUT OUTPUT_DIR TARGET)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "rrv_gs_control_generate_overlay requires ${_required}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_target_property(_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_keys runtime memory vif1 gif interrupt system stubs_gs)
    set(_names ps2_runtime.cpp ps2_memory.cpp ps2_vif1_interpreter.cpp ps2_gif_arbiter.cpp Interrupt.cpp System.cpp GS.cpp)
    set(_paths src/lib/ps2_runtime.cpp src/lib/ps2_memory.cpp src/lib/ps2_vif1_interpreter.cpp
        src/lib/ps2_gif_arbiter.cpp src/lib/Kernel/Syscalls/Interrupt.cpp src/lib/Kernel/Syscalls/System.cpp
        src/lib/Kernel/Stubs/GS.cpp)
    foreach(_entry IN LISTS _sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        get_filename_component(_name "${_source}" NAME)
        list(FIND _names "${_name}" _index)
        # Kernel/Stubs/System.cpp shares this basename but is not the syscall
        # owner of GsPutIMR. Preserve that unrelated translation unit verbatim.
        if(_name STREQUAL "System.cpp" AND NOT _source MATCHES "/src/lib/Kernel/Syscalls/System\\.cpp$")
            set(_index -1)
        endif()
        if(_name STREQUAL "GS.cpp" AND NOT _source MATCHES "/src/lib/Kernel/Stubs/GS\\.cpp$")
            set(_index -1)
        endif()
        if(_index LESS 0)
            list(APPEND _new_sources "${_source}")
            continue()
        endif()
        list(GET _keys ${_index} _key)
        list(GET _paths ${_index} _path)
        if(DEFINED _input_${_key})
            message(FATAL_ERROR "GS control found duplicate ${_name}")
        endif()
        set(_input_${_key} "${_source}")
        set(_output "${ARG_OUTPUT_DIR}/${_path}")
        list(APPEND _new_sources "${_output}")
        get_filename_component(_original_parent "${ARG_RUNTIME_SOURCE_DIR}/${_path}" DIRECTORY)
        set_source_files_properties("${_output}" TARGET_DIRECTORY ${ARG_TARGET}
            PROPERTIES INCLUDE_DIRECTORIES "${_original_parent}")
    endforeach()
    set(_command "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/gs_control_overlay.py"
        --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --include-input "${ARG_INCLUDE_INPUT}"
        --output-dir "${ARG_OUTPUT_DIR}")
    if(ARG_CALLBACK_STACK_MAIN_RESERVATION)
        list(APPEND _command --callback-stack-main-reservation)
    endif()
    foreach(_key IN LISTS _keys)
        if(NOT DEFINED _input_${_key})
            message(FATAL_ERROR "GS control missing selected ${_key} source")
        endif()
        list(APPEND _command "--${_key}-input" "${_input_${_key}}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input_${_key}}")
    endforeach()
    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS "${ARG_INCLUDE_INPUT}/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_headers}
        "${CMAKE_SOURCE_DIR}/scripts/gs_control_overlay.py"
        "${CMAKE_SOURCE_DIR}/scripts/gs_control_memory_overlay.py"
        "${CMAKE_SOURCE_DIR}/scripts/gs_control_gif_overlay.py"
        "${CMAKE_SOURCE_DIR}/scripts/gs_control_runtime_overlay.py"
        "${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        "${CMAKE_SOURCE_DIR}/src/gs-control/rrv_gs_control.h"
        "${CMAKE_SOURCE_DIR}/src/gs-control/rrv_gs_result_boundary.h"
        "${CMAKE_SOURCE_DIR}/src/gs-control/rrv_gif_control_stream.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Interrupt.h")
    execute_process(COMMAND ${_command}
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${ARG_OUTPUT_DIR}/gs-control-overlay-manifest.json")
        message(FATAL_ERROR "GS control overlay failed: ${_stdout}${_stderr}")
    endif()
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC
        "${ARG_OUTPUT_DIR}/include" "${ARG_OUTPUT_DIR}/src/lib/Kernel"
        "${CMAKE_SOURCE_DIR}/src/gs-control")
    # Keep every outer TU (including runtime-layout consumers without direct
    # ps2_runtime linkage) on this same complete header closure.
    include_directories(BEFORE "${ARG_OUTPUT_DIR}/include" "${ARG_OUTPUT_DIR}/src/lib/Kernel"
        "${CMAKE_SOURCE_DIR}/src/gs-control")
    target_compile_definitions(${ARG_TARGET} PUBLIC RRV_GS_PRODUCER_CONTROL=1)
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
endfunction()
