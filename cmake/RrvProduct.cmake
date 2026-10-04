# M1 product graph.  It intentionally reuses the same reconstructed producer
# and field-only guest runtime as rrv-legacy-live, with an SDL-only product
# host for audio and the existing direct Metal presentation surface.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(READ "${CMAKE_SOURCE_DIR}/config/dependencies.lock.toml" RRV_DEPENDENCY_LOCK_TEXT)

string(FIND "${RRV_DEPENDENCY_LOCK_TEXT}" "[sdl]" RRV_SDL_SECTION)
if(RRV_SDL_SECTION EQUAL -1)
    message(FATAL_ERROR "dependencies.lock.toml is missing the immutable SDL 2.32.10 pin")
endif()
string(SUBSTRING "${RRV_DEPENDENCY_LOCK_TEXT}" ${RRV_SDL_SECTION} -1 RRV_SDL_LOCK_TEXT)
string(REGEX MATCH "version = \"([^\"]+)\"" RRV_SDL_VERSION_MATCH "${RRV_SDL_LOCK_TEXT}")
set(RRV_SDL_VERSION "${CMAKE_MATCH_1}")
string(REGEX MATCH "revision = \"([0-9a-f]+)\"" RRV_SDL_REVISION_MATCH "${RRV_SDL_LOCK_TEXT}")
set(RRV_SDL_2_32_10_REVISION "${CMAKE_MATCH_1}")
if(NOT RRV_SDL_VERSION STREQUAL "2.32.10" OR
   NOT RRV_SDL_2_32_10_REVISION STREQUAL "5d249570393f7a37e037abf22cd6012a4cc56a71")
    message(FATAL_ERROR "SDL lock must pin release-2.32.10 commit 5d249570393f7a37e037abf22cd6012a4cc56a71")
endif()

if(APPLE)
    # SDL is installed once outside disposable product build trees. Configure is
    # deliberately offline and fails closed unless its pin and installed hashes match.
    if(CMAKE_OSX_ARCHITECTURES)
        set(RRV_SDL_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(RRV_SDL_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT RRV_SDL_ARCH MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "Product SDL requires one native macOS architecture: arm64 or x86_64")
    endif()
    set(RRV_SDL_PREFIX "${CMAKE_SOURCE_DIR}/runtime-deps/sdl/${RRV_SDL_VERSION}/macos-${RRV_SDL_ARCH}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/prepare_sdl.py"
            --verify --prefix "${RRV_SDL_PREFIX}" --platform "macos-${RRV_SDL_ARCH}"
        RESULT_VARIABLE RRV_SDL_VERIFY_RESULT
        OUTPUT_VARIABLE RRV_SDL_VERIFY_OUTPUT
        ERROR_VARIABLE RRV_SDL_VERIFY_ERROR)
    if(NOT RRV_SDL_VERIFY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Stable SDL installation is missing or invalid: ${RRV_SDL_VERIFY_ERROR}"
            "Run python3 scripts/prepare_sdl.py --prepare before configuring the product.")
    endif()
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${RRV_SDL_PREFIX}/lib/libSDL2-2.0.0.dylib"
        IMPORTED_SONAME "@rpath/libSDL2-2.0.0.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${RRV_SDL_PREFIX}/include/SDL2")
    # Reconfigure when the installed receipt changes. No FetchContent SDL runtime
    # directory is part of this graph; CMake derives RPATH from the imported prefix.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${RRV_SDL_PREFIX}/manifest.json")
