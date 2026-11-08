// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 tile generator, drawn on the CPU into two layers.
//
// Derived from Supermodel's Src/Model3/TileGen.cpp and TileGen.h, with the
// hardware description from Src/Graphics/Render2D.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2011-2012 Bart Trzynadlowski, Nik Henson.
// sm2-emu changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to sm2-emu's changes only.
//
// Draws from a VideoFrame's VRAM and registers. Palettes persist across
// frames and are updated from VRAM changes; VRAM stores made during active
// display (tilegen_writes) are applied per line. VRAM words are loaded
// little-endian. Widescreen side columns come from the map and flat edges.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"

#include <array>
#include <span>
#include <vector>

namespace sm2::render::model3 {

class TileGen {
public:
    static constexpr u32 kWidth        = 512;  ///< Row stride of a layer.
    static constexpr u32 kVisibleWidth = 496;
    static constexpr u32 kHeight       = 384;
    static constexpr u32 kVramBytes    = 0x120000;
    static constexpr u32 kRegs         = 64;

    TileGen();

    void prepare(std::span<const u8> vram, std::span<const u32> regs);

    void clear();
    void clear_line(u32 line);

    void draw_line(int line, std::span<const u32> regs);

    void draw_frame(std::span<const u8> vram, std::span<const u32> regs);

    void draw_frame(std::span<const u8> vram, std::span<const u32> regs,
                    std::span<const hw::model3::TileGenLine> lines,
                    std::span<const hw::model3::TileGenWrite> writes = {});

    [[nodiscard]] std::span<const u32> layer(int index) const { return m_surface[static_cast<usize>(index)]; }

    /// Widescreen: `columns` more of the map drawn each side of the bottom
    /// layer into wide_layer(), for the layers set_extend() names.
    void set_side_columns(u32 columns);
    [[nodiscard]] u32 side_columns() const { return m_side; }
    [[nodiscard]] u32 wide_width() const { return kVisibleWidth + 2 * m_side; }
    /// `edges` also carries a row out whose 16 pixels at that edge are one
    /// colour (a sky gradient).
    void set_extend(std::array<bool, 4> extend, bool edges)
    {
        m_extend       = extend;
        m_extend_edges = edges;
    }

    /// A layer wide_width() across: the sides, then the 496 columns of
    /// layer() copied in by finish_wide().
    [[nodiscard]] std::span<const u32> wide_layer(int index) const { return m_wide[static_cast<usize>(index)]; }
    void finish_wide();

    /// Per layer, a hash of the X scroll down the frame last drawn, and
    /// whether it was enabled.
    [[nodiscard]] const std::array<u32, 4>&  scroll_hash() const { return m_scroll_hash; }
    [[nodiscard]] const std::array<bool, 4>& enabled() const { return m_enabled; }

private:
    struct ColourOffsetRegister {
        void Update(u32 reg)
        {
            b = static_cast<s32>(static_cast<s8>((reg >> 16) & 0xFF));
            g = static_cast<s32>(static_cast<s8>((reg >> 8) & 0xFF));
            r = static_cast<s32>(static_cast<s8>((reg >> 0) & 0xFF));
            b *= 2;
            g *= 2;
            r *= 2;
        }

        s32 r = 0;
        s32 g = 0;
        s32 b = 0;
    };

    [[nodiscard]] u32 vram32(u32 word_index) const;

    [[nodiscard]] bool IsEnabled(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] bool Above3D(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] bool Is4Bit(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] int  GetYScroll(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] int  GetXScroll(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] bool LineScrollMode(std::span<const u32> regs, int layerNumber) const;
    [[nodiscard]] int  GetLineScroll(int layerNumber, int yCoord) const;
    [[nodiscard]] static int GetTilePairNumber(int xCoord, int yCoord, int xScroll, int yScroll);
    [[nodiscard]] int  GetTileData(int layerNum, int tilePairNumber) const;
    [[nodiscard]] static int GetVFine(int yCoord, int yScroll) { return (yCoord + yScroll) & 7; }
    [[nodiscard]] static int GetHFine(int xCoord, int xScroll) { return (xCoord + xScroll) & 15; }
    [[nodiscard]] int  GetLineMask(int layerNumber, int yCoord) const;
    [[nodiscard]] static int GetPixelMask(int lineMask, int xCoord);
    [[nodiscard]] u32  GetColour32(int layer, u32 data) const;

    void Draw4BitTilePair(int tileData, int hStart, int hEnd, int vFine, u32* lineBuffer, const u32* pal,
                          int& x) const;
    void Draw8BitTilePair(int tileData, int hStart, int hEnd, int vFine, u32* lineBuffer, const u32* pal,
                          int& x) const;

    void clear_state();
    void sync_palette_ram();
    void build_palettes(u32 reg40, u32 reg44);
    void build_palette(int layer);

    [[nodiscard]] u32 tile_pixel(int index, bool four_bit, int x, int line, int scroll_x, int scroll_y) const;
    void draw_sides(int line, std::span<const u32> regs, int primary, int line_mask, const int scroll_x[2],
                    const int scroll_y[2], const bool has_layer[2]);

    std::span<const u8> m_vram;
    std::vector<u8>     m_vram_work;  // VRAM as of the line being drawn, when stores are applied
    u32                 m_palette_regs[2] = {~0u, ~0u};
    std::vector<u8>     m_pal_ram;  // the palette VRAM m_pal was built from
    bool                m_pal_valid = false;

    ColourOffsetRegister m_colourOffsetRegs[2];
    std::array<std::vector<u32>, 2> m_pal;      // computed palettes: [0] A/A', [1] B/B'
    std::array<std::vector<u32>, 2> m_surface;  // [0] bottom, [1] top

    u32                  m_side = 0;
    std::array<bool, 4>  m_extend{};
    bool                 m_extend_edges = false;
    std::array<std::vector<u32>, 2> m_wide;
    std::array<u32, 4>   m_scroll_hash{};
    std::array<bool, 4>  m_enabled{};
};

}  // namespace sm2::render::model3
