# Isolated M0R diagnostic graph. This target intentionally excludes every
# Renderer-IR, F7/P3, gs-frame, and progressive presentation target.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(RRV_DEPENDENCY_LOCK "${CMAKE_SOURCE_DIR}/config/dependencies.lock.toml")
file(READ "${RRV_DEPENDENCY_LOCK}" RRV_DEPENDENCY_LOCK_TEXT)
function(rrv_lock_value key output)
    string(REGEX MATCH "${key} = \"([^\"]+)\"" match "${RRV_DEPENDENCY_LOCK_TEXT}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "Missing ${key} in ${RRV_DEPENDENCY_LOCK}")
    endif()
    set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
rrv_lock_value("default_checkout" RRV_DEFAULT_PRODUCER_CHECKOUT)

set(RRV_PS2RECOMP_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/${RRV_DEFAULT_PRODUCER_CHECKOUT}"
    CACHE PATH "Reconstructed d52-compatible PS2Recomp source")
set(RRV_GENERATED_SOURCE_DIR "" CACHE PATH
    "Generated RR5 C++ directory (requires matching generation provenance)")
set(RRV_GENERATION_MANIFEST "" CACHE FILEPATH
    "Manifest emitted by scripts/generate_rr5_reconstructed.py")
set(RRV_PCSX2_GS_BRIDGE_MANIFEST "" CACHE FILEPATH
    "Manifest emitted by scripts/build_pcsx2_gs_bridge.sh")

if(RRV_PRODUCT_OWNED_SOURCE)
    if(NOT RRV_BUILD_PRODUCT OR RRV_GS_PRODUCER_CONTROL OR RRV_M2_CAUSAL_TRACE OR
       RRV_M2P_PAD_OBSERVER OR RRV_M2P_GAME001_FAIL_CLOSED)
        message(FATAL_ERROR "RRV_PRODUCT_OWNED_SOURCE is the plain product only (no diagnostic overlays)")
    endif()
    set(RRV_PS2RECOMP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/ps2recomp" CACHE PATH "" FORCE)
    set(RRV_GENERATED_SOURCE_DIR "${CMAKE_SOURCE_DIR}/generated/rr5/output" CACHE PATH "" FORCE)
    set(RRV_GENERATION_MANIFEST "${CMAKE_SOURCE_DIR}/generated/rr5/gate3-accounted-generation-manifest.json"
        CACHE FILEPATH "" FORCE)
endif()
foreach(required_path IN ITEMS RRV_PS2RECOMP_SOURCE_DIR RRV_GENERATED_SOURCE_DIR)
    if(NOT IS_ABSOLUTE "${${required_path}}")
        get_filename_component(${required_path} "${${required_path}}" ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif()
endforeach()

if(NOT EXISTS "${RRV_PS2RECOMP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing reconstructed producer at ${RRV_PS2RECOMP_SOURCE_DIR}. "
        "Run: python3 scripts/prepare_dependencies.py --prepare --component ps2recomp")
endif()
if(NOT EXISTS "${RRV_GENERATED_SOURCE_DIR}/register_functions.cpp")
    message(FATAL_ERROR
        "RRV_GENERATED_SOURCE_DIR must name an explicit out-of-tree generation result")
endif()
if(NOT EXISTS "${RRV_GENERATION_MANIFEST}")
    message(FATAL_ERROR
        "RRV_GENERATION_MANIFEST must name the manifest for RRV_GENERATED_SOURCE_DIR")
endif()
if(NOT EXISTS "${RRV_PCSX2_GS_BRIDGE_MANIFEST}")
    message(FATAL_ERROR
        "RRV_PCSX2_GS_BRIDGE_MANIFEST must name a verified source-built bridge manifest")
endif()
set(RRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER
    "${CMAKE_SOURCE_DIR}/scripts/pcsx2_bridge_manifest.py" CACHE FILEPATH
    "Strict bridge-manifest verifier used for source-built bridge provenance")
set(RRV_BRIDGE_MANIFEST_VERIFIER_TEXT "${RRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER}")
if(NOT EXISTS "${RRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER}")
    message(FATAL_ERROR "RRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER must name an existing verifier")
endif()
file(REAL_PATH "${RRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER}" RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED)
file(SHA256 "${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED}" RRV_BRIDGE_MANIFEST_VERIFIER_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED}")

execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE RRV_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" status --porcelain=v1
    OUTPUT_VARIABLE RRV_SOURCE_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(RRV_SOURCE_STATUS)
    set(RRV_SOURCE_CLEAN "no")
else()
    set(RRV_SOURCE_CLEAN "yes")
endif()

rrv_lock_value("compatible_name" RRV_PRODUCER_NAME)
rrv_lock_value("compatible_revision" RRV_PRODUCER_COMMIT)
rrv_lock_value("compatible_tree" RRV_PRODUCER_TREE)
rrv_lock_value("revision" RRV_PRODUCER_BASE)
string(FIND "${RRV_DEPENDENCY_LOCK_TEXT}" "[pcsx2]" RRV_PCSX2_SECTION)
string(SUBSTRING "${RRV_DEPENDENCY_LOCK_TEXT}" ${RRV_PCSX2_SECTION} -1 RRV_PCSX2_LOCK_TEXT)
string(REGEX MATCH "revision = \"([^\"]+)\"" match "${RRV_PCSX2_LOCK_TEXT}")
set(RRV_PCSX2_COMMIT "${CMAKE_MATCH_1}")
file(SHA256 "${CMAKE_SOURCE_DIR}/tools/patches/pcsx2-gs-bridge-target.patch"
    RRV_BRIDGE_PATCH_SHA256)
# Linux (Gate 5): the Vulkan bridge stacks tools/patches/pcsx2-gs-bridge-linux.patch
# on the target patch; its SHA-256 is pinned in the lock and required here.
set(RRV_BRIDGE_LINUX_VERIFY_ARGS)
set(RRV_BRIDGE_LINUX_PATCH_SHA256 "")
if(UNIX AND NOT APPLE)
    string(FIND "${RRV_DEPENDENCY_LOCK_TEXT}" "[[pcsx2_bridge_linux_patches]]" _rrv_linux_patch_section)
    if(_rrv_linux_patch_section EQUAL -1)
        message(FATAL_ERROR "dependencies.lock.toml lacks [[pcsx2_bridge_linux_patches]] (Linux bridge patch pin)")
    endif()
    string(SUBSTRING "${RRV_DEPENDENCY_LOCK_TEXT}" ${_rrv_linux_patch_section} -1 _rrv_linux_patch_text)
    string(REGEX MATCH "sha256 = \"([0-9a-f]+)\"" _rrv_linux_patch_match "${_rrv_linux_patch_text}")
    set(RRV_BRIDGE_LINUX_PATCH_SHA256 "${CMAKE_MATCH_1}")
    file(SHA256 "${CMAKE_SOURCE_DIR}/tools/patches/pcsx2-gs-bridge-linux.patch" _rrv_linux_patch_actual)
    if(NOT RRV_BRIDGE_LINUX_PATCH_SHA256 OR NOT _rrv_linux_patch_actual STREQUAL RRV_BRIDGE_LINUX_PATCH_SHA256)
        message(FATAL_ERROR "tools/patches/pcsx2-gs-bridge-linux.patch does not match its locked SHA-256")
    endif()
    set(RRV_BRIDGE_LINUX_VERIFY_ARGS --expected-linux-bridge-patch-sha256 "${RRV_BRIDGE_LINUX_PATCH_SHA256}")
endif()

if(RRV_PRODUCT_OWNED_SOURCE)
    # The runtime and recompiler are ordinary source in this repository; the producer
    # identity below is the recorded provenance of the imported tree (dependencies.lock.toml),
    # and the game code is checked against generated/rr5/source-manifest.json.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/verify_owned_source.py"
        RESULT_VARIABLE RRV_OWNED_VERIFY_RESULT
        OUTPUT_VARIABLE RRV_OWNED_VERIFY_OUTPUT
        ERROR_VARIABLE RRV_OWNED_VERIFY_ERROR)
    if(NOT RRV_OWNED_VERIFY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Committed game source verification failed:\n${RRV_OWNED_VERIFY_OUTPUT}${RRV_OWNED_VERIFY_ERROR}")
    endif()
    string(JSON RRV_GENERATION_RRV_COMMIT GET "${RRV_OWNED_VERIFY_OUTPUT}" recompiler_rrv_commit)
    string(JSON RRV_GENERATION_PRODUCER_COMMIT GET "${RRV_OWNED_VERIFY_OUTPUT}" producer_commit)
    string(JSON RRV_GENERATION_MANIFEST_SHA256 GET "${RRV_OWNED_VERIFY_OUTPUT}" accounted_manifest_sha256)
    set(RRV_GENERATION_HOOK_FINGERPRINT "source-owned")
    file(READ "${CMAKE_SOURCE_DIR}/generated/rr5/source-manifest.json" RRV_SOURCE_MANIFEST_TEXT)
    string(JSON RRV_GAME_ELF_SHA256 GET "${RRV_SOURCE_MANIFEST_TEXT}" game_input sha256)
    if(NOT RRV_GAME_ELF_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "generated/rr5/source-manifest.json has no game_input.sha256")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/generated/rr5/source-manifest.json"
        "${CMAKE_SOURCE_DIR}/scripts/verify_owned_source.py")
else()
execute_process(
    COMMAND git -C "${RRV_PS2RECOMP_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE RRV_ACTUAL_PRODUCER_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND git -C "${RRV_PS2RECOMP_SOURCE_DIR}" rev-parse "HEAD^{tree}"
    OUTPUT_VARIABLE RRV_ACTUAL_PRODUCER_TREE OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND git -C "${RRV_PS2RECOMP_SOURCE_DIR}" status --porcelain=v1
    OUTPUT_VARIABLE RRV_ACTUAL_PRODUCER_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT RRV_ACTUAL_PRODUCER_COMMIT STREQUAL RRV_PRODUCER_COMMIT OR
   NOT RRV_ACTUAL_PRODUCER_TREE STREQUAL RRV_PRODUCER_TREE OR
   RRV_ACTUAL_PRODUCER_STATUS)
    message(FATAL_ERROR
        "PS2Recomp source is not the clean compatible producer: "
        "HEAD=${RRV_ACTUAL_PRODUCER_COMMIT} tree=${RRV_ACTUAL_PRODUCER_TREE}")
endif()
rrv_lock_value("compatible_patch" RRV_PRODUCER_PATCH)
file(SHA256 "${CMAKE_SOURCE_DIR}/${RRV_PRODUCER_PATCH}"
    RRV_ACTUAL_PRODUCER_PATCH_SHA256)
rrv_lock_value("compatible_patch_sha256" RRV_PRODUCER_PATCH_SHA256)
if(NOT RRV_ACTUAL_PRODUCER_PATCH_SHA256 STREQUAL RRV_PRODUCER_PATCH_SHA256)
    message(FATAL_ERROR "Compatible producer patch does not match dependencies.lock.toml")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/verify_rr5_generation_manifest.py"
        --manifest "${RRV_GENERATION_MANIFEST}"
        --output-dir "${RRV_GENERATED_SOURCE_DIR}"
        --expected-producer-commit "${RRV_PRODUCER_COMMIT}"
        --expected-producer-tree "${RRV_PRODUCER_TREE}"
        --expected-pcsx2-commit "${RRV_PCSX2_COMMIT}"
    RESULT_VARIABLE RRV_GENERATION_VERIFY_RESULT
    OUTPUT_VARIABLE RRV_GENERATION_VERIFY_OUTPUT
    ERROR_VARIABLE RRV_GENERATION_VERIFY_ERROR)
if(NOT RRV_GENERATION_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Generated RR5 provenance verification failed:\n${RRV_GENERATION_VERIFY_OUTPUT}${RRV_GENERATION_VERIFY_ERROR}")
endif()
file(READ "${RRV_GENERATION_MANIFEST}" RRV_GENERATION_MANIFEST_TEXT)
string(JSON RRV_GENERATION_RRV_COMMIT GET "${RRV_GENERATION_MANIFEST_TEXT}" rrv_commit)
string(JSON RRV_GENERATION_PRODUCER_COMMIT GET "${RRV_GENERATION_MANIFEST_TEXT}" producer commit)
string(JSON RRV_GENERATION_HOOK_FINGERPRINT GET "${RRV_GENERATION_MANIFEST_TEXT}" hook_interface fingerprint_sha256)
file(SHA256 "${RRV_GENERATION_MANIFEST}" RRV_GENERATION_MANIFEST_SHA256)

endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED}" verify
        --manifest "${RRV_PCSX2_GS_BRIDGE_MANIFEST}"
        --expected-rrv-commit "${RRV_SOURCE_COMMIT}"
        --expected-pcsx2-commit "${RRV_PCSX2_COMMIT}"
        --expected-bridge-patch-sha256 "${RRV_BRIDGE_PATCH_SHA256}"
        ${RRV_BRIDGE_LINUX_VERIFY_ARGS}
    RESULT_VARIABLE RRV_BRIDGE_VERIFY_RESULT
    OUTPUT_VARIABLE RRV_BRIDGE_VERIFY_OUTPUT
    ERROR_VARIABLE RRV_BRIDGE_VERIFY_ERROR)
