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
#include "render/gl/gl_poly3d_pass.h"

#include "core/log.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include "shaders/fullscreen_quad_vert_glsl.h"
#include "shaders/polygon_frag_glsl.h"
#include "shaders/polygon_vert_glsl.h"
#include "shaders/texel_decode_comp_glsl.h"
#include "shaders/tilemap_composite_frag_glsl.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::gl {
namespace {

constexpr usize kLumaBytes = 0x8000;
constexpr u32   kModeBlendOver = 1;

}  // namespace

Poly3DPass::~Poly3DPass()
{
    shutdown();
}

bool Poly3DPass::init()
{
    m_frame_geometry.vertices.reserve(1 << 14);
    m_frame_geometry.polygons.reserve(1 << 12);
    m_tone_curve.assign(static_cast<usize>(hw::Model2Video::kToneShades)
                            * hw::Model2Video::kToneComponents,
                        0);

    return create_framebuffer() && create_programs() && create_buffers();
}

void Poly3DPass::shutdown()
{
    if (m_fbo != 0) {
        DeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_colour_texture != 0) {
        DeleteTextures(1, &m_colour_texture);
        m_colour_texture = 0;
    }
    if (m_stencil_renderbuffer != 0) {
        DeleteRenderbuffers(1, &m_stencil_renderbuffer);
        m_stencil_renderbuffer = 0;
    }
    if (m_decoded_texture != 0) {
        DeleteTextures(1, &m_decoded_texture);
        m_decoded_texture = 0;
    }
    if (m_tone_texture != 0) {
        DeleteTextures(1, &m_tone_texture);
        m_tone_texture = 0;
    }
    if (m_polygon_program != 0) {
        DeleteProgram(m_polygon_program);
        m_polygon_program = 0;
    }
    if (m_decode_program != 0) {
        DeleteProgram(m_decode_program);
        m_decode_program = 0;
    }
    if (m_composite_program != 0) {
        DeleteProgram(m_composite_program);
        m_composite_program = 0;
    }
    if (m_vao != 0) {
        DeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_composite_vao != 0) {
        DeleteVertexArrays(1, &m_composite_vao);
        m_composite_vao = 0;
    }
    if (m_composite_push_ubo != 0) {
        DeleteBuffers(1, &m_composite_push_ubo);
        m_composite_push_ubo = 0;
    }
    if (m_polygon_push_ubo != 0) {
        DeleteBuffers(1, &m_polygon_push_ubo);
        m_polygon_push_ubo = 0;
    }
    destroy_persistent_buffer(&m_vertex_buffer);
    destroy_persistent_buffer(&m_polygon_buffer);
    destroy_persistent_buffer(&m_sheets_buffer);
    destroy_persistent_buffer(&m_luma_buffer);
}

bool Poly3DPass::create_framebuffer()
{
    GenTextures(1, &m_colour_texture);
    BindTexture(GL_TEXTURE_2D, m_colour_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(kWidth),
                static_cast<GLsizei>(kHeight));
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // GL_STENCIL_INDEX8 is a mandatory renderable format at this floor, so
    // -- unlike render::vk::Context::pick_stencil_format() -- there is no
    // combined-depth-stencil fallback to write; see gl_context.h's own note
    // on why this is one fewer thing to get wrong, not a gap relative to the
    // Vulkan path.
    GenRenderbuffers(1, &m_stencil_renderbuffer);
    BindRenderbuffer(GL_RENDERBUFFER, m_stencil_renderbuffer);
    RenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, static_cast<GLsizei>(kWidth),
                       static_cast<GLsizei>(kHeight));

    GenFramebuffers(1, &m_fbo);
    BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colour_texture,
                        0);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                           m_stencil_renderbuffer);

    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SM2_ERROR("gl: 3d pass framebuffer is incomplete");
        return false;
    }
    return true;
}

