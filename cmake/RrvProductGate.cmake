# Kept separate from the product binary gate so the default asset-free graph
# can prove the intended M1 graph without fetching SDL or generated sources.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RRV_SIGNED_BRANCH_PRODUCER_BUILD "" CACHE PATH
    "Optional clean locked PS2Recomp build used by the signed branch execution control")
add_custom_target(rrv-product-direct-source-gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_product_direct.py"
        --source-root "${CMAKE_SOURCE_DIR}")
if(BUILD_TESTING)
    add_test(NAME rrv-product-locked-patch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_product_locked_patch.py")
    if(RRV_SIGNED_BRANCH_PRODUCER_BUILD)
        file(STRINGS "${CMAKE_SOURCE_DIR}/config/dependencies.lock.toml"
            _rrv_signed_branch_checkout_line REGEX "^default_checkout = \"")
        list(GET _rrv_signed_branch_checkout_line 0 _rrv_signed_branch_checkout_line)
        string(REGEX REPLACE "^default_checkout = \"([^\"]+)\"$" "\\1"
            _rrv_signed_branch_checkout "${_rrv_signed_branch_checkout_line}")
        set(_rrv_signed_branch_source
            "${CMAKE_SOURCE_DIR}/${_rrv_signed_branch_checkout}")
        if(NOT IS_DIRECTORY "${_rrv_signed_branch_source}")
            message(FATAL_ERROR
                "RRV_SIGNED_BRANCH_PRODUCER_BUILD requires locked producer checkout: "
                "${_rrv_signed_branch_source}")
        endif()
        add_test(NAME rrv-signed-branch-codegen-execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tests/run_signed_branch_codegen_execution.py"
                --source "${_rrv_signed_branch_source}"
                --build "${RRV_SIGNED_BRANCH_PRODUCER_BUILD}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/signed-branch-codegen-execution"
                --cxx "${CMAKE_CXX_COMPILER}")
    endif()
endif()