else()
    # Linux / Steam Deck (Gate 5): SDL 2.32.10 built from the same pinned
    # release by tools/linux-build (X11 + Wayland + Vulkan), found through
    # CMAKE_PREFIX_PATH (/opt/rrv-deps in the rrv-linux-build image) or
    # RRV_LINUX_SDL_PREFIX. The shared library ships in the app's lib/.
    set(RRV_SDL_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(RRV_LINUX_SDL_PREFIX "" CACHE PATH "SDL 2.32.10 install prefix (Linux)")
    find_path(RRV_SDL_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2 HINTS "${RRV_LINUX_SDL_PREFIX}/include" REQUIRED)
    find_library(RRV_SDL_LIBRARY NAMES SDL2-2.0 SDL2 HINTS "${RRV_LINUX_SDL_PREFIX}/lib" REQUIRED)
    file(STRINGS "${RRV_SDL_INCLUDE_DIR}/SDL_version.h" _rrv_sdl_version_lines
        REGEX "#define SDL_(MAJOR_VERSION|MINOR_VERSION|PATCHLEVEL) +[0-9]+")
    string(REGEX REPLACE ".*SDL_MAJOR_VERSION +([0-9]+).*SDL_MINOR_VERSION +([0-9]+).*SDL_PATCHLEVEL +([0-9]+).*"
        "\\1.\\2.\\3" _rrv_sdl_found_version "${_rrv_sdl_version_lines}")
    if(NOT _rrv_sdl_found_version STREQUAL RRV_SDL_VERSION)
        message(FATAL_ERROR "Linux product requires SDL ${RRV_SDL_VERSION}, found ${_rrv_sdl_found_version} at ${RRV_SDL_INCLUDE_DIR}")
    endif()
    get_filename_component(RRV_SDL_PREFIX "${RRV_SDL_INCLUDE_DIR}/../.." ABSOLUTE)
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${RRV_SDL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${RRV_SDL_INCLUDE_DIR}")
    message(STATUS "RRV product (Linux): SDL ${_rrv_sdl_found_version} ${RRV_SDL_LIBRARY}")
endif()

# Prepare SDL before selecting the product-only runtime host. Legacy-live
# configurations continue to use their original producer and host backend.
include("${CMAKE_SOURCE_DIR}/cmake/RrvLegacyLive.cmake")

add_library(rrv_sdl_product_host STATIC
    "${CMAKE_SOURCE_DIR}/src/host/rrv_sdl_presentation.cpp")
target_include_directories(rrv_sdl_product_host PUBLIC
    "${CMAKE_SOURCE_DIR}/src/host"
    "${CMAKE_SOURCE_DIR}/src/gs-backend"
    "${CMAKE_SOURCE_DIR}/src/diag"
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include")
target_link_libraries(rrv_sdl_product_host PUBLIC SDL2::SDL2)
if(RRV_M2P_PAD_OBSERVER)
    target_include_directories(rrv_sdl_product_host BEFORE PUBLIC
        "${CMAKE_BINARY_DIR}/m2p-pad-overlay/include")
    target_compile_definitions(rrv_sdl_product_host PUBLIC RRV_M2P_PAD_OBSERVER=1)
    target_link_libraries(rrv_sdl_product_host PUBLIC rrv_m2p_pad_observer)
endif()

# Test-only deterministic A/B control. It replays one recorded GS stream
# through either legacy snapshot presentation or the SDL-backed direct Metal
# seam. It is deliberately separate from the product compile/link graph.
add_executable(rrv-m1-present-replay EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tools/gs-replay/m1_present_replay.cpp")
target_include_directories(rrv-m1-present-replay PRIVATE
    "${CMAKE_SOURCE_DIR}/src/gs-record")
target_link_libraries(rrv-m1-present-replay PRIVATE
    rrv_legacy_gs_backend
    rrv_sdl_product_host)
target_compile_features(rrv-m1-present-replay PRIVATE cxx_std_20)

if(BUILD_TESTING)
    add_executable(rrv-sdl-audio-tests
        "${CMAKE_SOURCE_DIR}/tests/sdl_audio_tests.cpp"
        "${CMAKE_SOURCE_DIR}/src/host/rrv_sdl_audio.cpp")
    target_include_directories(rrv-sdl-audio-tests PRIVATE "${CMAKE_SOURCE_DIR}/src/host")
    target_compile_definitions(rrv-sdl-audio-tests PRIVATE RRV_SDL_AUDIO_TESTING=1)
    target_link_libraries(rrv-sdl-audio-tests PRIVATE SDL2::SDL2)
    add_test(NAME rrv-sdl-audio COMMAND rrv-sdl-audio-tests)
    set_tests_properties(rrv-sdl-audio PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
    add_test(NAME rrv-sdl-audio-unavailable COMMAND rrv-sdl-audio-tests --failure)
    set_tests_properties(rrv-sdl-audio-unavailable PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=rrv-unavailable-test-driver")
    add_test(NAME rrv-product-host-overlay
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_product_host_overlay.py")
endif()

if(RRV_PRODUCT_OWNED_SOURCE)
    add_executable(rrv-product
        "${CMAKE_SOURCE_DIR}/src/product/main_product.cpp"
        "${CMAKE_SOURCE_DIR}/src/product/patches.cpp")
else()
    add_executable(rrv-product
        "${CMAKE_SOURCE_DIR}/src/main_product.cpp"
        "${CMAKE_SOURCE_DIR}/src/patches.cpp")
endif()
target_include_directories(rrv-product PRIVATE
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
    "${RRV_GENERATED_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/gs-record"
    "${CMAKE_SOURCE_DIR}/src/host")
target_compile_definitions(rrv-product PRIVATE
    RRV_PRODUCT_DIRECT_PRESENT=1
    RRV_SOURCE_COMMIT="${RRV_SOURCE_COMMIT}"
    RRV_SOURCE_CLEAN="${RRV_SOURCE_CLEAN}"
    RRV_PRODUCER_NAME="${RRV_PRODUCER_NAME}"
    RRV_PRODUCER_COMMIT="${RRV_PRODUCER_COMMIT}"
    RRV_PRODUCER_TREE="${RRV_PRODUCER_TREE}"
    RRV_PRODUCER_BASE="${RRV_PRODUCER_BASE}"
    RRV_PCSX2_COMMIT="${RRV_PCSX2_COMMIT}"
    RRV_BRIDGE_PATCH_SHA256="${RRV_BRIDGE_PATCH_SHA256}"
    RRV_GENERATION_RRV_COMMIT="${RRV_GENERATION_RRV_COMMIT}"
    RRV_GENERATION_PRODUCER_COMMIT="${RRV_GENERATION_PRODUCER_COMMIT}"
    RRV_GENERATION_HOOK_FINGERPRINT="${RRV_GENERATION_HOOK_FINGERPRINT}"
    RRV_GENERATION_MANIFEST_SHA256="${RRV_GENERATION_MANIFEST_SHA256}"
    RRV_BRIDGE_MANIFEST_SHA256="${RRV_BRIDGE_MANIFEST_SHA256}"
    RRV_BRIDGE_LIBRARY_SHA256="${RRV_BRIDGE_LIBRARY_SHA256}"
    RRV_METAL_SOURCE_MANIFEST_SHA256="${RRV_METAL_SOURCE_MANIFEST_SHA256}")
if(RRV_PRODUCT_OWNED_SOURCE)
    # The ELF the committed game code was extracted from (generated/rr5/source-manifest.json).
    target_compile_definitions(rrv-product PRIVATE RRV_GAME_ELF_SHA256="${RRV_GAME_ELF_SHA256}")
endif()
if(RRV_GS_PRODUCER_CONTROL)
    target_compile_definitions(rrv-product PRIVATE RRV_GS_PRODUCER_CONTROL=1)
endif()
target_link_libraries(rrv-product PRIVATE
    rrv_legacy_game_funcs
    rrv_legacy_stubs
    rrv_legacy_hle
    rrv_legacy_gs_record
    rrv_legacy_snapshot
    rrv_legacy_vu_compat
    rrv_legacy_gs_backend
    rrv_sdl_product_host
    ps2_runtime)
rrv_use_gs_resource_package(TARGET rrv-product PACKAGE pcsx2-gs)
include("${CMAKE_SOURCE_DIR}/cmake/RrvFukamiApp.cmake")
if(RRV_GS_PRODUCER_CONTROL AND BUILD_TESTING)
    # This invokes the actual rrv-product executable twice, once per supported
    # synchronous execution mode. The comparator rejects empty receipts and
    # requires exact logical equivalence without comparing wall-clock time.
    add_test(NAME rrv-product-gate1-producer-control
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_gate1_producer_control_product.py"
            --product "$<TARGET_FILE:rrv-product>")
    set_tests_properties(rrv-product-gate1-producer-control PROPERTIES
        LABELS "input-dependent;gate1-producer-control"
        TIMEOUT 780
        RUN_SERIAL TRUE
        RESOURCE_LOCK "rrv-product-direct-metal")
endif()
set(RRV_IDENTITY_M2_CAUSAL_STAGE "m2-causal=inactive")
if(RRV_M2_CAUSAL_TRACE)
    set(RRV_IDENTITY_M2_CAUSAL_STAGE "m2-causal=active:${CMAKE_BINARY_DIR}/m2-causal-overlay/m2-causal-overlay-manifest.json")
endif()
set(RRV_IDENTITY_M2P_PAD_STAGE "m2p-pad-observer=inactive")
if(RRV_M2P_PAD_OBSERVER)
    set(RRV_IDENTITY_M2P_PAD_STAGE "m2p-pad-observer=active:${CMAKE_BINARY_DIR}/m2p-pad-overlay/m2p-pad-overlay-manifest.json")
endif()
set(RRV_IDENTITY_M2P_GAME001_STAGE "m2p-game001-fail-closed=inactive")
if(RRV_M2P_GAME001_FAIL_CLOSED)
    set(RRV_IDENTITY_M2P_GAME001_STAGE "m2p-game001-fail-closed=active:${CMAKE_BINARY_DIR}/m2p-game001-fail-closed-overlay/m2p-game001-fail-closed-overlay-manifest.json")
endif()
set(RRV_IDENTITY_CALLBACK_STACK_MAIN_RESERVATION_STAGE "callback-stack-main-reservation=inactive")
if(RRV_CALLBACK_STACK_MAIN_RESERVATION)
    set(RRV_IDENTITY_CALLBACK_STACK_MAIN_RESERVATION_STAGE "callback-stack-main-reservation=active:${RRV_CALLBACK_STACK_MAIN_RESERVATION_MANIFEST}")
endif()
set(RRV_IDENTITY_IOP_HEAP_STAGE "iop-heap=inactive")
if(RRV_IOP_HEAP_ISOLATION)
    set(RRV_IDENTITY_IOP_HEAP_STAGE "iop-heap=active:${RRV_IOP_HEAP_MANIFEST}")
endif()
set(RRV_IDENTITY_RPC_MEMORY_SAFETY_STAGE "rpc-memory-safety=inactive")
if(RRV_RPC_MEMORY_SAFETY)
    set(RRV_IDENTITY_RPC_MEMORY_SAFETY_STAGE "rpc-memory-safety=active:${RRV_RPC_MEMORY_SAFETY_MANIFEST}")
endif()
set(RRV_IDENTITY_GS_CONTROL_STAGE "gs-control=inactive")
if(RRV_GS_PRODUCER_CONTROL)
    set(RRV_IDENTITY_GS_CONTROL_STAGE "gs-control=active:${CMAKE_BINARY_DIR}/gs-control-overlay/gs-control-overlay-manifest.json")
endif()
set(RRV_IDENTITY_JIT_ARGS)
if(RRV_SDL_ARCH MATCHES "^(arm64|aarch64)$")
    # sse2neon's project-level declaration happens after this product module;
    # the explicitly configured FetchContent override is its effective source.
    # (No VU JIT and no VIXL since 2026-09-30.)
    if(NOT FETCHCONTENT_SOURCE_DIR_SSE2NEON)
        message(FATAL_ERROR "ARM64 product identity requires a resolved SSE2NEON source directory")
    endif()
    list(APPEND RRV_IDENTITY_JIT_ARGS
        --jit-source "${FETCHCONTENT_SOURCE_DIR_SSE2NEON}")
endif()
# The identity receipt checks are macOS product gates (otool/Mach-O, Metal
# resources); the Linux candidate is identified by its build receipt instead.
if(APPLE AND NOT RRV_PRODUCT_OWNED_SOURCE)
add_custom_command(TARGET rrv-product POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_product_direct.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --binary "$<TARGET_FILE:rrv-product>"
        --producer "${RRV_PS2RECOMP_SOURCE_DIR}"
        --expected-producer-commit "${RRV_PRODUCER_COMMIT}"
        --expected-producer-tree "${RRV_PRODUCER_TREE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_product_no_raylib.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --binary "$<TARGET_FILE:rrv-product>"
        --receipt "${CMAKE_BINARY_DIR}/product-no-raylib.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/product_identity_receipt.py" create
        --source-root "${CMAKE_SOURCE_DIR}"
        --build-dir "${CMAKE_BINARY_DIR}"
        --binary "$<TARGET_FILE:rrv-product>"
        --generation-manifest "${RRV_GENERATION_MANIFEST}"
        --generated-dir "${RRV_GENERATED_SOURCE_DIR}"
        --bridge-manifest "${RRV_PCSX2_GS_BRIDGE_MANIFEST}"
        --bridge-manifest-verifier "${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED}"
        --bridge-manifest-verifier-text "${RRV_BRIDGE_MANIFEST_VERIFIER_TEXT}"
        --resource-package-spec "${CMAKE_BINARY_DIR}/CMakeFiles/rrv-resource-package/spec.json"
        --resource-package-root "${CMAKE_BINARY_DIR}/.rrv-resource-packages/current"
        --sdl-manifest "${RRV_SDL_PREFIX}/manifest.json"
        --architecture "${RRV_SDL_ARCH}"
        ${RRV_IDENTITY_JIT_ARGS}
        --abi-header "${CMAKE_SOURCE_DIR}/tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"
        --overlay-stage "product-host=active:${CMAKE_BINARY_DIR}/product-host/product-host-overlay-manifest.json"
        --overlay-stage "${RRV_IDENTITY_M2_CAUSAL_STAGE}"
        --overlay-stage "spr-pending-chain=active:${CMAKE_BINARY_DIR}/spr-pending-chain-overlay/spr-pending-chain-overlay-manifest.json"
        --overlay-stage "${RRV_IDENTITY_M2P_PAD_STAGE}"
        --overlay-stage "pad-pressure=active:${CMAKE_BINARY_DIR}/pad-pressure-overlay/pad-pressure-overlay-manifest.json"
        --overlay-stage "${RRV_IDENTITY_M2P_GAME001_STAGE}"
        --overlay-stage "product-host-final=active:${CMAKE_BINARY_DIR}/product-host-final/product-host-final-manifest.json"
        --overlay-stage "${RRV_IDENTITY_CALLBACK_STACK_MAIN_RESERVATION_STAGE}"
        --overlay-stage "${RRV_IDENTITY_IOP_HEAP_STAGE}"
        --overlay-stage "${RRV_IDENTITY_RPC_MEMORY_SAFETY_STAGE}"
        --overlay-stage "${RRV_IDENTITY_GS_CONTROL_STAGE}"
        --overlay-script "product-host=${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        --overlay-script "m2-causal=${CMAKE_SOURCE_DIR}/cmake/RrvM2CausalTrace.cmake"
        --overlay-script "spr-pending-chain=${CMAKE_SOURCE_DIR}/cmake/RrvSprPendingChain.cmake"
        --overlay-script "m2p-pad-observer=${CMAKE_SOURCE_DIR}/cmake/RrvM2PPadObserver.cmake"
        --overlay-script "pad-pressure=${CMAKE_SOURCE_DIR}/cmake/RrvPadPressure.cmake"
        --overlay-script "m2p-game001-fail-closed=${CMAKE_SOURCE_DIR}/cmake/RrvM2PGame001FailClosed.cmake"
        --overlay-script "product-host-final=${CMAKE_SOURCE_DIR}/scripts/product_host_overlay.py"
        --overlay-script "callback-stack-main-reservation=${CMAKE_SOURCE_DIR}/cmake/RrvCallbackStackMainReservation.cmake"
        --overlay-script "iop-heap=${CMAKE_SOURCE_DIR}/cmake/RrvIopHeap.cmake"
        --overlay-script "rpc-memory-safety=${CMAKE_SOURCE_DIR}/cmake/RrvRpcMemorySafety.cmake"
        --overlay-script "gs-control=${CMAKE_SOURCE_DIR}/cmake/RrvGsControl.cmake"
        --output "$<TARGET_FILE_DIR:rrv-product>/product-identity-receipt.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/product_identity_receipt.py" verify
        --receipt "$<TARGET_FILE_DIR:rrv-product>/product-identity-receipt.json"
    VERBATIM)
endif()
if(RRV_PRODUCT_OWNED_SOURCE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvProductOwned.cmake")
endif()

if(APPLE)
    message(STATUS "RRV product: direct PCSX2 Metal presentation through SDL 2.32.10")
else()
    message(STATUS "RRV product: direct PCSX2 Vulkan presentation through SDL 2.32.10")
endif()