bool Poly3DPass::create_programs()
{
    GenVertexArrays(1, &m_vao);

    const std::string vertex_source =
        prepare_gl_source(shaders::kPolygonVertGlsl, kDesktopVersionDirective);
    const std::string fragment_source =
        prepare_gl_source(shaders::kPolygonFragGlsl, kDesktopVersionDirective);
    m_polygon_program = compile_program(vertex_source.c_str(), fragment_source.c_str());
    if (m_polygon_program == 0) {
        return false;
    }
    // polygon.vert's Push block took binding 4 under SM2_TARGET_GL -- see
    // that shader's own SM2_TARGET_GL comment.
    const u32 polygon_block = GetUniformBlockIndex(m_polygon_program, "Push");
    UniformBlockBinding(m_polygon_program, polygon_block, 4);
    GenBuffers(1, &m_polygon_push_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_polygon_push_ubo);
    BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 2), nullptr,
              GL_DYNAMIC_DRAW);

    const std::string decode_source =
        prepare_gl_source(shaders::kTexelDecodeCompGlsl, kDesktopVersionDirective);
    m_decode_program = compile_compute_program(decode_source.c_str());
    if (m_decode_program == 0) {
        return false;
    }

    GenVertexArrays(1, &m_composite_vao);
    const std::string composite_vertex_source =
        prepare_gl_source(shaders::kFullscreenQuadVertGlsl, kDesktopVersionDirective);
    const std::string composite_fragment_source =
        prepare_gl_source(shaders::kTilemapCompositeFragGlsl, kDesktopVersionDirective);
    m_composite_program =
        compile_program(composite_vertex_source.c_str(), composite_fragment_source.c_str());
    if (m_composite_program == 0) {
        return false;
    }
    const u32 composite_block = GetUniformBlockIndex(m_composite_program, "Push");
    UniformBlockBinding(m_composite_program, composite_block, 1);
    GenBuffers(1, &m_composite_push_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_composite_push_ubo);
    BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 4 + sizeof(u32)),
              nullptr, GL_DYNAMIC_DRAW);

    return true;
}

bool Poly3DPass::create_buffers()
{
    m_vertex_buffer =
        create_persistent_buffer(static_cast<usize>(render::kMaxVertices) * sizeof(render::Vertex),
                                 GL_ARRAY_BUFFER);
    m_polygon_buffer = create_persistent_buffer(
        static_cast<usize>(render::kMaxPolygons) * sizeof(render::PolyParams),
        GL_SHADER_STORAGE_BUFFER);
    m_sheets_buffer =
        create_persistent_buffer(static_cast<usize>(kSheetWords) * 2 * sizeof(u32),
                                 GL_SHADER_STORAGE_BUFFER);
    m_luma_buffer = create_persistent_buffer(kLumaBytes, GL_SHADER_STORAGE_BUFFER);

    GenTextures(1, &m_decoded_texture);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_decoded_texture);
    TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, static_cast<GLsizei>(kDecodedWidth),
                static_cast<GLsizei>(kDecodedHeight), 2);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    m_tone_texture = create_tone_texture(hw::Model2Video::kToneShades,
                                        hw::Model2Video::kToneComponents);

    return m_vertex_buffer.handle != 0 && m_polygon_buffer.handle != 0
        && m_sheets_buffer.handle != 0 && m_luma_buffer.handle != 0
        && m_decoded_texture != 0 && m_tone_texture != 0;
}

