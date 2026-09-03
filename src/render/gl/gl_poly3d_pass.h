//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Draws the emulated 3D output. GL analogue of render::vk::Poly3DPass -- see
// that class's own doc comment for the hardware reasoning this pass exists
// to satisfy (no depth buffer, native-resolution rasterisation); the
// algorithm is unchanged here, only the GL entry points differ.
//
// No frames-in-flight ring, unlike the Vulkan path: GL's driver manages
// buffer lifetime itself, so there is exactly one copy of each resource, not
// three -- see gl_context.h's own doc comment and design.md sec 3 for why
// this is correct rather than a simplification made for convenience.
#pragma once

#include "render/gl/gl_common.h"
#include "render/geometry.h"

#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::render::gl {

class Poly3DPass {
public:
    static constexpr u32 kWidth  = render::kNativeWidth;
    static constexpr u32 kHeight = render::kNativeHeight;

    /// Words in one texture sheet -- see render::vk::Poly3DPass::kSheetWords
    /// for why only the low half of texture RAM is uploaded.
    static constexpr u32 kSheetWords = 0x40000;

    /// Physical texel containers per sheet, matching
    /// render::vk::Poly3DPass::kDecodedWidth/kDecodedHeight exactly.
    static constexpr u32 kDecodedWidth  = 512;
    static constexpr u32 kDecodedHeight = 1024;

    Poly3DPass() = default;
    ~Poly3DPass();

    Poly3DPass(const Poly3DPass&)            = delete;
    Poly3DPass& operator=(const Poly3DPass&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    /// As render::vk::Poly3DPass::build(): triangulate this frame's polygons
    /// and refresh whatever machine memory changed since it last ran.
    void build(const hw::Model2MachineBase* machine, const hw::Model2Video& video);

    /// Draw what build() prepared directly into the bound native framebuffer,
    /// between the below and above tilemap layers. Clears the fill-mask stencil,
    /// then blends the 3D over the below-tilemap with premultiplied-over -- the
    /// same result the old draw-to-offscreen then composite produced.
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

    // -- the 3D draw ------------------------------------------------------

    u32 m_polygon_program = 0;
    u32 m_polygon_program_early = 0;  ///< early-fragment-tests variant
    u32 m_polygon_push_ubo = 0;  ///< backing buffer for polygon.vert's Push block
    u32 m_vao = 0;

    PersistentBuffer m_vertex_buffer;
    PersistentBuffer m_polygon_buffer;

    // -- texture decode -----------------------------------------------------

    u32 m_decode_program = 0;
    u32 m_decoded_texture = 0;  ///< GL_TEXTURE_2D_ARRAY, RGBA8UI, 2 layers
    PersistentBuffer m_sheets_buffer;

    // -- tone curve --------------------------------------------------------

    u32 m_tone_texture = 0;
    PersistentBuffer m_luma_buffer;

    /// Change counters of the machine data this pass's copies were made
    /// from. Zero means never copied, matching the Vulkan path's convention.
    u64 m_texture_generation = 0;
    u64 m_table_generation   = 0;

    render::TriangulatedFrame m_frame_geometry;
    std::vector<u32>          m_tone_curve;

    u32  m_vertex_count    = 0;
    bool m_capacity_warned = false;
};

}  // namespace sm2::render::gl
