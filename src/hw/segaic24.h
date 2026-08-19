// SPDX-License-Identifier: BSD-3-Clause
//
// Sega System 24 tilemap chip (315-5292), as reused on Model 1 and Model 2.
//
// Ported from MAME's src/mame/sega/segaic24.cpp, which is BSD-3-Clause,
// copyright-holders Olivier Galibert. The composition rules, the window mask
// semantics and the four-way wrap splitting are his; this file keeps the same
// structure so the two stay comparable.
#pragma once

#include "core/types.h"

#include <span>
#include <vector>

namespace sm2::hw {

/// Number of tilemap layers: two pairs, each a "screen" and a "window" map.
inline constexpr u32 kTileLayers = 4;

/// One entry of a System 24 name table.
///
/// The tile index and the colour field overlap in the 16-bit word: bits 13:0 are
/// the index and bits 14:7 the colour. That is not a mistake in the decode, it is
/// how the chip is wired, and it means changing a tile's colour also changes
/// which tile it is unless the game keeps the indices low.
struct TileCell {
    u16  tile       = 0;      ///< Character RAM index, 14 bits.
    u8   colour     = 0;      ///< Palette granule of sixteen pens.
    bool foreground = false;  ///< Priority category: drawn after the 3D layer.
};

/// Read one name table entry. `layer` is 0 to 3, `index` is 0 to 4095 in row
/// order across a 64x64 grid.
[[nodiscard]] TileCell decode_tile_cell(std::span<const u8> tile_ram, u32 layer, u32 index);

/// One 4-bit pixel of a character.
///
/// Characters are 8x8, four bits deep, 32 bytes each, packed most significant
/// nibble first. The byte index is exclusive-ORed with one: MAME builds its
/// graphics element over a 16-bit array with a little-endian byte swap, which
/// amounts to swapping the two bytes of every word. Getting this wrong swaps
/// pixels in pairs, which looks almost right and is correspondingly hard to spot.
///
/// This is the readable reference form. The renderer decodes a whole row at a
/// time instead; a unit test holds the two to the same answer.
[[nodiscard]] u8 decode_tile_pixel(std::span<const u8> char_ram, u16 tile, u32 x, u32 y);

/// The System 24 tilemap chip.
///
/// Four 64x64 layers of 8x8 four-bit characters, arranged as two pairs. Within
/// a pair one layer is the "screen" and the other the "window"; an eight-pixel
/// granular mask chooses which of the two is visible at each position, which is
/// how the hardware does split-screen and picture-in-picture without a second
/// tilemap generator.
///
/// Every layer is 512x512 pixels and wraps, so the visible 496x384 region can
/// straddle the wrap in both axes. Drawing therefore decomposes into up to four
/// rectangles.
///
/// The chip caches a decoded 512x512 pixel map per layer and rebuilds only the
/// characters whose name-table entry or pattern data changed. That matters: a
/// full rebuild of all four layers is a million pixel writes, and a game that
/// streams character data would otherwise pay it every frame.
class Segaic24Tile {
public:
    /// Each layer is a 64x64 grid of 8x8 characters.
    static constexpr u32 kMapWidth   = 512;
    static constexpr u32 kMapHeight  = 512;
    static constexpr u32 kCellsWide  = 64;
    static constexpr u32 kCellsHigh  = 64;
    static constexpr u32 kCellCount  = kCellsWide * kCellsHigh;
    static constexpr u32 kLayerCount = 4;

    /// Characters available in the 512 KB character RAM, 32 bytes each.
    static constexpr u32 kCharacterCount = 0x4000;

    /// Model 2 wires all fourteen index bits, so every character is reachable.
    static constexpr u16 kTileMask = 0x3fff;

    /// Visible area, in pixels. The chip generates a wider raster but this is
    /// what the monitor shows and what MAME's tilemap code hard-codes.
    static constexpr u32 kScreenWidth  = 496;
    static constexpr u32 kScreenHeight = 384;

    /// Per-pixel flag bits, matching MAME's tilemap layer flags so the ported
    /// comparison against `tpri` reads the same.
    static constexpr u8 kPixelOpaque   = 0x10;  ///< TILEMAP_PIXEL_LAYER0
    static constexpr u8 kCategoryMask  = 0x0f;

    Segaic24Tile();

    /// Point the chip at the machine's tile and character RAM.
    ///
    /// Both spans must outlive the chip. Model 2 allocates them once during
    /// init and never resizes, so they are stable for the life of the machine.
    void attach(std::span<const u8> tile_ram, std::span<const u8> char_ram);

    /// Discard every cached character. Called on reset and whenever the caller
    /// cannot say precisely what changed.
    void invalidate_all();

