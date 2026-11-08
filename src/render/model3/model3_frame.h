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
//
// One Model 3 frame built on the CPU for a GPU backend. submit() copies all it
// needs out of the VideoFrame (only the VROM view is kept); not thread-safe.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"
#include "render/model3/frame_plan.h"
#include "render/model3/line_of_sight.h"
#include "render/model3/scene.h"
#include "render/model3/textures.h"
#include "render/model3/tilegen.h"

#include <array>
#include <span>
#include <vector>

namespace sm2::render::model3 {

class FrameBuilder {
public:
    void set_geometry(const SceneGeometry& geometry) { m_scene.set_geometry(geometry); }
    [[nodiscard]] const SceneGeometry& geometry() const { return m_scene.geometry(); }

    void reset();

    void submit(const hw::model3::VideoFrame& frame, bool encode_depth);

    [[nodiscard]] bool has_frame() const { return m_has_frame; }
    [[nodiscard]] u64  frame_number() const { return m_frame_number; }

    [[nodiscard]] const Scene&     scene() const { return m_scene; }
    [[nodiscard]] const FramePlan& plan() const { return m_plan; }

    [[nodiscard]] bool top_layer_hides_3d() const;
    [[nodiscard]] TextureSheets&   textures() { return m_textures; }

    [[nodiscard]] std::span<const u32> layer(int index) const;

private:
    void plan_layers();
    void track_scroll();

    Scene         m_scene;
    TextureSheets m_textures;
    TileGen       m_tilegen;
    FramePlan     m_plan;
    std::array<u32, 4>  m_scroll_hash{};
    std::array<u32, 4>  m_scroll_age{};  // frames since the layer last scrolled
    std::array<bool, 4> m_scrolled{};
    std::array<bool, 4> m_scroll_known{};
    bool m_has_frame      = false;
    u64  m_frame_number   = 0;
};

}  // namespace sm2::render::model3
