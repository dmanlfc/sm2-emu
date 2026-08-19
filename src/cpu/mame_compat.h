//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#pragma once

#include "core/types.h"

// Small helpers that MAME's ported CPU cores expect from emucore.h / osdcomm.h.
//
// Reimplemented here with the same names and semantics so the ported sources can
// keep using them verbatim, which is what keeps them diffable against upstream.
// Add to this only when a port actually needs something.

namespace sm2::cpu {

/// Unsigned 32x32 to 64 multiply.
///
/// Spelled out rather than left as `a * b` because the operands are 32-bit and
/// the product must not be computed at 32 bits first, which is exactly the bug
/// the named helper exists to prevent.
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
