// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 tile generator line drawing.
//
// Derived from Supermodel's Src/Model3/TileGen.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2011-2012 Bart Trzynadlowski, Nik Henson.
// sm2-emu changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to sm2-emu's changes only.
//
// Registers:
//
//  0x20: -------- -------- x------- -------- Layer 3 bitdepth (0 = 8-bit, 1 = 4-bit)
//        -------- -------- -x------ -------- Layer 2 bitdepth
//        -------- -------- --x----- -------- Layer 1 bitdepth
//        -------- -------- ---x---- -------- Layer 0 bitdepth
//        -------- -------- ----x--- -------- Layer 3 priority (0 = below 3D, 1 = above 3D)
//        -------- -------- -----x-- -------- Layer 2 priority
//        -------- -------- ------x- -------- Layer 1 priority
//        -------- -------- -------x -------- Layer 0 priority
//  0x40: layer 0 & 1 colour offset, 0x44: layer 2 & 3 (signed 8-bit R, G, B)
//  0x60..0x6C: x------- -------- -------- -------- Layer enable
//              -------x xxxxxxxx -------- -------- Y scroll
//              -------- -------- x------- -------- X line scroll enable
//              -------- -------- -------x xxxxxxxx X scroll
//
// VRAM: tiles from 0, per-line H scroll tables at F6000 (0x400 per layer),
// the stencil mask at F7000, name tables at F8000 (0x2000 per layer), and the
// 32768-entry palette at 100000 (tbbbbbgggggrrrrr, t = transparent).

#include "render/model3/tilegen.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::model3 {

TileGen::TileGen()
{
    for (auto& p : m_pal) {
        p.assign(0x8000, 0);
    }
    for (auto& s : m_surface) {
        s.assign(static_cast<usize>(kWidth) * kHeight, 0);
    }
}

u32 TileGen::vram32(u32 word_index) const
{
    const usize offset = static_cast<usize>(word_index) * 4;
    if (offset + 4 > m_vram.size()) {
        return 0;
    }
    return static_cast<u32>(m_vram[offset]) | (static_cast<u32>(m_vram[offset + 1]) << 8)
           | (static_cast<u32>(m_vram[offset + 2]) << 16) | (static_cast<u32>(m_vram[offset + 3]) << 24);
}

void TileGen::prepare(std::span<const u8> vram, std::span<const u32> regs)
{
    m_vram            = vram;
    m_palette_regs[0] = ~0u;
    m_palette_regs[1] = ~0u;
    if (regs.size() < kRegs) {
        return;
    }
    build_palettes(regs[0x40 / 4], regs[0x44 / 4]);
}

// RecomputePalettes() for each layer pair whose offset changed; the palette
// is at VRAM word 0x40000.
void TileGen::build_palettes(u32 reg40, u32 reg44)
{
    const u32 regs[2] = {reg40, reg44};
    for (int layer = 0; layer < 2; layer++) {
        if (m_palette_regs[layer] == regs[layer]) {
            continue;
        }
        m_palette_regs[layer] = regs[layer];
        m_colourOffsetRegs[layer].Update(regs[layer]);
        u32* pal = m_pal[static_cast<usize>(layer)].data();
        for (u32 i = 0; i < 32768; i++) {
            pal[i] = GetColour32(layer, vram32(0x40000 + i));
        }
    }
}

void TileGen::clear()
{
    for (auto& s : m_surface) {
        std::fill(s.begin(), s.end(), 0u);
    }
}

void TileGen::draw_frame(std::span<const u8> vram, std::span<const u32> regs)
{
    clear();
    prepare(vram, regs);
    for (int i = 0; i < static_cast<int>(kHeight); i++) {
        draw_line(i, regs);
    }
}

void TileGen::draw_frame(std::span<const u8> vram, std::span<const u32> regs,
                         std::span<const hw::model3::TileGenLine> lines)
{
    if (regs.size() < kRegs || lines.size() < kHeight) {
        draw_frame(vram, regs);
        return;
    }
    clear();
    prepare(vram, regs);
    std::array<u32, kRegs> line_regs{};
    std::copy_n(regs.begin(), kRegs, line_regs.begin());
    for (u32 i = 0; i < kHeight; i++) {
        const hw::model3::TileGenLine& l = lines[i];
        line_regs[0x20 / 4] = l.reg20;
        line_regs[0x40 / 4] = l.reg40;
        line_regs[0x44 / 4] = l.reg44;
        for (u32 j = 0; j < 4; j++) {
            line_regs[0x60 / 4 + j] = l.scroll[j];
        }
        build_palettes(l.reg40, l.reg44);
        draw_line(static_cast<int>(i), line_regs);
    }
}

bool TileGen::IsEnabled(std::span<const u32> regs, int layerNumber) const
{
    return (regs[0x60 / 4 + static_cast<usize>(layerNumber)] & 0x80000000) > 0;
}

