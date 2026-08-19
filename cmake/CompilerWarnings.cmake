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

# Shared warning configuration.
#
# Two INTERFACE targets:
#
#   sm2_warnings         strict, for code written here
#   sm2_warnings_ported  relaxed, for code derived from MAME
#
# Both are built from one flag list rather than by having the relaxed target link
# the strict one. Linking would work, but CMake emits a target's own options
# before its dependencies', which puts the -Wno- overrides first where they have
# no effect: for GCC and Clang the last flag wins.

set(SM2_WARNING_FLAGS "")
set(SM2_WARNING_RELAXATIONS "")

if(MSVC)
    list(APPEND SM2_WARNING_FLAGS
        /W4
        /permissive-
        /wd4244   # conversion, narrowing: pervasive and intentional in CPU cores
        /wd4267
    )
    if(SM2_WERROR)
        list(APPEND SM2_WARNING_FLAGS /WX)
    endif()

    list(APPEND SM2_WARNING_RELAXATIONS
        /wd4100   # unreferenced formal parameter
        /wd4189   # local variable initialised but not referenced
    )
else()
    list(APPEND SM2_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough

        # Emulation code converts between integer widths constantly and by
        # design. Enabling these produces thousands of diagnostics that say
        # nothing about correctness.
        -Wno-conversion
        -Wno-sign-conversion
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        list(APPEND SM2_WARNING_FLAGS
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
        )
    endif()

    if(SM2_WERROR)
        list(APPEND SM2_WARNING_FLAGS -Werror)
    endif()

    # Ported cores are kept as textually close to their MAME originals as
    # possible so that upstream fixes can be diffed across. These are the checks
    # that fire on idioms pervasive in that code and cannot be silenced without
    # rewriting it.
    list(APPEND SM2_WARNING_RELAXATIONS
        -Wno-pedantic
        -Wno-shadow
        -Wno-unused-parameter
        -Wno-double-promotion   # float registers widened to double throughout
        -Wno-cast-align
        -Wno-sign-compare       # loop counters reused across signed and unsigned
    )
endif()

add_library(sm2_warnings INTERFACE)
target_compile_options(sm2_warnings INTERFACE ${SM2_WARNING_FLAGS})

add_library(sm2_warnings_ported INTERFACE)
target_compile_options(sm2_warnings_ported INTERFACE
    ${SM2_WARNING_FLAGS}
    ${SM2_WARNING_RELAXATIONS}
)
