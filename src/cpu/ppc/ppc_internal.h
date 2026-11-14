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

#include "cpu/ppc/ppc.h"

#include <bit>
#include <cstring>
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#endif

// Inline guest-memory fast paths.

namespace sm2::cpu::ppc {

/// std::byteswap is C++23.
template <class T>
[[nodiscard]] inline T byteswap_any(T v)
{
    if constexpr (sizeof(T) == 1) {
        return v;
#if defined(_MSC_VER) && !defined(__clang__)
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(_byteswap_ushort(static_cast<u16>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(_byteswap_ulong(static_cast<u32>(v)));
    } else {
        return static_cast<T>(_byteswap_uint64(static_cast<u64>(v)));
    }
#else
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<u16>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<u32>(v)));
    } else {
        return static_cast<T>(__builtin_bswap64(static_cast<u64>(v)));
    }
#endif
}

template <class T>
inline T Ppc::load(u32 ea)
{
    u8* const page = m_st.read_table[ea >> PageMap::kPageBits];
    const u32 off  = ea & PageMap::kPageMask;
    if (page != nullptr && off <= PageMap::kPageSize - sizeof(T)) [[likely]] {
        T v;
        std::memcpy(&v, page + off, sizeof(T));
        return byteswap_any(v);
    }
    if constexpr (sizeof(T) == 1) {
        return slow_read8(ea);
    } else if constexpr (sizeof(T) == 2) {
        return slow_read16(ea);
    } else if constexpr (sizeof(T) == 4) {
        return slow_read32(ea);
    } else {
        return slow_read64(ea);
    }
}

template <class T>
inline void Ppc::store(u32 ea, T v)
{
    u8* const page = m_st.write_table[ea >> PageMap::kPageBits];
    const u32 off  = ea & PageMap::kPageMask;
    if (page != nullptr && off <= PageMap::kPageSize - sizeof(T)) [[likely]] {
        const T s = byteswap_any(v);
        std::memcpy(page + off, &s, sizeof(T));
        return;
    }
    if constexpr (sizeof(T) == 1) {
        slow_write8(ea, v);
    } else if constexpr (sizeof(T) == 2) {
        slow_write16(ea, v);
    } else if constexpr (sizeof(T) == 4) {
        slow_write32(ea, v);
    } else {
        slow_write64(ea, v);
    }
}

}  // namespace sm2::cpu::ppc
