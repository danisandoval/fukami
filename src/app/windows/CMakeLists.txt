# The Windows launcher (src/app/fukami_app_win.cpp), added by cmake/RrvFukamiApp.cmake. A directory scope of its own so
# that its include path is src/app and what its libraries export, and never the runtime's directories: their
# Kernel/Syscalls/rpc.h would be found before the Windows SDK's <rpc.h>.
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
add_library(rrv_fukami_app_win STATIC "${CMAKE_SOURCE_DIR}/src/app/fukami_app_win.cpp")
target_compile_features(rrv_fukami_app_win PRIVATE cxx_std_20)
target_link_libraries(rrv_fukami_app_win PRIVATE rrv_fukami_settings rrv-disc-extract shell32 ole32 uuid)
