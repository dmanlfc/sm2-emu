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
#include "cpu/ppc/ppc_state.h"

#include <bit>
#include <cmath>
#if defined(__GNUC__) && defined(__x86_64__)
#include <cpuid.h>
#endif

// FP semantics shared by the interpreters (PEM chapter 3, appendix D).

namespace sm2::cpu::ppc::fpu {

inline constexpr u64 kSign = 0x8000000000000000ull;
inline constexpr u64 kExp  = 0x7ff0000000000000ull;
inline constexpr u64 kFrac = 0x000fffffffffffffull;
inline constexpr u64 kQuietBit = 0x0008000000000000ull;

[[nodiscard]] inline bool is_nan(u64 x) { return (x & ~kSign) > kExp; }
[[nodiscard]] inline bool is_snan(u64 x)
{
    return is_nan(x) && (x & kQuietBit) == 0;
}

[[nodiscard]] inline double as_double(u64 x) { return std::bit_cast<double>(x); }
[[nodiscard]] inline u64 as_bits(double x) { return std::bit_cast<u64>(x); }

/// lfs (PEM D.6): exact, and SNaNs stay signalling.
[[nodiscard]] inline u64 single_to_double(u32 w)
{
    if ((w & 0x7f800000u) == 0x7f800000u) {
        return (u64{w & 0x80000000u} << 32) | kExp | (u64{w & 0x007fffffu} << 29);
    }
    return as_bits(static_cast<double>(std::bit_cast<float>(w)));
}

/// stfs (PEM D.7): truncating, tiny values denormalised.
[[nodiscard]] inline u32 double_to_single(u64 x)
{
    const u32 e = static_cast<u32>((x >> 52) & 0x7ff);
    if (e > 896 || e == 0x7ff) {
        return static_cast<u32>(((x >> 32) & 0xc0000000u) | ((x >> 29) & 0x3fffffffu));
    }
    const u32 sign = static_cast<u32>(x >> 32) & 0x80000000u;
    if (e >= 874) {
        const u64 mant = (x & kFrac) | (1ull << 52);
        return sign | static_cast<u32>(mant >> (926 - e));
    }
    return sign;
}

[[nodiscard]] inline double round_single(double v)
{
    return static_cast<double>(static_cast<float>(v));
}

// a + b, a * b and fma(a, c, b) with the operands in this order: with two NaN
// inputs the host returns the first, and compilers would otherwise commute them.
#if defined(__GNUC__) && defined(__aarch64__)
[[nodiscard]] inline double add(double a, double b)
{
    double r;
    asm("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
[[nodiscard]] inline double mul(double a, double b)
{
    double r;
    asm("fmul %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
[[nodiscard]] inline double fma(double a, double c, double b)
{
    double r;
    asm("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(c), "w"(b));
    return r;
}
#elif defined(__GNUC__) && defined(__x86_64__)
[[nodiscard]] inline double add(double a, double b)
{
    asm("addsd %1, %0" : "+x"(a) : "x"(b));
    return a;
}
[[nodiscard]] inline double mul(double a, double b)
{
    asm("mulsd %1, %0" : "+x"(a) : "x"(b));
    return a;
}
/// FMA3 usable (CPU and OS). The x86-64 JIT emits the same instruction so
/// NaN precedence matches.
[[nodiscard]] inline bool host_has_fma()
{
    static const bool has = [] {
        unsigned a, b, c, d;
        if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
        if ((c & (1u << 12)) == 0 || (c & (1u << 27)) == 0 || (c & (1u << 28)) == 0) return false;
        unsigned lo, hi;
        asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        return (lo & 6) == 6;
    }();
    return has;
}
[[nodiscard]] inline double fma(double a, double c, double b)
{
    if (host_has_fma()) {
        asm("vfmadd213sd %2, %1, %0" : "+x"(a) : "x"(c), "x"(b));
        return a;
    }
    return std::fma(a, c, b);
}
#else
[[nodiscard]] inline double add(double a, double b) { return a + b; }
[[nodiscard]] inline double mul(double a, double b) { return a * b; }
[[nodiscard]] inline double fma(double a, double c, double b) { return std::fma(a, c, b); }
[[nodiscard]] inline bool host_has_fma() { return false; }
#endif

/// The negation is of the rounded result (an exact zero becomes -0): keep the
/// compiler from folding it into a fused negate-multiply-add, which rounds
/// the negated operands instead.
[[nodiscard]] inline double negate_unless_nan(double v)
{
#if defined(__GNUC__) && defined(__aarch64__)
    asm("" : "+w"(v));
#elif defined(__GNUC__) && defined(__x86_64__)
    asm("" : "+x"(v));
#endif
    return std::isnan(v) ? v : -v;
}

/// FPSCR[FPRF] for a result, in place.
[[nodiscard]] inline u32 fprf_of(u64 x)
{
    const bool neg = (x & kSign) != 0;
    const u64  ef  = x & ~kSign;
    u32 c;
    if (ef > kExp) {
        c = 0x11;                       // quiet NaN (SNaNs are reported as QNaN too)
    } else if (ef == kExp) {
        c = neg ? 0x09 : 0x05;          // -inf / +inf
    } else if (ef == 0) {
        c = neg ? 0x12 : 0x02;          // -0 / +0
    } else if ((ef & kExp) == 0) {
        c = neg ? 0x18 : 0x14;          // denormal
    } else {
        c = neg ? 0x08 : 0x04;          // normal
    }
    return c << 12;
}

/// Recompute VX and FEX.
[[nodiscard]] inline u32 fpscr_summarise(u32 f)
{
    f &= ~(kFpscrVX | kFpscrFEX);
    if (f & kFpscrVXAll) {
        f |= kFpscrVX;
    }
    if (((f & kFpscrVX) && (f & kFpscrVE)) || ((f & kFpscrOX) && (f & kFpscrOE)) ||
        ((f & kFpscrUX) && (f & kFpscrUE)) || ((f & kFpscrZX) && (f & kFpscrZE)) ||
        ((f & kFpscrXX) && (f & kFpscrXE))) {
        f |= kFpscrFEX;
    }
    return f;
}

/// fctiw(z) result for an already rounded value.
[[nodiscard]] inline u64 fctiw_bits(double r)
{
    if (std::isnan(r)) {
        return 0xffffffff80000000ull;
    }
    if (r > 2147483647.0) {
        return 0x000000007fffffffull;
    }
    if (r < -2147483648.0) {
        return 0xffffffff80000000ull;
    }
    return static_cast<u32>(static_cast<s32>(r));
}

[[nodiscard]] inline u8 compare(double a, double b)
{
    if (a < b) {
        return 8;
    }
    if (a > b) {
        return 4;
    }
    if (a == b) {
        return 2;
    }
    return 1;
}

}  // namespace sm2::cpu::ppc::fpu
