// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#pragma once

#include "core/types.h"

// Helpers MAME's ported CPU cores expect from emucore.h / osdcomm.h, with the
// same names and semantics so the ports stay diffable against upstream.

namespace sm2::cpu {

/// Unsigned 32x32 to 64 multiply.
[[nodiscard]] constexpr u64 mulu_32x32(u32 a, u32 b) noexcept
{
    return static_cast<u64>(a) * static_cast<u64>(b);
}

/// Signed 32x32 to 64 multiply.
[[nodiscard]] constexpr s64 mul_32x32(s32 a, s32 b) noexcept
{
    return static_cast<s64>(a) * static_cast<s64>(b);
}

}  // namespace sm2::cpu
