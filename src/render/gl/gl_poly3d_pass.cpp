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
#include "render/gl/gl_poly3d_pass.h"

#include "core/log.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"
#include "render/texture_replace.h"

#include "shaders/polygon_frag_early_glsl.h"
#include "shaders/polygon_frag_glsl.h"
#include "shaders/polygon_vert_glsl.h"
#include "shaders/texel_decode_comp_glsl.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::gl {
namespace {

constexpr usize kLumaBytes = 0x8000;

}  // namespace

Poly3DPass::~Poly3DPass()
{
    shutdown();
}

bool Poly3DPass::init(u32 render_scale)
{
    m_render_scale = render_scale;
    m_frame_geometry.vertices.reserve(1 << 14);
    m_frame_geometry.polygons.reserve(1 << 12);
    m_tone_curve.assign(static_cast<usize>(hw::Model2Video::kToneShades)
                            * hw::Model2Video::kToneComponents,
                        0);

    return create_programs() && create_buffers();
}

void Poly3DPass::shutdown()
{
    if (m_decoded_texture != 0) {
        DeleteTextures(1, &m_decoded_texture);
        m_decoded_texture = 0;
    }
    if (m_atlas_texture != 0) {
        DeleteTextures(1, &m_atlas_texture);
        m_atlas_texture = 0;
    }
    if (m_tone_texture != 0) {
        DeleteTextures(1, &m_tone_texture);
        m_tone_texture = 0;
    }
    if (m_polygon_program != 0) {
        DeleteProgram(m_polygon_program);
        m_polygon_program = 0;
    }
    if (m_polygon_program_early != 0) {
        DeleteProgram(m_polygon_program_early);
        m_polygon_program_early = 0;
    }
    if (m_decode_program != 0) {
        DeleteProgram(m_decode_program);
        m_decode_program = 0;
    }
    if (m_vao != 0) {
        DeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_polygon_push_ubo != 0) {
        DeleteBuffers(1, &m_polygon_push_ubo);
        m_polygon_push_ubo = 0;
    }
    if (m_buffer_textures[0] != 0) {
        DeleteTextures(3, m_buffer_textures);
        std::fill(std::begin(m_buffer_textures), std::end(m_buffer_textures), 0U);
    }
    destroy_persistent_buffer(&m_vertex_buffer);
    destroy_persistent_buffer(&m_polygon_buffer);
    destroy_persistent_buffer(&m_geometry_buffer);
    destroy_persistent_buffer(&m_sheets_buffer);
    destroy_persistent_buffer(&m_luma_buffer);
}

bool Poly3DPass::create_programs()
{
    GenVertexArrays(1, &m_vao);

    const std::string vertex_source =
        prepare_gl_source(shaders::kPolygonVertGlsl, active_version_directive());
    const std::string fragment_source =
        prepare_gl_source(shaders::kPolygonFragGlsl, active_version_directive());
    m_polygon_program = compile_program(vertex_source.c_str(), fragment_source.c_str());
    if (m_polygon_program == 0) {
        return false;
    }

    // GL 4.1 has no early_fragment_tests, so there the variant is the same program.
    const std::string fragment_source_early = prepare_gl_source(
        shaders::kPolygonFragEarlyGlsl, active_version_directive(),
        is_gl41() ? nullptr : "SM2_EARLY_FRAGMENT_TESTS");
    m_polygon_program_early =
        compile_program(vertex_source.c_str(), fragment_source_early.c_str());
    if (m_polygon_program_early == 0) {
        return false;
    }

    // polygon.vert's Push block is at binding 4 under SM2_TARGET_GL.
    const u32 polygon_block = GetUniformBlockIndex(m_polygon_program, "Push");
    UniformBlockBinding(m_polygon_program, polygon_block, 4);
    const u32 polygon_block_early = GetUniformBlockIndex(m_polygon_program_early, "Push");
    UniformBlockBinding(m_polygon_program_early, polygon_block_early, 4);
    GenBuffers(1, &m_polygon_push_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_polygon_push_ubo);
    // vec2 invRaster + uint renderScale + uint textureQuality = 16 bytes (std140).
    BufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);

    if (is_gl41()) {
        return true;  // decoded on the CPU
    }
    const std::string decode_source =
        prepare_gl_source(shaders::kTexelDecodeCompGlsl, active_version_directive());
    m_decode_program = compile_compute_program(decode_source.c_str());
    if (m_decode_program == 0) {
        return false;
    }

    return true;
}

