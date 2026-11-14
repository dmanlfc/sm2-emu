cmake_minimum_required(VERSION 3.24)

# ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
# ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
# █████╔╝ █████╗  ██║███████╗███████║██║   ██║
# ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
# ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
# ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
#            継 承  ( k e i s h ō )
#
# A Sega Model 1, 2 & 3 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This header must not be removed. Keisho is licensed under the GNU General
# Public License, version 3 or later, with additional terms under section 7 of
# that licence. See LICENSE and NOTICE.
#

project(keisho
    VERSION     0.9.61
    DESCRIPTION "Sega Model 1, 2 & 3 arcade emulator"
    LANGUAGES   C CXX
)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Apple's ld warns on every static library CMake repeats for link order.
if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" SM2_LD_NO_DUP_WARN)
    if(SM2_LD_NO_DUP_WARN)
        add_link_options(LINKER:-no_warn_duplicate_libraries)
    endif()
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep binaries in one predictable place; the emulator resolves data files
# relative to the executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# Default validation on for Debug builds. Spelled as a plain conditional
# because option() stores its default verbatim and would keep a generator
# expression as a literal string.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SM2_VALIDATION_DEFAULT ON)
else()
    set(SM2_VALIDATION_DEFAULT OFF)
endif()
option(SM2_ENABLE_VALIDATION
    "Request Vulkan validation layers at runtime by default"
    ${SM2_VALIDATION_DEFAULT})
option(SM2_WERROR       "Treat compiler warnings as errors"        OFF)
option(SM2_SHADER_WERROR "Treat shader compiler warnings as errors" ON)
option(SM2_BUILD_TESTS  "Build unit tests"                          OFF)

# Dependencies come from the system where available, falling back to the copies
# under 3rdparty/, so a recursive checkout builds offline. See
# cmake/Dependencies.cmake.

# Software rendering is always built. Vulkan defaults off so a bare build needs
# no Vulkan driver. SM2_BUILD_OPENGL_DESKTOP and SM2_BUILD_OPENGL_ES are mutually
# exclusive; GLES is for ARM targets.
option(SM2_BUILD_VULKAN         "Build the Vulkan backend"                 OFF)
option(SM2_BUILD_OPENGL_DESKTOP "Build the OpenGL 4.3 core desktop backend" ON)
option(SM2_BUILD_OPENGL_ES      "Build the OpenGL ES 3.1 backend (ARM)"    OFF)

# Per-device light guns on Linux via evdev and libudev: SDL merges all mice into
# one pointer, so this is the only way to tell two guns apart. ON makes a missing
# libudev an error. Windows uses Raw Input, which is always built.
set(SM2_EVDEV "AUTO" CACHE STRING "evdev light guns via libudev: AUTO, ON or OFF")
set_property(CACHE SM2_EVDEV PROPERTY STRINGS AUTO ON OFF)

# ---------------------------------------------------------------------------
# Platform identification
# ---------------------------------------------------------------------------
# Linux is the primary target. macOS is supported for development and runs
# Vulkan through MoltenVK, which is why the portability-subset extensions are
# requested opportunistically rather than gated on the platform.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SM2_PLATFORM_LINUX TRUE)
elseif(APPLE)
    set(SM2_PLATFORM_MACOS TRUE)
elseif(WIN32)
    set(SM2_PLATFORM_WINDOWS TRUE)
    # <windows.h>, pulled in by some vendored deps, defines min/max macros that
    # break std::min/std::max.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # Source files are UTF-8 (the file banner); MSVC otherwise reads them in
    # the system code page.
    if(MSVC)
        add_compile_options(/utf-8)
    endif()
else()
    message(WARNING "Keisho has not been tested on ${CMAKE_SYSTEM_NAME}")
endif()

set(SM2_HAVE_EVDEV FALSE)
if(NOT SM2_EVDEV STREQUAL "OFF" AND SM2_PLATFORM_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(UDEV IMPORTED_TARGET libudev)
    endif()
    if(UDEV_FOUND)
        set(SM2_HAVE_EVDEV TRUE)
    elseif(SM2_EVDEV STREQUAL "ON")
        message(FATAL_ERROR
            "SM2_EVDEV=ON but libudev was not found. Install libudev-dev "
            "(Debian/Ubuntu), systemd-devel (Fedora) or systemd-libs (Arch), "
            "or configure with -DSM2_EVDEV=OFF.")
    endif()
elseif(SM2_EVDEV STREQUAL "ON")
    message(FATAL_ERROR "SM2_EVDEV=ON is only supported on Linux.")
endif()

include(Optimization)
include(CompilerWarnings)
include(Dependencies)
include(EmbedShaders)

add_subdirectory(shaders)
add_subdirectory(src)

if(SM2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "Keisho ${PROJECT_VERSION}")
message(STATUS "  system          : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(SM2_BUILD_OPENGL_ES)
    set(SM2_GL_BACKEND_SUMMARY "OpenGL ES 3.1")
elseif(SM2_BUILD_OPENGL_DESKTOP)
    set(SM2_GL_BACKEND_SUMMARY "OpenGL 4.3 core")
else()
    set(SM2_GL_BACKEND_SUMMARY "no")
endif()
message(STATUS "  backends        : software, ${SM2_GL_BACKEND_SUMMARY} GL, Vulkan=${SM2_BUILD_VULKAN}")
if(SM2_BUILD_VULKAN)
    message(STATUS "  Vulkan target   : 1.3 core (dynamic rendering, synchronization2)")
    message(STATUS "  Vulkan headers  : ${Vulkan_VERSION}")
    message(STATUS "  validation      : ${SM2_ENABLE_VALIDATION}")
endif()
message(STATUS "  glslc           : ${SM2_GLSLC}")
message(STATUS "  SDL3            : ${SM2_SDL3_ORIGIN}")
message(STATUS "  HarfBuzz        : ${SM2_HARFBUZZ_ORIGIN}")
message(STATUS "")
