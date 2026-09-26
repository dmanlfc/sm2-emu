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

# Optimisation policy. Two knobs, both opt-in and safe to leave off:
#
#   SM2_LTO    link-time optimisation for release configs
#   SM2_TUNE   target ISA level passed to -march (empty = inherit toolchain)
#
# CPU tuning defaults to OFF here, for the same reason it always has: it belongs
# to the toolchain, not the project. A buildroot/Batocera cross-build for the
# Pi 5 already injects -mcpu=cortex-a76 through the toolchain's
# CMAKE_C_FLAGS/CMAKE_CXX_FLAGS, and a desktop developer picks their own. When
# SM2_TUNE is empty the project adds no -march at all and inherits whatever the
# toolchain chose, exactly as before.
#
# SM2_TUNE exists for one case the toolchain cannot decide: a *distributed*
# binary that must run on an unknown CPU. There, the generic x86-64 baseline
# (no SSE4/POPCNT, VEX-less scalar FP) is a measurable loss, so the release CI
# opts in to a conservative floor (x86-64-v2) that every CPU from ~2009 on can
# run. It is a GCC/Clang -march value; on MSVC it is ignored (MSVC has no
# equivalent and /arch levels do not map cleanly).
#
# With both knobs off, the flags emitted are exactly what they were before this
# module existed: the desktop Vulkan/GL paths verified in phase 9 do not shift.

include(CheckIPOSupported)

option(SM2_LTO "Enable link-time optimisation for release builds" OFF)

# ---------------------------------------------------------------------------
# Link-time optimisation
# ---------------------------------------------------------------------------
# The emulator is split across ~12 static libraries (see src/CMakeLists.txt), so
# the hot i960/coprocessor interpreters and the geometry engine are compiled in
# one archive and called from another. Without LTO the compiler never sees
# across that boundary and cannot inline the per-instruction bus dispatch or the
# geometry inner loops into their callers. LTO is where that crossing opens up.
#
# check_ipo_supported() rather than assuming -flto works: a cross-toolchain or
# an unusual linker may not, and a hard failure over an optional speed knob is
# the wrong trade. Applied only to the release configs so a Debug build stays
# fast to link and easy to step through.

if(SM2_LTO)
    check_ipo_supported(RESULT sm2_ipo_ok OUTPUT sm2_ipo_msg LANGUAGES CXX)
    if(sm2_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON)
        message(STATUS "  LTO             : on (release configs)")
    else()
        message(STATUS "  LTO             : requested but unsupported — ${sm2_ipo_msg}")
    endif()
else()
    message(STATUS "  LTO             : off (toolchain flags inherited as-is)")
endif()

# ---------------------------------------------------------------------------
# Target ISA level (distributed-binary tuning)
# ---------------------------------------------------------------------------
# Empty by default: no -march, toolchain tuning inherited untouched. Set to an
# -march value (e.g. x86-64-v2, x86-64-v3, native) to raise the codegen floor
# for a binary shipped to unknown hardware. Applied globally to C and C++.
#
# GCC/Clang only. MSVC has no -march and its /arch levels do not map onto the
# x86-64-vN feature levels, so SM2_TUNE is a no-op there rather than a silent
# mistranslation.

set(SM2_TUNE "" CACHE STRING
    "Target ISA for -march (empty = inherit toolchain; e.g. x86-64-v2, native)")

if(SM2_TUNE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options("-march=${SM2_TUNE}")
        message(STATUS "  tuning          : -march=${SM2_TUNE}")
    else()
        message(STATUS "  tuning          : SM2_TUNE=${SM2_TUNE} ignored (not GCC/Clang)")
    endif()
else()
    message(STATUS "  tuning          : none (toolchain flags inherited as-is)")
endif()
