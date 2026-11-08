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
#include "render/model3/texture_dump.h"
#include "render/texture_replace.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace sm2::render::model3 {

FrameBuilder::~FrameBuilder()
{
    if (m_thread.joinable()) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
}

void FrameBuilder::submit_async(const hw::model3::VideoFrame& frame, bool encode_depth)
{
    wait();
    if (!m_thread.joinable()) {
        m_thread = std::thread([this] { worker(); });
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_job       = &frame;
        m_job_depth = encode_depth;
    }
    m_busy = true;
    m_last = &frame;
    m_cv.notify_all();
}

void FrameBuilder::wait()
{
    if (!m_busy) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_job == nullptr; });
    m_busy = false;
}

void FrameBuilder::release(const hw::model3::VideoFrame* keep)
{
    wait();
    if (m_last != keep) {
        m_last = nullptr;
    }
}

void FrameBuilder::match_geometry(const SceneGeometry& geometry, bool encode_depth)
{
    wait();
    if (m_last != nullptr && !(m_scene.geometry() == geometry)) {
        m_scene.set_geometry(geometry);
        build(*m_last, encode_depth);
    }
}

void FrameBuilder::worker()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [this] { return m_quit || m_job != nullptr; });
        if (m_quit) {
            return;
        }
        const hw::model3::VideoFrame* job   = m_job;
        const bool                    depth = m_job_depth;
        lock.unlock();
        build(*job, depth);
        lock.lock();
        m_job = nullptr;
        m_cv.notify_all();
    }
}

void FrameBuilder::reset()
{
    wait();
    m_last = nullptr;
    m_scene.reset();
    m_textures.invalidate();
    m_scrolled.fill(false);
    m_scroll_known.fill(false);
    m_has_frame = false;
}

void FrameBuilder::submit(const hw::model3::VideoFrame& frame, bool encode_depth)
{
    wait();
    m_last = &frame;
    build(frame, encode_depth);
}

void FrameBuilder::build(const hw::model3::VideoFrame& frame, bool encode_depth)
{
    using Clock      = std::chrono::steady_clock;
    const auto since = [](Clock::time_point& t) {
        const Clock::time_point now = Clock::now();
        const double            ms  = std::chrono::duration<double, std::milli>(now - t).count();
        t                           = now;
        return ms;
    };
    const Clock::time_point start = Clock::now();
    Clock::time_point       mark  = start;
    m_textures.submit(frame);
    m_times.textures = since(mark);
    const SceneGeometry& g = m_scene.geometry();
    m_tilegen.set_side_columns(g.columns);
    const bool wide_2d = g.wide_2d && g.widen;
    m_tilegen.set_extend(wide_2d ? m_scrolled : std::array<bool, 4>{}, wide_2d);
    m_tilegen.draw_frame(frame.tilegen_vram, frame.tilegen_regs, frame.tilegen_lines, frame.tilegen_writes);
    m_tilegen.finish_wide();
    m_times.tilegen = since(mark);

    if (frame.centre_letterbox) {
        const auto blank = [](std::span<const u32> layer) {
            return std::all_of(layer.begin(), layer.end(), [](u32 p) { return p == 0; });
        };
        m_scene.set_layers_empty(blank(m_tilegen.layer(0)) && blank(m_tilegen.layer(1)));
    } else {
        m_scene.set_layers_empty(false);
    }

    m_scene.build(frame);
    m_times.scene = since(mark);
    build_frame_plan(m_scene, encode_depth, {m_textures.page_blank(0), m_textures.page_blank(1)}, m_plan);

    // An opaque top layer hides the 3D (Le Mans 24's boot screen draws a full
    // scene under it); skip it unless line-of-sight still needs the depth.
    if (!m_plan.los_active() && top_layer_hides_3d()) {
        m_plan.passes.clear();
        m_plan.fogs.clear();
    }
    track_scroll();
    plan_layers();
    if (m_replacements != nullptr && !m_replacements->empty()) {
        apply_replacements();
    }
    if (m_dumper != nullptr) {
        m_dumper->scan(m_plan, m_textures, m_hasher);
    }
    m_times.plan = since(mark);

    m_frame_number = frame.frame_number;
    m_has_frame    = true;
    m_times.total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void FrameBuilder::apply_replacements()
{
    bool any = false;
    for (R3DUniforms& u : m_plan.uniforms) {
        if (u.textureEnabled == 0) {
            continue;
        }
        TextureReplacements::Placement p;
        if (!m_replacements->find(m_hasher.lookup(m_textures, texture_ref(u)), &p)) {
            continue;
        }
        u.replaceInfo = (p.layer + 1) | (std::min(p.max_lod, 15u) << 8);
        u.replaceXY   = p.x | (p.y << 16);
        u.replaceWH   = p.width | (p.height << 16);
        any           = true;
    }
    if (any) {
        for (R3DDraw& d : m_plan.draws) {
            d.variant = r3d_variant(m_plan.uniforms[d.uniforms]);
        }
    }
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
