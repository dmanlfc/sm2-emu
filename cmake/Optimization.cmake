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

# Optimisation policy. Two knobs, both opt-in and safe to leave off:
#
#   SM2_LTO    link-time optimisation for release configs
#   SM2_TUNE   target ISA level passed to -march (empty = inherit toolchain)
#
# CPU tuning belongs to the toolchain (a Batocera cross build already passes
# -mcpu), so SM2_TUNE is empty by default and adds no -march. It exists for
# distributed binaries that must run on an unknown CPU, where release CI opts in
# to a conservative floor such as x86-64-v2.

include(CheckIPOSupported)

option(SM2_LTO "Enable link-time optimisation for release builds" OFF)

# ---------------------------------------------------------------------------
# Link-time optimisation
# ---------------------------------------------------------------------------
# The hot interpreters and geometry code live in different static libraries
# from their callers, so only LTO can inline across them. Support is checked
# rather than assumed, since a cross toolchain may lack it, and Debug is left
# alone.

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
# GCC/Clang only: MSVC's /arch levels do not map onto the x86-64-vN levels, so
# SM2_TUNE is ignored there.

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

# ---------------------------------------------------------------------------
# Floating point contraction
# ---------------------------------------------------------------------------
# Fused multiply-adds on ARM move polygon edge pixels against x86.

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffp-contract=off)
endif()
