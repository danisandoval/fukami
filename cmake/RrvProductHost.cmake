# Product-only source specialization. Original producer paths remain the
# provenance and diagnostic-overlay inputs; no source dependency is modified.
function(rrv_product_host_prepare)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;OUTPUT_DIR;OUT_SOURCE_DIR" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_OUTPUT_DIR OR NOT ARG_OUT_SOURCE_DIR)
        message(FATAL_ERROR "rrv_product_host_prepare requires RUNTIME_SOURCE_DIR, OUTPUT_DIR, OUT_SOURCE_DIR")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --output-dir "${ARG_OUTPUT_DIR}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Product SDL host overlay failed: ${_stdout}${_stderr}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        "${ARG_RUNTIME_SOURCE_DIR}/CMakeLists.txt"
        "${ARG_RUNTIME_SOURCE_DIR}/include/ps2_runtime.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_audio.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Common.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.h"
        "${CMAKE_SOURCE_DIR}/src/rrv_guest_terminal_outcome.h")
    add_library(rrv_sdl_product_audio STATIC "${CMAKE_SOURCE_DIR}/src/host/rrv_sdl_audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/host/rrv_thread_cpu_log.cpp")
    target_compile_features(rrv_sdl_product_audio PUBLIC cxx_std_20)
    target_compile_definitions(rrv_sdl_product_audio PUBLIC RRV_PRODUCT_HOST_SDL=1)
    target_include_directories(rrv_sdl_product_audio PUBLIC "${CMAKE_SOURCE_DIR}/src/host")
    target_link_libraries(rrv_sdl_product_audio PUBLIC SDL2::SDL2)
    set(${ARG_OUT_SOURCE_DIR} "${ARG_OUTPUT_DIR}" PARENT_SCOPE)
endfunction()

# Run after optional runtime/pad diagnostic overlays. Keeping their transforms
# first preserves their exact input identities and original-relative matching.
function(rrv_product_host_finalize)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;OUTPUT_DIR;TARGET" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_OUTPUT_DIR OR NOT ARG_TARGET)
        message(FATAL_ERROR "rrv_product_host_finalize requires RUNTIME_SOURCE_DIR, OUTPUT_DIR, TARGET")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_target_property(_sources ${ARG_TARGET} SOURCES)
    set(_new_sources)
    set(_count 0)
    foreach(_entry IN LISTS _sources)
        get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
        if(_source MATCHES "/src/lib/(ps2_runtime|ps2_pad)\\.cpp$")
            set(_name "${CMAKE_MATCH_1}")
            set(_${_name}_input "${_source}")
            list(APPEND _new_sources "${ARG_OUTPUT_DIR}/src/lib/${_name}.cpp")
            math(EXPR _count "${_count}+1")
        else()
            list(APPEND _new_sources "${_source}")
        endif()
    endforeach()
    if(NOT _count EQUAL 2)
        message(FATAL_ERROR "Product SDL host expected exactly one runtime and pad source, found ${_count}")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --output-dir "${ARG_OUTPUT_DIR}"
        --runtime-input "${_ps2_runtime_input}" --pad-input "${_ps2_pad_input}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Product SDL host finalization failed: ${_stdout}${_stderr}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_ps2_runtime_input}" "${_ps2_pad_input}"
        "${ARG_RUNTIME_SOURCE_DIR}/include/ps2_runtime.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Common.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Thread.h"
        "${CMAKE_SOURCE_DIR}/src/rrv_guest_terminal_outcome.h")
    target_include_directories(${ARG_TARGET} PRIVATE
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls")
    target_include_directories(${ARG_TARGET} BEFORE PUBLIC "${ARG_OUTPUT_DIR}/include")
    # Consumers such as rrv_stubs include the producer's Stubs/Common.h and
    # inherit the causal overlay's directory include list before link usage
    # requirements. Apply this one-header override to that list as well.
    include_directories(BEFORE "${ARG_OUTPUT_DIR}/include")
    set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
endfunction()
