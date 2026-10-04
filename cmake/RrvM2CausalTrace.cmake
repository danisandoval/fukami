# Optional M2 causal trace.  The caller invokes these functions only for the
# compiled-in B/C build.  Mode A links neither this target nor the producer
# overlay; the header provides inline no-op calls in that configuration.
function(rrv_m2_causal_trace_add_library)
    if(TARGET rrv_m2_causal_trace)
        return()
    endif()
    add_library(rrv_m2_causal_trace STATIC
        "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2_causal_trace.cpp")
    target_include_directories(rrv_m2_causal_trace PUBLIC "${CMAKE_SOURCE_DIR}/src/diag")
    target_compile_features(rrv_m2_causal_trace PUBLIC cxx_std_20)
    target_compile_definitions(rrv_m2_causal_trace PUBLIC RRV_M2_CAUSAL_TRACE=1)
    if(RRV_M2_DMA_PROVENANCE)
        target_sources(rrv_m2_causal_trace PRIVATE
            "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2_dma_provenance.cpp")
        target_compile_definitions(rrv_m2_causal_trace PUBLIC RRV_M2_DMA_PROVENANCE=1)
    endif()
    if(RRV_M2_GUEST_PROVENANCE)
        target_sources(rrv_m2_causal_trace PRIVATE
            "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2_guest_provenance.cpp")
        target_compile_definitions(rrv_m2_causal_trace PUBLIC RRV_M2_GUEST_PROVENANCE=1)
    endif()
endfunction()

