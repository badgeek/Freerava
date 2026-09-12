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

if (WIN32)
    add_custom_command(TARGET cyclomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:cyclomp> $<TARGET_FILE_DIR:cyclomp>
        COMMAND_EXPAND_LISTS)
endif()