void Poly3DPass::refresh_machine_data(const hw::Model2MachineBase& machine,
                                      const hw::Model2Video&       video)
{
    if (m_texture_generation != machine.texture_generation()) {
        // Only the low half of each two-megabyte window is addressable by
        // the texel arithmetic and only that half is ever written, matching
        // render::vk::Poly3DPass::refresh_machine_data() exactly.
        std::vector<u32> words(static_cast<usize>(kSheetWords) * 2, 0);
        for (int sheet = 0; sheet < 2; ++sheet) {
            const std::span<const u32> source = machine.texture_ram(sheet);
            const usize count = std::min<usize>(kSheetWords, source.size());
            std::memcpy(words.data() + static_cast<usize>(sheet) * kSheetWords, source.data(),
                        count * sizeof(u32));
        }
        m_sheets_buffer.write(words.data(), words.size() * sizeof(u32));
        m_texture_generation = machine.texture_generation();
        decode_textures();
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

    // The polygon pass's fragment shader samples this image next; its
    // writes must be visible first. GL analogue of the one
    // record_image_barrier() call render::vk::Poly3DPass::decode_textures()
    // issues for the same reason.
    MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Poly3DPass::build(const hw::Model2MachineBase* machine, const hw::Model2Video& video)
{
    if (machine != nullptr) {
        refresh_machine_data(*machine, video);
    }

    m_frame_geometry = render::triangulate(machine, video, &m_capacity_warned);

    m_vertex_count = static_cast<u32>(m_frame_geometry.vertices.size());
    if (m_vertex_count != 0) {
        m_vertex_buffer.write(m_frame_geometry.vertices.data(),
                              static_cast<usize>(m_vertex_count) * sizeof(render::Vertex));
        m_polygon_buffer.write(m_frame_geometry.polygons.data(),
                               m_frame_geometry.polygons.size() * sizeof(render::PolyParams));
    }
}

void Poly3DPass::render()
{
    BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    Viewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));

    // Transparent, premultiplied clear: a pixel no polygon covers lets the
    // tilemap behind it through unaltered, matching
    // render::vk::Poly3DPass::render()'s own clear value.
    ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float clear_colour[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    ClearColor(clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3]);
    ClearStencil(0);
    Clear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    if (m_vertex_count == 0) {
        return;
    }

    UseProgram(m_polygon_program);

    struct PushBlock {
        float inv_raster[2];
    } push{};
    push.inv_raster[0] = 1.0F / static_cast<float>(kWidth);
    push.inv_raster[1] = 1.0F / static_cast<float>(kHeight);
    BindBuffer(GL_UNIFORM_BUFFER, m_polygon_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(push)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 4, m_polygon_push_ubo);

    // The fill mask: polygons arrive nearest first, and a fragment draws
    // only where nothing has drawn yet, claiming the pixel when it does.
    // Same rule render::vk::Poly3DPass::render()'s stencil state expresses,
    // in GL's terms.
    Enable(GL_STENCIL_TEST);
    StencilFunc(GL_EQUAL, 0, 0xff);
    StencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    StencilMask(0xff);
    Disable(GL_BLEND);

    // Texture units and buffer bindings match polygon.frag's own layout(...)
    // qualifiers exactly: uSheets binding 0, Luma binding 1, uTone binding 2,
    // Polys binding 3. layout(binding=N) on a sampler is honoured by the
    // driver from GLSL 4.20 (GL 4.2 core) onward -- below this project's 4.3
    // floor, so no per-shader glUniform1i call is needed, only binding the
    // right texture to the matching unit.
    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_decoded_texture);
    ActiveTexture(GL_TEXTURE2);
    BindTexture(GL_TEXTURE_2D, m_tone_texture);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_luma_buffer.handle);
    BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_polygon_buffer.handle);

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

    for (const render::Batch& batch : m_frame_geometry.batches) {
        if (batch.vertex_count == 0) {
            continue;
        }
        Scissor(batch.scissor.x, batch.scissor.y, static_cast<GLsizei>(batch.scissor.width),
               static_cast<GLsizei>(batch.scissor.height));
        DrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first_vertex),
                  static_cast<GLsizei>(batch.vertex_count));
    }

    Disable(GL_STENCIL_TEST);
}

void Poly3DPass::composite()
{
    struct PushBlock {
        float background[4];
        u32   mode;
    } push{};
    push.background[3] = 1.0F;
    push.mode           = kModeBlendOver;

    UseProgram(m_composite_program);
    BindBuffer(GL_UNIFORM_BUFFER, m_composite_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(push)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 1, m_composite_push_ubo);

    Enable(GL_BLEND);
    BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D, m_colour_texture);

    BindVertexArray(m_composite_vao);
    DrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace sm2::render::gl
