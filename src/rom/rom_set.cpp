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
#include "rom/rom_set.h"

#include <algorithm>

namespace sm2::rom {

void RomSet::add(std::string name, std::vector<u8> data)
{
    m_regions.insert_or_assign(std::move(name), std::move(data));
}

bool RomSet::has(std::string_view name) const
{
    return m_regions.find(std::string(name)) != m_regions.end();
}

std::span<const u8> RomSet::region(std::string_view name) const
{
    const auto entry = m_regions.find(std::string(name));
    if (entry == m_regions.end()) {
        return {};
    }
    return std::span<const u8>(entry->second);
}

std::span<u8> RomSet::region_mutable(std::string_view name)
{
    const auto entry = m_regions.find(std::string(name));
    if (entry == m_regions.end()) {
        return {};
    }
    return std::span<u8>(entry->second);
}

usize RomSet::region_size(std::string_view name) const
{
    return region(name).size();
}

usize RomSet::total_bytes() const
{
    usize total = 0;
    for (const auto& [name, data] : m_regions) {
        total += data.size();
    }
    return total;
}

std::vector<std::string> RomSet::region_names() const
{
    std::vector<std::string> names;
    names.reserve(m_regions.size());
    for (const auto& [name, data] : m_regions) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace sm2::rom
