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
#include "render/gl/gl_tilemap_pass.h"

#include "core/log.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include "shaders/fullscreen_quad_vert_glsl.h"
#include "shaders/tilemap_compose_comp_glsl.h"
#include "shaders/tilemap_composite_frag_glsl.h"

#include <cstring>
#include <vector>

namespace sm2::render::gl {
namespace {

constexpr u32 kModeResolveOverBackground = 0;
constexpr u32 kModeBlendOver             = 1;

constexpr usize kSurfaceBytes =
    static_cast<usize>(TilemapPass::kSourceWidth) * TilemapPass::kSourceHeight * sizeof(u32);

}  // namespace

TilemapPass::~TilemapPass()
{
    shutdown();
}

bool TilemapPass::init()
{
    return create_textures() && create_programs() && create_compute_resources();
}

void TilemapPass::shutdown()
{
    if (m_below_texture != 0) {
        DeleteTextures(1, &m_below_texture);
        m_below_texture = 0;
    }
    if (m_above_texture != 0) {
        DeleteTextures(1, &m_above_texture);
        m_above_texture = 0;
    }
    if (m_program != 0) {
        DeleteProgram(m_program);
        m_program = 0;
    }
    if (m_vao != 0) {
        DeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_push_ubo != 0) {
        DeleteBuffers(1, &m_push_ubo);
        m_push_ubo = 0;
    }
    if (m_compute_program != 0) {
        DeleteProgram(m_compute_program);
        m_compute_program = 0;
    }
    destroy_persistent_buffer(&m_tile_ram);
    destroy_persistent_buffer(&m_char_ram);
    destroy_persistent_buffer(&m_pens);
    destroy_persistent_buffer(&m_below_staging);
    destroy_persistent_buffer(&m_above_staging);
}

bool TilemapPass::create_textures()
{
    GenTextures(1, &m_below_texture);
    BindTexture(GL_TEXTURE_2D, m_below_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(kSourceWidth),
                static_cast<GLsizei>(kSourceHeight));
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GenTextures(1, &m_above_texture);
    BindTexture(GL_TEXTURE_2D, m_above_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(kSourceWidth),
                static_cast<GLsizei>(kSourceHeight));
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return m_below_texture != 0 && m_above_texture != 0;
}

bool TilemapPass::create_programs()
{
    GenVertexArrays(1, &m_vao);
    GenBuffers(1, &m_push_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_push_ubo);
    BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 4 + sizeof(u32)),
              nullptr, GL_DYNAMIC_DRAW);

    const std::string vertex_source =
        prepare_gl_source(shaders::kFullscreenQuadVertGlsl, active_version_directive());
    const std::string fragment_source =
        prepare_gl_source(shaders::kTilemapCompositeFragGlsl, active_version_directive());

    m_program = compile_program(vertex_source.c_str(), fragment_source.c_str());
    if (m_program == 0) {
        return false;
    }

    // tilemap_composite.frag's Push block took binding 1 under SM2_TARGET_GL
    // (binding 0 is uLayer) -- see that shader's own SM2_TARGET_GL comment.
    m_uniform_block_index = GetUniformBlockIndex(m_program, "Push");
    UniformBlockBinding(m_program, m_uniform_block_index, 1);
    return true;
}

bool TilemapPass::create_compute_resources()
{
    const std::string compute_source =
        prepare_gl_source(shaders::kTilemapComposeCompGlsl, active_version_directive());
    m_compute_program = compile_compute_program(compute_source.c_str());
    if (m_compute_program == 0) {
        return false;
    }

    m_tile_ram      = create_persistent_buffer(kTileRamBytes, GL_SHADER_STORAGE_BUFFER);
    m_char_ram      = create_persistent_buffer(kCharRamBytes, GL_SHADER_STORAGE_BUFFER);
    m_pens          = create_persistent_buffer(static_cast<usize>(kPenCount) * sizeof(u32),
                                               GL_SHADER_STORAGE_BUFFER);
    m_below_staging = create_persistent_buffer(kSurfaceBytes, GL_SHADER_STORAGE_BUFFER);
    m_above_staging = create_persistent_buffer(kSurfaceBytes, GL_SHADER_STORAGE_BUFFER);
    return m_tile_ram.handle != 0 && m_char_ram.handle != 0 && m_pens.handle != 0
        && m_below_staging.handle != 0 && m_above_staging.handle != 0;
}

void TilemapPass::upload_surface_to_texture(u32 texture, std::span<const u32> pixels)
{
    BindTexture(GL_TEXTURE_2D, texture);
    // The CPU composes this surface top-origin (row 0 = top), but the GL path
    // stores tilemap textures bottom-origin so fullscreen_quad.vert's clip-Y
    // flip lands them right-side-up -- the same convention tilemap_compose.comp
    // writes under SM2_TARGET_GL. This is the only tilemap upload that isn't
    // already GPU-side, so it flips here. Reached by render test mode (Last
    // Bronx's framebuffer title) and the CPU tilemap fallback.
    std::vector<u32> flipped(static_cast<usize>(kSourceWidth) * kSourceHeight);
    for (u32 y = 0; y < kSourceHeight; ++y) {
        const u32 src_row = kSourceHeight - 1 - y;
        std::memcpy(flipped.data() + static_cast<usize>(y) * kSourceWidth,
                    pixels.data() + static_cast<usize>(src_row) * kSourceWidth,
                    static_cast<usize>(kSourceWidth) * sizeof(u32));
    }
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(kSourceWidth),
                 static_cast<GLsizei>(kSourceHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                 flipped.data());
}

