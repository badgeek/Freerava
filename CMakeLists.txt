cmake_minimum_required(VERSION 3.21)
project(cyclomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Slint QUIET)
if (NOT Slint_FOUND)
  message("Slint not found in CMake search path — fetching and building from Git.")
  include(FetchContent)
  FetchContent_Declare(
    Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG release/1        # latest Slint 1.x. Build with the rustup toolchain
                             # (put ~/.cargo/bin on PATH): needs Rust >= 1.92.
                             # NOTE: nix rustc 1.84 is too old; rustc 1.98 works.
    SOURCE_SUBDIR api/cpp
  )
  FetchContent_MakeAvailable(Slint)
endif (NOT Slint_FOUND)

# Android builds a shared lib loaded by NativeActivity; desktop builds an exe.
if(ANDROID)
    add_library(cyclomp SHARED src/main.cpp)
else()
    add_executable(cyclomp src/main.cpp)
endif()

target_link_libraries(cyclomp PRIVATE Slint::Slint)
slint_target_sources(cyclomp ui/cyclomp.slint)

# ---- Optional MapLibre map (mbgl core embedded, no Java SDK) ----
# Expects a maplibre-native checkout with prebuilt cores:
#   build-macos-metal/  (desktop)   build-android/  (arm64-v8a)
# Produce it with scripts/build-maplibre.sh (see docs/maplibre.md). The map
# is simply left out of the build when the directory isn't there.
set(CYCLOMP_MLN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/maplibre-native" CACHE PATH
    "maplibre-native checkout containing prebuilt core libraries")

# Compile in the chatty per-frame / per-request map logs (tags cyclomp-map,
# cyclomp-http). mbgl's own error channel stays on regardless.
option(CYCLOMP_MAP_VERBOSE "Verbose MapLibre logging" OFF)
if(CYCLOMP_MAP_VERBOSE)
    target_compile_definitions(cyclomp PRIVATE CYCLOMP_MAP_VERBOSE)
endif()
set(_mln_vendor_includes
    ${CYCLOMP_MLN_DIR}/vendor/expected-lite/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/deps/cheap-ruler-cpp/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/deps/geojson-vt-cpp/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/deps/geojson.hpp/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/deps/geometry.hpp/include
    ${CYCLOMP_MLN_DIR}/vendor/maplibre-native-base/deps/variant/include
    ${CYCLOMP_MLN_DIR}/vendor/rapidjson/include
    ${CYCLOMP_MLN_DIR}/vendor/unordered_dense/include)

set(_mln_build_mac "${CYCLOMP_MLN_DIR}/build-macos-metal")
if(APPLE AND NOT ANDROID AND EXISTS "${_mln_build_mac}/libmbgl-core.a")
    message(STATUS "cyclomp: MapLibre map enabled (${_mln_build_mac})")
    target_sources(cyclomp PRIVATE src/map_service.cpp)
    target_compile_definitions(cyclomp PRIVATE CYCLOMP_HAVE_MAPLIBRE)
    target_include_directories(cyclomp PRIVATE
        ${CYCLOMP_MLN_DIR}/include
        ${CYCLOMP_MLN_DIR}/platform/darwin/include
        ${CYCLOMP_MLN_DIR}/platform/default/include)
    target_include_directories(cyclomp SYSTEM PRIVATE ${_mln_vendor_includes})
    target_link_libraries(cyclomp PRIVATE
        ${_mln_build_mac}/libmbgl-core.a
        ${_mln_build_mac}/libmbgl-vendor-parsedate.a
        ${_mln_build_mac}/vendor/maplibre-tile-spec/cpp/libmlt-cpp.a
        ${_mln_build_mac}/vendor/maplibre-tile-spec/cpp/libfastpfor-lib.a
        ${_mln_build_mac}/libmbgl-vendor-csscolorparser.a
        ${_mln_build_mac}/libmbgl-harfbuzz.a
        ${_mln_build_mac}/libmbgl-freetype.a
        ${_mln_build_mac}/libmbgl-vendor-icu.a
        sqlite3 z
        /opt/homebrew/opt/libuv/lib/libuv.dylib
        "-framework AppKit"
        "-framework CoreGraphics"
        "-framework CoreImage"
        "-framework CoreLocation"
        "-framework Metal"
        "-framework SystemConfiguration")
endif()

set(_mln_build_android "${CYCLOMP_MLN_DIR}/build-android")
if(ANDROID)
    target_link_libraries(cyclomp PRIVATE log android)
    if(EXISTS "${_mln_build_android}/libmbgl-core.a")
        message(STATUS "cyclomp: MapLibre map enabled for Android (${_mln_build_android})")
        target_sources(cyclomp PRIVATE
            src/map_service.cpp
            src/http_file_source_android.cpp
            # Platform pieces the Java SDK would normally provide:
            ${CYCLOMP_MLN_DIR}/platform/android/src/test/collator_test_stub.cpp
            ${CYCLOMP_MLN_DIR}/platform/android/src/test/number_format_test_stub.cpp
            ${CYCLOMP_MLN_DIR}/platform/default/src/mln/text/local_glyph_rasterizer.cpp)
        target_compile_definitions(cyclomp PRIVATE CYCLOMP_HAVE_MAPLIBRE)
        target_include_directories(cyclomp PRIVATE
            ${CYCLOMP_MLN_DIR}/include
            ${CYCLOMP_MLN_DIR}/src           # HTTPFileSource declaration
            ${CYCLOMP_MLN_DIR}/platform/default/include)
        target_include_directories(cyclomp SYSTEM PRIVATE ${_mln_vendor_includes})
        target_link_libraries(cyclomp PRIVATE
            "-Wl,--start-group"
            ${_mln_build_android}/libmbgl-core.a
            ${_mln_build_android}/libmbgl-vendor-parsedate.a
            ${_mln_build_android}/vendor/maplibre-tile-spec/cpp/libmlt-cpp.a
            ${_mln_build_android}/vendor/maplibre-tile-spec/cpp/libfastpfor-lib.a
            ${_mln_build_android}/libmbgl-vendor-csscolorparser.a
            ${_mln_build_android}/libmbgl-harfbuzz.a
            ${_mln_build_android}/libmbgl-freetype.a
            ${_mln_build_android}/libmbgl-vendor-icu.a
            ${_mln_build_android}/libmbgl-vendor-sqlite.a
            "-Wl,--end-group"
            EGL GLESv3 z atomic jnigraphics)
    endif()
endif()

if (WIN32)
    add_custom_command(TARGET cyclomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:cyclomp> $<TARGET_FILE_DIR:cyclomp>
        COMMAND_EXPAND_LISTS)
endif()
