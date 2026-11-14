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
// R3DFrameBuffers.cpp, R3DShader.cpp and TextureBank.cpp, Copyright (c)
// 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Model 3 on GL 4.3 / GLES 3.1: the Real3D passes, composited between the
// tile layers.
#pragma once

#include "render/gl/gl_common.h"
#include "render/model3/model3_frame.h"

#include <array>

namespace sm2::render {
class TextureReplacements;
}

namespace sm2::render::gl {

class Model3Pass {
public:
    Model3Pass() = default;
    ~Model3Pass();

    Model3Pass(const Model3Pass&)            = delete;
    Model3Pass& operator=(const Model3Pass&) = delete;

    [[nodiscard]] bool init(u32 render_scale, u32 width);
    /// keep_atlas holds on to the custom texture atlas.
    void shutdown(bool keep_atlas = false);
    [[nodiscard]] bool ready() const { return m_ready; }

    void set_smooth_2d(bool smooth) { m_smooth_2d = smooth; }
    /// Anisotropic taps in the 3D pass; 1 is the hardware's own filter.
    void set_aniso_taps(u32 taps) { m_aniso_taps = taps; }
    void set_texture_replacements(TextureReplacements* replacements) { m_replacements = replacements; }

    void render(model3::FrameBuilder& frame, u32 native_fbo);

    [[nodiscard]] const model3::LineOfSightMailbox& line_of_sight() const { return m_los; }

private:
    [[nodiscard]] bool create_programs();
    [[nodiscard]] bool create_targets();
    [[nodiscard]] bool create_textures();
    [[nodiscard]] bool create_buffers();

    void upload_textures(model3::TextureSheets& sheets);
    void sync_atlas();
    void upload_vertices(const model3::Scene& scene);
    void upload_uniforms(const model3::FramePlan& plan);
    void draw_layer(u32 texture, const model3::LayerDraw& d);
    void draw_range(const model3::FramePlan& plan, model3::DrawRange range, bool colour_pass);
    void draw_3d(const model3::FramePlan& plan, u64 frame_number);

    bool m_ready     = false;
    bool m_smooth_2d = false;
    u32  m_aniso_taps = 1;
    u32  m_render_scale = 1;
    u32  m_width        = 0;  ///< Target size, scaled.
    u32  m_height       = 0;

    // programs
    u32 m_r3d_program       = 0;
    u32 m_r3d_aniso_program = 0;
    u32 m_fog_program       = 0;
    u32 m_base_program      = 0;
    u32 m_trans_program     = 0;
    u32 m_layer_program     = 0;
    u32 m_empty_vao         = 0;

    // Real3D targets
    u32 m_r3d_fbo        = 0;
    u32 m_copy_fbo       = 0;
    u32 m_colour[3]      = {0, 0, 0};
    u32 m_los_rb         = 0;
    u32 m_depth_rb       = 0;
    u32 m_depth_copy_rb  = 0;

    // inputs
    u32 m_bank      = 0;
    u32 m_layers[2] = {0, 0};
    u32 m_layer_width[2] = {0, 0};
    u32 m_vao       = 0;
    u32 m_vbo       = 0;
    u64 m_rom_generation = 0;
    usize m_rom_uploaded = 0;

    // uniform buffers: R3D blocks, model blocks, fog blocks, layer block
    u32   m_ubo[4]          = {0, 0, 0, 0};
    usize m_ubo_size[4]     = {0, 0, 0, 0};
    u32   m_ubo_alignment   = 256;
    u32   m_uniform_stride  = 0;
    u32   m_model_stride    = 0;
    u32   m_fog_stride      = 0;

    // Custom textures; their pixels go once uploaded.
    TextureReplacements* m_replacements     = nullptr;
    u32                  m_atlas            = 0;
    u64                  m_atlas_generation = 0;
    bool                 m_atlas_live       = false;

    model3::LineOfSightMailbox m_los;
    struct LosSlot {
        model3::LineOfSight los;
        bool                set = false;
    };
    std::array<LosSlot, 4> m_los_slots{};  // the Vulkan pass's kFramesInFlight + 1
    u64                    m_los_records = 0;
};

}  // namespace sm2::render::gl
