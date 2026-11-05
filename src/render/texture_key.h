//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Identifies a polygon's texture by the content of its level-0 texels, so the
// texture dumper and the replacement loader agree on a name wherever the game
// happens to have uploaded it.
#pragma once

#include "core/types.h"
#include "render/geometry.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace sm2::hw {
class Model2MachineBase;
}

namespace sm2::render {

/// polygon.frag's fetchTexel against the packed sheet rather than the decoded
/// image.
[[nodiscard]] u32 fetch_texel(std::span<const u32> sheet, u32 base_x, u32 base_y, u32 x,
                              u32 y);

/// The level-0 texels of a textured polygon's texture, one 4-bit level per byte.
[[nodiscard]] std::vector<u8> read_texels(const hw::Model2MachineBase& machine,
                                          const PolyParams&            params);

/// The sixteen colours a texture's levels resolve to for one colour base, tone
/// table and lighting term, as polygon.frag would shade them unfiltered. RGBA8,
/// red in the low byte. `tone` is Model2Video::build_tone_curve()'s output.
[[nodiscard]] std::array<u32, 16> level_palette(std::span<const u8>  luma_ram,
                                                std::span<const u32> tone,
                                                u32 colour_components, u32 luma_base,
                                                u32 luma);

class TextureHasher {
public:
    struct Entry {
        u64                  hash = 0;
        std::array<u32, 16> histogram{};
    };

    /// Content hash (and level histogram) of `params`' texture, cached per
    /// region until texture RAM next changes.
    const Entry& lookup(const hw::Model2MachineBase& machine, const PolyParams& params,
                        std::vector<u8>* texels = nullptr);

private:
    u64                               m_generation = 0;
    std::unordered_map<u64, Entry>    m_regions;
};

}  // namespace sm2::render
