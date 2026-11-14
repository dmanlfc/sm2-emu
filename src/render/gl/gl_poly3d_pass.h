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
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Draws the emulated 3D output: the GL counterpart of render::vk::Poly3DPass,
// whose comment explains the algorithm. GL manages buffer lifetime itself, so
// there is one copy of each resource rather than a frames-in-flight ring.
#pragma once

#include "render/gl/gl_common.h"
#include "render/geometry.h"

#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::render {
class TextureReplacements;
}

namespace sm2::render::gl {

class Poly3DPass {
public:
    static constexpr u32 kWidth  = render::kNativeWidth;
    static constexpr u32 kHeight = render::kNativeHeight;

    /// Words in one texture sheet; see render::vk::Poly3DPass::kSheetWords.
    static constexpr u32 kSheetWords = 0x40000;

    /// Physical texel containers per sheet, as in render::vk::Poly3DPass.
    static constexpr u32 kDecodedWidth  = 512;
    static constexpr u32 kDecodedHeight = 1024;

    Poly3DPass() = default;
    ~Poly3DPass();

    Poly3DPass(const Poly3DPass&)            = delete;
    Poly3DPass& operator=(const Poly3DPass&) = delete;

    /// `render_scale` (N) scales the per-batch scissor to the N*native target;
    /// the vertex path stays in native coordinates. Identity at N=1.
    [[nodiscard]] bool init(u32 render_scale);

    /// Raster width to draw at, in native pixels; live.
    void set_width(u32 width) { m_width = width; }
    void shutdown();

    /// Anisotropic texture-filter quality (0 = faithful single-tap, else the
    /// tap ceiling). Uploaded to the shader each draw; live, no realloc.
    void set_texture_quality(u32 quality) { m_texture_quality = quality; }

    /// Draw the stipple as the 50% blend it's trying to achieve.
    void set_blend_translucency(bool blend) { m_blend_translucency = blend; }

    /// Custom textures to draw in place of the hardware's, or null for none.
    void set_texture_replacements(TextureReplacements* replacements)
    {
        m_replacements = replacements;
    }

    /// As render::vk::Poly3DPass::build(): triangulate this frame's polygons
    /// and refresh whatever machine memory changed since it last ran.
    void build(const hw::Model2MachineBase* machine, const hw::Model2Video& video);

    /// Draw what build() prepared into the bound native framebuffer, between
    /// the below and above tilemap layers: clears the fill-mask stencil, then
    /// blends premultiplied-over the below layer.
    void draw_polygons();

    // -- diagnostics -------------------------------------------------------

    [[nodiscard]] u32 triangles() const { return m_vertex_count / 3; }
    [[nodiscard]] u32 drawn_polygons() const { return m_frame_geometry.drawn_polygons; }
    [[nodiscard]] u32 blank_polygons() const { return m_frame_geometry.blank_polygons; }

private:
    [[nodiscard]] bool create_programs();
    [[nodiscard]] bool create_buffers();
    void               refresh_machine_data(const hw::Model2MachineBase& machine,
                                           const hw::Model2Video&       video);
    void               decode_textures();
    /// GL 4.1: texel_decode.comp's unpacking on the CPU.
    void               decode_textures_cpu(const std::vector<u32>& words);
    /// Bring the atlas in line with m_replacements.
    void               sync_replacements();

    // -- the 3D draw ------------------------------------------------------

    u32 m_polygon_program = 0;
    u32 m_polygon_program_early = 0;  ///< early-fragment-tests variant
    u32 m_polygon_push_ubo = 0;  ///< backing buffer for polygon.vert's Push block
    u32 m_vao = 0;

    PersistentBuffer m_vertex_buffer;
    PersistentBuffer m_polygon_buffer;
    PersistentBuffer m_geometry_buffer;

    /// GL 4.1: buffer textures over the luma, polygon and geometry buffers,
    /// standing in for polygon.frag's storage buffers.
    u32 m_buffer_textures[3] = {};

    // -- texture decode -----------------------------------------------------

    u32 m_decode_program = 0;
    u32 m_decoded_texture = 0;  ///< GL_TEXTURE_2D_ARRAY, RGBA8UI, 2 layers
    PersistentBuffer m_sheets_buffer;

    // -- tone curve --------------------------------------------------------

    u32 m_tone_texture = 0;
    PersistentBuffer m_luma_buffer;

    /// Change counters of the machine data last copied; zero means never.
    u64 m_texture_generation = 0;
    u64 m_table_generation   = 0;

    render::TriangulatedFrame m_frame_geometry;
    std::vector<u32>          m_tone_curve;

    u32  m_vertex_count    = 0;
    bool m_capacity_warned = false;

    /// Internal 3D render scale; the per-batch scissor is scaled by N.
    u32 m_render_scale = 1;
    u32 m_width        = kWidth;

    u32 m_texture_quality = 0;  ///< 0 = faithful single-tap, else tap ceiling

    bool m_blend_translucency = false;

    // -- custom textures ---------------------------------------------------

    TextureReplacements* m_replacements     = nullptr;
    u32                  m_atlas_texture    = 0;  ///< GL_TEXTURE_2D_ARRAY, RGBA8
    u64                  m_atlas_generation = 0;
    bool                 m_atlas_live       = false;
};

}  // namespace sm2::render::gl
