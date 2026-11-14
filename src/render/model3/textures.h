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
//
// Texture RAM as two 2048x1024 R16UI pages (mips are fixed regions of the
// page); dirty rectangles become per-page, per-level uploads.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"

#include <array>
#include <span>
#include <vector>

namespace sm2::render::model3 {

struct TextureUpload {
    u32 page  = 0;  ///< 0 or 1: texture RAM rows 0..1023 or 1024..2047.
    u32 level = 0;
    u32 dst_x = 0;  ///< Within the level.
    u32 dst_y = 0;
    u32 width  = 0;
    u32 height = 0;
    u32 src_x = 0;  ///< Within texture RAM (2048 texels per row).
    u32 src_y = 0;
};

class TextureSheets {
public:
    static constexpr u32 kSize       = hw::model3::VideoFrame::kTextureSize;  ///< 2048.
    static constexpr u32 kPageWidth  = 2048;
    static constexpr u32 kPageHeight = 1024;
    static constexpr u32 kLevels = 12;
    static constexpr u32 kStoredLevels = 11;

    static constexpr std::array<u32, 12> kMipXBase = {0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047};
    static constexpr std::array<u32, 11> kMipYBase = {0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023};

    TextureSheets();

    void submit(const hw::model3::VideoFrame& frame);

    void invalidate();

    [[nodiscard]] std::span<const TextureUpload> uploads() const { return m_uploads; }
    void clear_uploads()
    {
        m_uploads.clear();
        m_full_pending = false;
    }

    [[nodiscard]] std::span<const u16> texels() const { return m_texels; }
    /// No non-zero texel has ever reached this page (page 0 or 1).
    [[nodiscard]] bool page_blank(u32 page) const { return !m_page_used[page]; }

    /// Changes whenever texture RAM may have changed wholesale.
    [[nodiscard]] u64 epoch() const { return m_epoch; }
    /// The stamp of the latest texture write.
    [[nodiscard]] u32 stamp() const { return m_stamp; }
    /// Last write stamp of any 32x32 tile under a page's level-0 rectangle
    /// (wrapping as the shader does).
    [[nodiscard]] u32 region_stamp(u32 page, u32 x, u32 y, u32 width, u32 height) const;
    /// The level-0 texel the shader fetches at (x, y) of a page.
    [[nodiscard]] u16 texel(u32 page, u32 x, u32 y) const
    {
        return m_texels[(static_cast<usize>(page & 1) * kPageHeight + (y & (kPageHeight - 1))) * kSize + (x & (kPageWidth - 1))];
    }

    [[nodiscard]] static constexpr u32 level_width(u32 level) { return kPageWidth >> level > 0 ? kPageWidth >> level : 1; }
    [[nodiscard]] static constexpr u32 level_height(u32 level) { return kPageHeight >> level > 0 ? kPageHeight >> level : 1; }

private:
    void add_full_upload();
    void add_rect(u32 x, u32 y, u32 width, u32 height);

    std::vector<u16>          m_texels;
    std::vector<TextureUpload> m_uploads;
    bool                      m_full_pending = true;
    std::array<bool, 2>       m_page_used{};
    u64                       m_last_frame   = 0;
    u64                       m_epoch        = 1;
    u32                       m_stamp        = 0;
    std::vector<u32>          m_tile_stamps;
};

// model3_r3d.frag's ExtractColour() on the CPU (formats 0..11).
[[nodiscard]] std::array<float, 4> decode_texel(int type, u16 value);

}  // namespace sm2::render::model3
