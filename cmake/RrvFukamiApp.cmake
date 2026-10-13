# Fukami.app (Gate-9 APP1-APP7): the settings module, the app-mode startup hook
# and the CHD extractor, linked into the product. Included by RrvProduct.cmake
# after rrv-product exists; the Gate-3 candidate copies rrv-product's sources
# and link libraries, so the candidate gets them too.
if(NOT TARGET rrv_fukami_settings)
    add_library(rrv_fukami_settings STATIC "${CMAKE_SOURCE_DIR}/src/app/fukami_settings.cpp")
    target_include_directories(rrv_fukami_settings PUBLIC "${CMAKE_SOURCE_DIR}/src/app")
    target_compile_features(rrv_fukami_settings PUBLIC cxx_std_20)
endif()

if(TARGET rrv-product)
    # rrv-disc-extract comes from cmake/RrvDiscExtract.cmake (root CMakeLists);
    # asset-free builds without build-deps skip it, product builds need it.
    if(NOT TARGET rrv-disc-extract)
        message(FATAL_ERROR "Fukami.app needs rrv-disc-extract (build-deps/pcsx2-2.8.2 and zstd)")
    endif()
    # The menu (in the host library) reads and writes the settings file.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/app/fukami_menu.cpp")
        target_sources(rrv_sdl_product_host PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fukami_menu.cpp")
    endif()
    target_link_libraries(rrv_sdl_product_host PUBLIC rrv_fukami_settings)
    # What the three app layers share (disc marker, launch settings, environment de-duplication).
    target_sources(rrv-product PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fukami_app_common.cpp")
    # The startup hook must be an object of the executable itself: an
    # otherwise unreferenced constructor in a static library is not linked.
    if(APPLE)
        target_sources(rrv-product PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fukami_app.mm")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/app/fukami_app.mm" PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(rrv-product PRIVATE rrv_fukami_settings rrv-disc-extract
            "-framework AppKit" "-framework UniformTypeIdentifiers")
    elseif(WIN32)
        # Windows: Fukami.exe next to resources\, %LOCALAPPDATA%\Fukami data folder, Win32 dialogs. A GUI-subsystem
        # executable that still enters through main() (/ENTRY:mainCRTStartup) so the product's main() is unchanged.
        file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" RRV_FUKAMI_VERSION LIMIT_COUNT 1)
        if(NOT RRV_FUKAMI_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
            message(FATAL_ERROR "VERSION must read MAJOR.MINOR.PATCH, found '${RRV_FUKAMI_VERSION}'")
        endif()
        set(RRV_FUKAMI_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(RRV_FUKAMI_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(RRV_FUKAMI_VERSION_PATCH "${CMAKE_MATCH_3}")
        set(RRV_FUKAMI_ICON "${CMAKE_SOURCE_DIR}/src/app/windows/fukami.ico")
        set(RRV_FUKAMI_MANIFEST "${CMAKE_SOURCE_DIR}/src/app/windows/fukami.manifest")
        configure_file("${CMAKE_SOURCE_DIR}/src/app/windows/fukami.rc.in" "${CMAKE_BINARY_DIR}/fukami.rc" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
        enable_language(RC)
        # The launcher is its own library in its own directory scope (src/app/windows/CMakeLists.txt): the runtime's
        # include directories are a directory property of this scope, added to every target here, and they hold a
        # Kernel/Syscalls/rpc.h that shadows the Windows SDK's <rpc.h>, which <objbase.h> includes. main() calls
        # fukami::app::entry() by name, so a static library is linked (there is no constructor hook to lose).
        add_subdirectory("${CMAKE_SOURCE_DIR}/src/app/windows" "${CMAKE_BINARY_DIR}/fukami-app-win")
        target_sources(rrv-product PRIVATE "${CMAKE_BINARY_DIR}/fukami.rc")
        set_source_files_properties("${CMAKE_BINARY_DIR}/fukami.rc" PROPERTIES
            OBJECT_DEPENDS "${RRV_FUKAMI_ICON};${RRV_FUKAMI_MANIFEST}")
        set_target_properties(rrv-product PROPERTIES WIN32_EXECUTABLE TRUE)
        # The manifest is a resource of fukami.rc (UTF-8 code page, long paths, DPI); the linker must not add its own.
        target_link_options(rrv-product PRIVATE "/ENTRY:mainCRTStartup" "/MANIFEST:NO")
        target_link_libraries(rrv-product PRIVATE rrv_fukami_app_win rrv_fukami_settings rrv-disc-extract)
    else()
        # Linux / Steam Deck (Gate 5): the same hook without AppKit
        # (Fukami/bin/Fukami layout, XDG data folder, zenity/kdialog dialogs).
        target_sources(rrv-product PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fukami_app_linux.cpp")
        target_link_libraries(rrv-product PRIVATE rrv_fukami_settings rrv-disc-extract)
    endif()
endif()

if(BUILD_TESTING AND NOT TARGET rrv-fukami-settings-tests)
    add_executable(rrv-fukami-settings-tests "${CMAKE_SOURCE_DIR}/tests/fukami_settings_tests.cpp")
    target_link_libraries(rrv-fukami-settings-tests PRIVATE rrv_fukami_settings)
    target_compile_definitions(rrv-fukami-settings-tests PRIVATE
        RRV_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME rrv-fukami-settings COMMAND rrv-fukami-settings-tests)
endif()

if(BUILD_TESTING AND NOT TARGET rrv-launch-env)
    add_executable(rrv-launch-env "${CMAKE_SOURCE_DIR}/src/app/launch_env_main.cpp")
    target_link_libraries(rrv-launch-env PRIVATE rrv_fukami_settings)
endif()

if(BUILD_TESTING AND NOT TARGET rrv-fukami-menu-tests)
    add_executable(rrv-fukami-menu-tests "${CMAKE_SOURCE_DIR}/tests/fukami_menu_tests.cpp"
        "${CMAKE_SOURCE_DIR}/src/app/fukami_menu.cpp")
    target_link_libraries(rrv-fukami-menu-tests PRIVATE rrv_fukami_settings)
    target_include_directories(rrv-fukami-menu-tests PRIVATE "${CMAKE_SOURCE_DIR}/tools/pcsx2-gs-bridge")
    add_test(NAME rrv-fukami-menu COMMAND rrv-fukami-menu-tests)
endif()