    /// A name-table entry changed. `byte_offset` is relative to tile RAM;
    /// offsets at or beyond the name tables (0x8000) are ignored here because
    /// they are scroll and mask registers, which are read fresh every frame.
    void note_tile_write(u32 byte_offset, u32 width);

    /// Character pattern data changed. Every cell that references the affected
    /// character has to be re-decoded, which is resolved lazily in update().
    void note_char_write(u32 byte_offset, u32 width);

    /// Re-decode whatever is stale. Call once per frame before drawing.
    void update();

    /// Draw one layer and category into an RGBA8 surface.
    ///
    /// `layer_and_category` is MAME's packed argument: bits 3:1 select the
    /// tilemap layer 0 to 3, bit 0 selects the priority category. Category 0 is
    /// composited below the 3D output and category 1 above it, which is what
    /// lets a game put a HUD in front of the scene using the same layers.
    ///
    /// `opaque` draws transparent pixels as well and ignores the category,
    /// which is how the second pair becomes the backdrop.
    ///
    /// `pens` holds premultiplied-nothing RGBA8 colours, 0x1000 of them; a
    /// drawn pixel is written with alpha 0xff so the compositor can tell it
    /// apart from a pixel nothing touched.
    void draw(u32                  layer_and_category,
              bool                 opaque,
              std::span<const u32> pens,
              u32*                 dest,
              u32                  dest_stride);

    // -- diagnostics -------------------------------------------------------

    /// Cached pixel map of one layer: palette indices, 512x512, row order.
    [[nodiscard]] std::span<const u16> pixmap(u32 layer) const;

    /// Matching per-pixel flags: kPixelOpaque set for a non-transparent pixel,
    /// low bits the priority category.
    [[nodiscard]] std::span<const u8> flagsmap(u32 layer) const;

private:
    /// Everything a draw needs that does not change between its rectangles.
    struct DrawState {
        std::span<const u32> pens;
        u32*                 dest        = nullptr;
        u32                  dest_stride = 0;
        u32                  mask_base   = 0;   ///< Tile RAM word index of the mask.
        u8                   match       = 0;   ///< Flag byte a drawn pixel must have.
        bool                 opaque      = false;
        bool                 window      = false;  ///< Invert the mask's sense.
        /// Whether the window mask applies at all. It does not in the split
        /// modes, which choose between the pair by position instead.
        bool                 masked      = true;
    };

    /// One 16-bit word of tile RAM, addressed the way the chip sees it.
    [[nodiscard]] u16 tile_word(u32 word_index) const;

    void decode_cell(u32 layer, u32 cell);

    /// Draw part of the screen from one layer, splitting at the map's wrap.
    ///
    /// `h` and `v` are the source origin for the destination rectangle's top left
    /// corner, already reduced to the map's 512 pixels. The rectangle is cut into
    /// as many pieces as the wrap requires, up to four.
    void draw_region(const DrawState& state,
                     u32              layer,
                     int              h,
                     int              v,
                     int              x0,
                     int              y0,
                     int              x1,
                     int              y1);

    /// Copy a rectangle of one layer's pixel map into the destination.
    ///
    /// `sx`/`sy` are the source origin in the 512x512 map and must not wrap within
    /// this call; draw_region is what guarantees that. Ported from
    /// segas24_tile_device::draw_rect, the bitmap_rgb32 overload, which needs no
    /// priority buffer because Model 1 and Model 2 have no sprites to interleave.
    void draw_rect(const DrawState& state,
                   u32              layer,
                   int              sx,
                   int              sy,
                   int              xx1,
                   int              yy1,
                   int              xx2,
                   int              yy2);

    /// The split and window modes, where a pair is divided at a boundary and its
    /// two layers supply opposite sides.
    void draw_split(const DrawState& state, u32 layer, u32 mode, u32 hscr, u32 vscr);

    std::span<const u8> m_tile_ram;
    std::span<const u8> m_char_ram;

    /// Decoded palette indices, one 512x512 map per layer.
    std::vector<u16> m_pixmap;

    /// Per-pixel opacity and category, parallel to m_pixmap.
    std::vector<u8> m_flagsmap;

    /// Cells whose decoded pixels are stale, one byte per cell per layer.
    std::vector<u8> m_cell_dirty;

    /// Characters whose pattern data changed since the last update.
    std::vector<u8> m_char_dirty;

    /// Number of set entries in m_char_dirty, so the common case of "nothing
    /// changed" costs one comparison instead of a scan.
    u32 m_char_dirty_count = 0;

    /// Number of set entries in m_cell_dirty, same reasoning.
    u32 m_cell_dirty_count = 0;
};

}  // namespace sm2::hw
