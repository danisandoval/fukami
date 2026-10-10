# These checks compile only checked-in source.  They intentionally cover the
# ABI seam and portable file/IR contracts while M0 keeps game-derived sources,
# PS2Recomp, raylib, and the production executable out of the default build.
add_library(rrv_gs_backend STATIC
    "${CMAKE_SOURCE_DIR}/src/gs-backend/rrv_gs_backend.cpp"
)
target_include_directories(rrv_gs_backend PUBLIC
    "${CMAKE_SOURCE_DIR}/src/gs-backend"
    "${CMAKE_SOURCE_DIR}/src/gs-record"
    "${CMAKE_SOURCE_DIR}/src/diag"
)
target_link_libraries(rrv_gs_backend PRIVATE ${CMAKE_DL_LIBS})

# Gate 0: payload-free, runtime-owned terminal outcome state. This leaf keeps
# the qualification policy deterministic without requiring the producer or a
# user ELF; product-overlay anchoring is checked separately below.
add_executable(rrv-guest-terminal-outcome-tests
    "${CMAKE_SOURCE_DIR}/tests/guest_terminal_outcome_tests.cpp")
target_include_directories(rrv-guest-terminal-outcome-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(rrv-guest-terminal-outcome-tests PRIVATE Threads::Threads)
add_test(NAME rrv-guest-terminal-outcome COMMAND rrv-guest-terminal-outcome-tests)

# rrv::fp::ScopedRoundTowardZero (runtime/rrv_fp_rounding.h) replaces std::fesetround on the VU hot
# paths. Rounds like fesetround, restores the mode, nests; `--bench` prints the cost comparison.
add_executable(rrv-fp-rounding-tests
    "${CMAKE_SOURCE_DIR}/tests/fp_rounding_tests.cpp")
target_include_directories(rrv-fp-rounding-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/ps2recomp/ps2xRuntime/include")
add_test(NAME rrv-fp-rounding COMMAND rrv-fp-rounding-tests)

# rrv::ringCopy (runtime/rrv_ring_copy.h): the scratchpad DMA channels' block copy against the byte loop it
# replaces, wraps of either buffer included.
add_executable(rrv-ring-copy-tests
    "${CMAKE_SOURCE_DIR}/tests/ring_copy_tests.cpp")
target_include_directories(rrv-ring-copy-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/ps2recomp/ps2xRuntime/include")
add_test(NAME rrv-ring-copy COMMAND rrv-ring-copy-tests)

# rrv::host::CursorIdle (src/host/rrv_cursor_idle.h): when the full-screen product window hides the pointer.
add_executable(rrv-cursor-idle-tests
    "${CMAKE_SOURCE_DIR}/tests/cursor_idle_tests.cpp")
target_include_directories(rrv-cursor-idle-tests PRIVATE "${CMAKE_SOURCE_DIR}/src/host")
add_test(NAME rrv-cursor-idle COMMAND rrv-cursor-idle-tests)

# VU lean entry (VU1Interpreter::aotLean, src/vu-aot/rrv_vu_aot_engine.inc) against execute(), on real
# instruction sequences from the AOT catalogue, for both units. Compiles the runtime's VU source whole
# (with three external symbols stubbed), so it needs the committed catalogue and a SIMD backend: native SSE
# on x86-64, or the locked sse2neon checkout on ARM64.
set(_rrv_vu_lean_sse2neon "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_rrv_vu_lean_ok TRUE)
elseif(FETCHCONTENT_SOURCE_DIR_SSE2NEON AND EXISTS "${FETCHCONTENT_SOURCE_DIR_SSE2NEON}/sse2neon.h")
    set(_rrv_vu_lean_ok TRUE)
    set(_rrv_vu_lean_sse2neon "${FETCHCONTENT_SOURCE_DIR_SSE2NEON}")
else()
    set(_rrv_vu_lean_ok FALSE)
endif()
if(_rrv_vu_lean_ok AND EXISTS "${CMAKE_SOURCE_DIR}/generated/rr5/vu/rrv_vu_aot_blocks.inc")
    set(_rrv_rt "${CMAKE_SOURCE_DIR}/third_party/ps2recomp/ps2xRuntime")
    add_executable(rrv-vu-lean-diff-tests "${CMAKE_SOURCE_DIR}/tests/vu_lean_diff_tests.cpp")
    target_include_directories(rrv-vu-lean-diff-tests PRIVATE
        "${_rrv_rt}/src" "${_rrv_rt}/src/lib" "${_rrv_rt}/include" "${_rrv_rt}/include/runtime"
        "${_rrv_rt}/src/lib/Kernel" "${_rrv_rt}/src/lib/Kernel/Syscalls"
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/gs-backend" "${CMAKE_SOURCE_DIR}/src/gs-record"
        "${CMAKE_SOURCE_DIR}/src/snapshot" "${CMAKE_SOURCE_DIR}/src/diag" "${CMAKE_SOURCE_DIR}/src/host"
        "${CMAKE_SOURCE_DIR}/src/guest-time" "${CMAKE_SOURCE_DIR}/src/vu-aot"
        "${CMAKE_SOURCE_DIR}/src/vu-aot/compat" "${CMAKE_SOURCE_DIR}/generated/rr5/vu")
    target_compile_definitions(rrv-vu-lean-diff-tests PRIVATE PS2X_RRV_FIELD_ONLY=1 RRV_GS_FIELD_ONLY=1)
    # The same flags the runtime's VU source is built with: no FMA contraction (the VU FMAC rounds twice).
    target_compile_options(rrv-vu-lean-diff-tests PRIVATE -ffp-contract=off)
    if(_rrv_vu_lean_sse2neon)
        target_compile_definitions(rrv-vu-lean-diff-tests PRIVATE USE_SSE2NEON)
        target_include_directories(rrv-vu-lean-diff-tests PRIVATE "${_rrv_vu_lean_sse2neon}")
    else()
        target_compile_options(rrv-vu-lean-diff-tests PRIVATE -march=x86-64-v3)
    endif()
    target_link_libraries(rrv-vu-lean-diff-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME rrv-vu-lean-diff COMMAND rrv-vu-lean-diff-tests)

    # Whole-program native VU microprograms (RRV_VU_PROG, src/vu-aot/rrv_vu_prog_engine.inc) against the
    # interpreter: every program of the committed catalogue from hostile random state, at address 0 and
    # elsewhere, with the product's slot budget and with small ones (tests/vu_prog_diff_tests.cpp). Same
    # sources, include roots and flags as the test above.
    if(EXISTS "${CMAKE_SOURCE_DIR}/generated/rr5/vu/rrv_vu_aot_programs.inc")
        add_executable(rrv-vu-prog-diff-tests "${CMAKE_SOURCE_DIR}/tests/vu_prog_diff_tests.cpp")
        get_target_property(_rrv_vu_prog_includes rrv-vu-lean-diff-tests INCLUDE_DIRECTORIES)
        target_include_directories(rrv-vu-prog-diff-tests PRIVATE ${_rrv_vu_prog_includes})
        target_compile_definitions(rrv-vu-prog-diff-tests PRIVATE PS2X_RRV_FIELD_ONLY=1 RRV_GS_FIELD_ONLY=1)
        target_compile_options(rrv-vu-prog-diff-tests PRIVATE -ffp-contract=off)
        if(_rrv_vu_lean_sse2neon)
            target_compile_definitions(rrv-vu-prog-diff-tests PRIVATE USE_SSE2NEON)
        else()
            target_compile_options(rrv-vu-prog-diff-tests PRIVATE -march=x86-64-v3)
        endif()
        target_link_libraries(rrv-vu-prog-diff-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME rrv-vu-prog-diff COMMAND rrv-vu-prog-diff-tests)
    endif()

    # PS2Runtime::isSpecialAddress (ps2_runtime.h, header only) against the range tests it shortcuts. Same
    # include roots and SIMD backend as the test above.
    add_executable(rrv-special-address-tests "${CMAKE_SOURCE_DIR}/tests/special_address_tests.cpp")
    get_target_property(_rrv_vu_lean_includes rrv-vu-lean-diff-tests INCLUDE_DIRECTORIES)
    target_include_directories(rrv-special-address-tests PRIVATE ${_rrv_vu_lean_includes})
    target_compile_definitions(rrv-special-address-tests PRIVATE PS2X_RRV_FIELD_ONLY=1 RRV_GS_FIELD_ONLY=1)
    if(_rrv_vu_lean_sse2neon)
        target_compile_definitions(rrv-special-address-tests PRIVATE USE_SSE2NEON)
    else()
        target_compile_options(rrv-special-address-tests PRIVATE -march=x86-64-v3)
    endif()
    add_test(NAME rrv-special-address COMMAND rrv-special-address-tests)
endif()
if(RRV_M2_CAUSAL_TRACE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvM2CausalTrace.cmake")
    rrv_m2_causal_trace_add_library()
    target_link_libraries(rrv_gs_backend PUBLIC rrv_m2_causal_trace)
    add_executable(rrv-m2-causal-trace-tests
        "${CMAKE_SOURCE_DIR}/tests/m2_causal_trace_tests.cpp")
    target_link_libraries(rrv-m2-causal-trace-tests PRIVATE rrv_m2_causal_trace)
    add_test(NAME rrv-m2-causal-trace COMMAND rrv-m2-causal-trace-tests)
    if(RRV_M2_DMA_PROVENANCE)
        add_executable(rrv-m2-dma-provenance-tests
            "${CMAKE_SOURCE_DIR}/tests/m2_dma_provenance_tests.cpp")
        target_link_libraries(rrv-m2-dma-provenance-tests PRIVATE rrv_m2_causal_trace)
        add_test(NAME rrv-m2-dma-provenance COMMAND rrv-m2-dma-provenance-tests)
    endif()
    if(RRV_M2_GUEST_PROVENANCE)
        add_executable(rrv-m2-guest-provenance-tests
            "${CMAKE_SOURCE_DIR}/tests/m2_guest_provenance_tests.cpp")
        target_link_libraries(rrv-m2-guest-provenance-tests PRIVATE rrv_m2_causal_trace)
        add_test(NAME rrv-m2-guest-provenance COMMAND rrv-m2-guest-provenance-tests)
    endif()
endif()
target_compile_definitions(rrv_gs_backend PUBLIC RRV_GS_EPOCH_DIAGNOSTICS=1)
if(RRV_M2_NEUTRALITY_CONTROL)
    target_sources(rrv_gs_backend PRIVATE
        "${CMAKE_SOURCE_DIR}/src/gs-backend/rrv_m2_neutrality.cpp")
    target_compile_definitions(rrv_gs_backend PUBLIC RRV_M2_NEUTRALITY_CONTROL=1)
endif()
if(RRV_GS_CONSUMER_RECEIPT_CLIENT)
    # Match the product target's optional client ABI. This asset-free build
    # exercises the dynamic post-VSync arm contract against the fake bridge;
    # production remains unaffected when the option is OFF.
    target_compile_definitions(rrv_gs_backend PUBLIC
        RRV_GS_CONSUMER_RECEIPT_CLIENT=1)
endif()

# The recovered backend test also exercises the GS recorder hooks. Keep this
# checked-in leaf available in asset-free mode without pulling PS2Recomp or the
# diagnostic renderer graph below the M0 return boundary.
file(GLOB RRV_GS_RECORD_SRCS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/gs-record/*.cpp")
add_library(rrv_gs_record STATIC ${RRV_GS_RECORD_SRCS})
target_include_directories(rrv_gs_record
    PUBLIC "${CMAKE_SOURCE_DIR}/src/gs-record"
    PRIVATE "${CMAKE_SOURCE_DIR}/src/gs-backend"
)

add_library(rrv-test-fake-pcsx2-gs-bridge SHARED
    "${CMAKE_SOURCE_DIR}/tests/fake_pcsx2_gs_bridge.cpp"
)
target_compile_definitions(rrv_gs_backend PUBLIC
    RRV_GS_TEST_FAKE_BRIDGE_PATH="$<TARGET_FILE:rrv-test-fake-pcsx2-gs-bridge>")
add_executable(rrv-gs-backend-tests
    "${CMAKE_SOURCE_DIR}/tests/gs_backend_tests.cpp"
)
target_link_libraries(rrv-gs-backend-tests PRIVATE rrv_gs_backend rrv_gs_record)
target_compile_definitions(rrv-gs-backend-tests PRIVATE
    RRV_TEST_FAKE_GS_BRIDGE="$<TARGET_FILE:rrv-test-fake-pcsx2-gs-bridge>"
)
add_dependencies(rrv-gs-backend-tests rrv-test-fake-pcsx2-gs-bridge)
add_test(NAME rrv-gs-backend COMMAND rrv-gs-backend-tests)

add_executable(rrv-canonical-receipt-tests
    "${CMAKE_SOURCE_DIR}/tests/canonical_receipt_tests.cpp"
    "${CMAKE_SOURCE_DIR}/tools/pcsx2-gs-bridge/canonical_receipt.cpp"
)
target_include_directories(rrv-canonical-receipt-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/tools/pcsx2-gs-bridge"
)
add_test(NAME rrv-canonical-receipt COMMAND rrv-canonical-receipt-tests)

if(RRV_M2_NEUTRALITY_CONTROL)
    add_executable(rrv-m2-neutrality-control-tests
        "${CMAKE_SOURCE_DIR}/tests/m2_neutrality_control_tests.cpp")
    target_link_libraries(rrv-m2-neutrality-control-tests PRIVATE rrv_gs_backend)
    add_test(NAME rrv-m2-neutrality-control COMMAND rrv-m2-neutrality-control-tests)
    # A stale/failed optional capture must not erase the completed independent
    # workload manifest. This fixture has no active backend by construction.
    add_test(NAME rrv-m2-neutrality-control-failed-capture
        COMMAND rrv-m2-neutrality-control-tests --failed-capture)
endif()

# M2 field-identity receipt generator contract. The fixture is synthetic and
# contains no game data; it fails closed on a skipped guest field.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME rrv-asset-free-runner-self-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_asset_free_qualification.py")
    add_test(NAME rrv-vu-eatan
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_vu_eatan.py")
    add_test(NAME rrv-vu-aot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_vu_aot.py")
    add_test(NAME rrv-vu-prog
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_vu_prog.py")
    add_test(NAME rrv-vu-codegen-writers
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_vu_codegen_writers.py")
    add_test(NAME rrv-pad-rumble
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_pad_rumble.py")
    add_test(NAME rrv-wait-idle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_wait_idle.py")
    add_test(NAME rrv-product-elf-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_product_elf_check.py")
    add_test(NAME rrv-pcsx2-patch-series
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_pcsx2_patch_series.py" SeriesTests)
    add_test(NAME rrv-pcsx2-patch-series-tree
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_pcsx2_patch_series.py" TreeTests)
    add_test(NAME rrv-ee-fpclamp
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_ee_fpclamp.py")
    if(TARGET rrv-launch-env)
        add_test(NAME rrv-launch-env-golden
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_launch_env_golden.py")
        set_tests_properties(rrv-launch-env-golden PROPERTIES
            ENVIRONMENT "RRV_LAUNCH_ENV_TOOL=$<TARGET_FILE:rrv-launch-env>")
    endif()
    add_test(NAME rrv-source-ownership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_source_ownership.py")
    add_test(NAME rrv-ee-native-gen
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_ee_native_gen.py")
    add_test(NAME rrv-product-no-raylib-gate
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/product_no_raylib_gate_tests.py")
    add_test(NAME rrv-product-identity-receipt
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_product_identity_receipt.py")
    add_test(NAME rrv-package-product-runtime
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_package_product_runtime.py")
    # G1-H1E has a deliberately native macOS ARM64 contract. It is visible to
    # native CTest selection but never inflates the portable asset-free suite.
    if(APPLE AND (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64") AND CMAKE_GENERATOR STREQUAL "Ninja" AND CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME rrv-resource-package
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/test_resource_package.py")
        add_test(NAME rrv-resource-package-build
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/test_resource_package_build.py")
        add_test(NAME rrv-resource-package-lifecycle
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/test_resource_package_lifecycle.py")
        add_test(NAME rrv-adr0006-feedback-fixture
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/test_adr0006_feedback_fixture.py"
                --source-root "${CMAKE_SOURCE_DIR}"
                --build-root "${CMAKE_SOURCE_DIR}/build/adr0006-integration/feedback-fixture")
        set_tests_properties(rrv-resource-package rrv-resource-package-build rrv-resource-package-lifecycle rrv-adr0006-feedback-fixture
            PROPERTIES LABELS "g1-h1e-native")
    endif()
    set(RRV_CANONICAL_RECEIPT_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/canonical-receipt-parser-fixture.bin")
    add_test(NAME rrv-canonical-receipt-fixture
        COMMAND rrv-canonical-receipt-tests "${RRV_CANONICAL_RECEIPT_FIXTURE}")
    set_tests_properties(rrv-canonical-receipt-fixture PROPERTIES
        FIXTURES_SETUP RRV_CANONICAL_RECEIPT_FIXTURE)
endif()

# Development-phase harness tests (M2 / Gate-era controls, overlay checks, renderer-IR header contracts) live in their
# own file, which the public export leaves out. It comes before the property lists below, which skip tests that do not
# exist in a checkout without it.
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/RrvHarnessTests.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/RrvHarnessTests.cmake")
endif()

# set_tests_properties for the tests that exist (the harness ones are absent from a public checkout).
function(rrv_set_existing_tests_properties)
    cmake_parse_arguments(RRV_P "" "" "PROPERTIES" ${ARGN})
    set(_existing "")
    foreach(_t IN LISTS RRV_P_UNPARSED_ARGUMENTS)
        if(TEST ${_t})
            list(APPEND _existing ${_t})
        endif()
    endforeach()
    if(_existing)
        set_tests_properties(${_existing} PROPERTIES ${RRV_P_PROPERTIES})
    endif()
endfunction()

rrv_set_existing_tests_properties(
    rrv-m2-causal-overlay rrv-m2p-pad-overlay rrv-product-host-overlay
    rrv-m2p-game001-overlay rrv-m2-guest-overlay rrv-ee-fpclamp
    rrv-pcsx2-patch-series-tree rrv-product-elf-check
    rrv-callback-stack-main-reservation-overlay
    PROPERTIES LABELS input-dependent)




add_executable(rrv-snapshot-tests
    "${CMAKE_SOURCE_DIR}/tests/snapshot_container_tests.cpp"
    "${CMAKE_SOURCE_DIR}/src/snapshot/rrv_snapshot_file.cpp"
    "${CMAKE_SOURCE_DIR}/src/snapshot/rrv_snapshot_cli.cpp"
)
target_include_directories(rrv-snapshot-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/snapshot"
)
add_test(NAME rrv-snapshot-container COMMAND rrv-snapshot-tests)

# Gate 3 pure guest-time modules and their asset-free tests.
add_executable(rrv-gate3-guest-time-tests
    "${CMAKE_SOURCE_DIR}/src/guest-time/rrv_guest_time.cpp"
    "${CMAKE_SOURCE_DIR}/tests/gate3_guest_time_tests.cpp")
target_include_directories(rrv-gate3-guest-time-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/guest-time")
target_compile_features(rrv-gate3-guest-time-tests PRIVATE cxx_std_20)
add_test(NAME rrv-gate3-guest-time COMMAND rrv-gate3-guest-time-tests)

add_executable(rrv-gate3-ee-timers-tests
    "${CMAKE_SOURCE_DIR}/src/guest-time/rrv_ee_timers.cpp"
    "${CMAKE_SOURCE_DIR}/tests/gate3_ee_timers_tests.cpp")
set_target_properties(rrv-gate3-ee-timers-tests PROPERTIES CXX_STANDARD 17)
add_test(NAME rrv-gate3-ee-timers COMMAND rrv-gate3-ee-timers-tests)

add_executable(rrv-gate3-intc-tests
    "${CMAKE_SOURCE_DIR}/src/guest-time/rrv_intc.cpp"
    "${CMAKE_SOURCE_DIR}/tests/gate3_intc_tests.cpp")
set_target_properties(rrv-gate3-intc-tests PROPERTIES CXX_STANDARD 17)
add_test(NAME rrv-gate3-intc COMMAND rrv-gate3-intc-tests)

add_executable(rrv-gate3-guest-rtc-tests
    "${CMAKE_SOURCE_DIR}/src/guest-time/rrv_guest_rtc.cpp"
    "${CMAKE_SOURCE_DIR}/tests/gate3_guest_rtc_tests.cpp")
set_target_properties(rrv-gate3-guest-rtc-tests PROPERTIES CXX_STANDARD 17)
add_test(NAME rrv-gate3-guest-rtc COMMAND rrv-gate3-guest-rtc-tests)

# G0-B portable qualification target. Keep this list in lock-step with
# scripts/asset_free_suite.json; the runner validates registration and result
# selection against that manifest before executing it.
add_custom_target(rrv-asset-free-tests)
set(RRV_ASSET_FREE_REQUIRED_TARGETS
    rrv-gs-worker-tests
    rrv-gs-control-tests
    rrv-gs-control-stream-tests
    rrv-gs-result-boundary-tests
    rrv-guest-terminal-outcome-tests
    rrv-gs-backend-tests
    rrv-canonical-receipt-tests
    rrv-ir-metal-resident-manifest-tests
    rrv-ir-metal-feedback-gate-tests
    rrv-ir-metal-blend-cache-tests
    rrv-snapshot-tests
    rrv-gate3-guest-time-tests
    rrv-gate3-ee-timers-tests
    rrv-gate3-intc-tests
    rrv-gate3-guest-rtc-tests
    rrv-fukami-settings-tests
    rrv-fukami-menu-tests
    rrv-fp-rounding-tests
    rrv-ring-copy-tests
    rrv-cursor-idle-tests
    rrv-launch-env)
foreach(RRV_ASSET_FREE_TARGET IN LISTS RRV_ASSET_FREE_REQUIRED_TARGETS)
    # a target of the harness file is absent from a public checkout (scripts/asset_free_suite.json is pruned to match)
    if(TARGET ${RRV_ASSET_FREE_TARGET})
        add_dependencies(rrv-asset-free-tests ${RRV_ASSET_FREE_TARGET})
    endif()
endforeach()
rrv_set_existing_tests_properties(
    rrv-asset-free-runner-self-test
    rrv-gs-worker rrv-gs-control rrv-gs-control-stream rrv-gs-result-boundary
    rrv-guest-terminal-outcome rrv-gs-backend rrv-canonical-receipt
    rrv-m2-initial-state-controls rrv-m2-closeout-controls
    rrv-m2-causal-controls rrv-product-no-raylib-gate
    rrv-product-identity-receipt rrv-package-product-runtime
    rrv-m2p-game001-runner rrv-m2-dma-runtime-controls rrv-m2-dma-controls
    rrv-m2-dma-analyze rrv-m2-guest-runtime-controls rrv-m2-guest-analyze
    rrv-m2-guest-live-controls rrv-m2-replay-matrix rrv-m2-live-edge-matrix
    rrv-m2-field-manifest rrv-canonical-receipt-fixture
    rrv-canonical-receipt-parser rrv-ir-metal-resident-manifest
    rrv-ir-metal-feedback-gate rrv-ir-metal-blend-cache rrv-snapshot-container
    rrv-product-locked-patch rrv-gate3-workload-manifest rrv-gate3-guest-time
    rrv-gate3-ee-timers rrv-gate3-intc rrv-gate3-guest-rtc
    rrv-gate3-compile-workload rrv-gate3-hle-cost-inventory rrv-pad-rumble
    rrv-wait-idle rrv-vu-eatan rrv-vu-aot rrv-vu-prog rrv-make-replay rrv-check-docs rrv-launch-env-golden rrv-source-ownership rrv-ee-native-gen rrv-pcsx2-patch-series rrv-fukami-settings rrv-fukami-menu
    rrv-fp-rounding rrv-ring-copy rrv-cursor-idle rrv-vu-codegen-writers
    PROPERTIES LABELS portable-asset-free)
rrv_set_existing_tests_properties(
    rrv-asset-free-runner-self-test rrv-m2-initial-state-controls
    rrv-m2-closeout-controls rrv-m2-causal-controls rrv-product-no-raylib-gate
    rrv-product-identity-receipt rrv-package-product-runtime
    rrv-m2p-game001-runner rrv-m2-dma-runtime-controls rrv-m2-dma-controls
    rrv-m2-dma-analyze rrv-m2-guest-runtime-controls rrv-m2-guest-analyze
    rrv-m2-guest-live-controls rrv-m2-replay-matrix rrv-m2-live-edge-matrix
    rrv-m2-field-manifest rrv-canonical-receipt-parser rrv-product-locked-patch
    rrv-gate3-workload-manifest rrv-gate3-compile-workload rrv-gate3-hle-cost-inventory
    rrv-pad-rumble rrv-wait-idle rrv-vu-eatan rrv-vu-aot rrv-vu-prog rrv-make-replay rrv-check-docs rrv-launch-env-golden rrv-source-ownership
    rrv-ee-native-gen rrv-pcsx2-patch-series rrv-vu-codegen-writers
    PROPERTIES FAIL_REGULAR_EXPRESSION "OK \\(skipped=[1-9][0-9]*\\)")

# Three of those tests also check the generated game code. A checkout without it (a public one, before
# scripts/fukami_generate.py has run) has nothing to check there: those cases skip, which is printed, and
# the skip is not a failure. With the generated tree present a skip still fails the run.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/generated/rr5/output")
    message(STATUS "rrv asset-free: generated game code absent; its checks skip (run scripts/fukami_generate.py)")
    set_tests_properties(rrv-vu-prog rrv-source-ownership rrv-ee-native-gen PROPERTIES FAIL_REGULAR_EXPRESSION "")
endif()
