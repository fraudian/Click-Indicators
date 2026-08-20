cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# libGDR templates ICE MSVC when in PCH, so disable it
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

# Build a universal binary on macOS. GitHub's macos-latest runners are Apple Silicon, so
# without this CMake builds for the host only and the .dylib is arm64. It loads fine on an M-series
# Mac and does not exist at all on an Intel one - which is a silent failure for the customer,
# since Geode simply finds no binary for their platform. Every other Geode mod on disk ships
# x86_64 + arm64; this matches them. Must be set before project() or it does not take effect.
#
# MUST NOT fire for iOS. CMake's APPLE describes the HOST, and an iOS build is cross-compiled from
# a macOS runner - so a bare `if (APPLE)` is true there too, and this FORCEs x86_64 into the cache
# before project(). Geode cannot undo it: its iOS branch (cmake/Platform.cmake) sets
# CMAKE_OSX_ARCHITECTURES directory-scoped inside its own add_subdirectory and applies
# set_target_properties to ITS target, while ClickGuide is created below at add_library - ten lines
# before add_subdirectory($ENV{GEODE_SDK}) runs. So the target keeps x86_64 and clang is pointed at
# the iPhoneOS sysroot with an architecture it cannot build. build-geode-mod passes
# -DGEODE_TARGET_PLATFORM=iOS on the configure line, and a -D cache entry is readable here, before
# project().
if (APPLE AND NOT IOS
         AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS"
         AND NOT GEODE_TARGET_PLATFORM STREQUAL "iOS")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS universal binary" FORCE)
endif()

project(ClickGuide VERSION 1.0.0)

# Fail loudly rather than shipping a bad slice. If the guard above ever stops holding, this turns a
# silently broken iOS artifact into a red build.
if (GEODE_TARGET_PLATFORM STREQUAL "iOS" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    message(FATAL_ERROR
        "iOS build picked up CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'. "
        "The macOS universal-binary block leaked into the iOS target.")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})

# dev unlock - only for local testing, must be OFF in CI builds
option(CI_DEV_UNLOCK "Allow the developer key to unlock the mod" OFF)
if (CI_DEV_UNLOCK)
    if (DEFINED ENV{CI})
        message(FATAL_ERROR "CI_DEV_UNLOCK is ON in a CI build. Refusing to produce a bypassable binary.")
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE CI_DEV_UNLOCK)
    message(WARNING "CI_DEV_UNLOCK is ON - this build accepts the developer key. Do not ship it.")
endif()
