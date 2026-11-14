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

#include "render/texture_key.h"

#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <algorithm>

namespace sm2::render {
namespace {

[[nodiscard]] u64 mix(u64 hash, u64 value)
{
    hash ^= value;
    return hash * 0x100000001b3ull;
}

}  // namespace

u32 fetch_texel(std::span<const u32> sheet, u32 base_x, u32 base_y, u32 x, u32 y)
{
    u32 x2 = base_x + x;
    u32 y2 = base_y + y;
    if (x2 >= 1024u) {
        x2 -= 1024u;
        y2 ^= 1024u;
    }
    const u32 offset = (y2 >> 1) * 512u + (x2 >> 1);
    const u32 index  = offset >> 1;
    const u32 word   = index < sheet.size() ? sheet[index] : 0u;
    const u32 half   = (offset & 1u) != 0u ? word >> 16 : word & 0xffffu;
    const u32 byte   = (y2 & 1u) == 0u ? (half >> 8) & 0xffu : half & 0xffu;
    return (x2 & 1u) == 0u ? byte >> 4 : byte & 0xfu;
}

std::vector<u8> read_texels(const hw::Model2MachineBase& machine, const PolyParams& params)
{
    const int sheet_index = (params.flags & kFlagSheet) != 0 ? 1 : 0;
    const std::span<const u32> sheet = machine.texture_ram(sheet_index);
    std::vector<u8> texels(static_cast<usize>(params.tex_width) * params.tex_height);
    for (u32 y = 0; y < params.tex_height; ++y) {
        for (u32 x = 0; x < params.tex_width; ++x) {
            texels[static_cast<usize>(y) * params.tex_width + x] =
                static_cast<u8>(fetch_texel(sheet, params.tex_x, params.tex_y, x, y));
        }
    }
    return texels;
}

std::array<u32, 16> level_palette(std::span<const u8> luma_ram, std::span<const u32> tone,
                                  u32 colour_components, u32 luma_base, u32 luma)
{
    std::array<u32, 16> palette{};
    for (u32 level = 0; level < 16; ++level) {
        const u32 curve_index = luma_base + ((level << 4) >> 1);
        const u32 curve = curve_index < luma_ram.size() ? luma_ram[curve_index] : 0u;
        const u32 shade = std::min((curve * luma) / 256u, 0x3fu);
        u32 rgba = 0xff000000u;
        for (u32 shift = 0; shift < 24; shift += 8) {
            const usize at = static_cast<usize>((colour_components >> shift) & 0x1fu)
                               * hw::Model2Video::kToneShades
                           + shade;
            if (at < tone.size()) {
                rgba |= tone[at] & (0xffu << shift);
            }
        }
        palette[level] = rgba;
    }
    return palette;
}

const TextureHasher::Entry& TextureHasher::lookup(const hw::Model2MachineBase& machine,
                                                  const PolyParams&            params,
                                                  std::vector<u8>*             texels)
{
    if (m_generation != machine.texture_generation()) {
        m_generation = machine.texture_generation();
        m_regions.clear();
    }

    const u64 sheet  = (params.flags & kFlagSheet) != 0 ? 1u : 0u;
    const u64 region = (sheet << 50) | (static_cast<u64>(params.tex_x) << 38)
                     | (static_cast<u64>(params.tex_y) << 26)
                     | (static_cast<u64>(params.tex_width) << 13) | params.tex_height;

    auto [slot, inserted] = m_regions.try_emplace(region);
    if (!inserted) {
        if (texels != nullptr) {
            *texels = read_texels(machine, params);
        }
        return slot->second;
    }

    std::vector<u8> read = read_texels(machine, params);
    u64 hash = mix(0xcbf29ce484222325ull,
                   (static_cast<u64>(params.tex_width) << 16) | params.tex_height);
    for (usize i = 0; i + 1 < read.size(); i += 2) {
        hash = mix(hash, static_cast<u64>(read[i]) | (static_cast<u64>(read[i + 1]) << 4));
    }
    for (const u8 level : read) {
        ++slot->second.histogram[level];
    }
    slot->second.hash = hash;
    if (texels != nullptr) {
        *texels = std::move(read);
    }
    return slot->second;
}

}  // namespace sm2::render
