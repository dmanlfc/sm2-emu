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

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sm2::rom {

/// Assembled ROM regions for one game, each a flat byte array in the layout
/// the hardware sees.
class RomSet {
public:
    void add(std::string name, std::vector<u8> data);

    [[nodiscard]] bool has(std::string_view name) const;

    /// Contents of a region, or an empty span if it is absent.
    [[nodiscard]] std::span<const u8> region(std::string_view name) const;

    /// Mutable view, for the few regions the hardware writes back to.
    [[nodiscard]] std::span<u8> region_mutable(std::string_view name);

    [[nodiscard]] usize region_size(std::string_view name) const;

    [[nodiscard]] usize total_bytes() const;

    [[nodiscard]] std::vector<std::string> region_names() const;

private:
    // Keyed by MAME's region tags, looked up once at startup.
    std::unordered_map<std::string, std::vector<u8>> m_regions;
};

}  // namespace sm2::rom
