// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from MAME's src/mame/sega/segaic24.cpp (BSD-3-Clause, copyright-holders
// Olivier Galibert), together with the parts of src/emu/tilemap.cpp that decide
// what a decoded tile pixel and its flag byte contain.
//
// The character format is MAME's char_layout:
//
//   8x8, 0x4000 tiles, 4 bits per pixel, planes {0,1,2,3},
//   x offsets STEP8(0,4), y offsets STEP8(0,32), 32 bytes per tile
//
// which is eight rows of four bytes, two pixels per byte, most significant
// nibble first. The byte index is exclusive-ORed with one because MAME builds the
// graphics element over a 16-bit array with a little-endian byte swap. Omitting
// that swaps pixels in pairs, which looks almost right.

#include "hw/segaic24.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

namespace sm2::hw {
namespace {

/// Byte offset of each layer's name table. MAME indexes tile RAM as 16-bit
/// words at 0x0000, 0x1000, 0x2000 and 0x3000.
constexpr std::array<u32, kTileLayers> kNameTableBytes = {0x0000, 0x2000, 0x4000, 0x6000};

/// First tile RAM word that is a register rather than a name table entry.
constexpr u32 kNameTableEndBytes = 0x8000;

/// Tile RAM word indices of the scroll and mask registers.
constexpr u32 kRowScrollTable = 0x4000;  ///< +0x200 per layer, one word per line
constexpr u32 kHorizontalScroll = 0x5000;  ///< +1 per layer, bit 15 enables rowscroll
constexpr u32 kVerticalScroll   = 0x5004;  ///< +1 per layer, bit 15 disables the layer
constexpr u32 kWindowMaskA      = 0x6000;  ///< four words per line, 64 flags of 8 px
constexpr u32 kWindowMaskB      = 0x6800;

}  // namespace

// ---------------------------------------------------------------------------
// Format decode. Shared with the diagnostics in model2_debug.
// ---------------------------------------------------------------------------

TileCell decode_tile_cell(std::span<const u8> tile_ram, u32 layer, u32 index)
{
    if (layer >= kTileLayers) {
        return {};
    }
    const u32 offset = kNameTableBytes[layer] + index * 2;
    if (offset + 1 >= tile_ram.size()) {
        return {};
    }
    const u16 value = static_cast<u16>(tile_ram[offset] | (tile_ram[offset + 1] << 8));

    TileCell cell;
    cell.tile       = value & Segaic24Tile::kTileMask;
    cell.colour     = static_cast<u8>((value >> 7) & 0xff);
    cell.foreground = (value & 0x8000) != 0;
    return cell;
}

u8 decode_tile_pixel(std::span<const u8> char_ram, u16 tile, u32 x, u32 y)
{
    const u32 index = tile * 32 + ((y * 4 + (x >> 1)) ^ 1);
    if (index >= char_ram.size()) {
        return 0;
    }
    const u8 byte = char_ram[index];
    return ((x & 1) == 0) ? static_cast<u8>(byte >> 4) : static_cast<u8>(byte & 0x0f);
}

// ---------------------------------------------------------------------------
// Construction and cache management
// ---------------------------------------------------------------------------

Segaic24Tile::Segaic24Tile()
    : m_pixmap(static_cast<usize>(kLayerCount) * kMapWidth * kMapHeight, 0)
    , m_flagsmap(static_cast<usize>(kLayerCount) * kMapWidth * kMapHeight, 0)
    , m_cell_dirty(static_cast<usize>(kLayerCount) * kCellCount, 1)
    , m_char_dirty(kCharacterCount, 0)
    , m_cell_dirty_count(kLayerCount * kCellCount)
{
}

void Segaic24Tile::attach(std::span<const u8> tile_ram, std::span<const u8> char_ram)
{
    m_tile_ram = tile_ram;
    m_char_ram = char_ram;
    invalidate_all();
}

void Segaic24Tile::invalidate_all()
{
    std::fill(m_cell_dirty.begin(), m_cell_dirty.end(), u8{1});
    m_cell_dirty_count = kLayerCount * kCellCount;
    std::fill(m_char_dirty.begin(), m_char_dirty.end(), u8{0});
    m_char_dirty_count = 0;
}

void Segaic24Tile::note_tile_write(u32 byte_offset, u32 width)
{
    if (byte_offset >= kNameTableEndBytes) {
        // Scroll and mask registers. Read fresh every frame, so nothing to
        // invalidate.
        return;
    }
    // A 32-bit write covers two adjacent entries, and an unaligned one can
    // straddle a boundary, so work in whole words across the touched range.
    const u32 first = byte_offset / 2;
    const u32 last  = (byte_offset + width - 1) / 2;

    for (u32 word = first; word <= last; ++word) {
        const u32 layer = word / kCellCount;
        const u32 cell  = word % kCellCount;
        if (layer >= kLayerCount) {
            continue;
        }
        u8& flag = m_cell_dirty[static_cast<usize>(layer) * kCellCount + cell];
        if (flag == 0) {
            flag = 1;
            ++m_cell_dirty_count;
        }
    }
}

void Segaic24Tile::note_char_write(u32 byte_offset, u32 width)
{
    const u32 first = byte_offset / 32;
    const u32 last  = (byte_offset + width - 1) / 32;

    for (u32 character = first; character <= last && character < kCharacterCount;
         ++character) {
        if (m_char_dirty[character] == 0) {
            m_char_dirty[character] = 1;
            ++m_char_dirty_count;
        }
    }
}

void Segaic24Tile::update()
{
    if (m_tile_ram.empty() || m_char_ram.empty()) {
        return;
    }

    // Resolve character changes into cell changes. MAME does this lazily through
    // the graphics element's dirty sequence number; the effect is the same, and
    // scanning 16384 name table entries is far cheaper than re-decoding them.
    if (m_char_dirty_count != 0) {
        for (u32 layer = 0; layer < kLayerCount; ++layer) {
            for (u32 cell = 0; cell < kCellCount; ++cell) {
                const usize slot = static_cast<usize>(layer) * kCellCount + cell;
                if (m_cell_dirty[slot] != 0) {
                    continue;
                }
                const TileCell info = decode_tile_cell(m_tile_ram, layer, cell);
                if (m_char_dirty[info.tile] != 0) {
                    m_cell_dirty[slot] = 1;
                    ++m_cell_dirty_count;
                }
            }
        }
        std::fill(m_char_dirty.begin(), m_char_dirty.end(), u8{0});
        m_char_dirty_count = 0;
    }

    if (m_cell_dirty_count == 0) {
        return;
    }

    for (u32 layer = 0; layer < kLayerCount; ++layer) {
        for (u32 cell = 0; cell < kCellCount; ++cell) {
            u8& flag = m_cell_dirty[static_cast<usize>(layer) * kCellCount + cell];
            if (flag == 0) {
                continue;
            }
            decode_cell(layer, cell);
            flag = 0;
            --m_cell_dirty_count;
        }
    }
    assert(m_cell_dirty_count == 0);
}

void Segaic24Tile::decode_cell(u32 layer, u32 cell)
{
    const TileCell info = decode_tile_cell(m_tile_ram, layer, cell);

    // Sixteen pens per colour, so the palette base is the colour field scaled by
    // the granularity of a four-bit character.
    const u16 palette_base = static_cast<u16>(info.colour) * 16;
    const u8  category     = info.foreground ? u8{1} : u8{0};

    const u32   origin_x   = (cell % kCellsWide) * 8;
    const u32   origin_y   = (cell / kCellsWide) * 8;
    const usize layer_base = static_cast<usize>(layer) * kMapWidth * kMapHeight;

    const u32 character_base = static_cast<u32>(info.tile) * 32;
    if (character_base + 32 > m_char_ram.size()) {
        return;
    }

    for (u32 ty = 0; ty < 8; ++ty) {
        const u8* row = m_char_ram.data() + character_base + ty * 4;
        usize     out = layer_base + static_cast<usize>(origin_y + ty) * kMapWidth + origin_x;

        // Byte order 1, 0, 3, 2 across the row: the pattern index is XORed with
        // one, so each pair of bytes is visited swapped.
        for (u32 pair = 0; pair < 4; ++pair) {
            const u8 byte = row[pair ^ 1u];
            const u8 high = static_cast<u8>(byte >> 4);
            const u8 low  = static_cast<u8>(byte & 0x0f);

            m_pixmap[out]       = static_cast<u16>(palette_base + high);
            m_flagsmap[out]     = static_cast<u8>((high != 0 ? kPixelOpaque : 0) | category);
            m_pixmap[out + 1]   = static_cast<u16>(palette_base + low);
            m_flagsmap[out + 1] = static_cast<u8>((low != 0 ? kPixelOpaque : 0) | category);
            out += 2;
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

u16 Segaic24Tile::tile_word(u32 word_index) const
{
    const u32 offset = word_index * 2;
    if (offset + 1 >= m_tile_ram.size()) {
        return 0;
    }
    return static_cast<u16>(m_tile_ram[offset] | (m_tile_ram[offset + 1] << 8));
}

std::span<const u16> Segaic24Tile::pixmap(u32 layer) const
{
    if (layer >= kLayerCount) {
        return {};
    }
    return std::span<const u16>(m_pixmap.data() + static_cast<usize>(layer) * kMapWidth * kMapHeight,
                                static_cast<usize>(kMapWidth) * kMapHeight);
}

std::span<const u8> Segaic24Tile::flagsmap(u32 layer) const
{
    if (layer >= kLayerCount) {
        return {};
    }
    return std::span<const u8>(m_flagsmap.data() + static_cast<usize>(layer) * kMapWidth * kMapHeight,
                               static_cast<usize>(kMapWidth) * kMapHeight);
}

void Segaic24Tile::draw(u32                  layer_and_category,
                        bool                 opaque,
                        std::span<const u32> pens,
                        u32*                 dest,
                        u32                  dest_stride)
{
    if (m_tile_ram.empty() || dest == nullptr || pens.size() < 0x1000) {
        return;
    }

    const u32 packed = layer_and_category;
    const u32 layer  = (packed >> 1) & 3;

    const u32 hscr = tile_word(kHorizontalScroll + layer);
    const u32 vscr = tile_word(kVerticalScroll + layer);
    const u32 ctrl = tile_word(kVerticalScroll + (layer & 2));

    // Bit 15 of the vertical scroll register blanks the layer outright.
    if ((vscr & 0x8000) != 0) {
        return;
    }

    DrawState state;
    state.pens        = pens;
    state.dest        = dest;
    state.dest_stride = dest_stride;
    // Pair A and pair B have separate window masks.
    state.mask_base = (packed & 4) != 0 ? kWindowMaskB : kWindowMaskA;
    // A pixel is drawn when its flag byte is exactly "opaque, this category".
    state.match  = static_cast<u8>((packed & 1) | kPixelOpaque);
    state.opaque = opaque;
    // Within a pair the odd layer is the window, whose visibility is the
    // complement of the mask.
    state.window = (layer & 1) != 0;
    state.masked = true;

    if ((ctrl & 0x6000) != 0) {
        // A split mode. The pair's two layers supply opposite sides of a boundary,
        // so the odd layer takes no pass of its own: the even one draws both.
        if ((layer & 1) != 0) {
            return;
        }
        // Position decides which layer is visible here, not the mask.
        state.masked = false;
        draw_split(state, layer, (ctrl & 0x6000) >> 13, hscr, vscr);
        return;
    }

    const int screen_width  = static_cast<int>(kScreenWidth);
    const int screen_height = static_cast<int>(kScreenHeight);
    const int v             = static_cast<int>(vscr & 0x1ff);

    if ((hscr & 0x8000) != 0) {
        // Per-line horizontal scroll. One word per raster line, and the vertical
        // scroll advances with the line so the layer is sampled row by row.
        const u32 table = kRowScrollTable + 0x200 * layer;
        for (int y = 0; y < screen_height; ++y) {
            const int h = static_cast<int>(
                (0u - static_cast<u32>(tile_word(table + static_cast<u32>(y)))) & 0x1ff);
            draw_region(state, layer, h, (v + y) & 0x1ff, 0, y, screen_width, y + 1);
        }
        return;
    }

    draw_region(state, layer, static_cast<int>((0u - hscr) & 0x1ff), v, 0, 0, screen_width,
                screen_height);
}

void Segaic24Tile::draw_split(const DrawState& state,
                             u32              layer,
                             u32              mode,
                             u32              hscr,
                             u32              vscr)
{
    // Both layers of the pair share the vertical scroll here, and the row scroll
    // table is always the even layer's: MAME binds it before the layer index is
    // flipped, so the odd layer never has one of its own in these modes.
    const int screen_width  = static_cast<int>(kScreenWidth);
    const int screen_height = static_cast<int>(kScreenHeight);
    const int v             = static_cast<int>(vscr & 0x1ff);

    if ((hscr & 0x8000) != 0) {
        const u32 table = kRowScrollTable + 0x200 * layer;

        if (mode == 1) {
            // Horizon split. Rows above the boundary come from one layer of the
            // pair and rows below from the other, each with its own horizontal
            // scroll per line. This is how a game draws a sky over a receding
            // ground plane without any geometry.
            const int  boundary = static_cast<int>((0u - vscr) & 0x1ff);
            const u32  first    = ((0u - vscr) & 0x200) != 0 ? layer : (layer ^ 1);
            for (int y = 0; y < screen_height; ++y) {
                const u32 chosen = y >= boundary ? (first ^ 1) : first;
                const int h      = static_cast<int>(
                    (0u - static_cast<u32>(tile_word(table + static_cast<u32>(y)))) & 0x1ff);
                draw_region(state, chosen, h, (v + y) & 0x1ff, 0, y, screen_width, y + 1);
            }
            return;
        }

        // Modes 2 and 3: a vertical boundary whose position is the line's own
        // horizontal scroll, so the split itself can move down the screen.
        for (int y = 0; y < screen_height; ++y) {
            const u32 raw      = tile_word(table + static_cast<u32>(y));
            const int boundary = static_cast<int>(raw & 0x1ff);
            const int h        = static_cast<int>((0u - raw) & 0x1ff);
            const u32 left     = (raw & 0x200) != 0 ? layer : (layer ^ 1);

            const int split = std::clamp(boundary, 0, screen_width);
            draw_region(state, left, h, (v + y) & 0x1ff, 0, y, split, y + 1);
            draw_region(state, left ^ 1, (h + split) & 0x1ff, (v + y) & 0x1ff, split, y,
                        screen_width, y + 1);
        }
        return;
    }

    const int h = static_cast<int>((0u - hscr) & 0x1ff);

    if (mode == 1) {
        // A fixed horizon.
        const int boundary = std::clamp(static_cast<int>((0u - vscr) & 0x1ff), 0, screen_height);
        const u32 top      = ((0u - vscr) & 0x200) != 0 ? layer : (layer ^ 1);

        draw_region(state, top, h, v, 0, 0, screen_width, boundary);
        draw_region(state, top ^ 1, h, (v + boundary) & 0x1ff, 0, boundary, screen_width,
                    screen_height);
        return;
    }

    // Modes 2 and 3 without row scroll: a fixed vertical boundary, which is how a
    // game splits the screen for two players or insets a window.
    //
    // Note the boundary is the unnegated scroll value while the source origin is
    // the negated one. That asymmetry is in the original.
    const int boundary = std::clamp(static_cast<int>(hscr & 0x1ff), 0, screen_width);
    const u32 left     = (hscr & 0x200) != 0 ? layer : (layer ^ 1);

    draw_region(state, left, h, v, 0, 0, boundary, screen_height);
    draw_region(state, left ^ 1, (h + boundary) & 0x1ff, v, boundary, 0, screen_width,
                screen_height);
}

void Segaic24Tile::draw_region(const DrawState& state,
                              u32              layer,
                              int              h,
                              int              v,
                              int              x0,
                              int              y0,
                              int              x1,
                              int              y1)
{
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    // The layer wraps every 512 pixels in both axes, so the destination is cut
    // into runs that each read a contiguous span of the map. A full-screen draw
    // needs at most four.
    for (int y = y0; y < y1;) {
        const int source_y = (v + (y - y0)) & 0x1ff;
        const int rows     = std::min(y1 - y, static_cast<int>(kMapHeight) - source_y);

        for (int x = x0; x < x1;) {
            const int source_x = (h + (x - x0)) & 0x1ff;
            const int columns  = std::min(x1 - x, static_cast<int>(kMapWidth) - source_x);

            draw_rect(state, layer, source_x, source_y, x, y, x + columns, y + rows);
            x += columns;
        }
        y += rows;
    }
}

void Segaic24Tile::draw_rect(const DrawState& state,
                             u32              layer,
                             int              sx,
                             int              sy,
                             int              xx1,
                             int              yy1,
                             int              xx2,
                             int              yy2)
{
    const usize layer_base = static_cast<usize>(layer) * kMapWidth * kMapHeight;
    const u16*  pixels     = m_pixmap.data() + layer_base;
    const u8*   flags      = m_flagsmap.data() + layer_base;
    const u32*  pen        = state.pens.data();

    const u32  dest_stride = state.dest_stride;
    u32* const dest        = state.dest;
    const u8   match       = state.match;
    const bool opaque      = state.opaque;
    const bool win         = state.window;

    // Both origins are fixed before xx1 is folded into the mask index below,
    // exactly as in the original.
    usize source_row = static_cast<usize>(sy) * kMapWidth + static_cast<usize>(sx);
    usize dest_row   = static_cast<usize>(yy1) * dest_stride + static_cast<usize>(xx1);
    u32   mask_row   = state.mask_base + static_cast<u32>(yy1) * 4;

    yy2 -= yy1;

    // The mask covers 128 pixels per word, so skip whole words rather than
    // testing a bit offset per pixel.
    while (xx1 >= 128) {
        xx1 -= 128;
        xx2 -= 128;
        ++mask_row;
    }

    for (int y = 0; y < yy2; ++y) {
        usize src       = source_row;
        usize dst       = dest_row;
        u32   mask_word = mask_row;
        int   llx       = xx2;
        int   cur_x     = xx1;

        while (llx > 0) {
            // In the split modes the pair is chosen by position, so the mask does
            // not take part and every pixel of the selected layer is a candidate.
            u16 m = state.masked ? tile_word(mask_word) : u16{0};
            ++mask_word;
            if (win && state.masked) {
                m = static_cast<u16>(~m);
            }

            if (cur_x == 0 && llx >= 128) {
                // Whole 128-pixel block with no side clipping.
                if (m == 0) {
                    // Entirely this layer.
                    for (int x = 0; x < 128; ++x) {
                        if (flags[src] == match || opaque) {
                            dest[dst] = pen[pixels[src]];
                        }
                        ++src;
                        ++dst;
                    }
                } else if (m == 0xffff) {
                    // Entirely the other layer of the pair.
                    src += 128;
                    dst += 128;
                } else {
                    // Mixed, decided in groups of eight.
                    for (int x = 0; x < 128; x += 8) {
                        if ((m & 0x8000) == 0) {
                            for (int xx = 0; xx < 8; ++xx) {
                                if (flags[src + static_cast<usize>(xx)] == match || opaque) {
                                    dest[dst + static_cast<usize>(xx)] =
                                        pen[pixels[src + static_cast<usize>(xx)]];
                                }
                            }
                        }
                        src += 8;
                        dst += 8;
                        m = static_cast<u16>(m << 1);
                    }
                }
            } else {
                const int llx1 = llx >= 128 ? 128 : llx;

                if (m == 0) {
                    for (int x = cur_x; x < llx1; ++x) {
                        if (flags[src] == match || opaque) {
                            dest[dst] = pen[pixels[src]];
                        }
                        ++src;
                        ++dst;
                    }
                } else if (m == 0xffff) {
                    src += static_cast<usize>(128 - cur_x);
                    dst += static_cast<usize>(128 - cur_x);
                } else {
                    for (int x = cur_x; x < llx1; ++x) {
                        if ((flags[src] == match || opaque)
                            && (m & (0x8000 >> (x >> 3))) == 0) {
                            dest[dst] = pen[pixels[src]];
                        }
                        ++src;
                        ++dst;
                    }
                }
            }
            llx -= 128;
            cur_x = 0;
        }

        source_row += kMapWidth;
        dest_row   += dest_stride;
        mask_row   += 4;
    }
}

}  // namespace sm2::hw
