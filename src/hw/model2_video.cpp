// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from MAME's src/mame/sega/model2_v.cpp and model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels).

#include "hw/model2_video.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>

namespace sm2::hw {
namespace {

/// Where each colour component's block starts in the translation table, as word
/// indices.
///
/// The table is three blocks of 32 components by 256 shades. Within a block the
/// component selects a curve and the shade indexes along it.
constexpr u32 kRedBlockBase   = 0x0000 >> 1;
constexpr u32 kGreenBlockBase = 0x4000 >> 1;
constexpr u32 kBlueBlockBase  = 0x8000 >> 1;

/// Stride between adjacent components within a block.
constexpr u32 kComponentStride = 0x100;

/// The shade flat 2D output always uses. The 3D pipeline picks its own from the
/// polygon's luminance, which is six bits, so the shades below this one belong to
/// 3D and this one sits just above them. It is also where the gamma curve has its
/// bias.
constexpr u32 kFlatShade = 0x40;

/// Palette RAM is split: the 3D pipeline's colour bases are resolved against the
/// second half.
constexpr u32 kPolygonPaletteBase = 0x1000;

[[nodiscard]] inline u32 pack_rgba(u8 red, u8 green, u8 blue)
{
    // Little-endian VK_FORMAT_R8G8B8A8_UNORM: byte 0 is red.
    return static_cast<u32>(red) | (static_cast<u32>(green) << 8)
         | (static_cast<u32>(blue) << 16) | 0xff000000u;
}

}  // namespace

Model2Video::Model2Video()
    : m_pens(kPenCount, 0xff000000u)
    , m_gamma(256, 0)
    , m_below(static_cast<usize>(kWidth) * kHeight, 0)
    , m_above(static_cast<usize>(kWidth) * kHeight, 0)
{
    // Bias 64, gain 51: the monitor showed nothing below a quarter scale and
    // reached full white early. Without this the whole image is washed out.
    for (u32 index = 0; index < 256; ++index) {
        const double raw =
            std::max((static_cast<double>(index) - 64.0) * 255.0 / 191.0, 0.0);
        m_gamma[index] = static_cast<u8>(raw);
    }
}

void Model2Video::attach(std::span<const u8>  tile_ram,
                         std::span<const u8>  char_ram,
                         std::span<const u16> palette_ram,
                         std::span<const u16> colour_translate)
{
    m_tiles.attach(tile_ram, char_ram);
    m_palette_ram      = palette_ram;
    m_colour_translate = colour_translate;
    refresh_pens();
}

void Model2Video::reset()
{
    m_tiles.invalidate_all();
    std::fill(m_below.begin(), m_below.end(), 0u);
    std::fill(m_above.begin(), m_above.end(), 0u);
    m_crtc_x_offset = 84;
    m_crtc_y_offset = 130;
    refresh_pens();
}

void Model2Video::refresh_pens()
{
    if (m_palette_ram.empty() || m_colour_translate.empty()) {
        return;
    }

    const u32 count = static_cast<u32>(std::min<usize>(kPenCount, m_palette_ram.size()));
    for (u32 entry = 0; entry < count; ++entry) {
        m_pens[entry] = shade_colour(m_palette_ram[entry], kFlatShade);
    }
}

u8 Model2Video::translate(u32 block_base, u32 component, u32 shade) const
{
    const u32 index = block_base + component * kComponentStride + shade;
    if (index >= m_colour_translate.size()) {
        return 0;
    }
    // MAME assigns the 16-bit table entry to a u8, so only the low byte reaches
    // the gamma curve.
    return m_gamma[m_colour_translate[index] & 0xff];
}

u32 Model2Video::shade_colour(u16 colour, u32 shade) const
{
    const u8 red   = translate(kRedBlockBase, (colour >> 0) & 0x1f, shade);
    const u8 green = translate(kGreenBlockBase, (colour >> 5) & 0x1f, shade);
    const u8 blue  = translate(kBlueBlockBase, (colour >> 10) & 0x1f, shade);
    return pack_rgba(red, green, blue);
}

u32 Model2Video::polygon_colour_components(u32 colour_base) const
{
    const u32 entry = kPolygonPaletteBase + (colour_base & 0x3ff);
    if (entry >= m_palette_ram.size()) {
        return 0;
    }
    const u16 colour = m_palette_ram[entry];
    return static_cast<u32>((colour >> 0) & 0x1f)
         | (static_cast<u32>((colour >> 5) & 0x1f) << 8)
         | (static_cast<u32>((colour >> 10) & 0x1f) << 16);
}

void Model2Video::build_tone_curve(std::span<u32> out) const
{
    const usize needed = static_cast<usize>(kToneShades) * kToneComponents;
    if (out.size() < needed) {
        return;
    }
    for (u32 component = 0; component < kToneComponents; ++component) {
        for (u32 shade = 0; shade < kToneShades; ++shade) {
            out[static_cast<usize>(component) * kToneShades + shade] =
                pack_rgba(translate(kRedBlockBase, component, shade),
                          translate(kGreenBlockBase, component, shade),
                          translate(kBlueBlockBase, component, shade));
        }
    }
}

void Model2Video::compose()
{
    m_tiles.update();

    std::fill(m_below.begin(), m_below.end(), 0u);
    std::fill(m_above.begin(), m_above.end(), 0u);

    // Composition order, from MAME's model2_state::screen_update.
    //
    // Pair B, the second pair, is drawn opaque: it is the backdrop, so its
    // transparent pen is written as a colour and its priority category is
    // ignored. Pair A is then drawn over it with the transparent pen honoured.
    // Layers descend so that layer 0 ends up on top within each pair.
    for (u32 layer = kTileLayers; layer-- > 2;) {
        m_tiles.draw(layer << 1, /*opaque=*/true, m_pens, m_below.data(), kWidth);
    }
    for (u32 layer = 2; layer-- > 0;) {
        m_tiles.draw(layer << 1, /*opaque=*/false, m_pens, m_below.data(), kWidth);
    }

    // Everything with category one, in front of the 3D output. Pair B takes part
    // here too, and because it was already drawn opaque below, a game that marks
    // its backdrop as category one ends up covering the scene. MAME behaves the
    // same way and records it as an open problem for one of the light gun games.
    for (u32 layer = kTileLayers; layer-- > 0;) {
        m_tiles.draw((layer << 1) | 1, /*opaque=*/false, m_pens, m_above.data(), kWidth);
    }
}

void Model2Video::set_horizontal_sync(u16 value)
{
    m_crtc_x_offset = static_cast<s16>(84 + static_cast<s16>(value));
    SM2_TRACE("model2: horizontal sync %04x -> x offset %d", value, m_crtc_x_offset);
}

void Model2Video::set_vertical_sync(u16 value)
{
    m_crtc_y_offset = static_cast<s16>(130 + static_cast<s16>(value));
    SM2_TRACE("model2: vertical sync %04x -> y offset %d", value, m_crtc_y_offset);
}

}  // namespace sm2::hw
