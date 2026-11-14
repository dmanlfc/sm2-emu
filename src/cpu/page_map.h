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

#include <memory>

// 4 KiB page tables for a 32-bit guest: host-backed pages (RAM/ROM) are read
// directly, null entries go to the bus slow path. Host memory is in guest byte
// order. Code pages are dropped from the write table only, so stores to them
// take the slow path, which invalidates the translated blocks.

namespace sm2::cpu {

class PageMap {
public:
    static constexpr u32 kPageBits = 12;
    static constexpr u32 kPageSize = 1U << kPageBits;
    static constexpr u32 kPageMask = kPageSize - 1;
    static constexpr u32 kPages    = 1U << (32 - kPageBits);

    enum class Access : u8 {
        ReadOnly,   ///< ROM: writes go to the slow path.
        ReadWrite,  ///< RAM.
    };

    PageMap()
        : m_read(std::make_unique<u8*[]>(kPages))
        , m_write(std::make_unique<u8*[]>(kPages))
        , m_ram_write(std::make_unique<u8*[]>(kPages))
        , m_code(std::make_unique<u8[]>(kPages))
    {
    }

    PageMap(const PageMap&)            = delete;
    PageMap& operator=(const PageMap&) = delete;

    /// Back [base, base + size) with `host`. Both base and size must be
    /// page-aligned. A mapping smaller than its window (a mirrored ROM) is
    /// expressed by calling this once per mirror.
    void map(u32 base, u64 size, u8* host, Access access)
    {
        for (u64 offset = 0; offset < size; offset += kPageSize) {
            const u32 page = static_cast<u32>((base + offset) >> kPageBits);
            u8* const p    = host + offset;
            m_read[page]      = p;
            m_ram_write[page] = access == Access::ReadWrite ? p : nullptr;
            m_write[page]     = m_code[page] != 0 ? nullptr : m_ram_write[page];
        }
    }

    /// Send [base, base + size) to the slow path.
    void unmap(u32 base, u64 size)
    {
        for (u64 offset = 0; offset < size; offset += kPageSize) {
            const u32 page    = static_cast<u32>((base + offset) >> kPageBits);
            m_read[page]      = nullptr;
            m_write[page]     = nullptr;
            m_ram_write[page] = nullptr;
            m_code[page]      = 0;
        }
    }

    /// Host address of guest byte `address`, or nullptr for the slow path.
    [[nodiscard]] u8* read_ptr(u32 address) const
    {
        u8* const page = m_read[address >> kPageBits];
        return page != nullptr ? page + (address & kPageMask) : nullptr;
    }
    [[nodiscard]] u8* write_ptr(u32 address) const
    {
        u8* const page = m_write[address >> kPageBits];
        return page != nullptr ? page + (address & kPageMask) : nullptr;
    }

    /// The RAM behind `address` even when it is a code page, for the slow path.
    [[nodiscard]] u8* ram_write_ptr(u32 address) const
    {
        u8* const page = m_ram_write[address >> kPageBits];
        return page != nullptr ? page + (address & kPageMask) : nullptr;
    }

    /// Mark the page holding `address` as holding translated code, or clear it.
    void set_code_page(u32 address, bool code)
    {
        const u32 page = address >> kPageBits;
        m_code[page]   = code ? 1 : 0;
        m_write[page]  = code ? nullptr : m_ram_write[page];
    }
    [[nodiscard]] bool is_code_page(u32 address) const { return m_code[address >> kPageBits] != 0; }

    /// For code that inlines the lookup.
    [[nodiscard]] u8* const* read_table() const { return m_read.get(); }
    [[nodiscard]] u8* const* write_table() const { return m_write.get(); }

private:
    std::unique_ptr<u8*[]> m_read;
    std::unique_ptr<u8*[]> m_write;
    std::unique_ptr<u8*[]> m_ram_write;
    std::unique_ptr<u8[]>  m_code;
};

}  // namespace sm2::cpu
