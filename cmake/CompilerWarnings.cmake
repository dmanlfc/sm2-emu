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

# Shared warning configuration.
#
#   sm2_warnings         strict, for code written here
#   sm2_warnings_ported  relaxed, for code derived from MAME
#
# The relaxed target repeats the strict flags rather than linking the strict
# target: CMake emits a target's own options before its dependencies', which
# would put the -Wno- overrides first, and for GCC and Clang the last flag wins.

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

        # Emulation code converts between integer widths constantly and by design.
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

    # Ported cores stay textually close to MAME so upstream fixes can be diffed
    # across; these checks fire on idioms pervasive in that code.
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
