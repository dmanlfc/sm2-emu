//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// Model 2 video output: the colour chain and the tilemap composite.
//
// Colour handling follows MAME's src/mame/sega/model2_v.cpp and model2.cpp
// (BSD-3-Clause, copyright-holders R. Belmont, Olivier Galibert, ElSemi,
// Angelo Salese, Matthew Daniels).
#pragma once

#include "core/types.h"
#include "hw/geometrizer.h"
#include "hw/segaic24.h"

#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// The video output stage: palette conversion and 2D composition.
///
/// Model 2 composites in three passes: the tilemap layers whose priority
/// category is zero, then the 3D output, then the layers whose category is one.
/// That is what lets a game put a status display in front of the scene using the
/// same tilemap hardware that draws the background behind it.
///
/// This class produces the two tilemap results as separate RGBA8 surfaces so the
/// 3D pass can be inserted between them later without disturbing either. A pixel
/// nothing wrote has alpha zero.
class Model2Video {
public:
    static constexpr u32 kWidth  = Segaic24Tile::kScreenWidth;
    static constexpr u32 kHeight = Segaic24Tile::kScreenHeight;

    /// Colour entries the tilemap can reach: 256 colours of 16 pens.
    static constexpr u32 kPenCount = 0x1000;

    Model2Video();

    /// Point the video stage at the machine's memory. The spans must outlive it.
    void attach(std::span<const u8>  tile_ram,
                std::span<const u8>  char_ram,
                std::span<const u16> palette_ram,
                std::span<const u16> colour_translate);

    void reset();

    /// Take the tile RAM the display reads from. The beam scans the map during
    /// the picture, so a game writing through vblank and into the next frame's
    /// first lines still shows whole; call this at the start of vblank.
    /// True when the shown copy changed.
    bool latch_tile_ram();

    /// The tile RAM as last latched.
    [[nodiscard]] std::span<const u8> tile_ram_shown() const { return m_tile_ram_shown; }

    [[nodiscard]] Segaic24Tile& tiles() { return m_tiles; }
    [[nodiscard]] const Segaic24Tile& tiles() const { return m_tiles; }

    /// Rebuild the pen table from palette RAM through the colour translation and
    /// gamma tables. Cheap enough to do whenever the machine reports a change.
    void refresh_pens();

    /// The ramp index that shows as black: 40 by default, fitted to an arcade
    /// capture. Call refresh_pens() after a change.
    static constexpr u32 kDefaultBlackLevel = 40;
    void set_black_level(u32 bias);
    [[nodiscard]] u32 black_level() const { return m_black_level; }
    /// The 256-entry ramp for a black level.
    static void build_gamma(u32 bias, std::span<u8> out);

    /// Model 1: tile pens are the palette's xBGR555 straight, bit 15 clear
    /// halving the entry, with no colour translation.
    void set_direct_palette(bool direct) { m_direct_palette = direct; }

    /// Compose one frame's tilemap layers.
    ///
    /// Produces `below()` and `above()`. Call refresh_pens() first if the
    /// machine's palette changed, and note that this also runs the tile chip's
    /// deferred character decode.
    void compose();

    /// compose() in two halves, so compose_into() can run on another thread
    /// from a copy of the tile RAM. The next prepare_compose() must wait for it.
    void prepare_compose()
    {
        m_tiles.update();
        m_overlay_shown = m_overlay;
    }

    /// Flat polygons drawn over the top tile layers where those are clear or
    /// near black, as Model 1 draws its radar blips. Taken up by the next
    /// prepare_compose(), so a compose on another thread never sees it change.
    void set_overlay(const std::vector<RenderPolygon>& polygons, s32 x_offset)
    {
        m_overlay          = polygons;
        m_overlay_x_offset = x_offset;
    }
    [[nodiscard]] bool has_overlay() const { return !m_overlay.empty(); }
    void compose_into(std::span<const u8> tile_ram, bool mask_a_hidden,
                      std::span<const u32> pens, std::vector<u32>& below,
                      std::vector<u32>& above, std::vector<u8>& front) const;

    /// compose_into() for rows `row_begin` to `row_end` only.
    void compose_rows(std::span<const u8> tile_ram, bool mask_a_hidden,
                      std::span<const u32> pens, u32* below, u32* above, u8* front,
                      u32 row_begin, u32 row_end) const;

    /// Hide the gun games' shot flash. Call after each frame with its IN1 port.
    void filter_gun_flash(bool enabled, u8 in1);

    /// Whether the front tilemap pair's window mask is ignored this frame.
    [[nodiscard]] bool window_mask_a_hidden() const { return m_tiles.window_mask_a_hidden(); }

    /// Draw one framebuffer bank over the layers already in `below()`.
    ///
    /// Only used in render test mode, where the DSP stops drawing and clearing the
    /// framebuffer and the host draws into it directly. Opaque over the whole
    /// raster, as in MAME's draw_framebuffer, so it hides the tilemap pair
    /// underneath it; the category-one layers still go on top.
    ///
    /// `pixels` is one bank, 512 pixels per line, xGGGGGRRRRRBBBBB.
    void draw_framebuffer(std::span<const u16> pixels);

