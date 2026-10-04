cmake_minimum_required(VERSION 3.21)
project(RRV-Recomp VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(rrv-gs-worker-tests tests/gs_worker_tests.cpp)
    target_include_directories(rrv-gs-worker-tests PRIVATE src/gs-backend)
    target_link_libraries(rrv-gs-worker-tests PRIVATE Threads::Threads)
    add_test(NAME rrv-gs-worker COMMAND rrv-gs-worker-tests)
    add_executable(rrv-gs-control-tests tests/gs_control_tests.cpp)
    add_test(NAME rrv-gs-control COMMAND rrv-gs-control-tests)
    add_executable(rrv-gs-control-stream-tests tests/gs_control_stream_tests.cpp)
    add_test(NAME rrv-gs-control-stream COMMAND rrv-gs-control-stream-tests)
    add_executable(rrv-gs-result-boundary-tests tests/gs_result_boundary_tests.cpp)
    add_test(NAME rrv-gs-result-boundary COMMAND rrv-gs-result-boundary-tests)
endif()


# M0 product/oracle boundary.  The existing runtime remains available only as
# an explicitly requested diagnostic build while its bridge ABI and present
# path are brought back into a reproducible state.  The default configuration
# is intentionally asset-free: it neither requires a user ELF nor reaches out
# to PS2Recomp, raylib, or FetchContent.
option(RRV_BUILD_DIAGNOSTIC_RUNTIME
    "Build the legacy rrv-recomp diagnostic runtime (requires local ELF and PS2Recomp)" OFF)
option(RRV_BUILD_LEGACY_LIVE
    "Build the source-matched ABI-5 field diagnostic target" OFF)
option(RRV_BUILD_PRODUCT
    "Build the macOS Metal direct-present product with its SDL-only host" OFF)
option(RRV_PRODUCT_OWNED_SOURCE
    "Build the product from the committed third_party/ps2recomp, src/product and generated/rr5 sources (no stage, no overlays)" OFF)
option(RRV_BUILD_RECOMPILER
    "Build the PS2Recomp analyzer and recompiler from third_party/ps2recomp (re-extraction only; never part of the product)" OFF)
option(RRV_GS_PRODUCER_CONTROL
    "Build the isolated synchronous producer GS control candidate (startup opt-in)" OFF)
option(RRV_CALLBACK_STACK_MAIN_RESERVATION
    "Reserve guest main-stack space before callback-stack allocation" ${RRV_BUILD_PRODUCT})
option(RRV_IOP_HEAP_ISOLATION
    "Keep synthetic IOP heap allocations separate from EE RDRAM" ${RRV_CALLBACK_STACK_MAIN_RESERVATION})
option(RRV_RPC_MEMORY_SAFETY
    "Bound RPC receive writes and isolate nested guest RPC stacks" ${RRV_IOP_HEAP_ISOLATION})
option(RRV_BUILD_ORACLE_F7
    "Build the archived F7/P3 offline oracle from RRV_ORACLE_F7_SOURCE_DIR" OFF)
option(RRV_M2_NEUTRALITY_CONTROL
    "Enable the bounded in-memory M2 semantic-interval control" OFF)
option(RRV_GS_CONSUMER_RECEIPT_CLIENT
    "Enable the optional M2 consumer-receipt bridge client" OFF)
option(RRV_M2_CAUSAL_TRACE
    "Compile the bounded diagnostic-only M2 upstream causal trace" OFF)
option(RRV_M2_DMA_PROVENANCE
    "Compile bounded VIF1 origin/source-chain diagnostics (requires causal trace)" OFF)
option(RRV_M2_GUEST_PROVENANCE
    "Compile bounded actual guest exit and exact-range write diagnostics" OFF)
option(RRV_M2_INITIAL_STATE_FINGERPRINT
    "Compile one-shot pre-execution state fingerprint (diagnostic overlay only)" OFF)
option(RRV_M2P_PAD_OBSERVER
    "Compile the bounded host-to-guest pad packet observer (diagnostic overlay only)" OFF)
option(RRV_M2P_GAME001_FAIL_CLOSED
    "Compile the bounded GAME-001 pre-lookup fail-closed diagnostic overlay" OFF)
if(RRV_M2_INITIAL_STATE_FINGERPRINT AND NOT RRV_M2_CAUSAL_TRACE)
    message(FATAL_ERROR "RRV_M2_INITIAL_STATE_FINGERPRINT requires the diagnostic producer overlay")
endif()
if(RRV_M2_GUEST_PROVENANCE AND NOT RRV_M2_DMA_PROVENANCE)
    message(FATAL_ERROR "RRV_M2_GUEST_PROVENANCE requires RRV_M2_DMA_PROVENANCE")
endif()
if(RRV_M2_DMA_PROVENANCE AND NOT RRV_M2_CAUSAL_TRACE)
    message(FATAL_ERROR "RRV_M2_DMA_PROVENANCE requires RRV_M2_CAUSAL_TRACE")
endif()
if(RRV_GS_PRODUCER_CONTROL AND NOT RRV_BUILD_PRODUCT)
    message(FATAL_ERROR "RRV_GS_PRODUCER_CONTROL requires the isolated macOS product candidate")
endif()
if(RRV_CALLBACK_STACK_MAIN_RESERVATION AND NOT RRV_BUILD_PRODUCT)
    message(FATAL_ERROR "RRV_CALLBACK_STACK_MAIN_RESERVATION requires RRV_BUILD_PRODUCT")
endif()
if(RRV_IOP_HEAP_ISOLATION AND NOT RRV_CALLBACK_STACK_MAIN_RESERVATION)
    message(FATAL_ERROR "RRV_IOP_HEAP_ISOLATION requires RRV_CALLBACK_STACK_MAIN_RESERVATION")
endif()
if(RRV_RPC_MEMORY_SAFETY AND NOT RRV_IOP_HEAP_ISOLATION)
    message(FATAL_ERROR "RRV_RPC_MEMORY_SAFETY requires RRV_IOP_HEAP_ISOLATION")
endif()
if(RRV_CALLBACK_STACK_MAIN_RESERVATION AND RRV_M2_CAUSAL_TRACE)
    message(FATAL_ERROR
        "RRV_CALLBACK_STACK_MAIN_RESERVATION is intentionally isolated from the M2 causal trace")
endif()
if(RRV_GS_PRODUCER_CONTROL AND
   (RRV_M2_CAUSAL_TRACE OR RRV_M2P_PAD_OBSERVER OR RRV_M2P_GAME001_FAIL_CLOSED))
    message(FATAL_ERROR "GS control qualification requires ordinary producer sources without intrusive overlays")
endif()

set(RRV_SELECTED_RUNTIME_MODES 0)
foreach(RRV_RUNTIME_MODE IN ITEMS
        RRV_BUILD_ORACLE_F7 RRV_BUILD_DIAGNOSTIC_RUNTIME RRV_BUILD_LEGACY_LIVE
        RRV_BUILD_PRODUCT)
    if(${RRV_RUNTIME_MODE})
        math(EXPR RRV_SELECTED_RUNTIME_MODES "${RRV_SELECTED_RUNTIME_MODES}+1")
    endif()
endforeach()
if(RRV_SELECTED_RUNTIME_MODES GREATER 1)
    message(FATAL_ERROR
        "RRV_BUILD_ORACLE_F7, RRV_BUILD_DIAGNOSTIC_RUNTIME, RRV_BUILD_LEGACY_LIVE, and RRV_BUILD_PRODUCT "
        "are mutually exclusive. The F7/P3 oracle must remain an independent offline build.")
endif()
if(RRV_M2P_PAD_OBSERVER AND RRV_M2_GUEST_PROVENANCE)
    message(FATAL_ERROR "RRV_M2P_PAD_OBSERVER does not compose with RRV_M2_GUEST_PROVENANCE")
endif()
if(RRV_M2P_PAD_OBSERVER AND NOT RRV_BUILD_PRODUCT)
    message(FATAL_ERROR "RRV_M2P_PAD_OBSERVER requires RRV_BUILD_PRODUCT")
endif()
if(RRV_M2P_GAME001_FAIL_CLOSED AND NOT RRV_BUILD_PRODUCT)
    message(FATAL_ERROR "RRV_M2P_GAME001_FAIL_CLOSED requires RRV_BUILD_PRODUCT")
endif()
if(RRV_M2P_GAME001_FAIL_CLOSED AND RRV_M2_CAUSAL_TRACE)
    message(FATAL_ERROR
        "RRV_M2P_GAME001_FAIL_CLOSED is intentionally isolated from the M2 causal/write overlay")
endif()
if(RRV_M2P_GAME001_FAIL_CLOSED AND RRV_M2P_PAD_OBSERVER)
    message(FATAL_ERROR
        "RRV_M2P_GAME001_FAIL_CLOSED is intentionally isolated from PAD-001 instrumentation")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/RrvProductGate.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/RrvDiscExtract.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/RrvFukamiApp.cmake")

if(RRV_BUILD_ORACLE_F7)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/cmake/RrvOracleF7.cmake")
        message(FATAL_ERROR "RRV_BUILD_ORACLE_F7 is not available in this checkout")
    endif()
    include("${CMAKE_SOURCE_DIR}/cmake/RrvOracleF7.cmake")
endif()

if(RRV_BUILD_RECOMPILER)
    # Re-extraction tooling only (generated/rr5/README.md). Builds ps2_analyzer and ps2_recomp
    # from the committed sources; the runtime, tests and studio are not built.
    if(RRV_BUILD_PRODUCT OR RRV_BUILD_LEGACY_LIVE OR RRV_BUILD_DIAGNOSTIC_RUNTIME)
        message(FATAL_ERROR "RRV_BUILD_RECOMPILER is a standalone configuration")
    endif()
    set(PS2X_BUILD_RECOMP ON CACHE BOOL "" FORCE)
    set(PS2X_BUILD_ANALYZER ON CACHE BOOL "" FORCE)
    set(PS2X_BUILD_RUNTIME OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/ps2recomp" ps2recomp EXCLUDE_FROM_ALL)
    message(STATUS "RRV recompiler: build targets ps2_analyzer and ps2_recomp (not part of the product)")
    return()
endif()

if(RRV_BUILD_LEGACY_LIVE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvLegacyLive.cmake")
    return()
endif()

if(RRV_BUILD_PRODUCT)
    if(NOT APPLE AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"))
        message(FATAL_ERROR
            "rrv-product is a macOS Metal or Linux x86-64 Vulkan direct-present target; refusing a fallback presentation path")
    endif()
    include("${CMAKE_SOURCE_DIR}/cmake/RrvProduct.cmake")
    return()
endif()

if(NOT RRV_BUILD_DIAGNOSTIC_RUNTIME)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvAssetFreeTests.cmake")
    message(STATUS "RRV M0 mode: asset-free verification; legacy diagnostic runtime is disabled")
    message(STATUS "RRV product status: disabled (enable RRV_BUILD_PRODUCT with verified M1 inputs)")
    return()
endif()

# The pre-M0 diagnostic runtime and the offline oracle tools around it live in their own file. A checkout without it
# (the public repository) does not offer them.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/cmake/RrvDiagnosticRuntime.cmake")
    message(FATAL_ERROR "RRV_BUILD_DIAGNOSTIC_RUNTIME is not available in this checkout")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/RrvDiagnosticRuntime.cmake")