bool TileGen::Above3D(std::span<const u32> regs, int layerNumber) const
{
    return ((regs[0x20 / 4] >> (8 + layerNumber)) & 0x1) != 0;
}

bool TileGen::Is4Bit(std::span<const u32> regs, int layerNumber) const
{
    return (regs[0x20 / 4] & (1u << (12 + layerNumber))) != 0;
}

int TileGen::GetYScroll(std::span<const u32> regs, int layerNumber) const
{
    return static_cast<int>((regs[0x60 / 4 + static_cast<usize>(layerNumber)] >> 16) & 0x1FF);
}

int TileGen::GetXScroll(std::span<const u32> regs, int layerNumber) const
{
    return static_cast<int>(regs[0x60 / 4 + static_cast<usize>(layerNumber)] & 0x3FF);
}

bool TileGen::LineScrollMode(std::span<const u32> regs, int layerNumber) const
{
    return (regs[0x60 / 4 + static_cast<usize>(layerNumber)] & 0x8000) != 0;
}

int TileGen::GetLineScroll(int layerNumber, int yCoord) const
{
    const u32 index = static_cast<u32>(((0xF6000 + (layerNumber * 0x400)) / 4) + (yCoord / 2));
    const u32 shift = static_cast<u32>((1 - (yCoord % 2)) * 16);

    return static_cast<int>((vram32(index) >> shift) & 0xFFFFu);
}

int TileGen::GetTilePairNumber(int xCoord, int yCoord, int xScroll, int yScroll)
{
    const int xIndex = ((xCoord + xScroll) / 16) & 0x1F;
    const int yIndex = ((yCoord + yScroll) / 8) & 0x3F;

    return (yIndex * 32) + xIndex;
}

int TileGen::GetTileData(int layerNum, int tilePairNumber) const
{
    const u32 addressBase = static_cast<u32>((0xF8000 + (layerNum * 0x2000)) / 4);
    return static_cast<int>(vram32(addressBase + static_cast<u32>(tilePairNumber)));
}

int TileGen::GetLineMask(int layerNumber, int yCoord) const
{
    const u32 shift = (layerNumber < 2) ? 16u : 0u;
    const u32 index = static_cast<u32>((0xF7000 / 4) + yCoord);

    return static_cast<int>((vram32(index) >> shift) & 0xFFFFu);
}

int TileGen::GetPixelMask(int lineMask, int xCoord)
{
    const int maskTest = 1 << (15 - (xCoord / 32));

    return (lineMask & maskTest) != 0;  // zero means alt layer
}

u32 TileGen::GetColour32(int layer, u32 data) const
{
    const int a = static_cast<int>((((data >> 15) + 1) & 1) * 255);
    const int b = static_cast<int>(((((data >> 10) & 0x1F) * 255) / 31)) & a;
    const int g = static_cast<int>(((((data >> 5) & 0x1F) * 255) / 31)) & a;
    const int r = static_cast<int>((((data & 0x1F) * 255) / 31)) & a;

    const auto& offset = m_colourOffsetRegs[layer];
    const int   rr     = std::clamp(offset.r + r, 0, 255);
    const int   gg     = std::clamp(offset.g + g, 0, 255);
    const int   bb     = std::clamp(offset.b + b, 0, 255);

    return (static_cast<u32>(a) << 24) | (static_cast<u32>(bb) << 16) | (static_cast<u32>(gg) << 8)
           | static_cast<u32>(rr);
}

void TileGen::Draw4BitTilePair(int tileData, int hStart, int hEnd, int vFine, u32* const lineBuffer,
                               const u32* const pal, int& x) const
{
    // Tile pattern offset: each tile occupies 32 bytes when using 4-bit pixels (offset of tile pattern within VRAM)
    int patternOffset[2];

    patternOffset[0] = (((tileData >> 16) & 0x3FFF) << 1) | ((tileData >> 31) & 1);
    patternOffset[0] *= 32;
    patternOffset[0] /= 4;

    patternOffset[1] = ((tileData & 0x3FFF) << 1) | ((tileData >> 15) & 1);
    patternOffset[1] *= 32;
    patternOffset[1] /= 4;

    // Upper colour bits; the lower 4 bits come from the tile pattern
    const int paletteIndex[] = {(tileData >> 16) & 0x7FF0, tileData & 0x7FF0};

    const u32 patterns[] = {vram32(static_cast<u32>(patternOffset[0] + vFine)),
                            vram32(static_cast<u32>(patternOffset[1] + vFine))};

    for (int i = hStart; i < hEnd; i++, x++) {
        const u32 pattern  = patterns[i / 8];  // first 8 pixels use pattern 1, next 8 pattern 2
        const u32 p        = (pattern >> ((7 - (i % 8)) * 4)) & 0xFu;
        const u32 colour32 = pal[static_cast<u32>(paletteIndex[i / 8]) | p];

        if (colour32 >= 0x1000000 && x >= 0 && x < static_cast<int>(kWidth)) {  // write the pixel unless transparent
            lineBuffer[x] = colour32;
        }
    }
}