function(rrv_m2_causal_trace_generate_overlay)
    cmake_parse_arguments(ARG "" "RUNTIME_SOURCE_DIR;OUTPUT_DIR;TARGET" "" ${ARGN})
    if(NOT ARG_RUNTIME_SOURCE_DIR OR NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "rrv_m2_causal_trace_generate_overlay requires RUNTIME_SOURCE_DIR and OUTPUT_DIR")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_manifest "${ARG_OUTPUT_DIR}/m2-causal-overlay-manifest.json")
    set(_sources
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Interrupt.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_vif1_interpreter.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_memory.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_runtime.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Helpers/Runtime.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Common.h"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls/Sync.cpp"
        "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/CD.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/m2_causal_overlay.py" ${_sources})
    set(_provenance_args)
    if(RRV_M2_INITIAL_STATE_FINGERPRINT)
        list(APPEND _provenance_args --initial-state-fingerprint)
    endif()
    set(_expected_replacements 6)
    if(RRV_M2_DMA_PROVENANCE)
        list(APPEND _provenance_args --dma-provenance)
        list(APPEND _sources
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/ps2_gif_arbiter.cpp"
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/DMA.cpp")
        file(GLOB_RECURSE _provenance_headers CONFIGURE_DEPENDS
            "${ARG_RUNTIME_SOURCE_DIR}/include/*.h")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/scripts/m2_dma_overlay.py" ${_provenance_headers} ${_sources})
        set(_expected_replacements 8)
    endif()
    if(RRV_M2_GUEST_PROVENANCE)
        list(APPEND _provenance_args --guest-provenance)
        file(GLOB _guest_stubs CONFIGURE_DEPENDS
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/*.cpp")
        foreach(_stub IN LISTS _guest_stubs)
            # CD/DMA are already present in the accepted eight replacements.
            get_filename_component(_stem "${_stub}" NAME_WE)
            string(TOLOWER "${_stem}" _stem)
            if(NOT _stem STREQUAL "cd" AND NOT _stem STREQUAL "dma")
                list(APPEND _sources "${_stub}")
                math(EXPR _expected_replacements "${_expected_replacements}+1")
            endif()
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/scripts/m2_guest_overlay.py"
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Common.h"
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Stubs/Helpers/Support.h" ${_guest_stubs})
    endif()
    # This is a configure-time overlay, not a build-time mutation of the
    # pinned dependency.  A drifted producer fails the configure before CMake
    # can compile an accidental approximation of a hook.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/m2_causal_overlay.py"
            --runtime-source "${ARG_RUNTIME_SOURCE_DIR}" --output-dir "${ARG_OUTPUT_DIR}" ${_provenance_args}
        RESULT_VARIABLE _overlay_result
        OUTPUT_VARIABLE _overlay_stdout
        ERROR_VARIABLE _overlay_stderr)
    if(NOT _overlay_result EQUAL 0)
        message(FATAL_ERROR "M2 causal overlay generation failed: ${_overlay_stderr}${_overlay_stdout}")
    endif()
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "M2 causal overlay did not create its manifest")
    endif()
    if(ARG_TARGET)
        get_target_property(_target_sources ${ARG_TARGET} SOURCES)
        set(_new_sources)
        set(_replaced 0)
        foreach(_entry IN LISTS _target_sources)
            get_filename_component(_source "${_entry}" ABSOLUTE BASE_DIR "${ARG_RUNTIME_SOURCE_DIR}")
            file(RELATIVE_PATH _relative "${ARG_RUNTIME_SOURCE_DIR}" "${_source}")
            # macOS may preserve upstream's Dma.cpp spelling in the target
            # although the pinned Git entry is DMA.cpp. Normalize only for
            # identity matching; the manifest always retains the Git spelling.
            string(TOLOWER "${_source}" _source_folded)
            set(_matched_source "")
            foreach(_candidate IN LISTS _sources)
                string(TOLOWER "${_candidate}" _candidate_folded)
                if(_source_folded STREQUAL _candidate_folded)
                    set(_matched_source "${_candidate}")
                endif()
            endforeach()
            if(_matched_source AND _source MATCHES "\\.cpp$")
                file(RELATIVE_PATH _relative "${ARG_RUNTIME_SOURCE_DIR}" "${_matched_source}")
                list(APPEND _new_sources "${ARG_OUTPUT_DIR}/${_relative}")
                get_filename_component(_original_parent "${_source}" DIRECTORY)
                set_source_files_properties("${ARG_OUTPUT_DIR}/${_relative}"
                    TARGET_DIRECTORY ${ARG_TARGET}
                    PROPERTIES INCLUDE_DIRECTORIES "${_original_parent}")
                math(EXPR _replaced "${_replaced}+1")
            else()
                list(APPEND _new_sources "${_source}")
            endif()
        endforeach()
        if(NOT _replaced EQUAL _expected_replacements)
            message(FATAL_ERROR "M2 causal overlay expected ${_expected_replacements} producer translation units, found ${_replaced}")
        endif()
        set_property(TARGET ${ARG_TARGET} PROPERTY SOURCES ${_new_sources})
        target_include_directories(${ARG_TARGET} PRIVATE
            "${ARG_RUNTIME_SOURCE_DIR}/src/lib/Kernel/Syscalls")
        target_include_directories(${ARG_TARGET} PUBLIC "${CMAKE_SOURCE_DIR}/src/diag")
        target_compile_definitions(${ARG_TARGET} PUBLIC RRV_M2_CAUSAL_TRACE=1)
        if(RRV_M2_GUEST_PROVENANCE)
            target_compile_definitions(${ARG_TARGET} PUBLIC RRV_M2_GUEST_PROVENANCE=1)
        endif()
        if(RRV_M2_DMA_PROVENANCE)
            target_compile_definitions(${ARG_TARGET} PUBLIC RRV_M2_DMA_PROVENANCE=1)
            target_include_directories(${ARG_TARGET} BEFORE PUBLIC "${ARG_OUTPUT_DIR}/include")
            # All parent RRV/game consumers must see the same header closure,
            # ahead of their direct pinned-runtime includes as well.
            include_directories(BEFORE "${ARG_OUTPUT_DIR}/include")
        endif()
    endif()
endfunction()
