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
    m_scrolled.fill(false);
    m_scroll_known.fill(false);
    m_has_frame = false;
}

void FrameBuilder::submit(const hw::model3::VideoFrame& frame, bool encode_depth)
{
    m_textures.submit(frame);
    const SceneGeometry& g = m_scene.geometry();
    m_tilegen.set_side_columns(g.columns);
    const bool wide_2d = g.wide_2d && g.widen;
    m_tilegen.set_extend(wide_2d ? m_scrolled : std::array<bool, 4>{}, wide_2d);
    m_tilegen.draw_frame(frame.tilegen_vram, frame.tilegen_regs, frame.tilegen_lines, frame.tilegen_writes);
    m_tilegen.finish_wide();

    if (frame.centre_letterbox) {
        const auto blank = [](std::span<const u32> layer) {
            return std::all_of(layer.begin(), layer.end(), [](u32 p) { return p == 0; });
        };
        m_scene.set_layers_empty(blank(m_tilegen.layer(0)) && blank(m_tilegen.layer(1)));
    } else {
        m_scene.set_layers_empty(false);
    }

    m_scene.build(frame);
    build_frame_plan(m_scene, encode_depth, {m_textures.page_blank(0), m_textures.page_blank(1)}, m_plan);

    // An opaque top layer hides the 3D (Le Mans 24's boot screen draws a full
    // scene under it); skip it unless line-of-sight still needs the depth.
    if (!m_plan.los_active() && top_layer_hides_3d()) {
        m_plan.passes.clear();
        m_plan.fogs.clear();
    }
    track_scroll();
    plan_layers();

    m_frame_number = frame.frame_number;
    m_has_frame    = true;
}

bool FrameBuilder::top_layer_hides_3d() const
{
    const PixelRect& l = m_plan.layer_2d;
    PixelRect        d = m_plan.display;
    if (m_scene.geometry().wide_screen()) {
        // The 4:3 screen: the side columns of a hidden scene go black too.
        d.x     = l.x + 2 * d.y;
        d.width = l.width - 2 * d.y;
    }
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
    return m_tilegen.side_columns() != 0 ? m_tilegen.wide_layer(index) : m_tilegen.layer(index);
}

// A layer counts as scrolling background from when its scroll first moves
// until it is switched off or keeps still for a while.
void FrameBuilder::track_scroll()
{
    constexpr u32 kStill = 60 * 20;
    const auto& hash    = m_tilegen.scroll_hash();
    const auto& enabled = m_tilegen.enabled();
    for (usize i = 0; i < 4; i++) {
        if (!enabled[i]) {
            m_scrolled[i] = false;
        } else if (m_scroll_known[i] && hash[i] != m_scroll_hash[i]) {
            m_scrolled[i]   = true;
            m_scroll_age[i] = 0;
        } else if (++m_scroll_age[i] >= kStill) {
            m_scrolled[i] = false;
        }
        m_scroll_known[i] = enabled[i];
        m_scroll_hash[i]  = hash[i];
    }
}

void FrameBuilder::plan_layers()
{
    const SceneGeometry& g = m_scene.geometry();
    for (int i = 0; i < 2; i++) {
        LayerDraw& d = m_plan.layers[i];
        d.rect       = m_plan.layer_2d;
        d.uv_scale   = static_cast<float>(TileGen::kVisibleWidth) / static_cast<float>(TileGen::kWidth);
        d.clip_lo    = 0.0f;
        d.clip_hi    = 1.0f;
    }
    const u32 side = m_tilegen.side_columns();
    if (side == 0) {
        return;
    }
    const u32 width = m_tilegen.wide_width();
    for (LayerDraw& d : m_plan.layers) {
        d.rect.x -= static_cast<s32>(side * g.render_scale);
        d.rect.width = static_cast<s32>(width * g.render_scale);
        d.uv_scale   = 1.0f;
        if (!m_plan.widened) {
            d.clip_lo = static_cast<float>(side) / static_cast<float>(width);
            d.clip_hi = static_cast<float>(side + TileGen::kVisibleWidth) / static_cast<float>(width);
        }
    }
}

}  // namespace sm2::render::model3
