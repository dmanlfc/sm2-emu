cmake_minimum_required(VERSION 3.24)

#  ____  __  __  ____         _____ __  __ _   _
# / ___||  \/  ||___ \       | ____|  \/  | | | |
# \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
#  ___) | |  | | / __/|_____|| |___| |  | | |_| |
# |____/|_|  |_||_____|      |_____|_|  |_|\___/
#
# sm2-emu — A Sega Model 2 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: BSD-3-Clause
#
# This header must not be removed. The source files in this project may not be
# used to contribute to commercial projects or for monetary gain without the
# express written permission of the author.
#

project(sm2-emu
    VERSION     0.9.2
    DESCRIPTION "Sega Model 2 arcade emulator"
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
option(SM2_BUILD_TESTS  "Build unit tests"                          ON)

# Dependencies come from the system where available (find_package). Anything
# not found falls back to the copy under 3rdparty/ (git submodules, plus the
# vendored LZMA SDK), so a recursive checkout builds unattended with no network
# access at configure time. See cmake/Dependencies.cmake.

# Render backends. Software is always built; the GPU backends are opt-in.
# Vulkan defaults off so a bare build needs no Vulkan driver (for ARM boards
# without one). SM2_BUILD_OPENGL_DESKTOP and SM2_BUILD_OPENGL_ES are mutually
# exclusive; GLES is for ARM targets.
option(SM2_BUILD_VULKAN         "Build the Vulkan backend"                 OFF)
option(SM2_BUILD_OPENGL_DESKTOP "Build the OpenGL 4.3 core desktop backend" ON)
option(SM2_BUILD_OPENGL_ES      "Build the OpenGL ES 3.1 backend (ARM)"    OFF)

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
else()
    message(WARNING "sm2-emu has not been tested on ${CMAKE_SYSTEM_NAME}")
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
message(STATUS "sm2-emu ${PROJECT_VERSION}")
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
message(STATUS "")
