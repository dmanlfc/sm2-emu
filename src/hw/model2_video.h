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
//
// Model 2 video output: the colour chain and the tilemap composite.
//
// Colour handling follows MAME's src/mame/sega/model2_v.cpp and model2.cpp
// (BSD-3-Clause, copyright-holders R. Belmont, Olivier Galibert, ElSemi,
// Angelo Salese, Matthew Daniels).
#pragma once

#include "core/types.h"
#include "hw/segaic24.h"

#include <span>
#include <vector>

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

    [[nodiscard]] Segaic24Tile& tiles() { return m_tiles; }
    [[nodiscard]] const Segaic24Tile& tiles() const { return m_tiles; }

    /// Rebuild the pen table from palette RAM through the colour translation and
    /// gamma tables. Cheap enough to do whenever the machine reports a change.
    void refresh_pens();

    /// Compose one frame's tilemap layers.
    ///
    /// Produces `below()` and `above()`. Call refresh_pens() first if the
    /// machine's palette changed, and note that this also runs the tile chip's
    /// deferred character decode.
    void compose();

    /// Tilemap pixels that belong behind the 3D output, RGBA8, kWidth by kHeight.
    [[nodiscard]] std::span<const u32> below() const { return m_below; }

    /// Tilemap pixels that belong in front of the 3D output.
    [[nodiscard]] std::span<const u32> above() const { return m_above; }

    /// Colour of palette entry zero, which the hardware shows where nothing else
    /// is drawn.
    [[nodiscard]] u32 background() const { return m_pens.empty() ? 0xff000000u : m_pens[0]; }

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

private:
    /// One component of the tone curve: the translation table entry for this
    /// component value at this shade, through the gamma ramp.
    [[nodiscard]] u8 translate(u32 block_base, u32 component, u32 shade) const;

    /// A 15-bit BGR palette word resolved at a given shade.
    [[nodiscard]] u32 shade_colour(u16 colour, u32 shade) const;

    Segaic24Tile m_tiles;

    std::span<const u16> m_palette_ram;
    std::span<const u16> m_colour_translate;

    /// Converted palette, RGBA8 with alpha 0xff.
    std::vector<u32> m_pens;

    /// Colour space correction. MAME's comment records that a bias of 64 and a
    /// gain of 51 suits vf2, fvipers and schamp; real cabinets were calibrated
    /// per game, so a single curve can only be a good average.
    std::vector<u8> m_gamma;

    std::vector<u32> m_below;
    std::vector<u32> m_above;

    /// MAME's defaults, which correspond to no sync register having been written.
    s16 m_crtc_x_offset = 84;
    s16 m_crtc_y_offset = 130;
};

}  // namespace sm2::hw
