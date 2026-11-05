// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Ported from MAME's src/mame/sega/model2_v.cpp and model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels).

#include "hw/model2_video.h"

#include "core/archive.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
    , m_front(static_cast<usize>(kWidth) * kHeight, 0)
{
    build_gamma(m_black_level, m_gamma);
}

void Model2Video::build_gamma(u32 bias, std::span<u8> out)
{
    const double black = static_cast<double>(std::min<u32>(bias, 128));
    for (usize index = 0; index < out.size(); ++index) {
        out[index] = static_cast<u8>(
            std::max((static_cast<double>(index) - black) * 255.0 / (255.0 - black), 0.0));
    }
}

void Model2Video::set_black_level(u32 bias)
{
    m_black_level = bias;
    build_gamma(bias, m_gamma);
}

void Model2Video::attach(std::span<const u8>  tile_ram,
                         std::span<const u8>  char_ram,
                         std::span<const u16> palette_ram,
                         std::span<const u16> colour_translate)
{
    m_tile_ram_live = tile_ram;
    m_tile_ram_shown.assign(tile_ram.begin(), tile_ram.end());
    m_tiles.attach(m_tile_ram_shown, char_ram);
    m_palette_ram      = palette_ram;
    m_colour_translate = colour_translate;
    refresh_pens();
}