bool Poly3DPass::create_buffers()
{
    m_vertex_buffer =
        create_persistent_buffer(static_cast<usize>(render::kMaxVertices) * sizeof(render::Vertex),
                                 GL_ARRAY_BUFFER);
    const bool gl41   = is_gl41();
    const u32  target = gl41 ? GL_TEXTURE_BUFFER : GL_SHADER_STORAGE_BUFFER;
    m_polygon_buffer = create_persistent_buffer(
        static_cast<usize>(render::kMaxPolygons) * sizeof(render::PolyParams), target);
    m_geometry_buffer = create_persistent_buffer(
        static_cast<usize>(render::kMaxPolygons) * sizeof(render::PolyGeom), target);
    if (!gl41) {
        m_sheets_buffer =
            create_persistent_buffer(static_cast<usize>(kSheetWords) * 2 * sizeof(u32),
                                     GL_SHADER_STORAGE_BUFFER);
    }
    m_luma_buffer = create_persistent_buffer(kLumaBytes, target);

    if (gl41) {
        s32 max_texels = 0;
        GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
        const usize needed = m_geometry_buffer.size / 16;
        if (static_cast<usize>(max_texels) < needed) {
            SM2_ERROR("gl: GL_MAX_TEXTURE_BUFFER_SIZE %d is below the %zu texels the 3D pass "
                      "needs", max_texels, needed);
            return false;
        }
        // Units 6..8, matching polygon.frag's SM2_GL41 samplers.
        const u32    buffers[3] = {m_luma_buffer.handle, m_polygon_buffer.handle,
                                   m_geometry_buffer.handle};
        const GLenum formats[3] = {GL_R32UI, GL_RGBA32UI, GL_RGBA32UI};
        GenTextures(3, m_buffer_textures);
        for (u32 i = 0; i < 3; ++i) {
            BindTexture(GL_TEXTURE_BUFFER, m_buffer_textures[i]);
            TexBuffer(GL_TEXTURE_BUFFER, formats[i], buffers[i]);
        }
        BindTexture(GL_TEXTURE_BUFFER, 0);
    }

    GenTextures(1, &m_decoded_texture);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_decoded_texture);
    TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, static_cast<GLsizei>(kDecodedWidth),
                static_cast<GLsizei>(kDecodedHeight), 2);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    m_tone_texture = create_tone_texture(hw::Model2Video::kToneShades,
                                        hw::Model2Video::kToneComponents);

    // 1x1 placeholder until custom textures are loaded; never sampled.
    GenTextures(1, &m_atlas_texture);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_atlas_texture);
    TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);

    return m_vertex_buffer.handle != 0 && m_polygon_buffer.handle != 0
        && m_geometry_buffer.handle != 0
        && (gl41 || m_sheets_buffer.handle != 0) && m_luma_buffer.handle != 0
        && m_decoded_texture != 0 && m_tone_texture != 0;
}

void Poly3DPass::refresh_machine_data(const hw::Model2MachineBase& machine,
                                      const hw::Model2Video&       video)
{
    if (m_texture_generation != machine.texture_generation()) {
        // Only the low half of each two-megabyte window is addressable by the
        // texel arithmetic, as in the Vulkan pass.
        std::vector<u32> words(static_cast<usize>(kSheetWords) * 2, 0);
        for (int sheet = 0; sheet < 2; ++sheet) {
            const std::span<const u32> source = machine.texture_ram(sheet);
            const usize count = std::min<usize>(kSheetWords, source.size());
            std::memcpy(words.data() + static_cast<usize>(sheet) * kSheetWords, source.data(),
                        count * sizeof(u32));
        }
        if (is_gl41()) {
            decode_textures_cpu(words);
        } else {
            m_sheets_buffer.write(words.data(), words.size() * sizeof(u32));
            decode_textures();
        }
        m_texture_generation = machine.texture_generation();
    }

    if (m_table_generation == machine.table_generation()) {
        return;
    }
    m_table_generation = machine.table_generation();

    const std::span<const u8> luma = machine.luma_ram();
    m_luma_buffer.write(luma.data(), std::min(kLumaBytes, luma.size()));

    video.build_tone_curve(m_tone_curve);
    BindTexture(GL_TEXTURE_2D, m_tone_texture);
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                 static_cast<GLsizei>(hw::Model2Video::kToneShades),
                 static_cast<GLsizei>(hw::Model2Video::kToneComponents), GL_RGBA,
                 GL_UNSIGNED_BYTE, m_tone_curve.data());
}

