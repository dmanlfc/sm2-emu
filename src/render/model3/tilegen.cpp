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
    for (auto& w : m_wide) {
        std::fill(w.begin(), w.end(), 0u);
    }
    m_scroll_hash.fill(0);
    m_enabled.fill(false);
}

void TileGen::set_side_columns(u32 columns)
{
    if (columns != m_side) {
        m_side = columns;
        for (auto& w : m_wide) {
            w.assign(columns != 0 ? static_cast<usize>(wide_width()) * kHeight : 0, 0u);
        }
    }
}

void TileGen::finish_wide()
{
    if (m_side == 0) {
        return;
    }
    // An edge within kSpread of one colour (a dithered gradient, a mask or
    // frame on the top layer) is tiled out.
    constexpr int kFlat   = 16;
    constexpr int kSpread = 24;
    const auto flat = [](const u32* p) {
        int lo[3] = {255, 255, 255};
        int hi[3] = {0, 0, 0};
        for (int i = 0; i < kFlat; i++) {
            if ((p[i] >> 24) == 0) {
                return false;
            }
            for (int c = 0; c < 3; c++) {
                const int v = static_cast<int>((p[i] >> (c * 8)) & 0xFF);
                lo[c]       = std::min(lo[c], v);
                hi[c]       = std::max(hi[c], v);
            }
        }
        return hi[0] - lo[0] <= kSpread && hi[1] - lo[1] <= kSpread && hi[2] - lo[2] <= kSpread;
    };
    const int width = static_cast<int>(wide_width());
    const int side  = static_cast<int>(m_side);
    const int right = static_cast<int>(kVisibleWidth);
    for (usize l = 0; l < 2; l++) {
        for (u32 y = 0; y < kHeight; y++) {
            u32* const       row    = m_wide[l].data() + static_cast<usize>(y) * static_cast<usize>(width);
            const u32* const centre = m_surface[l].data() + static_cast<usize>(y) * kWidth;
            std::memcpy(row + side, centre, kVisibleWidth * 4);
            // Behind the 3D, only rows the layers fill are carried out: a
            // backdrop, not a panel sliding across.
            if (l == 0 && std::any_of(centre, centre + right, [](u32 v) { return (v >> 24) == 0; })) {
                std::fill(row, row + side, 0u);
                std::fill(row + side + right, row + width, 0u);
                continue;
            }
            if (!m_extend_edges) {
                continue;
            }
            if (flat(centre)) {
                for (int x = 0; x < side; x++) {
                    if (row[x] == 0) {
                        row[x] = centre[(x - side) & (kFlat - 1)];
                    }
                }
            }
            if (flat(centre + right - kFlat)) {
                for (int x = side + right; x < width; x++) {
                    if (row[x] == 0) {
                        row[x] = centre[right - kFlat + ((x - side - right) & (kFlat - 1))];
                    }
                }
            }
        }
    }
}

// One pixel of the map at screen column x, which may be off the 496.
u32 TileGen::tile_pixel(int index, bool four_bit, int x, int line, int scroll_x, int scroll_y) const
{
    const int  sx       = x + scroll_x;
    const int  pair     = (((line + scroll_y) >> 3) & 0x3F) * 32 + ((sx >> 4) & 0x1F);
    const int  i        = sx & 15;
    const int  vFine    = GetVFine(line, scroll_y);
    const u32  tileData = static_cast<u32>(GetTileData(index, pair));
    const u32* pal      = m_pal[static_cast<usize>(index / 2)].data();
    const u32  half     = i < 8 ? tileData >> 16 : tileData & 0xFFFF;
    u32        colour   = 0;
    if (four_bit) {
        const u32 pattern = ((half & 0x3FFF) << 1) | ((half >> 15) & 1);
        const u32 bits    = vram32(pattern * 8 + static_cast<u32>(vFine));
        colour            = pal[(half & 0x7FF0) | ((bits >> ((7 - (i % 8)) * 4)) & 0xF)];
    } else {
        const u32 bits = vram32((half & 0x3FFF) * 16 + static_cast<u32>(vFine) * 2 + static_cast<u32>((i % 8) / 4));
        colour         = pal[(half & 0x7F00) | ((bits >> ((3 - (i % 4)) * 8)) & 0xFF)];
    }
    return colour >= 0x1000000 ? colour : 0;
}

void TileGen::draw_sides(int line, std::span<const u32> regs, int primary, int line_mask, const int scroll_x[2],
                         const int scroll_y[2], const bool has_layer[2])
{
    u32* const row   = m_wide[0].data() + static_cast<usize>(wide_width()) * static_cast<usize>(line);
    const int  side  = static_cast<int>(m_side);
    const int  right = static_cast<int>(kVisibleWidth);
    for (int x = -side; x < right + side; x++) {
        if (x == 0) {
            x = right;
        }
        const int layer = (GetPixelMask(line_mask, std::clamp(x, 0, right - 1)) + 1) & 1;
        const int index = primary + layer;
        if (!has_layer[layer] || !m_extend[static_cast<usize>(index)] || Above3D(regs, index)) {
            continue;
        }
        const u32 colour = tile_pixel(index, Is4Bit(regs, index), x, line, scroll_x[layer], scroll_y[layer]);
        if (colour != 0) {
            row[x + side] = colour;
        }
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
                         std::span<const hw::model3::TileGenLine> lines,
                         std::span<const hw::model3::TileGenWrite> writes)
{
    if (regs.size() < kRegs || lines.size() < kHeight) {
        draw_frame(vram, regs);
        return;
    }
    clear();
    if (vram.size() < kVramBytes) {
        writes = {};
    }
    if (!writes.empty()) {
        m_vram_work.assign(vram.begin(), vram.begin() + kVramBytes);
        vram = m_vram_work;
    }
    prepare(vram, regs);
    std::array<u32, kRegs> line_regs{};
    std::copy_n(regs.begin(), kRegs, line_regs.begin());
    usize next = 0;
    for (u32 i = 0; i < kHeight; i++) {
        for (; next < writes.size() && writes[next].line <= i; ++next) {
            const hw::model3::TileGenWrite& w = writes[next];
            if (w.offset + 4 > kVramBytes) {
                continue;
            }
            std::memcpy(m_vram_work.data() + w.offset, w.bytes.data(), 4);
            if (w.offset >= 0x100000) {
                const u32 index = (w.offset - 0x100000) / 4;
                for (int layer = 0; layer < 2; layer++) {
                    m_pal[static_cast<usize>(layer)][index] = GetColour32(layer, vram32(0x40000 + index));
                }
            }
        }
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

        for (int k = 0; k < 2; k++) {
            if (hasLayer[k]) {
                u32& h = m_scroll_hash[static_cast<usize>(primaryIndex + k)];
                h      = (h ^ static_cast<u32>(scrollX[k] | (scrollY[k] << 16))) * 0x01000193u;
                m_enabled[static_cast<usize>(primaryIndex + k)] = true;
            }
        }
        if (m_side != 0 && (m_extend[static_cast<usize>(primaryIndex)] || m_extend[static_cast<usize>(altIndex)])) {
            draw_sides(line, regs, primaryIndex, lineMask, scrollX, scrollY, hasLayer);
        }

        for (int x = 0; x < static_cast<int>(kVisibleWidth);) {

            const int  layer      = (GetPixelMask(lineMask, x) + 1) & 1;  // 1 means primary layer; flip it to zero
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
