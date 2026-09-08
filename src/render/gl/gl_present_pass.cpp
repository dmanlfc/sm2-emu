//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#include "render/gl/gl_present_pass.h"

#include "core/log.h"

#include "shaders/fullscreen_quad_vert_glsl.h"
#include "shaders/tilemap_composite_frag_glsl.h"

#include <cstring>

namespace sm2::render::gl {
namespace {

/// Mode 2: copy an already-finished opaque frame, matching
/// render::vk::PresentPass::record()'s own kModeCopy.
constexpr u32 kModeCopy = 2;

}  // namespace

PresentPass::~PresentPass()
{
    shutdown();
}

bool PresentPass::init(u32 render_scale)
{
    m_render_scale = render_scale;
    return create_target() && create_program();
}

void PresentPass::shutdown()
{
    if (m_fbo != 0) {
        DeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_native_texture != 0) {
        DeleteTextures(1, &m_native_texture);
        m_native_texture = 0;
    }
    if (m_stencil_renderbuffer != 0) {
        DeleteRenderbuffers(1, &m_stencil_renderbuffer);
        m_stencil_renderbuffer = 0;
    }
    if (m_program != 0) {
        DeleteProgram(m_program);
        m_program = 0;
    }
    if (m_push_ubo != 0) {
        DeleteBuffers(1, &m_push_ubo);
        m_push_ubo = 0;
    }
    if (m_vao != 0) {
        DeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
}

bool PresentPass::create_target()
{
    // The composite target is N*native; the software upload and a capture stay
    // native (subregions of this texture). At N=1 these are kWidth x kHeight.
    const auto w = static_cast<GLsizei>(width());
    const auto h = static_cast<GLsizei>(height());

    GenTextures(1, &m_native_texture);
    BindTexture(GL_TEXTURE_2D, m_native_texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    // Linear, matching render::vk::PresentPass's kMagnifyFilter exactly: the
    // raster is scaled by a non-integer factor at almost every window size,
    // where nearest sampling makes glyph stems alternate between one and two
    // pixels wide.
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // The 3D pass draws its fill mask into this framebuffer now, so it carries a
    // stencil sized to match the colour attachment. GL_STENCIL_INDEX8 is a
    // mandatory renderable format at this floor, matching gl_poly3d_pass's old
    // offscreen stencil renderbuffer exactly.
    GenRenderbuffers(1, &m_stencil_renderbuffer);
    BindRenderbuffer(GL_RENDERBUFFER, m_stencil_renderbuffer);
    RenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, w, h);

    GenFramebuffers(1, &m_fbo);
    BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_native_texture,
                        0);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                           m_stencil_renderbuffer);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SM2_ERROR("gl: present pass framebuffer is incomplete");
        return false;
    }
    return true;
}

bool PresentPass::create_program()
{
    GenVertexArrays(1, &m_vao);

    const std::string vertex_source =
        prepare_gl_source(shaders::kFullscreenQuadVertGlsl, active_version_directive());
    const std::string fragment_source =
        prepare_gl_source(shaders::kTilemapCompositeFragGlsl, active_version_directive());
    m_program = compile_program(vertex_source.c_str(), fragment_source.c_str());
    if (m_program == 0) {
        return false;
    }

    const u32 block = GetUniformBlockIndex(m_program, "Push");
    UniformBlockBinding(m_program, block, 1);
    GenBuffers(1, &m_push_ubo);
    BindBuffer(GL_UNIFORM_BUFFER, m_push_ubo);
    BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 4 + sizeof(u32)),
              nullptr, GL_DYNAMIC_DRAW);
    return true;
}

void PresentPass::begin_frame()
{
    BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    // Bind the composite scope's full N*native viewport so the draws don't
    // inherit present()'s letterbox viewport.
    Viewport(0, 0, static_cast<GLsizei>(width()), static_cast<GLsizei>(height()));
}

void PresentPass::upload_from_host(std::span<const u32> pixels)
{
    // `pixels` must hold exactly kWidth * kHeight texels -- see this
    // method's own header doc comment; unlike
    // render::vk::PresentPass::upload_from_host()'s std::min-clamped memcpy,
    // TexSubImage2D has no partial form to fall back to, so a short span
    // here is the caller's bug to fix, not this method's to paper over.
    BindTexture(GL_TEXTURE_2D, m_native_texture);
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(kWidth),
                 static_cast<GLsizei>(kHeight), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
}

void PresentPass::present(u32 window_width, u32 window_height)
{
    BindFramebuffer(GL_FRAMEBUFFER, 0);

    // Clears the whole window so the letterbox bars are defined, matching
    // render::vk::PresentPass::record()'s own VK_ATTACHMENT_LOAD_OP_CLEAR.
    Viewport(0, 0, static_cast<GLsizei>(window_width), static_cast<GLsizei>(window_height));
    ClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    Clear(GL_COLOR_BUFFER_BIT);

    // No glEnable(GL_SCISSOR_TEST) needed: the viewport alone already
    // constrains the fullscreen triangle to the letterbox rectangle, and the
    // clear above already covers the full window for the bars outside it.
    const render::Letterbox box = render::compute_letterbox(window_width, window_height);
    Viewport(static_cast<GLint>(box.x), static_cast<GLint>(box.y),
            static_cast<GLsizei>(box.width), static_cast<GLsizei>(box.height));

    struct PushBlock {
        float background[4];
        u32   mode;
    } push{};
    push.background[3] = 1.0F;
    push.mode           = kModeCopy;

    UseProgram(m_program);
    BindBuffer(GL_UNIFORM_BUFFER, m_push_ubo);
    BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(push)), &push);
    BindBufferBase(GL_UNIFORM_BUFFER, 1, m_push_ubo);

    Disable(GL_BLEND);
    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D, m_native_texture);

    BindVertexArray(m_vao);
    DrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace sm2::render::gl