void Poly3DPass::decode_textures()
{
    UseProgram(m_decode_program);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_sheets_buffer.handle);
    BindImageTexture(1, m_decoded_texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8UI);

    const u32 groups_x = (kDecodedWidth + 15) / 16;
    const u32 groups_y = (kDecodedHeight + 15) / 16;
    DispatchCompute(groups_x, groups_y, 2);

    // The polygon pass samples this image next.
    MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Poly3DPass::decode_textures_cpu(const std::vector<u32>& words)
{
    // texel_decode.comp, one RGBA8UI pixel per 2x2 texel container.
    constexpr usize kContainers = static_cast<usize>(kDecodedWidth) * kDecodedHeight;
    std::vector<u8> decoded(kContainers * 2 * 4);
    u8* out = decoded.data();
    for (usize layer = 0; layer < 2; ++layer) {
        for (usize offset = 0; offset < kContainers; ++offset, out += 4) {
            const usize index = offset >> 1;
            const u32   word  = index < kSheetWords ? words[layer * kSheetWords + index] : 0;
            const u32   half  = (offset & 1) != 0 ? (word >> 16) : (word & 0xffff);
            const u32   even  = (half >> 8) & 0xff;
            const u32   odd   = half & 0xff;
            out[0] = static_cast<u8>(even >> 4);
            out[1] = static_cast<u8>(even & 0xf);
            out[2] = static_cast<u8>(odd >> 4);
            out[3] = static_cast<u8>(odd & 0xf);
        }
    }
    BindTexture(GL_TEXTURE_2D_ARRAY, m_decoded_texture);
    TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, static_cast<GLsizei>(kDecodedWidth),
                  static_cast<GLsizei>(kDecodedHeight), 2, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                  decoded.data());
}

void Poly3DPass::sync_replacements()
{
    const bool wanted = m_replacements != nullptr && !m_replacements->empty()
                     && m_replacements->layer_count() != 0;
    const u64 generation = wanted ? m_replacements->generation() : 0;
    if (generation == m_atlas_generation) {
        return;
    }
    m_atlas_generation = generation;

    bool have_pixels = generation != 0;
    for (u32 layer = 0; have_pixels && layer < m_replacements->layer_count(); ++layer) {
        have_pixels = !m_replacements->layer_pixels(layer).empty();
    }

    DeleteTextures(1, &m_atlas_texture);
    GenTextures(1, &m_atlas_texture);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_atlas_texture);
    m_atlas_live = have_pixels;
    if (!have_pixels) {
        TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
        return;
    }

    const auto size   = static_cast<GLsizei>(kReplacementLayerSize);
    const u32  layers = m_replacements->layer_count();
    TexStorage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLsizei>(TextureReplacements::mip_levels()),
                 GL_RGBA8, size, size, static_cast<GLsizei>(layers));
    for (u32 layer = 0; layer < layers; ++layer) {
        TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(layer), size, size, 1,
                      GL_RGBA, GL_UNSIGNED_BYTE, m_replacements->layer_pixels(layer).data());
    }
    GenerateMipmap(GL_TEXTURE_2D_ARRAY);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_replacements->release_pixels();
}

void Poly3DPass::build(const hw::Model2MachineBase* machine, const hw::Model2Video& video)
{
    if (machine != nullptr) {
        refresh_machine_data(*machine, video);
    }

    sync_replacements();

    m_frame_geometry = render::triangulate(machine, video, &m_capacity_warned,
                                           m_atlas_live ? m_replacements : nullptr,
                                           m_blend_translucency);

    m_vertex_count = static_cast<u32>(m_frame_geometry.vertices.size());
    if (m_vertex_count != 0) {
        m_vertex_buffer.write(m_frame_geometry.vertices.data(),
                              static_cast<usize>(m_vertex_count) * sizeof(render::Vertex));
        m_polygon_buffer.write(m_frame_geometry.polygons.data(),
                               m_frame_geometry.polygons.size() * sizeof(render::PolyParams));
        m_geometry_buffer.write(m_frame_geometry.geometry.data(),
                                m_frame_geometry.geometry.size() * sizeof(render::PolyGeom));
    }
}

