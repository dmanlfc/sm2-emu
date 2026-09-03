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

# Optimisation policy. One knob, opt-in and safe to leave off:
#
#   SM2_LTO   link-time optimisation for release configs
#
# CPU tuning is deliberately NOT set here. It belongs to the toolchain, not the
# project: a buildroot/Batocera cross-build for the Pi 5 already injects
# -mcpu=cortex-a76 and its own optimisation level through the toolchain's
# CMAKE_C_FLAGS/CMAKE_CXX_FLAGS, and a desktop developer picks their own via the
# build-type flags or a CMAKE_CXX_FLAGS override. Hardcoding -mcpu here would
# fight whatever the toolchain already chose. So the project inherits tuning and
# only adds the one cross-cutting codegen decision the toolchain does not make
# for it — LTO — and that only when asked.
#
# With SM2_LTO off, the flags emitted are exactly what they were before this
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
