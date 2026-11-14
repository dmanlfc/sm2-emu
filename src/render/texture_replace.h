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
// Custom textures: full-colour images that stand in for a game's 4-bit
// textures, matched by the content hash in their file name and packed into
// atlas layers the GPU backends sample in place of the hardware texel path.
#pragma once

#include "core/types.h"
#include "render/geometry.h"
#include "render/texture_key.h"

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
struct RenderPolygon;
}  // namespace sm2::hw

namespace sm2::render {

/// Side of one square atlas layer. polygon.frag carries the same constant.
constexpr u32 kReplacementLayerSize = 4096;

/// Transform from a texture's stored layout to the upright one a dump shows:
/// bit 0 transposes, bit 1 then flips x, bit 2 then flips y.
[[nodiscard]] const char* orientation_tag(u32 orientation);

/// `tex_<hash>_<colour base><tone table><lighting>[_<orientation>].png`: the
/// texture, and the colouring its dumped image was drawn in, which is what a
/// replacement's tint for other colourings is measured against.
[[nodiscard]] std::string replacement_file_name(u64 hash, u32 colour_base, u32 luma_table,
                                                u32 luma, u32 orientation);

/// `tex_<hash>.png`: a Model 3 texture, already in full colour.
[[nodiscard]] std::string replacement_file_name(u64 hash);

class TextureReplacements {
public:
    /// Where a replacement sits in the atlas.
    struct Placement {
        u32 layer   = 0;
        u32 x       = 0;
        u32 y       = 0;
        u32 width   = 0;
        u32 height  = 0;
        u32 max_lod = 0;
    };

    /// Load every replacement under `directory`, recursively, replacing
    /// whatever was loaded before. Returns how many were loaded.
    usize load(const std::string& directory);

    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] usize size() const { return m_entries.size(); }

    /// Bumped by every load(), so a backend knows to upload the atlas again.
    [[nodiscard]] u64 generation() const { return m_generation; }

    [[nodiscard]] u32 layer_count() const { return static_cast<u32>(m_layers.size()); }
    [[nodiscard]] static u32 mip_levels();

    /// Level 0 of one layer, RGBA8, kReplacementLayerSize square. Empty once
    /// release_pixels() has run.
    [[nodiscard]] const std::vector<u8>& layer_pixels(u32 layer) const { return m_layers[layer]; }

    /// Drop the CPU copy once a backend holds the atlas.
    void release_pixels();

    /// Refresh what the frame's tints are measured with. Call before resolve().
    void begin_frame(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// The replacement for a Model 3 texture hash, if any.
    [[nodiscard]] bool find(u64 hash, Placement* out) const;

    /// Point `params` at a replacement when there is one for this polygon.
    void resolve(const hw::Model2MachineBase& machine, const hw::Model2Video& video,
                 const hw::RenderPolygon& poly, PolyParams* params);

private:
    struct Entry {
        u64 hash        = 0;
        u32 colour_base = 0;
        u32 luma_table  = 0;
        u32 luma        = 0;
        u32 layer       = 0;
        u32 x           = 0;
        u32 y           = 0;
        u32 width       = 0;
        u32 height      = 0;
        u32 max_lod     = 0;
        u64 palette_frame = 0;
        std::array<u32, 16> reference{};
    };

    std::vector<Entry>                        m_entries;
    std::unordered_map<u64, std::vector<u32>> m_by_hash;
    std::vector<std::vector<u8>>              m_layers;
    u64                                       m_generation = 0;
    u64                                       m_frame      = 0;
    TextureHasher                             m_hasher;
    std::vector<u32>                          m_tone;
    std::span<const u8>                       m_luma;
};

}  // namespace sm2::render