void Poly3DPass::draw_polygons()
{
    // Runs between draw_below() and draw_above(), blending straight onto the
    // below-tilemap already in the framebuffer, so colour is not cleared.
    ClearStencil(0);
    DepthMask(GL_TRUE);
    Clear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (m_vertex_count == 0) {
        return;
    }

    // polygon.{vert,frag}'s shared Push block under std140.
    struct PushBlock {
        float inv_raster[2];
        u32   render_scale;
        u32   texture_quality;  ///< 0 = faithful; else anisotropic tap ceiling
    } push{};
    push.inv_raster[0]   = 1.0F / static_cast<float>(m_width);
    push.inv_raster[1]   = 1.0F / static_cast<float>(kHeight);
    push.render_scale    = m_render_scale;
    push.texture_quality = m_texture_quality;
    BindBuffer(GL_UNIFORM_BUFFER, m_polygon_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(push)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 4, m_polygon_push_ubo);

    // The fill mask: polygons arrive nearest first, and a fragment draws only
    // where nothing has drawn yet, claiming the pixel when it does.
    Enable(GL_STENCIL_TEST);
    StencilFunc(GL_EQUAL, 0, 0xff);
    StencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    StencilMask(0xff);

    // Everything but the blended pass keeps the fill mask; with blending on it
    // also records its draw order as depth for that pass to test.
    if (m_blend_translucency) {
        Enable(GL_DEPTH_TEST);
        DepthFunc(GL_ALWAYS);
    }

    // Premultiplied over, as in the Vulkan polygon blend state.
    Enable(GL_BLEND);
    BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Units and bindings match polygon.frag's layout qualifiers; on GL 4.1
    // compile_program() sets the sampler units with glUniform1i instead.
    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_decoded_texture);
    ActiveTexture(GL_TEXTURE4);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_atlas_texture);
    ActiveTexture(GL_TEXTURE2);
    BindTexture(GL_TEXTURE_2D, m_tone_texture);
    if (is_gl41()) {
        for (u32 i = 0; i < 3; ++i) {
            ActiveTexture(GL_TEXTURE6 + i);
            BindTexture(GL_TEXTURE_BUFFER, m_buffer_textures[i]);
        }
    } else {
        BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_luma_buffer.handle);
        BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_polygon_buffer.handle);
        BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_geometry_buffer.handle);
    }

    BindVertexArray(m_vao);
    BindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer.handle);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::Vertex),
                       reinterpret_cast<void*>(offsetof(render::Vertex, x)));
    VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::Vertex),
                       reinterpret_cast<void*>(offsetof(render::Vertex, u)));
    VertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(render::Vertex),
                       reinterpret_cast<void*>(offsetof(render::Vertex, depth)));
    VertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(render::Vertex),
                        reinterpret_cast<void*>(offsetof(render::Vertex, polygon)));
    EnableVertexAttribArray(0);
    EnableVertexAttribArray(1);
    EnableVertexAttribArray(2);
    EnableVertexAttribArray(3);

    // Bindings are global, shared by both programs; only UseProgram switches.
    u32  bound   = 0;
    bool blended = false;
    for (const render::Batch& batch : m_frame_geometry.batches) {
        if (batch.vertex_count == 0) {
            continue;
        }
        if (batch.blended && !blended) {
            blended = true;
            DepthFunc(GL_LESS);
            DepthMask(GL_FALSE);
            Disable(GL_STENCIL_TEST);
        }
        const u32 wanted = batch.early ? m_polygon_program_early : m_polygon_program;
        if (wanted != bound) {
            UseProgram(wanted);
            bound = wanted;
        }
        // Native-pixel window scaled to the N*native target; identity at N=1.
        const GLint n = static_cast<GLint>(m_render_scale);
        Scissor(batch.scissor.x * n, batch.scissor.y * n,
               static_cast<GLsizei>(batch.scissor.width * m_render_scale),
               static_cast<GLsizei>(batch.scissor.height * m_render_scale));
        DrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first_vertex),
                  static_cast<GLsizei>(batch.vertex_count));
    }

    Disable(GL_STENCIL_TEST);
    Disable(GL_DEPTH_TEST);
    DepthMask(GL_TRUE);
}

}  // namespace sm2::render::gl