if(NOT RRV_BRIDGE_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Source-built bridge provenance verification failed:\n${RRV_BRIDGE_VERIFY_OUTPUT}${RRV_BRIDGE_VERIFY_ERROR}")
endif()
file(READ "${RRV_PCSX2_GS_BRIDGE_MANIFEST}" RRV_BRIDGE_MANIFEST_TEXT)
string(JSON RRV_SOURCE_BRIDGE_PATH GET "${RRV_BRIDGE_MANIFEST_TEXT}" library path)
string(JSON RRV_BRIDGE_LIBRARY_SHA256 GET "${RRV_BRIDGE_MANIFEST_TEXT}" library sha256)
string(JSON RRV_BRIDGE_BUILD_RECEIPT GET
    "${RRV_BRIDGE_MANIFEST_TEXT}" build_receipt path)
string(JSON RRV_SOURCE_BRIDGE_RESOURCES GET
    "${RRV_BRIDGE_MANIFEST_TEXT}" runtime_resources directory)
# macOS bridges carry precompiled Metal resources; a Linux (Gate 5) Vulkan
# bridge compiles its shaders at run time and its manifest has no
# metal_resources object, so those provenance inputs are simply absent there.
string(JSON RRV_METAL_TYPE ERROR_VARIABLE RRV_METAL_ABSENT TYPE
    "${RRV_BRIDGE_MANIFEST_TEXT}" metal_resources)
if(RRV_METAL_ABSENT OR NOT RRV_METAL_TYPE STREQUAL "OBJECT")
    if(APPLE)
        message(FATAL_ERROR "macOS bridge manifest lacks metal_resources")
    endif()
    set(RRV_METAL_SOURCE_MANIFEST_SHA256 "none")
    set(RRV_METAL_VERIFIER_MANIFEST "")
    set(RRV_METAL_SOURCE_MANIFEST "")
    set(_rrv_metal_manifest_paths)
else()
    string(JSON RRV_METAL_SOURCE_MANIFEST_SHA256 GET
        "${RRV_BRIDGE_MANIFEST_TEXT}" metal_resources source_manifest_sha256)
    string(JSON RRV_METAL_VERIFIER_MANIFEST GET
        "${RRV_BRIDGE_MANIFEST_TEXT}" metal_resources verifier_manifest)
    string(JSON RRV_METAL_SOURCE_MANIFEST GET
        "${RRV_BRIDGE_MANIFEST_TEXT}" metal_resources source_manifest)
    set(_rrv_metal_manifest_paths RRV_METAL_VERIFIER_MANIFEST RRV_METAL_SOURCE_MANIFEST)
endif()
foreach(manifest_path IN ITEMS
    RRV_SOURCE_BRIDGE_PATH
    RRV_SOURCE_BRIDGE_RESOURCES
    RRV_BRIDGE_BUILD_RECEIPT
    ${_rrv_metal_manifest_paths})
    if(NOT IS_ABSOLUTE "${${manifest_path}}")
        set(${manifest_path} "${CMAKE_SOURCE_DIR}/${${manifest_path}}")
    endif()
endforeach()
file(SHA256 "${RRV_PCSX2_GS_BRIDGE_MANIFEST}" RRV_BRIDGE_MANIFEST_SHA256)
get_filename_component(RRV_BRIDGE_LIBRARY_NAME "${RRV_SOURCE_BRIDGE_PATH}" NAME)

# Construct the sole configure-time package input from the already verified
# source-built bridge receipt. Runtime readers receive only the generated typed
# binding; neither this JSON nor the bridge manifest is runtime authority.
set(RRV_RESOURCE_PACKAGE_INPUT
    "${CMAKE_BINARY_DIR}/CMakeFiles/rrv-resource-package/input.json")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CMakeFiles/rrv-resource-package")
# CMake does not JSON-escape interpolated strings. Keep the producer input
# valid for legal native paths (including quotes, backslashes and Unicode).
function(rrv_resource_package_json_quote out value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    set(${out} "\"${escaped}\"" PARENT_SCOPE)
endfunction()
foreach(RRV_RESOURCE_PACKAGE_JSON_VALUE IN ITEMS
    RRV_BRIDGE_LIBRARY_NAME RRV_SOURCE_BRIDGE_PATH RRV_SOURCE_BRIDGE_RESOURCES
    RRV_PCSX2_GS_BRIDGE_MANIFEST RRV_BRIDGE_BUILD_RECEIPT
    RRV_METAL_VERIFIER_MANIFEST RRV_METAL_SOURCE_MANIFEST RRV_SOURCE_COMMIT
    RRV_PCSX2_COMMIT RRV_BRIDGE_PATCH_SHA256 RRV_BRIDGE_MANIFEST_SHA256
    RRV_BRIDGE_LIBRARY_SHA256 RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED)
    rrv_resource_package_json_quote(${RRV_RESOURCE_PACKAGE_JSON_VALUE}_JSON
        "${${RRV_RESOURCE_PACKAGE_JSON_VALUE}}")
endforeach()
rrv_resource_package_json_quote(RRV_BRIDGE_PATCH_JSON
    "${CMAKE_SOURCE_DIR}/tools/patches/pcsx2-gs-bridge-target.patch")
set(RRV_METAL_PROVENANCE_SOURCES_JSON "")
if(RRV_BRIDGE_LINUX_PATCH_SHA256)
    rrv_resource_package_json_quote(RRV_BRIDGE_LINUX_PATCH_JSON
        "${CMAKE_SOURCE_DIR}/tools/patches/pcsx2-gs-bridge-linux.patch")
    set(RRV_METAL_PROVENANCE_SOURCES_JSON "\"bridge-linux-patch\":${RRV_BRIDGE_LINUX_PATCH_JSON},")
endif()
if(_rrv_metal_manifest_paths)
    set(RRV_METAL_PROVENANCE_SOURCES_JSON
        "\"metal-verifier-manifest\":${RRV_METAL_VERIFIER_MANIFEST_JSON},\"metal-source-manifest\":${RRV_METAL_SOURCE_MANIFEST_JSON},")
endif()
file(WRITE "${RRV_RESOURCE_PACKAGE_INPUT}"
    "{\"schema\":\"rrv-resource-package-input-v1\",\"bridge\":${RRV_BRIDGE_LIBRARY_NAME_JSON},\"entries\":[{\"path\":${RRV_BRIDGE_LIBRARY_NAME_JSON},\"source\":${RRV_SOURCE_BRIDGE_PATH_JSON}}")
file(GLOB_RECURSE RRV_RESOURCE_PACKAGE_RESOURCE_FILES LIST_DIRECTORIES false
    RELATIVE "${RRV_SOURCE_BRIDGE_RESOURCES}" "${RRV_SOURCE_BRIDGE_RESOURCES}/*")
foreach(RRV_RESOURCE_PACKAGE_RESOURCE IN LISTS RRV_RESOURCE_PACKAGE_RESOURCE_FILES)
    rrv_resource_package_json_quote(RRV_RESOURCE_PACKAGE_RESOURCE_PATH_JSON
        "resources/${RRV_RESOURCE_PACKAGE_RESOURCE}")
    rrv_resource_package_json_quote(RRV_RESOURCE_PACKAGE_RESOURCE_SOURCE_JSON
        "${RRV_SOURCE_BRIDGE_RESOURCES}/${RRV_RESOURCE_PACKAGE_RESOURCE}")
    string(APPEND RRV_RESOURCE_PACKAGE_INPUT_ENTRIES
        ",{\"path\":${RRV_RESOURCE_PACKAGE_RESOURCE_PATH_JSON},\"source\":${RRV_RESOURCE_PACKAGE_RESOURCE_SOURCE_JSON}}")
endforeach()
file(APPEND "${RRV_RESOURCE_PACKAGE_INPUT}"
    "${RRV_RESOURCE_PACKAGE_INPUT_ENTRIES}],\"directories\":[\"resources\"],\"provenance\":{\"bridge_manifest_sha256\":${RRV_BRIDGE_MANIFEST_SHA256_JSON},\"bridge_library_sha256\":${RRV_BRIDGE_LIBRARY_SHA256_JSON},\"bridge_patch_sha256\":${RRV_BRIDGE_PATCH_SHA256_JSON}},\"provenance_sources\":{\"bridge-manifest\":${RRV_PCSX2_GS_BRIDGE_MANIFEST_JSON},\"bridge-build-receipt\":${RRV_BRIDGE_BUILD_RECEIPT_JSON},${RRV_METAL_PROVENANCE_SOURCES_JSON}\"bridge-patch\":${RRV_BRIDGE_PATCH_JSON}},\"bridge_provenance\":{\"manifest\":${RRV_PCSX2_GS_BRIDGE_MANIFEST_JSON},\"verifier\":${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED_JSON},\"expected_rrv_commit\":${RRV_SOURCE_COMMIT_JSON},\"expected_pcsx2_commit\":${RRV_PCSX2_COMMIT_JSON},\"expected_bridge_patch_sha256\":${RRV_BRIDGE_PATCH_SHA256_JSON}}}\n")
include("${CMAKE_SOURCE_DIR}/cmake/RrvResourcePackage.cmake")
rrv_select_gs_resource_package(PACKAGE pcsx2-gs MANIFEST "${RRV_RESOURCE_PACKAGE_INPUT}")
# The producer's source-byte pins catch payload drift.  Re-run the strict
# source-build provenance verifier at every package build as well, so an
# unchanged dylib cannot mask a changed bridge/Metal/build receipt.
add_custom_target(rrv-adr0006-bridge-provenance
    COMMAND "${Python3_EXECUTABLE}" "${RRV_BRIDGE_MANIFEST_VERIFIER_RESOLVED}" verify
        --manifest "${RRV_PCSX2_GS_BRIDGE_MANIFEST}"
        --expected-rrv-commit "${RRV_SOURCE_COMMIT}"
        --expected-pcsx2-commit "${RRV_PCSX2_COMMIT}"
        --expected-bridge-patch-sha256 "${RRV_BRIDGE_PATCH_SHA256}"
        ${RRV_BRIDGE_LINUX_VERIFY_ARGS}
    VERBATIM)
add_dependencies(rrv-resource-package-producer rrv-adr0006-bridge-provenance)

set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_RRV_FIELD_ONLY ON CACHE BOOL "" FORCE)
set(PS2X_RRV_EXTENSION_SOURCE_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "" FORCE)
set(RRV_RUNTIME_BUILD_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime")
if(RRV_BUILD_PRODUCT AND RRV_PRODUCT_OWNED_SOURCE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvProductOwnedIncludes.cmake")
    # rrv_thread_cpu_log.cpp: the Linux [cpu] per-thread log (empty stubs elsewhere);
    # main_product.cpp calls it on Linux, so the Linux product does not link without it.
    add_library(rrv_sdl_product_audio STATIC "${CMAKE_SOURCE_DIR}/src/host/rrv_sdl_audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/host/rrv_thread_cpu_log.cpp")
    target_compile_features(rrv_sdl_product_audio PUBLIC cxx_std_20)
    target_compile_definitions(rrv_sdl_product_audio PUBLIC RRV_PRODUCT_HOST_SDL=1)
    target_include_directories(rrv_sdl_product_audio PUBLIC "${CMAKE_SOURCE_DIR}/src/host")
    target_link_libraries(rrv_sdl_product_audio PUBLIC SDL2::SDL2)
    set(RRV_RUNTIME_BUILD_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime")
    add_subdirectory("${CMAKE_SOURCE_DIR}/cmake/product-runtime" ps2recomp/ps2xRuntime EXCLUDE_FROM_ALL)
elseif(RRV_BUILD_PRODUCT)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvProductHost.cmake")
    rrv_product_host_prepare(
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/product-host"
        OUT_SOURCE_DIR RRV_RUNTIME_BUILD_SOURCE_DIR)
    add_subdirectory("${RRV_RUNTIME_BUILD_SOURCE_DIR}" ps2recomp/ps2xRuntime EXCLUDE_FROM_ALL)
else()
    add_subdirectory("${RRV_PS2RECOMP_SOURCE_DIR}" ps2recomp EXCLUDE_FROM_ALL)
endif()

# Diagnostic-only additive source copies; the pinned producer stays immutable.
# OFF uses its original translation units and compiles every causal hook out.
if(RRV_M2_CAUSAL_TRACE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvM2CausalTrace.cmake")
    rrv_m2_causal_trace_add_library()
    rrv_m2_causal_trace_generate_overlay(
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/m2-causal-overlay"
        TARGET ps2_runtime)
endif()

if(NOT RRV_PRODUCT_OWNED_SOURCE)
include("${CMAKE_SOURCE_DIR}/cmake/RrvSprPendingChain.cmake")
rrv_spr_pending_chain_generate_overlay(
    RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/spr-pending-chain-overlay"
    TARGET ps2_runtime)
endif()

if(RRV_M2P_PAD_OBSERVER)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvM2PPadObserver.cmake")
    rrv_m2p_pad_observer_add_library()
    rrv_m2p_pad_observer_generate_overlay(
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/m2p-pad-overlay"
        TARGET ps2_runtime)
endif()

if(NOT RRV_PRODUCT_OWNED_SOURCE)
include("${CMAKE_SOURCE_DIR}/cmake/RrvPadPressure.cmake")
rrv_pad_pressure_generate_overlay(
    RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/pad-pressure-overlay"
    TARGET ps2_runtime)
endif()

if(RRV_M2P_GAME001_FAIL_CLOSED)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvM2PGame001FailClosed.cmake")
    rrv_m2p_game001_fail_closed_add_library()
    rrv_m2p_game001_fail_closed_generate_overlay(
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        GENERATED_SOURCE "${RRV_GENERATED_SOURCE_DIR}/sub_002585D8_0x2585d8.cpp"
        REENTRY_SOURCE "${RRV_GENERATED_SOURCE_DIR}/sub_00266F40_0x266f40.cpp"
        OVERLAP_SOURCE "${RRV_GENERATED_SOURCE_DIR}/sub_002D2C40_0x2d2c40.cpp"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/m2p-game001-fail-closed-overlay"
        TARGET ps2_runtime)
endif()

if(RRV_BUILD_PRODUCT AND NOT RRV_PRODUCT_OWNED_SOURCE)
    rrv_product_host_finalize(
        TARGET ps2_runtime
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/product-host-final")
endif()

if(RRV_CALLBACK_STACK_MAIN_RESERVATION AND NOT RRV_PRODUCT_OWNED_SOURCE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvCallbackStackMainReservation.cmake")
    rrv_callback_stack_main_reservation_generate_overlay(
        TARGET ps2_runtime
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        HEADER_INPUT "${CMAKE_BINARY_DIR}/product-host/include/ps2_runtime.h"
        RUNTIME_INPUT "${CMAKE_BINARY_DIR}/product-host-final/src/lib/ps2_runtime.cpp"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/callback-stack-main-reservation-overlay"
        ALLOWED_OUTPUT_ROOT "${CMAKE_BINARY_DIR}")
endif()

if(RRV_IOP_HEAP_ISOLATION AND NOT RRV_PRODUCT_OWNED_SOURCE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvIopHeap.cmake")
    rrv_iop_heap_generate_overlay(
        TARGET ps2_runtime
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        CALLBACK_SOURCE_DIR "${CMAKE_BINARY_DIR}/callback-stack-main-reservation-overlay/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/iop-heap-overlay"
        ALLOWED_OUTPUT_ROOT "${CMAKE_BINARY_DIR}")
endif()

if(RRV_RPC_MEMORY_SAFETY AND NOT RRV_PRODUCT_OWNED_SOURCE)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvRpcMemorySafety.cmake")
    rrv_rpc_memory_safety_generate_overlay(
        TARGET ps2_runtime
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        CALLBACK_SOURCE_DIR "${CMAKE_BINARY_DIR}/callback-stack-main-reservation-overlay/ps2xRuntime"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/rpc-memory-safety-overlay"
        ALLOWED_OUTPUT_ROOT "${CMAKE_BINARY_DIR}")
endif()

if(RRV_GS_PRODUCER_CONTROL)
    include("${CMAKE_SOURCE_DIR}/cmake/RrvGsControl.cmake")
    set(_rrv_gs_control_include_input "${CMAKE_BINARY_DIR}/product-host/include")
    set(_rrv_gs_control_callback_args)
    if(RRV_CALLBACK_STACK_MAIN_RESERVATION)
        set(_rrv_gs_control_include_input
            "${CMAKE_BINARY_DIR}/callback-stack-main-reservation-overlay/ps2xRuntime/include")
        list(APPEND _rrv_gs_control_callback_args CALLBACK_STACK_MAIN_RESERVATION)
    endif()
    rrv_gs_control_generate_overlay(
        TARGET ps2_runtime
        RUNTIME_SOURCE_DIR "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        INCLUDE_INPUT "${_rrv_gs_control_include_input}"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/gs-control-overlay"
        ${_rrv_gs_control_callback_args})
endif()

# The pinned producer makes this definition PRIVATE even though it changes the
# public PS2Runtime class layout in ps2_runtime.h.  Every RRV translation unit
# which includes that header must see the same field-only layout or inline
# accessors (notably padBackend()) address the wrong member.  Keep the producer
# checkout immutable and propagate the already-selected mode to consumers here.
target_compile_definitions(ps2_runtime INTERFACE PS2X_RRV_FIELD_ONLY=1)

# PS2Recomp applies this compatibility header inside its own directory scope.
# Generated and RRV-owned translation units include the same runtime headers,
# so apply the identical pinned header to this isolated parent graph on ARM64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    if(NOT FETCHCONTENT_SOURCE_DIR_SSE2NEON OR
       NOT EXISTS "${FETCHCONTENT_SOURCE_DIR_SSE2NEON}/sse2neon.h")
        message(FATAL_ERROR
            "ARM64 legacy-live requires the pinned sse2neon source via "
            "-DFETCHCONTENT_SOURCE_DIR_SSE2NEON=<clean checkout>")
    endif()
    add_compile_definitions(USE_SSE2NEON)
    include_directories("${FETCHCONTENT_SOURCE_DIR_SSE2NEON}")
    if(RRV_BUILD_PRODUCT)
        # The product bypasses the upstream top-level host configuration.
        # Supply its unchanged ARM compatibility boundary to the child target.
        target_compile_definitions(ps2_runtime PUBLIC USE_SSE2NEON)
        target_include_directories(ps2_runtime PUBLIC "${FETCHCONTENT_SOURCE_DIR_SSE2NEON}")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    # Linux x86-64 (Gate 5): the generated game code and runtime use SSE4.1
    # intrinsics directly (_mm_blendv_ps etc.; sse2neon provides them on ARM64).
    # x86-64-v3 = AVX2/FMA/BMI2: the Steam Deck's Zen 2 has it (as does every
    # x86-64 PC since ~2015). FMA makes x86 contract multiply-adds exactly where
    # ARM64 already does; the files that must round twice like the VU FMAC are
    # compiled with -ffp-contract=off on both. Checked by RAM digests against
    # the macOS runtime (docs/TESTING.md T-LINUX-DECK).
    add_compile_options(-march=x86-64-v3)
    target_compile_options(ps2_runtime PUBLIC -march=x86-64-v3)
endif()

# One configuration for every variant of the backend library, so a variant
# presents exactly the same Backend header/ABI to its consumers.
function(rrv_configure_legacy_gs_backend target)
    target_compile_definitions(${target} PRIVATE
        RRV_GS_FIELD_ONLY=1)
    target_include_directories(${target} PUBLIC
        "${CMAKE_SOURCE_DIR}/src/gs-backend"
        "${CMAKE_SOURCE_DIR}/src/gs-record"
        "${CMAKE_SOURCE_DIR}/src/diag")
    if(RRV_M2_CAUSAL_TRACE)
        target_link_libraries(${target} PUBLIC rrv_m2_causal_trace)
    endif()
    if(RRV_M2_NEUTRALITY_CONTROL)
        target_sources(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/gs-backend/rrv_m2_neutrality.cpp")
        target_compile_definitions(${target} PUBLIC
            RRV_GS_EPOCH_DIAGNOSTICS=1
            RRV_M2_NEUTRALITY_CONTROL=1)
    endif()
    if(RRV_GS_CONSUMER_RECEIPT_CLIENT)
        target_compile_definitions(${target} PUBLIC
            RRV_GS_CONSUMER_RECEIPT_CLIENT=1)
    endif()
endfunction()

add_library(rrv_legacy_gs_backend STATIC
    "${CMAKE_SOURCE_DIR}/src/gs-backend/rrv_gs_backend.cpp")
rrv_configure_legacy_gs_backend(rrv_legacy_gs_backend)
# ADR-0006: the production backend opens its bridge only through the fixed
# resource package and rejects every runtime override.
target_link_libraries(rrv_legacy_gs_backend PUBLIC rrv-resource-package-binding)

# Test-only asset-free variant of the same source. It compiles in one fixed,
# target-derived fake bridge path instead of the package reader, never links
# the package binding, and is selected only by a test executable that sets
# RRV_GS_FAKE_BRIDGE_VARIANT (see ps2_runtime below). The product, legacy-live
# and every other consumer keep the production backend.
set(RRV_LEGACY_GS_BACKEND_FOR_RUNTIME rrv_legacy_gs_backend)
if(RRV_GS_PRODUCER_CONTROL AND BUILD_TESTING)
    add_library(rrv-gs-control-feedback-fake SHARED
        "${CMAKE_SOURCE_DIR}/tests/fake_pcsx2_gs_bridge.cpp")
    set_target_properties(rrv-gs-control-feedback-fake PROPERTIES
        OUTPUT_NAME "rrv-pcsx2-gs-bridge"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gs-control-feedback-fixture")
    add_library(rrv_legacy_gs_backend_fake_bridge STATIC EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/src/gs-backend/rrv_gs_backend.cpp")
    rrv_configure_legacy_gs_backend(rrv_legacy_gs_backend_fake_bridge)
    target_compile_definitions(rrv_legacy_gs_backend_fake_bridge PRIVATE
        RRV_GS_TEST_FAKE_BRIDGE_PATH="$<TARGET_FILE:rrv-gs-control-feedback-fake>")
    target_link_libraries(rrv_legacy_gs_backend_fake_bridge PUBLIC ${CMAKE_DL_LIBS})
    add_dependencies(rrv_legacy_gs_backend_fake_bridge rrv-gs-control-feedback-fake)
    set(RRV_LEGACY_GS_BACKEND_FOR_RUNTIME
        "$<IF:$<BOOL:$<TARGET_PROPERTY:RRV_GS_FAKE_BRIDGE_VARIANT>>,rrv_legacy_gs_backend_fake_bridge,rrv_legacy_gs_backend>")
endif()

file(GLOB RRV_LEGACY_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${RRV_GENERATED_SOURCE_DIR}/sub_*.cpp"
    "${RRV_GENERATED_SOURCE_DIR}/register_functions.cpp")
list(LENGTH RRV_LEGACY_GENERATED_SOURCES RRV_LEGACY_GENERATED_COUNT)
if(RRV_LEGACY_GENERATED_COUNT EQUAL 0)
    message(FATAL_ERROR "No generated sources found in ${RRV_GENERATED_SOURCE_DIR}")
endif()

# Diagnostic insertion only: the local generated input is never modified.
function(rrv_m2_helper_overlay source output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${source}" "${CMAKE_SOURCE_DIR}/scripts/m2_guest_function_overlay.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/m2_guest_function_overlay.py"
        --source "${source}" --output "${output}" ${ARGN}
        RESULT_VARIABLE _result ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "M2 helper overlay failed: ${_error}")
    endif()
endfunction()
if(RRV_M2_GUEST_PROVENANCE)
    set(_helper_source "${RRV_GENERATED_SOURCE_DIR}/sub_0021E698_0x21e698.cpp")
    set(_helper_overlay "${CMAKE_BINARY_DIR}/m2-helper-overlay/sub_0021E698_0x21e698.cpp")
    rrv_m2_helper_overlay("${_helper_source}" "${_helper_overlay}")
    list(FIND RRV_LEGACY_GENERATED_SOURCES "${_helper_source}" _helper_index)
    if(_helper_index LESS 0)
        message(FATAL_ERROR "M2 helper source absent from generated target")
    endif()
    list(REMOVE_AT RRV_LEGACY_GENERATED_SOURCES ${_helper_index})
    list(INSERT RRV_LEGACY_GENERATED_SOURCES ${_helper_index} "${_helper_overlay}")
endif()
if(RRV_M2P_GAME001_FAIL_CLOSED)
    set(_game001_owner_source "${RRV_GENERATED_SOURCE_DIR}/sub_002585D8_0x2585d8.cpp")
    set(_game001_reentry_source "${RRV_GENERATED_SOURCE_DIR}/sub_00266F40_0x266f40.cpp")
    set(_game001_overlap_source "${RRV_GENERATED_SOURCE_DIR}/sub_002D2C40_0x2d2c40.cpp")
    set(_game001_sources "${_game001_owner_source}" "${_game001_reentry_source}" "${_game001_overlap_source}")
    set(_game001_overlays "${RRV_M2P_GAME001_GENERATED_OWNER_OVERLAY}"
                           "${RRV_M2P_GAME001_REENTRY_OVERLAY}"
                           "${RRV_M2P_GAME001_OVERLAP_OVERLAY}")
    foreach(_game001_pair_index RANGE 0 2)
        list(GET _game001_sources ${_game001_pair_index} _game001_source)
        list(GET _game001_overlays ${_game001_pair_index} _game001_overlay)
        list(FIND RRV_LEGACY_GENERATED_SOURCES "${_game001_source}" _game001_index)
        if(_game001_index LESS 0 OR NOT EXISTS "${_game001_overlay}")
            message(FATAL_ERROR "M2P GAME-001 generated owner overlay is unavailable")
        endif()
        list(REMOVE_AT RRV_LEGACY_GENERATED_SOURCES ${_game001_index})
        list(INSERT RRV_LEGACY_GENERATED_SOURCES ${_game001_index} "${_game001_overlay}")
    endforeach()
endif()
add_library(rrv_legacy_game_funcs STATIC ${RRV_LEGACY_GENERATED_SOURCES})
set_target_properties(rrv_legacy_game_funcs PROPERTIES
    UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 50)
target_include_directories(rrv_legacy_game_funcs PUBLIC
    "${RRV_GENERATED_SOURCE_DIR}"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
target_link_libraries(rrv_legacy_game_funcs PUBLIC ps2_runtime)

file(GLOB RRV_LEGACY_HLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/hle/*.cpp")
add_library(rrv_legacy_hle STATIC ${RRV_LEGACY_HLE_SOURCES})
target_include_directories(rrv_legacy_hle PUBLIC
    "${CMAKE_SOURCE_DIR}/src/hle"
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
target_link_libraries(rrv_legacy_hle PUBLIC ps2_runtime)

add_library(rrv_legacy_gs_record STATIC "${CMAKE_SOURCE_DIR}/src/gs-record/rrv_gs_record.cpp")
target_include_directories(rrv_legacy_gs_record PUBLIC
    "${CMAKE_SOURCE_DIR}/src/gs-record"
    "${CMAKE_SOURCE_DIR}/src/gs-backend")

file(GLOB RRV_LEGACY_SNAPSHOT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/snapshot/*.cpp")
add_library(rrv_legacy_snapshot STATIC ${RRV_LEGACY_SNAPSHOT_SOURCES})
if(RRV_M2_INITIAL_STATE_FINGERPRINT)
    target_sources(ps2_runtime PRIVATE
        "${CMAKE_SOURCE_DIR}/src/diag/rrv_m2_initial_state.cpp")
endif()
# rrv_snapshot_runtime.cpp includes ps2_runtime.h but intentionally cannot link
# ps2_runtime (the runtime already depends on this snapshot library). Keep its
# view of the public PS2Runtime layout aligned with the selected field-only ABI.
target_compile_definitions(rrv_legacy_snapshot PRIVATE PS2X_RRV_FIELD_ONLY=1)
if(RRV_BUILD_PRODUCT)
    target_compile_definitions(rrv_legacy_snapshot PRIVATE RRV_PRODUCT_HOST_SDL=1)
    # The isolated snapshot library includes the product-host's pinned
    # ps2_runtime.h but deliberately cannot link ps2_runtime (the dependency
    # direction is the reverse).  The host header includes this RRV-owned
    # terminal-outcome header, so provide that exact source include locally.
    target_include_directories(rrv_legacy_snapshot PRIVATE "${CMAKE_SOURCE_DIR}/src")
endif()
target_include_directories(rrv_legacy_snapshot PUBLIC
    "${CMAKE_SOURCE_DIR}/src/snapshot"
    PRIVATE
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
    "${CMAKE_SOURCE_DIR}/src/gs-backend"
    "${CMAKE_SOURCE_DIR}/src/gs-record")

include("${CMAKE_SOURCE_DIR}/cmake/RrvVuAot.cmake")
rrv_add_vu_compat(rrv_legacy_vu_compat)

add_library(rrv_legacy_stubs STATIC "${CMAKE_SOURCE_DIR}/src/rrv_stubs.cpp")
# Source-owned product: the stubs resolve the exact ps2_recompiled_stubs.h the older
# generation provided (known item, generated/rr5/README.md), not the accounted copy.
set(RRV_LEGACY_STUBS_HEADER_DIR "${RRV_GENERATED_SOURCE_DIR}")
if(RRV_PRODUCT_OWNED_SOURCE)
    set(RRV_LEGACY_STUBS_HEADER_DIR "${CMAKE_SOURCE_DIR}/generated/rr5/legacy-stubs")
endif()
target_include_directories(rrv_legacy_stubs PRIVATE
    "${RRV_LEGACY_STUBS_HEADER_DIR}"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
    "${CMAKE_SOURCE_DIR}/src/hle")
target_link_libraries(rrv_legacy_stubs PUBLIC ps2_runtime rrv_legacy_hle)

target_include_directories(ps2_runtime PUBLIC
    "${CMAKE_SOURCE_DIR}/src/gs-backend"
    "${CMAKE_SOURCE_DIR}/src/gs-record"
    "${CMAKE_SOURCE_DIR}/src/snapshot"
    "${CMAKE_SOURCE_DIR}/src/diag"
    "${CMAKE_SOURCE_DIR}/src/host"
    "${CMAKE_SOURCE_DIR}/src/vu-aot/compat")
# $<TARGET_PROPERTY:...> in the link interface is evaluated on the final
# linked target. Only a test executable that opts into the fake-bridge variant
# receives it; ps2_runtime itself and all other consumers get production.
target_link_libraries(ps2_runtime PUBLIC
    "${RRV_LEGACY_GS_BACKEND_FOR_RUNTIME}" rrv_legacy_vu_compat)

if(RRV_RPC_MEMORY_SAFETY AND BUILD_TESTING)
    add_executable(rrv-rpc-memory-safety-tests
        "${CMAKE_SOURCE_DIR}/tests/rpc_memory_safety_native_tests.cpp")
    target_include_directories(rrv-rpc-memory-safety-tests PRIVATE
        "${CMAKE_BINARY_DIR}/rpc-memory-safety-overlay/ps2xRuntime/src/lib/Kernel/Syscalls"
        "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
        "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
    # Stubs/Common.h appears earlier in ps2_runtime's transitive -I list.
    # -iquote selects the effective Syscalls/Common.h for this native test.
    target_compile_options(rrv-rpc-memory-safety-tests PRIVATE
        "-iquote${CMAKE_BINARY_DIR}/rpc-memory-safety-overlay/ps2xRuntime/src/lib/Kernel/Syscalls")
    target_link_libraries(rrv-rpc-memory-safety-tests PRIVATE
        ps2_runtime rrv_legacy_gs_backend rrv_legacy_gs_record
        rrv_legacy_snapshot rrv_legacy_vu_compat)
    add_test(NAME rrv-rpc-memory-safety COMMAND rrv-rpc-memory-safety-tests)
endif()

# Asset-free test-only input control for deterministic presentation comparisons.
# It uses the reconstructed producer's public libpad seam and never opens a
# window or inspects a physical controller.
add_executable(rrv-pad-host-input-tests
    "${CMAKE_SOURCE_DIR}/tests/pad_host_input_tests.cpp")
target_compile_definitions(rrv-pad-host-input-tests PRIVATE RRV_PAD_PRESSURE_FIX=1)
target_include_directories(rrv-pad-host-input-tests PRIVATE
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
target_link_libraries(rrv-pad-host-input-tests PRIVATE
    ps2_runtime
    rrv_legacy_gs_backend
    rrv_legacy_gs_record
    rrv_legacy_snapshot
    rrv_legacy_vu_compat)
add_test(NAME rrv-pad-host-input COMMAND rrv-pad-host-input-tests)
add_test(NAME rrv-pad-pressure-overlay
    COMMAND "${CMAKE_COMMAND}" -E env
        "RRV_PAD_TEST_RUNTIME_SOURCE_DIR=${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/pad_pressure_overlay_tests.py")
add_test(NAME rrv-spr-pending-chain-overlay
    COMMAND "${CMAKE_COMMAND}" -E env
        "RRV_SPR_TEST_RUNTIME_SOURCE_DIR=${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime"
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_spr_pending_chain_overlay.py")

add_executable(rrv-neutral-pad-tests
    "${CMAKE_SOURCE_DIR}/tests/neutral_pad_tests.cpp")
target_include_directories(rrv-neutral-pad-tests PRIVATE
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include")
target_link_libraries(rrv-neutral-pad-tests PRIVATE ps2_runtime)
add_test(NAME rrv-neutral-pad COMMAND rrv-neutral-pad-tests)

if(NOT RRV_BUILD_PRODUCT)
add_executable(rrv-legacy-live
    "${CMAKE_SOURCE_DIR}/src/main_legacy_live.cpp"
    "${CMAKE_SOURCE_DIR}/src/patches.cpp")
target_include_directories(rrv-legacy-live PRIVATE
    "${RRV_RUNTIME_BUILD_SOURCE_DIR}/include"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
    "${RRV_GENERATED_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/gs-record")
target_compile_definitions(rrv-legacy-live PRIVATE
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
if(APPLE)
    target_compile_definitions(rrv-legacy-live PRIVATE RRV_LEGACY_REQUIRE_METAL=1)
endif()
target_link_libraries(rrv-legacy-live PRIVATE
    rrv_legacy_game_funcs
    rrv_legacy_stubs
    rrv_legacy_hle
    rrv_legacy_gs_record
    rrv_legacy_snapshot
    rrv_legacy_vu_compat
    rrv_legacy_gs_backend
    ps2_runtime)
rrv_use_gs_resource_package(TARGET rrv-legacy-live PACKAGE pcsx2-gs)
add_custom_command(TARGET rrv-legacy-live POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_legacy_live_no_f7.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --binary "$<TARGET_FILE:rrv-legacy-live>"
        --producer "${RRV_PS2RECOMP_SOURCE_DIR}"
        --expected-producer-commit "${RRV_PRODUCER_COMMIT}"
        --expected-producer-tree "${RRV_PRODUCER_TREE}"
    VERBATIM)

endif() # Original diagnostic executable exists only in its own configuration.

# Asset-free producer control: constructs memory and real DMA/VIF/arbiter
# state, but never starts the guest worker or a graphics backend.
add_executable(rrv-m2-dma-runtime-tests EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/m2_dma_runtime_tests.cpp")
target_include_directories(rrv-m2-dma-runtime-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/diag"
    "${CMAKE_SOURCE_DIR}/src/hle"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
target_compile_features(rrv-m2-dma-runtime-tests PRIVATE cxx_std_20)
target_link_libraries(rrv-m2-dma-runtime-tests PRIVATE
    rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
    rrv_legacy_vu_compat rrv_legacy_gs_backend ps2_runtime)

if(RRV_GS_PRODUCER_CONTROL AND BUILD_TESTING)
    add_executable(rrv-gs-control-integration-tests
        "${CMAKE_SOURCE_DIR}/tests/gs_control_integration_tests.cpp")
    target_include_directories(rrv-gs-control-integration-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src/diag" "${CMAKE_SOURCE_DIR}/src/hle"
        "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
    target_compile_features(rrv-gs-control-integration-tests PRIVATE cxx_std_20)
    target_link_libraries(rrv-gs-control-integration-tests PRIVATE
        rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
        rrv_legacy_vu_compat rrv_legacy_gs_backend ps2_runtime)
    add_test(NAME rrv-gs-control-integration COMMAND rrv-gs-control-integration-tests)

    # The fake fixture stays an explicit test-only injection. It never acquires
    # the production package or enables an external runtime override: this
    # executable links the compiled-in fake-bridge backend variant (defined
    # with rrv_legacy_gs_backend above) in place of the production backend.
    add_executable(rrv-gs-control-feedback-tests
        "${CMAKE_SOURCE_DIR}/tests/gs_control_feedback_tests.cpp")
    set_target_properties(rrv-gs-control-feedback-tests PROPERTIES
        RRV_GS_FAKE_BRIDGE_VARIANT ON)
    target_link_libraries(rrv-gs-control-feedback-tests PRIVATE
        rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
        rrv_legacy_vu_compat rrv_legacy_gs_backend_fake_bridge ps2_runtime ${CMAKE_DL_LIBS})
    target_compile_definitions(rrv-gs-control-feedback-tests PRIVATE
        RRV_TEST_FAKE_GS_BRIDGE="$<TARGET_FILE:rrv-gs-control-feedback-fake>")
    add_dependencies(rrv-gs-control-feedback-tests rrv-gs-control-feedback-fake)
    add_test(NAME rrv-gs-control-feedback COMMAND rrv-gs-control-feedback-tests)
    set_tests_properties(rrv-gs-control-feedback PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/gs-control-feedback-fixture")

    # G1-A native integration: a test-only AppKit view/CAMetalLayer supplies
    # the existing bridge owner-surface contract through the same typed package
    # reader as product and legacy live.
    if(APPLE)
        enable_language(OBJCXX)
        # ADR-0006 family 6 is intentionally narrower than the existing
        # producer-control integration suite: it opens the actual package,
        # identity-checks dladdr, initializes/destroys Metal, then repeats
        # that check after copying B to a distinct root.
        add_executable(rrv-adr0006-bridge-metal-smoke
            "${CMAKE_SOURCE_DIR}/tests/adr0006_bridge_metal_smoke.cpp")
        target_include_directories(rrv-adr0006-bridge-metal-smoke PRIVATE
            "${CMAKE_SOURCE_DIR}/src/host"
            "${CMAKE_SOURCE_DIR}/tools/pcsx2-gs-bridge")
        target_compile_features(rrv-adr0006-bridge-metal-smoke PRIVATE cxx_std_20)
        rrv_use_gs_resource_package(TARGET rrv-adr0006-bridge-metal-smoke PACKAGE pcsx2-gs)
        add_test(NAME rrv-adr0006-bridge-metal-relocation
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/test_adr0006_bridge_metal_relocation.py"
                --build-root "${CMAKE_BINARY_DIR}"
                --executable "$<TARGET_FILE:rrv-adr0006-bridge-metal-smoke>"
                --raw-log "${CMAKE_BINARY_DIR}/adr0006-bridge-metal-relocation.log")
        set_tests_properties(rrv-adr0006-bridge-metal-relocation PROPERTIES
            LABELS "g1-h1e-native;input-dependent")

        add_executable(rrv-gs-control-real-bridge-tests
            "${CMAKE_SOURCE_DIR}/tests/gs_control_real_bridge_tests.mm")
        target_include_directories(rrv-gs-control-real-bridge-tests PRIVATE
            "${CMAKE_SOURCE_DIR}/src/diag" "${CMAKE_SOURCE_DIR}/src/hle"
            "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
        target_compile_features(rrv-gs-control-real-bridge-tests PRIVATE cxx_std_20)
        target_compile_options(rrv-gs-control-real-bridge-tests PRIVATE -fno-objc-arc)
        target_link_libraries(rrv-gs-control-real-bridge-tests PRIVATE
            rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
            rrv_legacy_vu_compat rrv_legacy_gs_backend ps2_runtime
            "-framework AppKit" "-framework QuartzCore")
        rrv_use_gs_resource_package(TARGET rrv-gs-control-real-bridge-tests PACKAGE pcsx2-gs)
        add_test(NAME rrv-gs-control-real-bridge COMMAND rrv-gs-control-real-bridge-tests)
        set_tests_properties(rrv-gs-control-real-bridge PROPERTIES
            LABELS input-dependent)
        add_test(NAME rrv-gs-control-overlay-manifest
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_gs_control_overlay.py"
                --manifest "${CMAKE_BINARY_DIR}/gs-control-overlay/gs-control-overlay-manifest.json"
                --header "${CMAKE_SOURCE_DIR}/src/gs-control/rrv_gs_result_boundary.h"
                --producer-gs "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp")
        set_tests_properties(rrv-gs-control-overlay-manifest PROPERTIES LABELS input-dependent)
    endif()
endif()

set(_guest_harness "${CMAKE_SOURCE_DIR}/tests/m2_guest_runtime_tests.cpp")
if(RRV_M2_GUEST_PROVENANCE)
    set(_guest_harness_overlay "${CMAKE_BINARY_DIR}/m2-helper-overlay/m2_guest_runtime_tests.cpp")
    rrv_m2_helper_overlay("${_guest_harness}" "${_guest_harness_overlay}" --fixture)
    set(_guest_harness "${_guest_harness_overlay}")
endif()
add_executable(rrv-m2-guest-runtime-tests EXCLUDE_FROM_ALL "${_guest_harness}")
target_include_directories(rrv-m2-guest-runtime-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/diag"
    "${CMAKE_SOURCE_DIR}/src/hle"
    "${RRV_PS2RECOMP_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel")
target_compile_features(rrv-m2-guest-runtime-tests PRIVATE cxx_std_20)
target_link_libraries(rrv-m2-guest-runtime-tests PRIVATE
    rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
    rrv_legacy_vu_compat rrv_legacy_gs_backend ps2_runtime)

if(RRV_M2_INITIAL_STATE_FINGERPRINT)
    add_executable(rrv-m2-initial-state-tests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/m2_initial_state_tests.cpp")
    target_include_directories(rrv-m2-initial-state-tests PRIVATE "${CMAKE_SOURCE_DIR}/src/diag")
    target_compile_features(rrv-m2-initial-state-tests PRIVATE cxx_std_20)
    target_link_libraries(rrv-m2-initial-state-tests PRIVATE
        rrv_legacy_hle rrv_legacy_gs_record rrv_legacy_snapshot
        rrv_legacy_vu_compat rrv_legacy_gs_backend ps2_runtime)
endif()

message(STATUS "RRV legacy live: ${RRV_LEGACY_GENERATED_COUNT} generated files")
message(STATUS "RRV producer: ${RRV_PRODUCER_COMMIT} tree ${RRV_PRODUCER_TREE}")
if(NOT RRV_BUILD_PRODUCT)
    message(STATUS "RRV diagnostic configuration: rrv-legacy-live")
endif()
