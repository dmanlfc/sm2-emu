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

#include <cstddef>
#include <cstdint>

// Project-wide integer vocabulary.
//
// Emulation code lives and dies by exact widths, so everything is spelled with
// an explicit size. Deliberately short names: a memory map or a CPU core reads
// far better as `u32 addr` than `std::uint32_t addr`, and these appear on
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
// Model 2 ships floats down a 24-bit wire by discarding the low 8 mantissa
// bits, so the geometry code moves between float and its bit pattern
// constantly. These mirror MAME's f2u/u2f.

[[nodiscard]] inline u32 f2u(float value) noexcept
{
    u32 out;
    __builtin_memcpy(&out, &value, sizeof(out));
    return out;
}

[[nodiscard]] inline float u2f(u32 value) noexcept
{
    float out;
    __builtin_memcpy(&out, &value, sizeof(out));
    return out;
}

[[nodiscard]] inline u64 d2u(double value) noexcept
{
    u64 out;
    __builtin_memcpy(&out, &value, sizeof(out));
    return out;
}

[[nodiscard]] inline double u2d(u64 value) noexcept
{
    double out;
    __builtin_memcpy(&out, &value, sizeof(out));
    return out;
}

// ---------------------------------------------------------------------------
// Alignment predicates
// ---------------------------------------------------------------------------
// The i960 permits unaligned accesses and splits them into byte operations, so
// its load and store paths test alignment on every access. Named to match
// MAME's macros, which the ported CPU cores use.

[[nodiscard]] constexpr bool word_aligned(u32 address) noexcept
{
    return (address & 1) == 0;
}

[[nodiscard]] constexpr bool dword_aligned(u32 address) noexcept
{
    return (address & 3) == 0;
}

}  // namespace sm2
