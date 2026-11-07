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
// Draws from a VideoFrame's VRAM and registers. Palettes are rebuilt from
// VRAM. VRAM words are loaded little-endian.
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

    void draw_line(int line, std::span<const u32> regs);

    void draw_frame(std::span<const u8> vram, std::span<const u32> regs);

    void draw_frame(std::span<const u8> vram, std::span<const u32> regs,
                    std::span<const hw::model3::TileGenLine> lines);

    [[nodiscard]] std::span<const u32> layer(int index) const { return m_surface[static_cast<usize>(index)]; }

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

    void build_palettes(u32 reg40, u32 reg44);

    std::span<const u8> m_vram;
    u32                 m_palette_regs[2] = {~0u, ~0u};

    ColourOffsetRegister m_colourOffsetRegs[2];
    std::array<std::vector<u32>, 2> m_pal;      // computed palettes: [0] A/A', [1] B/B'
    std::array<std::vector<u32>, 2> m_surface;  // [0] bottom, [1] top
};

}  // namespace sm2::render::model3
