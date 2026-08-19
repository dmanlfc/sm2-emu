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

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sm2::rom {

/// Assembled ROM regions for one game.
///
/// Each region is a flat byte array in exactly the layout the hardware sees,
/// so the memory map can point straight at it with no further shuffling. This
/// is the loader's whole output; the machine takes ownership of it at startup
/// and the loader is then discarded.
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
    // Keyed by string because regions are named after MAME's tags, which keeps
    // ported memory maps readable. There are fewer than a dozen per game and
    // they are looked up once at startup, so the cost is irrelevant.
    std::unordered_map<std::string, std::vector<u8>> m_regions;
};

}  // namespace sm2::rom
