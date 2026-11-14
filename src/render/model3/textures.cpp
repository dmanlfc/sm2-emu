// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp,
// TextureBank.cpp and R3DShaderCommon.h, Copyright (c) 2003-2026 The
// Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.

#include "render/model3/textures.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::model3 {

namespace {

/// Past this many queued rectangles a full upload is cheaper than replaying them.
constexpr usize kMaxQueuedUploads = 4096;
constexpr u32   kTileShift        = 5;
constexpr u32   kTilesPerRow      = TextureSheets::kSize >> kTileShift;

}  // namespace

TextureSheets::TextureSheets()
{
    m_texels.assign(static_cast<usize>(kSize) * kSize, 0);
    m_tile_stamps.assign(static_cast<usize>(kTilesPerRow) * kTilesPerRow, 0);
    add_full_upload();
}

void TextureSheets::invalidate()
{
    m_uploads.clear();
    add_full_upload();
    m_epoch++;
}

u32 TextureSheets::region_stamp(u32 page, u32 x, u32 y, u32 width, u32 height) const
{
    const u32 tw = std::min((width + ((x & 31u)) + 31u) >> kTileShift, kTilesPerRow);
    const u32 th = std::min((height + ((y & 31u)) + 31u) >> kTileShift, kPageHeight >> kTileShift);
    const u32 tx = (x & (kPageWidth - 1)) >> kTileShift;
    const u32 ty = (y & (kPageHeight - 1)) >> kTileShift;
    const u32 page_row = (page & 1) * (kPageHeight >> kTileShift);
    u32 stamp = 0;
    for (u32 j = 0; j < th; j++) {
        const u32* row = &m_tile_stamps[static_cast<usize>(page_row + ((ty + j) & ((kPageHeight >> kTileShift) - 1))) * kTilesPerRow];
        for (u32 i = 0; i < tw; i++) {
            stamp = std::max(stamp, row[(tx + i) & (kTilesPerRow - 1)]);
        }
    }
    return stamp;
}

// Whole-sheet upload: every stored level of both pages, each from its fixed
// region.
void TextureSheets::add_full_upload()
{
    m_uploads.clear();
    for (u32 page = 0; page < 2; page++) {
        for (u32 level = 0; level < kStoredLevels; level++) {
            TextureUpload u;
            u.page   = page;
            u.level  = level;
            u.width  = std::min(level_width(level), kPageWidth - kMipXBase[level]);
            u.height = std::min(level_height(level), kPageHeight - kMipYBase[level]);
            u.src_x  = kMipXBase[level];
            u.src_y  = kMipYBase[level] + page * kPageHeight;
            m_uploads.push_back(u);
        }
    }
    m_full_pending = true;
}

void TextureSheets::add_rect(u32 x, u32 y, u32 width, u32 height)
{
    // Clip to texture RAM.
    if (x >= kSize || y >= kSize || width == 0 || height == 0) {
        return;
    }
    width  = std::min(width, kSize - x);
    height = std::min(height, kSize - y);

    // Split across the two pages, then copy into every level region it overlaps.
    for (u32 page = 0; page < 2; page++) {
        const u32 page_top = page * kPageHeight;
        const u32 y0       = std::max(y, page_top);
        const u32 y1       = std::min(y + height, page_top + kPageHeight);
        if (y0 >= y1) {
            continue;
        }
        const u32 ly0 = y0 - page_top;
        const u32 ly1 = y1 - page_top;

        for (u32 level = 0; level < kStoredLevels; level++) {
            const u32 rx0 = kMipXBase[level];
            const u32 ry0 = kMipYBase[level];
            const u32 rx1 = std::min(rx0 + level_width(level), kPageWidth);
            const u32 ry1 = std::min(ry0 + level_height(level), kPageHeight);

            const u32 ix0 = std::max(x, rx0);
            const u32 ix1 = std::min(x + width, rx1);
            const u32 iy0 = std::max(ly0, ry0);
            const u32 iy1 = std::min(ly1, ry1);
            if (ix0 >= ix1 || iy0 >= iy1) {
                continue;
            }

            TextureUpload u;
            u.page   = page;
            u.level  = level;
            u.dst_x  = ix0 - rx0;
            u.dst_y  = iy0 - ry0;
            u.width  = ix1 - ix0;
            u.height = iy1 - iy0;
            u.src_x  = ix0;
            u.src_y  = iy0 + page_top;
            m_uploads.push_back(u);
        }
    }
}

