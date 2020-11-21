cmake_minimum_required(VERSION 3.19)
project(OkageNative LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(okage_assets src/assets.cpp)
target_include_directories(okage_assets PUBLIC include)
target_compile_options(okage_assets PRIVATE -Wall -Wextra -Wpedantic)
add_executable(okage-assets src/assets_main.cpp)
target_link_libraries(okage-assets PRIVATE okage_assets)

if(APPLE)
    enable_language(OBJCXX)
    add_executable(okage-viewer MACOSX_BUNDLE src/viewer.mm)
    target_compile_options(okage-viewer PRIVATE -fobjc-arc)
    target_link_libraries(okage-viewer PRIVATE okage_assets "-framework AppKit")
    set_target_properties(okage-viewer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Okage Asset Viewer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.okage.asset-viewer")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(asset-tests tests/asset_tests.cpp)
    target_link_libraries(asset-tests PRIVATE okage_assets)
    add_test(NAME asset-tests COMMAND asset-tests)
endif()