void TilemapPass::upload_surface_from_buffer(u32 texture, u32 buffer_handle)
{
    // Bind the SSBO as a pixel-unpack source so TexSubImage2D reads from it
    // directly on the GPU rather than from system memory. This is the GL
    // equivalent of Vulkan's vkCmdCopyBufferToImage.
    BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer_handle);
    BindTexture(GL_TEXTURE_2D, texture);
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(kSourceWidth),
                 static_cast<GLsizei>(kSourceHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);  // null = read from bound PBO at offset 0
    BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void TilemapPass::upload(std::span<const u32> below, std::span<const u32> above)
{
    upload_surface_to_texture(m_below_texture, below);
    upload_surface_to_texture(m_above_texture, above);

    // This slot's staging buffers now hold a CPU-composited frame, not
    // whatever compute() last wrote -- force its next call to redo the
    // dispatch regardless of whether the generation counters moved, matching
    // render::vk::TilemapPass::upload()'s own reasoning exactly.
    m_tile_generation  = 0;
    m_char_generation  = 0;
    m_table_generation = 0;
}

void TilemapPass::dispatch_compose(const hw::Model2MachineBase& machine,
                                   const hw::Model2Video&       video)
{
    const bool tile_changed  = m_tile_generation != machine.tile_generation();
    const bool char_changed  = m_char_generation != machine.char_generation();
    const bool table_changed = m_table_generation != machine.table_generation();
    if (!tile_changed && !char_changed && !table_changed) {
        return;
    }

    if (tile_changed) {
        const std::span<const u8> tile_ram = machine.tile_ram();
        m_tile_ram.write(tile_ram.data(), std::min<usize>(kTileRamBytes, tile_ram.size()));
        m_tile_generation = machine.tile_generation();
    }
    if (char_changed) {
        const std::span<const u8> char_ram = machine.char_ram();
        m_char_ram.write(char_ram.data(), std::min<usize>(kCharRamBytes, char_ram.size()));
        m_char_generation = machine.char_generation();
    }
    if (table_changed) {
        const std::span<const u32> pens = video.pens();
        m_pens.write(pens.data(),
                     std::min<usize>(kPenCount, pens.size()) * sizeof(u32));
        m_table_generation = machine.table_generation();
    }

    UseProgram(m_compute_program);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_tile_ram.handle);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_char_ram.handle);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_pens.handle);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_below_staging.handle);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_above_staging.handle);

    const u32 groups_x = (kSourceWidth + 15) / 16;
    const u32 groups_y = (kSourceHeight + 15) / 16;
    DispatchCompute(groups_x, groups_y, 1);

    // The composite draws sample the textures these buffers are about to be
    // copied into, so the compute shader's writes must be visible before
    // that copy reads them. The GL analogue of the one VkBufferMemoryBarrier2
    // render::vk::TilemapPass::dispatch_compose() records for the same
    // reason.
    MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Upload compute results to textures via PBO bind rather than CPU
    // readback: binding the staging SSBO as GL_PIXEL_UNPACK_BUFFER and
    // calling TexSubImage2D with a null data pointer tells the driver to
    // source the pixel data from the bound buffer object -- a pure GPU-side
    // copy, matching the Vulkan path's vkCmdCopyBufferToImage. Reading from
    // the persistently mapped pointer would be undefined (the mapping is
    // write-only) and forces an implicit pipeline stall in practice.
    upload_surface_from_buffer(m_below_texture, m_below_staging.handle);
    upload_surface_from_buffer(m_above_texture, m_above_staging.handle);
}

void TilemapPass::compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video)
{
    dispatch_compose(machine, video);
}

void TilemapPass::draw_fullscreen(u32 texture, u32 mode, bool blend, u32 background_rgba)
{
    struct PushBlock {
        float background[4];
        u32   mode;
    } push{};
    push.background[0] = static_cast<float>(background_rgba & 0xff) / 255.0F;
    push.background[1] = static_cast<float>((background_rgba >> 8) & 0xff) / 255.0F;
    push.background[2] = static_cast<float>((background_rgba >> 16) & 0xff) / 255.0F;
    push.background[3] = 1.0F;
    push.mode           = mode;

    UseProgram(m_program);

    // A uniform block (this shader's GL-side substitute for Vulkan's push
    // constant, see tilemap_composite.frag's own SM2_TARGET_GL comment)
    // needs a real backing buffer, unlike a push constant -- m_push_ubo is
    // that buffer, created once in create_programs() and rewritten here
    // every draw.
    BindBuffer(GL_UNIFORM_BUFFER, m_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(PushBlock)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 1, m_push_ubo);

    // Premultiplied source, matching render::vk::TilemapPass::create_pipeline's
    // own blend factors exactly: ONE / ONE_MINUS_SRC_ALPHA on both channels.
    if (blend) {
        Enable(GL_BLEND);
        BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        Disable(GL_BLEND);
    }

    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D, texture);

    BindVertexArray(m_vao);
    DrawArrays(GL_TRIANGLES, 0, 3);
}

void TilemapPass::draw_below(u32 background_rgba)
{
    draw_fullscreen(m_below_texture, kModeResolveOverBackground, false, background_rgba);
}

void TilemapPass::draw_above()
{
    draw_fullscreen(m_above_texture, kModeBlendOver, true, 0);
}

}  // namespace sm2::render::gl