bool Model2Video::latch_tile_ram()
{
    bool        changed = false;
    const usize size = std::min(m_tile_ram_live.size(), m_tile_ram_shown.size());
    for (usize at = 0; at + 8 <= size; at += 8) {
        u64 live, shown;
        std::memcpy(&live, m_tile_ram_live.data() + at, 8);
        std::memcpy(&shown, m_tile_ram_shown.data() + at, 8);
        if (live == shown) {
            continue;
        }
        for (usize word = at; word < at + 8; word += 2) {
            if (std::memcmp(m_tile_ram_live.data() + word, m_tile_ram_shown.data() + word, 2) != 0) {
                m_tiles.note_tile_write(static_cast<u32>(word), 2);
            }
        }
        std::memcpy(m_tile_ram_shown.data() + at, &live, 8);
        changed = true;
    }
    return changed;
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

void Model2Video::serialize(Archive& ar)
{
    ar.raw(m_crtc_x_offset);
    ar.raw(m_crtc_y_offset);
    if (ar.loading() && !ar.failed()) {
        // The decode caches are re-derived from the (restored) tile/char RAM;
        // the pens from the (restored) palette RAM.
        m_tiles.invalidate_all();
        refresh_pens();
    }
}

void Model2Video::refresh_pens()
{
    if (m_direct_palette) {
        const u32 count = static_cast<u32>(std::min<usize>(kPenCount, m_palette_ram.size()));
        for (u32 entry = 0; entry < count; ++entry) {
            const u16 colour = m_palette_ram[entry];
            const u32 shift  = (colour & 0x8000) != 0 ? 0 : 1;
            const auto level = [shift](u32 v) { return static_cast<u8>(((v << 3) | (v >> 2)) >> shift); };
            m_pens[entry] = pack_rgba(level(colour & 0x1f), level((colour >> 5) & 0x1f),
                                      level((colour >> 10) & 0x1f));
        }
        return;
    }
    if (m_palette_ram.empty() || m_colour_translate.empty()) {
        return;
    }

    u8 red[32], green[32], blue[32];
    for (u32 level = 0; level < 32; ++level) {
        red[level]   = translate(kRedBlockBase, level, kFlatShade);
        green[level] = translate(kGreenBlockBase, level, kFlatShade);
        blue[level]  = translate(kBlueBlockBase, level, kFlatShade);
    }
    const u32 count = static_cast<u32>(std::min<usize>(kPenCount, m_palette_ram.size()));
    for (u32 entry = 0; entry < count; ++entry) {
        const u16 colour = m_palette_ram[entry];
        m_pens[entry] = pack_rgba(red[colour & 0x1f], green[(colour >> 5) & 0x1f],
                                  blue[(colour >> 10) & 0x1f]);
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

void Model2Video::filter_gun_flash(bool enabled, u8 in1)
{
    // Virtua Cop's flash lands up to two frames after the press.
    constexpr u8 kFlashLatency = 2;

    const u8   held    = static_cast<u8>(~in1 & 0x03);
    const bool full    = m_tiles.window_mask_a_full();
    if ((held & ~m_triggers_held) != 0) {
        m_frames_since_press = 0;
    } else if (m_frames_since_press <= kFlashLatency) {
        ++m_frames_since_press;
    }

    // A flash shows the front tilemap pair's window over the whole screen for one
    // frame after a shot. Fades hold the same mask, so one already up is left alone.
    m_tiles.set_window_mask_a_hidden(enabled && full && !m_window_mask_a_full
                                     && m_frames_since_press <= kFlashLatency);

    m_triggers_held      = held;
    m_window_mask_a_full = full;
}

void Model2Video::swap_layers(std::vector<u32>& below, std::vector<u32>& above)
{
    below.resize(m_below.size());
    above.resize(m_above.size());
    m_below.swap(below);
    m_above.swap(above);
}

void Model2Video::compose()
{
    const auto start = std::chrono::steady_clock::now();

    prepare_compose();
    compose_into(m_tile_ram_shown, m_tiles.window_mask_a_hidden(), m_pens, m_below, m_above,
                 m_front);

    m_last_compose_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

void Model2Video::compose_into(std::span<const u8> tile_ram, bool mask_a_hidden,
                               std::span<const u32> pens, std::vector<u32>& below,
                               std::vector<u32>& above, std::vector<u8>& front) const
{
    const usize pixels = static_cast<usize>(kWidth) * kHeight;
    below.resize(pixels);
    above.resize(pixels);
    front.resize(pixels);
    compose_rows(tile_ram, mask_a_hidden, pens, below.data(), above.data(), front.data(), 0,
                 kHeight);
}

void Model2Video::compose_rows(std::span<const u8> tile_ram, bool mask_a_hidden,
                               std::span<const u32> pens, u32* below, u32* above, u8* front,
                               u32 row_begin, u32 row_end) const
{
    const usize first = static_cast<usize>(row_begin) * kWidth;
    const usize last  = static_cast<usize>(row_end) * kWidth;
    std::fill(below + first, below + last, 0u);
    std::fill(above + first, above + last, 0u);
    std::fill(front + first, front + last, u8{0});

    // The layers are mixed among themselves first, back to front, so layer 0
    // ends up on top whatever the categories. Pair B is drawn opaque: it is the
    // backdrop. The category of the pixel that ends up on top then puts it
    // behind or in front of the 3D. The vcop2 stage select needs this: pair B
    // there is category one, yet "SHOOT TO SELECT" on pair A stays over it.
    const int begin = static_cast<int>(row_begin);
    const int end   = static_cast<int>(row_end);
    for (u32 layer = kTileLayers; layer-- > 0;) {
        m_tiles.draw(tile_ram, mask_a_hidden, layer << 1, /*opaque=*/layer >= 2, pens, below,
                     kWidth, front, 0, begin, end);
        m_tiles.draw(tile_ram, mask_a_hidden, (layer << 1) | 1, /*opaque=*/false, pens, below,
                     kWidth, front, 1, begin, end);
    }
    for (usize i = first; i < last; ++i) {
        if (front[i] != 0) {
            above[i] = below[i];
            below[i] = 0;
        }
    }
    if (!m_overlay_shown.empty()) {
        draw_overlay(above, row_begin, row_end);
    }
}

void Model2Video::draw_overlay(u32* above, u32 row_begin, u32 row_end) const
{
    // Pixel centres, convex polygons, scissored like the 3D. A HUD pixel with
    // any channel above this stays on top.
    constexpr u32 kFeatureLevel = 8;
    for (const RenderPolygon& poly : m_overlay_shown) {
        if (poly.num_vertices < 3 || (poly.direct_rgb & 0x80000000u) == 0) continue;
        const u32 rgb    = poly.direct_rgb;
        const u32 colour = pack_rgba(static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb));
        const bool checker = (poly.texheader[0] & 0x8000) != 0;
        const s32 x0 = std::max<s32>(0, poly.scissor[0] - m_overlay_x_offset);
        const s32 x1 = std::min<s32>(static_cast<s32>(kWidth), poly.scissor[2] - m_overlay_x_offset);
        const s32 y0 = std::max<s32>(static_cast<s32>(row_begin), poly.scissor[1]);
        const s32 y1 = std::min<s32>(static_cast<s32>(row_end), poly.scissor[3]);
        for (s32 y = y0; y < y1; ++y) {
            const float cy = static_cast<float>(y) + 0.5F;
            float       left = 1e9F, right = -1e9F;
            for (u32 i = 0; i < poly.num_vertices; ++i) {
                const PolyVertex& a = poly.v[i];
                const PolyVertex& b = poly.v[(i + 1) % poly.num_vertices];
                if ((a.y <= cy) == (b.y <= cy)) continue;
                const float x = a.x + (cy - a.y) * (b.x - a.x) / (b.y - a.y) - static_cast<float>(m_overlay_x_offset);
                left  = std::min(left, x);
                right = std::max(right, x);
            }
            const s32 start = std::max(x0, static_cast<s32>(std::ceil(left - 0.5F)));
            const s32 stop  = std::min(x1, static_cast<s32>(std::ceil(right - 0.5F)));
            u32* row = above + static_cast<usize>(y) * kWidth;
            for (s32 x = start; x < stop; ++x) {
                if (checker && ((x ^ y) & 1) == 0) continue;
                const u32 hud = row[x];
                const bool feature = (hud >> 24) != 0
                                  && ((hud & 0xff) > kFeatureLevel || ((hud >> 8) & 0xff) > kFeatureLevel
                                      || ((hud >> 16) & 0xff) > kFeatureLevel);
                if (!feature) row[x] = colour;
            }
        }
    }
}

void Model2Video::draw_framebuffer(std::span<const u16> pixels)
{
    if (pixels.empty()) {
        return;
    }

    // Derived from MAME's model2_state::draw_framebuffer. The framebuffer is 512
    // pixels wide and the visible raster is a window into it whose position is the
    // inverse of the CRTC offsets that move the 3D projection.
    //
    // The source index is deliberately one linear offset rather than a clamped
    // (x, y) pair: the horizontal offset is negative, so the leftmost columns of
    // each raster line come from the end of the line above it in the buffer. That
    // wrap is what the hardware reads and what MAME reproduces, and clamping the
    // two axes separately blanks those columns instead.
    constexpr s32 kFramebufferWidth  = 512;
    constexpr s32 kFramebufferHeight = 512;
    const s32     x_offset           = -m_crtc_x_offset;
    const s32     y_offset           = (kFramebufferHeight - static_cast<s32>(kHeight))
                            - m_crtc_y_offset;

    // MAME's loops stop one short of the clip rectangle's last row and column, so
    // the bottom row and right column keep the tilemap pixel underneath. Matched
    // here because the target is its output, not its intent.
    const u32 last_x = kWidth - 1;
    const u32 last_y = kHeight - 1;

    // Note the component order: unlike palette RAM, which is xBBBBBGGGGGRRRRR,
    // the framebuffer is xGGGGGRRRRRBBBBB. It is still resolved through the scroll
    // colour table, so a framebuffer pixel and a tilemap pixel of the same colour
    // come out the same.
    for (u32 y = 0; y < last_y; ++y) {
        const s32 row = (static_cast<s32>(y) + y_offset) * kFramebufferWidth;
        for (u32 x = 0; x < last_x; ++x) {
            const s32 index = (static_cast<s32>(x) + x_offset) + row;
            const u16 value = (index >= 0 && static_cast<usize>(index) < pixels.size())
                                ? pixels[static_cast<usize>(index)]
                                : u16{0};

            const u8 red   = translate(kRedBlockBase, (value >> 5) & 0x1f, kFlatShade);
            const u8 green = translate(kGreenBlockBase, (value >> 10) & 0x1f, kFlatShade);
            const u8 blue  = translate(kBlueBlockBase, (value >> 0) & 0x1f, kFlatShade);

            m_below[static_cast<usize>(y) * kWidth + x] = pack_rgba(red, green, blue);
        }
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
