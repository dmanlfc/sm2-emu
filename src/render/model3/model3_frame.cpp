//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.

#include "render/model3/model3_frame.h"

#include "core/log.h"

#include <algorithm>
#include <vector>

namespace sm2::render::model3 {

void FrameBuilder::reset()
{
    m_scene.reset();
    m_textures.invalidate();
    m_has_frame = false;
}

void FrameBuilder::submit(const hw::model3::VideoFrame& frame, bool encode_depth)
{
    m_textures.submit(frame);
    m_scene.build(frame);
    build_frame_plan(m_scene, encode_depth, {m_textures.page_blank(0), m_textures.page_blank(1)}, m_plan);

    m_tilegen.draw_frame(frame.tilegen_vram, frame.tilegen_regs, frame.tilegen_lines, frame.tilegen_writes);

    // An opaque top layer hides the 3D (Le Mans 24's boot screen draws a full
    // scene under it); skip it unless line-of-sight still needs the depth.
    if (!m_plan.los_active() && top_layer_hides_3d()) {
        m_plan.passes.clear();
        m_plan.fogs.clear();
    }

    m_frame_number = frame.frame_number;
    m_has_frame    = true;
}

bool FrameBuilder::top_layer_hides_3d() const
{
    const PixelRect& l = m_plan.layer_2d;
    const PixelRect& d = m_plan.display;
    if (l.x > d.x || l.y > d.y || l.x + l.width < d.x + d.width || l.y + l.height < d.y + d.height) {
        return false;
    }
    const std::span<const u32> top = m_tilegen.layer(1);
    for (u32 y = 0; y < TileGen::kHeight; y++) {
        const u32* row = top.data() + static_cast<usize>(y) * TileGen::kWidth;
        for (u32 x = 0; x < TileGen::kVisibleWidth; x++) {
            if ((row[x] >> 24) != 0xFFu) {
                return false;
            }
        }
    }
    return true;
}

std::span<const u32> FrameBuilder::layer(int index) const
{
    return m_tilegen.layer(index);
}

}  // namespace sm2::render::model3