void TextureSheets::submit(const hw::model3::VideoFrame& frame)
{
    const std::span<const u16> ram = frame.texture_ram;
    if (ram.size() < m_texels.size()) {
        return;  // no texture RAM attached yet
    }

    // A skipped frame's uploads were never seen.
    const bool skipped = m_last_frame != 0 && frame.frame_number != m_last_frame + 1;
    m_last_frame       = frame.frame_number;
    if (frame.texture_reset || skipped) {
        std::memcpy(m_texels.data(), ram.data(), m_texels.size() * sizeof(u16));
        for (u32 page = 0; page < 2; page++) {
            const auto first = m_texels.begin() + static_cast<std::ptrdiff_t>(page) * kPageHeight * kSize;
            m_page_used[page] = std::any_of(first, first + static_cast<std::ptrdiff_t>(kPageHeight) * kSize,
                                            [](u16 t) { return t != 0; });
        }
        add_full_upload();
        m_epoch++;
        return;
    }

    if (!frame.texture_uploads.empty()) {
        m_stamp++;
    }
    for (const hw::model3::TextureRect& rect : frame.texture_uploads) {
        if (rect.x >= kSize || rect.y >= kSize) {
            continue;
        }
        const u32 w = std::min(rect.width, kSize - rect.x);
        const u32 h = std::min(rect.height, kSize - rect.y);
        if (w == 0 || h == 0) {
            continue;
        }
        for (u32 row = 0; row < h; row++) {
            const usize offset = static_cast<usize>(rect.y + row) * kSize + rect.x;
            std::memcpy(&m_texels[offset], &ram[offset], w * sizeof(u16));
            bool& used = m_page_used[(rect.y + row) / kPageHeight];
            if (!used) {
                used = std::any_of(&ram[offset], &ram[offset] + w, [](u16 t) { return t != 0; });
            }
        }
        for (u32 ty = rect.y >> kTileShift; ty <= (rect.y + h - 1) >> kTileShift; ty++) {
            for (u32 tx = rect.x >> kTileShift; tx <= (rect.x + w - 1) >> kTileShift; tx++) {
                m_tile_stamps[static_cast<usize>(ty) * kTilesPerRow + tx] = m_stamp;
            }
        }
        if (!m_full_pending) {
            add_rect(rect.x, rect.y, w, h);
        }
    }

    if (m_uploads.size() > kMaxQueuedUploads) {
        add_full_upload();
    }
}

// model3_r3d.frag's ExtractColour(), on the CPU.
std::array<float, 4> decode_texel(int type, u16 v)
{
    const u32 value = v;
    float     c[4]  = {0.0f, 0.0f, 0.0f, 0.0f};

    auto nibble_lum = [&](u32 lum, u32 alpha) {
        c[0] = c[1] = c[2] = static_cast<float>(lum) / 15.0f;
        c[3] = static_cast<float>(alpha) / 15.0f;
    };
    // Formats 5, 6 and 8..11: luminance, transparent only at full scale.
    auto lum_key = [&](float lum) {
        c[0] = c[1] = c[2] = lum;
        c[3] = (lum == 1.0f) ? 0.0f : 1.0f;
    };

    switch (type) {
        case 0:  // T1RGB5
            c[0] = static_cast<float>((value >> 10) & 0x1Fu) * (1.0f / 31.0f);
            c[1] = static_cast<float>((value >> 5) & 0x1Fu) * (1.0f / 31.0f);
            c[2] = static_cast<float>(value & 0x1Fu) * (1.0f / 31.0f);
            c[3] = 1.0f - static_cast<float>((value >> 15) & 0x1u);
            break;
        case 1:  // Interleaved A4L4 (low byte)
            nibble_lum(value & 0xFu, (value >> 4) & 0xFu);
            break;
        case 2:
            nibble_lum((value >> 4) & 0xFu, value & 0xFu);
            break;
        case 3:
            nibble_lum((value >> 8) & 0xFu, (value >> 12) & 0xFu);
            break;
        case 4:
            nibble_lum((value >> 12) & 0xFu, (value >> 8) & 0xFu);
            break;
        case 5:
            lum_key(static_cast<float>(value & 0xFFu) / 255.0f);
            break;
        case 6:
            lum_key(static_cast<float>((value >> 8) & 0xFFu) / 255.0f);
            break;
        case 7:  // RGBA4
            c[0] = static_cast<float>((value >> 12) & 0xFu) / 15.0f;
            c[1] = static_cast<float>((value >> 8) & 0xFu) / 15.0f;
            c[2] = static_cast<float>((value >> 4) & 0xFu) / 15.0f;
            c[3] = static_cast<float>(value & 0xFu) / 15.0f;
            break;
        case 8:  // low byte, low nibble
            lum_key(static_cast<float>(value & 0xFu) / 15.0f);
            break;
        case 9:  // low byte, high nibble
            lum_key(static_cast<float>((value >> 4) & 0xFu) / 15.0f);
            break;
        case 10:  // high byte, low nibble
            lum_key(static_cast<float>((value >> 8) & 0xFu) / 15.0f);
            break;
        case 11:  // high byte, high nibble
            lum_key(static_cast<float>((value >> 12) & 0xFu) / 15.0f);
            break;
        default:
            break;
    }
    return {c[0], c[1], c[2], c[3]};
}

}  // namespace sm2::render::model3
