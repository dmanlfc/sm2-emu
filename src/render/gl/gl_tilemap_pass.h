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
// Draws the emulated 2D output into the native frame. GL analogue of
// render::vk::TilemapPass -- see that class's own doc comment for the
// hardware reasoning (why two surfaces, why native resolution, why the
// compute path exists at all); nothing about the algorithm differs here,
// only the GL entry points that carry it out.
#pragma once

#include "render/gl/gl_common.h"
#include "render/geometry.h"

#include <span>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::render::gl {

class TilemapPass {
public:
    static constexpr u32 kSourceWidth  = render::kNativeWidth;
    static constexpr u32 kSourceHeight = render::kNativeHeight;

    TilemapPass() = default;
    ~TilemapPass();

    TilemapPass(const TilemapPass&)            = delete;
    TilemapPass& operator=(const TilemapPass&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    /// As render::vk::TilemapPass::compute(): dispatch the compose shader
    /// against tile RAM, character RAM and the pen table, refreshing this
    /// pass's own copies first if their generation counters changed.
    void compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// As render::vk::TilemapPass::upload(): the CPU-composited path, for
    /// when compute() does not apply (render test mode, or a --soft-render
    /// comparison capture).
    void upload(std::span<const u32> below, std::span<const u32> above);

    /// Begin drawing into the currently bound framebuffer: resolve the below
    /// layers against the background colour, covering every pixel.
    void draw_below(u32 background_rgba);

    /// Draw the above layers over whatever is in the target.
    void draw_above();

private:
    [[nodiscard]] bool create_textures();
    [[nodiscard]] bool create_programs();
    [[nodiscard]] bool create_compute_resources();
    void               upload_surface_to_texture(u32 texture, std::span<const u32> pixels);
    void               dispatch_compose(const hw::Model2MachineBase& machine,
                                       const hw::Model2Video&       video);
    void               draw_fullscreen(u32 texture, u32 mode, bool blend, u32 background_rgba);

    u32 m_below_texture = 0;
    u32 m_above_texture = 0;

    // One program, not two: the Vulkan path bakes blend-enable into the
    // pipeline object and so needs a pipeline per state even though the
    // shader is identical (render::vk::TilemapPass::create_pipelines()'s own
    // create_pipeline(bool blend, ...) confirms this directly). GL's blend
    // state is a separate draw-time call (Enable(GL_BLEND)/BlendFunc), not
    // part of the program, so one compile serves both draws here.
    u32 m_program  = 0;
    u32 m_uniform_block_index = 0;

    u32 m_vao      = 0;  ///< empty VAO; fullscreen_quad.vert reads no vertex data
    u32 m_push_ubo = 0;  ///< backing buffer for the composite shader's Push block

    // -- the compute path -----------------------------------------------------

    u32               m_compute_program = 0;
    PersistentBuffer  m_tile_ram;
    PersistentBuffer  m_char_ram;
    PersistentBuffer  m_pens;
    PersistentBuffer  m_below_staging;  ///< the compute shader's writeonly target
    PersistentBuffer  m_above_staging;

    u64 m_tile_generation  = 0;
    u64 m_char_generation  = 0;
    u64 m_table_generation = 0;

    static constexpr usize kTileRamBytes = 0x10000;
    static constexpr usize kCharRamBytes = 0x80000;
    static constexpr usize kPenCount     = 0x1000;
};

}  // namespace sm2::render::gl