    /// Tilemap pixels that belong behind the 3D output, RGBA8, kWidth by kHeight.
    [[nodiscard]] std::span<const u32> below() const { return m_below; }

    /// Tilemap pixels that belong in front of the 3D output.
    [[nodiscard]] std::span<const u32> above() const { return m_above; }

    /// Exchange the layer buffers with the caller's, in constant time; compose()
    /// rewrites every pixel, so it does not care what it gets back.
    void swap_layers(std::vector<u32>& below, std::vector<u32>& above);

    /// Colour of palette entry zero, which the hardware shows where nothing else
    /// is drawn.
    [[nodiscard]] u32 background() const { return m_pens.empty() ? 0xff000000u : m_pens[0]; }

    /// Wall-clock time the last compose() call took, in nanoseconds -- the
    /// "tilemap composition" profiler stage.
    [[nodiscard]] u64 last_compose_nanoseconds() const { return m_last_compose_ns; }

    /// The converted palette, for diagnostics and for the 3D pass later.
    [[nodiscard]] std::span<const u32> pens() const { return m_pens; }

    /// Dimensions of the tone curve: 32 colour components by 256 shades.
    static constexpr u32 kToneShades     = 256;
    static constexpr u32 kToneComponents = 32;

    /// Flatten the colour translation table and the gamma ramp into one lookup.
    ///
    /// The textured 3D path picks a shade for every texel, so the curve cannot be
    /// evaluated on the host as the tilemaps' and the untextured path's can. This
    /// hands the whole thing to the renderer as an RGBA8 image, shade across and
    /// component down. Each channel reads its own component, so a pixel takes
    /// three lookups from the one image rather than one from three.
    ///
    /// `out` must hold kToneShades * kToneComponents entries.
    void build_tone_curve(std::span<u32> out) const;

    /// The three five-bit colour components of a 3D polygon's base colour, packed
    /// as red, green, blue in successive bytes.
    ///
    /// Components rather than a resolved colour, because the shade that completes
    /// the lookup is a per-texel matter on the textured path and so has to be
    /// chosen in the fragment shader. `colour_base` is the ten-bit index the
    /// polygon's texture header carries, resolved against the 3D half of palette
    /// RAM.
    [[nodiscard]] u32 polygon_colour_components(u32 colour_base) const;

    // -- CRTC ---------------------------------------------------------------
    // The sync registers shift the raster relative to the monitor. The tilemap
    // ignores them; the 3D projection and the framebuffer readback do not.

    void set_horizontal_sync(u16 value);
    void set_vertical_sync(u16 value);

    [[nodiscard]] s16 crtc_x_offset() const { return m_crtc_x_offset; }
    [[nodiscard]] s16 crtc_y_offset() const { return m_crtc_y_offset; }

    /// Save/restore the CRTC offsets. On load the tilemap decode caches and the
    /// pen table are re-derived rather than serialized: the decoded pixmaps come
    /// straight from tile/char RAM (invalidate_all) and the pens from palette
    /// RAM (a palette_dirty bump at the machine level), both of which ARE saved.
    void serialize(Archive& ar);

private:
    /// One component of the tone curve: the translation table entry for this
    /// component value at this shade, through the gamma ramp.
    [[nodiscard]] u8 translate(u32 block_base, u32 component, u32 shade) const;

    /// A 15-bit palette word resolved at a given shade, returned as RGBA8.
    ///
    /// The word is xRRRRRGGGGGBBBBB: bits 0-4 are red, 5-9 green, 10-14 blue,
    /// matching MAME's model2_state::palette_w. (An earlier comment here called
    /// it BGR, which sent a debugging session looking for a red/blue swap that
    /// does not exist — the channel order is correct throughout.)
    [[nodiscard]] u32 shade_colour(u16 colour, u32 shade) const;

    Segaic24Tile m_tiles;

    std::span<const u8> m_tile_ram_live;
    std::vector<u8>     m_tile_ram_shown;

    u8   m_triggers_held      = 0;
    u8   m_frames_since_press = 0xff;
    bool m_window_mask_a_full = false;

    std::span<const u16> m_palette_ram;
    std::span<const u16> m_colour_translate;

    /// Converted palette, RGBA8 with alpha 0xff.
    void draw_overlay(u32* above, u32 row_begin, u32 row_end) const;

    std::vector<RenderPolygon> m_overlay;
    std::vector<RenderPolygon> m_overlay_shown;
    s32                        m_overlay_x_offset = 0;

    std::vector<u32> m_pens;
    bool             m_direct_palette = false;

    /// Colour space correction. MAME's comment records that a bias of 64 and a
    /// gain of 51 suits vf2, fvipers and schamp; real cabinets were calibrated
    /// per game, so a single curve can only be a good average.
    std::vector<u8> m_gamma;
    u32             m_black_level = kDefaultBlackLevel;

    std::vector<u32> m_below;
    std::vector<u32> m_above;
    /// Per pixel, whether the topmost tile layer drew it in front of the 3D.
    std::vector<u8> m_front;

    /// MAME's defaults, which correspond to no sync register having been written.
    s16 m_crtc_x_offset = 84;
    s16 m_crtc_y_offset = 130;

    u64 m_last_compose_ns = 0;
};

}  // namespace sm2::hw
