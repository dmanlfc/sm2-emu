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

#include <bit>
#include <cstddef>
#include <cstdint>

// Project-wide fixed-width integer names, kept short because they appear on
// nearly every line of the hardware layer.

namespace sm2 {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using usize = std::size_t;

// ---------------------------------------------------------------------------
// Bit helpers
// ---------------------------------------------------------------------------

/// Single bit `n` of `value`, as 0 or 1.
template <typename T>
[[nodiscard]] constexpr T bit(T value, unsigned n) noexcept
{
    return (value >> n) & T{1};
}

/// `count` bits of `value` starting at bit `first`.
template <typename T>
[[nodiscard]] constexpr T bits(T value, unsigned first, unsigned count) noexcept
{
    return (value >> first) & static_cast<T>((T{1} << count) - T{1});
}

/// Sign-extend the low `width` bits of `value`.
template <typename T>
[[nodiscard]] constexpr s32 sign_extend(T value, unsigned width) noexcept
{
    const u32 mask = 1u << (width - 1);
    const u32 raw  = static_cast<u32>(value) & ((1u << width) - 1u);
    return static_cast<s32>((raw ^ mask) - mask);
}

// ---------------------------------------------------------------------------
// Float / integer reinterpretation
// ---------------------------------------------------------------------------
// Model 2 sends floats over a 24-bit bus by dropping the low 8 mantissa bits,
// so geometry code often works on the bit pattern. These mirror MAME's f2u/u2f.

[[nodiscard]] constexpr u32 f2u(float value) noexcept
{
    return std::bit_cast<u32>(value);
}

[[nodiscard]] constexpr float u2f(u32 value) noexcept
{
    return std::bit_cast<float>(value);
}

[[nodiscard]] constexpr u64 d2u(double value) noexcept
{
    return std::bit_cast<u64>(value);
}

[[nodiscard]] constexpr double u2d(u64 value) noexcept
{
    return std::bit_cast<double>(value);
}

// ---------------------------------------------------------------------------
// Alignment predicates
// ---------------------------------------------------------------------------
// Named to match MAME's macros, which the ported CPU cores use.

[[nodiscard]] constexpr bool word_aligned(u32 address) noexcept
{
    return (address & 1) == 0;
}

[[nodiscard]] constexpr bool dword_aligned(u32 address) noexcept
{
    return (address & 3) == 0;
}

}  // namespace sm2
