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
    # The startup hook must be an object of the executable itself: an
    # otherwise unreferenced constructor in a static library is not linked.
    if(APPLE)
        target_sources(rrv-product PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fukami_app.mm")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/app/fukami_app.mm" PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(rrv-product PRIVATE rrv_fukami_settings rrv-disc-extract
            "-framework AppKit" "-framework UniformTypeIdentifiers")
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
