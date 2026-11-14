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

/// A game's sky panorama names, two to a word.
constexpr usize kSkyWords = hw::Model2MachineBase::SkyPanorama::kCells
                          * hw::Model2MachineBase::SkyPanorama::kRows / 2;

constexpr u32 kModeResolveOverBackground = 0;
constexpr u32 kModeBlendOver             = 1;

}  // namespace

TilemapPass::~TilemapPass()
{
    shutdown();
}

bool TilemapPass::init(u32 columns)
{
    m_columns = columns;
    m_panorama.reset();
    m_sky_generation = 0;
    m_panorama_state = {};
    // GL 4.1 has no compute: the caller uploads the CPU composite instead.
    return create_textures() && create_programs() && (is_gl41() || create_compute_resources());
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
    if (m_compute_ubo != 0) {
        DeleteBuffers(1, &m_compute_ubo);
        m_compute_ubo = 0;
    }
    if (m_panorama_buffer != 0) {
        DeleteBuffers(1, &m_panorama_buffer);
        m_panorama_buffer = 0;
    }
    m_tile_generation  = 0;
    m_char_generation  = 0;
    m_table_generation = 0;
    destroy_persistent_buffer(&m_tile_ram);
    destroy_persistent_buffer(&m_char_ram);
    destroy_persistent_buffer(&m_pens);
    destroy_persistent_buffer(&m_below_staging);
    destroy_persistent_buffer(&m_above_staging);
}

bool TilemapPass::create_textures()
{
    // NEAREST: the N*native target upscales each texel to a crisp NxN block;
    // the final magnification to the window is PresentPass's.
    GenTextures(1, &m_below_texture);
    BindTexture(GL_TEXTURE_2D, m_below_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width()),
                static_cast<GLsizei>(kSourceHeight));
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GenTextures(1, &m_above_texture);
    BindTexture(GL_TEXTURE_2D, m_above_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width()),
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
    // std140 Push: vec4 background(0), uint mode(16), uint upscale(20),
    // vec2 source_size(24), padded to 32.
    BufferData(GL_UNIFORM_BUFFER, 32, nullptr, GL_DYNAMIC_DRAW);

    const std::string vertex_source =
        prepare_gl_source(shaders::kFullscreenQuadVertGlsl, active_version_directive());
    const std::string fragment_source =
        prepare_gl_source(shaders::kTilemapCompositeFragGlsl, active_version_directive());

    m_program = compile_program(vertex_source.c_str(), fragment_source.c_str());
    if (m_program == 0) {
        return false;
    }

    // tilemap_composite.frag's Push block is binding 1 under SM2_TARGET_GL
    // (binding 0 is uLayer).
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
    m_below_staging = create_persistent_buffer(surface_bytes(), GL_SHADER_STORAGE_BUFFER);
    m_above_staging = create_persistent_buffer(surface_bytes(), GL_SHADER_STORAGE_BUFFER);
    GenBuffers(1, &m_compute_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_compute_ubo);
    BufferData(GL_UNIFORM_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);

    // Zeroed: an entry from epoch zero is empty. A game's sky panorama names
    // follow the cache.
    const std::vector<u32> zeros(
        static_cast<usize>(4) * PanoramaTracker::kWidth * PanoramaTracker::kHeight * 2
            + kSkyWords,
        0);
    GenBuffers(1, &m_panorama_buffer);
    BindBuffer(GL_SHADER_STORAGE_BUFFER, m_panorama_buffer);
    BufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(zeros.size() * sizeof(u32)),
               zeros.data(), GL_DYNAMIC_COPY);
    return m_tile_ram.handle != 0 && m_char_ram.handle != 0 && m_pens.handle != 0
        && m_below_staging.handle != 0 && m_above_staging.handle != 0;
}

void TilemapPass::upload_surface_to_texture(u32 texture, std::span<const u32> pixels)
{
    BindTexture(GL_TEXTURE_2D, texture);
    // The CPU composes this surface top-origin, but the GL path stores tilemap
    // textures bottom-origin (as tilemap_compose.comp writes them under
    // SM2_TARGET_GL), so flip it. Used by render test mode and the CPU tilemap
    // fallback.
    std::vector<u32> flipped(static_cast<usize>(width()) * kSourceHeight);
    for (u32 y = 0; y < kSourceHeight; ++y) {
        const u32 src_row = kSourceHeight - 1 - y;
        std::memcpy(flipped.data() + static_cast<usize>(y) * width() + m_columns,
                    pixels.data() + static_cast<usize>(src_row) * kSourceWidth,
                    static_cast<usize>(kSourceWidth) * sizeof(u32));
    }
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width()),
                 static_cast<GLsizei>(kSourceHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                 flipped.data());
}

void TilemapPass::upload_surface_from_buffer(u32 texture, u32 buffer_handle)
{
    // Bind the SSBO as a pixel-unpack source so TexSubImage2D copies on the GPU.
    BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer_handle);
    BindTexture(GL_TEXTURE_2D, texture);
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width()),
                 static_cast<GLsizei>(kSourceHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);  // null = read from bound PBO at offset 0
    BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void TilemapPass::upload(std::span<const u32> below, std::span<const u32> above)
{
    upload_surface_to_texture(m_below_texture, below);
    upload_surface_to_texture(m_above_texture, above);

    // This slot's staging buffers now hold a CPU-composited frame, so force the
    // next compute() to redo the dispatch whatever the generation counters say.
    m_tile_generation  = 0;
    m_char_generation  = 0;
    m_table_generation = 0;
}