void TileGen::Draw8BitTilePair(int tileData, int hStart, int hEnd, int vFine, u32* const lineBuffer,
                               const u32* const pal, int& x) const
{
    // Tile pattern offset: each tile occupies 64 bytes when using 8-bit pixels
    int patternOffset[2];

    patternOffset[0] = (tileData >> 16) & 0x3FFF;
    patternOffset[0] *= 64;
    patternOffset[0] /= 4;

    patternOffset[1] = tileData & 0x3FFF;
    patternOffset[1] *= 64;
    patternOffset[1] /= 4;

    // Upper colour bits
    const int paletteIndex[2] = {(tileData >> 16) & 0x7F00, tileData & 0x7F00};

    const u32 patterns[] = {vram32(static_cast<u32>(patternOffset[0] + (vFine * 2))),
                            vram32(static_cast<u32>(patternOffset[0] + (vFine * 2) + 1)),
                            vram32(static_cast<u32>(patternOffset[1] + (vFine * 2))),
                            vram32(static_cast<u32>(patternOffset[1] + (vFine * 2) + 1))};

    for (int i = hStart; i < hEnd; i++, x++) {
        const u32 pattern  = patterns[i / 4];  // each pattern contains 4 pixels
        const u32 p        = (pattern >> ((3 - (i % 4)) * 8)) & 0xFFu;
        const u32 colour32 = pal[static_cast<u32>(paletteIndex[i / 8]) | p];

        if (colour32 >= 0x1000000 && x >= 0 && x < static_cast<int>(kWidth)) {  // write the pixel unless transparent
            lineBuffer[x] = colour32;
        }
    }
}

void TileGen::draw_line(int line, std::span<const u32> regs)
{
    if (regs.size() < kRegs || line < 0 || line >= static_cast<int>(kHeight)) {
        return;
    }

    for (int i = 2; i-- > 0;) {

        const int primaryIndex = i * 2;
        const int altIndex     = (i * 2) + 1;

        const bool hasLayer[2] = {IsEnabled(regs, primaryIndex), IsEnabled(regs, altIndex)};
        if (!hasLayer[0] && !hasLayer[1]) {
            continue;  // both disabled, try the next pair
        }

        u32* const drawLayers[2] = {
            m_surface[Above3D(regs, primaryIndex) ? 1 : 0].data() + static_cast<usize>(kWidth) * line,
            m_surface[Above3D(regs, altIndex) ? 1 : 0].data() + static_cast<usize>(kWidth) * line};

        const int lineMask   = GetLineMask(primaryIndex, line);
        const int scrollX[2] = {
            LineScrollMode(regs, primaryIndex) ? GetLineScroll(primaryIndex, line) : GetXScroll(regs, primaryIndex),
            LineScrollMode(regs, altIndex) ? GetLineScroll(altIndex, line) : GetXScroll(regs, altIndex)};
        const int scrollY[2] = {GetYScroll(regs, primaryIndex), GetYScroll(regs, altIndex)};

        for (int x = 0; x < static_cast<int>(kVisibleWidth);) {

            const int  layer      = (GetPixelMask(lineMask, x) + 1) & 1;  // 1 means primary layer, so we flip so it's zero
            const bool maskSwitch = GetPixelMask(lineMask, x) != GetPixelMask(lineMask, x + 15);

            if (hasLayer[layer]) {

                const int index = primaryIndex + layer;

                // Tiles are loaded in pairs, as the RAM is 32-bit and each name table entry 16-bit.
                // If the mask changes before all 16 pixels of the next pair are drawn, reuse the previous pair.
                const int tilePairNumber =
                    GetTilePairNumber(maskSwitch ? (x - 16) : x, line, scrollX[layer], scrollY[layer]);
                const int hStart   = GetHFine(x, scrollX[layer]);
                const int hEnd     = maskSwitch ? 16 - (x & 15) : 16;
                const int vFine    = GetVFine(line, scrollY[layer]);
                const int tileData = GetTileData(index, tilePairNumber);

                const int start = x;
                if (Is4Bit(regs, index)) {
                    Draw4BitTilePair(tileData, hStart, hEnd, vFine, drawLayers[layer],
                                     m_pal[static_cast<usize>(index / 2)].data(), x);
                } else {
                    Draw8BitTilePair(tileData, hStart, hEnd, vFine, drawLayers[layer],
                                     m_pal[static_cast<usize>(index / 2)].data(), x);
                }
                // Always advance, even when fine scroll passes the mask switch.
                if (x == start) {
                    ++x;
                }
            } else {
                const int hFine = GetHFine(x, 0);
                x += 32 - hFine;
            }
        }
    }
}

}  // namespace sm2::render::model3