void TilemapPass::dispatch_compose(const hw::Model2MachineBase& machine,
                                   const hw::Model2Video&       video)
{
    const bool tile_changed  = m_tile_generation != machine.tile_generation()
                            || m_window_mask_a_hidden != video.window_mask_a_hidden();
    const bool char_changed  = m_char_generation != machine.char_generation();
    const bool table_changed = m_table_generation != machine.table_generation();
    if (!tile_changed && !char_changed && !table_changed && m_composed_wide == m_wide_below) {
        return;
    }
    if (m_columns != 0 && tile_changed) {
        m_panorama_state = m_panorama.update(machine.tile_ram());
    }
    const hw::Model2MachineBase::SkyPanorama* sky =
        m_columns != 0 ? machine.sky_panorama() : nullptr;
    if (sky != nullptr && m_sky_generation != sky->generation) {
        BindBuffer(GL_SHADER_STORAGE_BUFFER, m_panorama_buffer);
        BufferSubData(GL_SHADER_STORAGE_BUFFER,
                      static_cast<GLintptr>(static_cast<usize>(4) * PanoramaTracker::kWidth
                                            * PanoramaTracker::kHeight * 2 * sizeof(u32)),
                      static_cast<GLsizeiptr>(kSkyWords * sizeof(u32)), sky->names.data());
        m_sky_generation = sky->generation;
    }
    m_composed_wide = m_wide_below;

    if (tile_changed) {
        const std::span<const u8> tile_ram = machine.tile_ram();
        const usize bytes = std::min<usize>(kTileRamBytes, tile_ram.size());
        m_window_mask_a_hidden = video.window_mask_a_hidden();
        if (m_window_mask_a_hidden && bytes >= hw::Segaic24Tile::kWindowMaskAOffset
                                                + hw::Segaic24Tile::kWindowMaskABytes) {
            m_tile_ram_scratch.assign(tile_ram.begin(), tile_ram.begin() + bytes);
            std::memset(m_tile_ram_scratch.data() + hw::Segaic24Tile::kWindowMaskAOffset, 0,
                        hw::Segaic24Tile::kWindowMaskABytes);
            m_tile_ram.write(m_tile_ram_scratch.data(), bytes);
        } else {
            m_tile_ram.write(tile_ram.data(), bytes);
        }
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

    u32 push[32] = {width(), m_columns, m_wide_below, m_panorama_state.cacheable};
    for (u32 i = 0; i < 4; ++i) push[28 + i] = m_hud_bands[i];
    if (sky != nullptr) {
        push[16] = sky->layer + 1;
        push[17] = sky->origin;
        push[18] = sky->first_row;
        push[19] = sky->rows;
    }
    for (u32 layer = 0; layer < 4; ++layer) {
        push[4 + layer] = m_panorama_state.offset[layer];
        push[8 + layer]  = m_panorama_state.offset_y[layer];
        push[12 + layer] = m_panorama_state.epoch[layer];
        push[20 + layer] = static_cast<u32>(m_panorama_state.wrap_rows[layer]);
        push[24 + layer] = static_cast<u32>(m_panorama_state.wrap_rows[layer] >> 32);
    }
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_panorama_buffer);
    BindBuffer(GL_UNIFORM_BUFFER, m_compute_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(push), push);
    BindBufferBase(GL_UNIFORM_BUFFER, 0, m_compute_ubo);

    const u32 groups_x = (width() + 15) / 16;
    const u32 groups_y = (kSourceHeight + 15) / 16;
    DispatchCompute(groups_x, groups_y, 1);

    // The compute shader's writes must be visible before the copy to textures.
    MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Copy the compute results to textures on the GPU through a PBO bind. The
    // persistent mapping is write-only, so it cannot be read back.
    upload_surface_from_buffer(m_below_texture, m_below_staging.handle);
    upload_surface_from_buffer(m_above_texture, m_above_staging.handle);
}

void TilemapPass::compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video)
{
    if (m_compute_program == 0) {
        return;
    }
    dispatch_compose(machine, video);
}

void TilemapPass::draw_fullscreen(u32 texture, u32 mode, bool blend, u32 background_rgba)
{
    struct PushBlock {
        float background[4];
        u32   mode;
        u32   upscale;
        float source_size[2];
    } push{};
    push.background[0] = static_cast<float>(background_rgba & 0xff) / 255.0F;
    push.background[1] = static_cast<float>((background_rgba >> 8) & 0xff) / 255.0F;
    push.background[2] = static_cast<float>((background_rgba >> 16) & 0xff) / 255.0F;
    push.background[3] = 1.0F;
    push.mode           = mode;
    push.upscale        = m_upscale_2d;
    push.source_size[0] = static_cast<float>(width());
    push.source_size[1] = static_cast<float>(kSourceHeight);

    UseProgram(m_program);

    // The uniform block standing in for Vulkan's push constant, rewritten
    // every draw.
    BindBuffer(GL_UNIFORM_BUFFER, m_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(PushBlock)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 1, m_push_ubo);

    // Premultiplied source: ONE / ONE_MINUS_SRC_ALPHA, as in the Vulkan pass.
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
