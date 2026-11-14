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
// All targets have row 0 at the top (the shaders negate clip Y), so viewport
// and scissor take the plan's rectangles unchanged.

#include "render/gl/model3_pass.h"

#include "core/log.h"

#include "render/texture_replace.h"
#include "shaders/fullscreen_quad_vert_glsl.h"
#include "shaders/model3_composite_base_frag_glsl.h"
#include "shaders/model3_composite_trans_frag_glsl.h"
#include "shaders/model3_layer_frag_glsl.h"
#include "shaders/model3_r3d_frag_glsl.h"
#include "shaders/model3_r3d_vert_glsl.h"
#include "shaders/model3_scroll_fog_frag_glsl.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sm2::render::gl {

using namespace render::model3;

namespace {

// Uniform buffer binding points, as the shaders declare them.
constexpr u32 kBindR3D   = 0;
constexpr u32 kBindModel = 2;
constexpr u32 kBindFog   = 3;
constexpr u32 kBindLayer = 4;

enum UboSlot : u32 { kUboR3D = 0, kUboModel = 1, kUboFog = 2, kUboLayer = 3 };

u32 round_up(u32 v, u32 a)
{
    return (v + a - 1) / a * a;
}

u32 program(const char* vs, const char* fs, const char* define = nullptr)
{
    const std::string v = prepare_gl_source(vs, active_version_directive(), define);
    const std::string f = prepare_gl_source(fs, active_version_directive(), define);
    return compile_program(v.c_str(), f.c_str());
}

void set_rect(void (*fn)(GLint, GLint, GLsizei, GLsizei), const PixelRect& r)
{
    fn(r.x, r.y, std::max(r.width, 0), std::max(r.height, 0));
}

}  // namespace

Model3Pass::~Model3Pass()
{
    shutdown();
}

bool Model3Pass::init(u32 render_scale, u32 width)
{
    shutdown(true);
    m_render_scale = render_scale;
    m_width        = width * render_scale;
    m_height       = 384 * render_scale;
    m_layer_width[0] = width > TileGen::kVisibleWidth ? width : TileGen::kWidth;
    m_layer_width[1] = m_layer_width[0];

    s32 alignment = 256;
    GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    m_ubo_alignment  = static_cast<u32>(std::max(alignment, 16));
    m_uniform_stride = round_up(sizeof(R3DUniforms), m_ubo_alignment);
    m_model_stride   = round_up(sizeof(R3DModelUniforms), m_ubo_alignment);
    m_fog_stride     = round_up(sizeof(ScrollFogUniforms), m_ubo_alignment);

    m_ready = create_programs() && create_targets() && create_textures() && create_buffers();
    if (!m_ready) {
        SM2_ERROR("gl: could not create the Model 3 pass");
        shutdown();
        return false;
    }
    m_rom_generation = 0;
    m_rom_uploaded   = 0;
    return true;
}

void Model3Pass::shutdown(bool keep_atlas)
{
    if (!keep_atlas && m_atlas != 0) {
        DeleteTextures(1, &m_atlas);
        m_atlas            = 0;
        m_atlas_generation = 0;
        m_atlas_live       = false;
    }
    for (u32* p : {&m_r3d_program, &m_r3d_aniso_program, &m_fog_program, &m_base_program, &m_trans_program, &m_layer_program}) {
        if (*p != 0) {
            DeleteProgram(*p);
            *p = 0;
        }
    }
    for (u32* fbo : {&m_r3d_fbo, &m_copy_fbo}) {
        if (*fbo != 0) {
            DeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    for (u32* rb : {&m_los_rb, &m_depth_rb, &m_depth_copy_rb}) {
        if (*rb != 0) {
            DeleteRenderbuffers(1, rb);
            *rb = 0;
        }
    }
    for (u32* t : {&m_colour[0], &m_colour[1], &m_colour[2], &m_bank, &m_layers[0], &m_layers[1]}) {
        if (*t != 0) {
            DeleteTextures(1, t);
            *t = 0;
        }
    }
    for (u32* vao : {&m_vao, &m_empty_vao}) {
        if (*vao != 0) {
            DeleteVertexArrays(1, vao);
            *vao = 0;
        }
    }
    if (m_vbo != 0) {
        DeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    for (u32 i = 0; i < 4; i++) {
        if (m_ubo[i] != 0) {
            DeleteBuffers(1, &m_ubo[i]);
            m_ubo[i]      = 0;
            m_ubo_size[i] = 0;
        }
    }
    m_ready = false;
}

bool Model3Pass::create_programs()
{
    m_r3d_program   = program(shaders::kModel3R3dVertGlsl, shaders::kModel3R3dFragGlsl);
    m_fog_program   = program(shaders::kFullscreenQuadVertGlsl, shaders::kModel3ScrollFogFragGlsl);
    m_base_program  = program(shaders::kFullscreenQuadVertGlsl, shaders::kModel3CompositeBaseFragGlsl);
    m_trans_program = program(shaders::kFullscreenQuadVertGlsl, shaders::kModel3CompositeTransFragGlsl);
    m_layer_program = program(shaders::kFullscreenQuadVertGlsl, shaders::kModel3LayerFragGlsl);
    GenVertexArrays(1, &m_empty_vao);
    return m_r3d_program != 0 && m_fog_program != 0 && m_base_program != 0 && m_trans_program != 0
           && m_layer_program != 0;
}

bool Model3Pass::create_targets()
{
    const auto w = static_cast<GLsizei>(m_width);
    const auto h = static_cast<GLsizei>(m_height);

    GenTextures(3, m_colour);
    for (u32 tex : m_colour) {
        BindTexture(GL_TEXTURE_2D, tex);
        TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    GenRenderbuffers(1, &m_los_rb);
    BindRenderbuffer(GL_RENDERBUFFER, m_los_rb);
    RenderbufferStorage(GL_RENDERBUFFER, GL_RG32UI, w, h);

    GenRenderbuffers(1, &m_depth_rb);
    BindRenderbuffer(GL_RENDERBUFFER, m_depth_rb);
    RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, w, h);

    GenRenderbuffers(1, &m_depth_copy_rb);
    BindRenderbuffer(GL_RENDERBUFFER, m_depth_copy_rb);
    RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, w, h);

    GenFramebuffers(1, &m_r3d_fbo);
    BindFramebuffer(GL_FRAMEBUFFER, m_r3d_fbo);
    for (u32 i = 0; i < 3; i++) {
        FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, m_colour[i], 0);
    }
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_RENDERBUFFER, m_los_rb);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_rb);
    const GLenum all[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    DrawBuffers(4, all);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SM2_ERROR("gl: Model 3 Real3D framebuffer is incomplete");
        return false;
    }

    GenFramebuffers(1, &m_copy_fbo);
    BindFramebuffer(GL_FRAMEBUFFER, m_copy_fbo);
    FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_copy_rb);
    const GLenum none = GL_NONE;
    DrawBuffers(1, &none);
    ReadBuffer(GL_NONE);
    if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SM2_ERROR("gl: Model 3 depth copy framebuffer is incomplete");
        return false;
    }
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool Model3Pass::create_textures()
{
    // The texture bank: one R16UI array texture, a layer per page, with the full
    // mip chain so the fetches in model3_r3d.frag see a complete texture.
    GenTextures(1, &m_bank);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_bank);
    TexStorage3D(GL_TEXTURE_2D_ARRAY, TextureSheets::kLevels, GL_R16UI, TextureSheets::kPageWidth,
                 TextureSheets::kPageHeight, 2);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GenTextures(2, m_layers);
    for (int i = 0; i < 2; i++) {
        BindTexture(GL_TEXTURE_2D, m_layers[i]);
        TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(m_layer_width[i]), TileGen::kHeight);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return true;
}

bool Model3Pass::create_buffers()
{
    // One vertex buffer: ROM vertices from 0, this frame's RAM vertices from
    // kMaxRomVerts.
    GenVertexArrays(1, &m_vao);
    BindVertexArray(m_vao);
    GenBuffers(1, &m_vbo);
    BindBuffer(GL_ARRAY_BUFFER, m_vbo);
    BufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(static_cast<usize>(kMaxRomVerts + kMaxRamVerts) * FVertexLayout::kStride),
               nullptr, GL_DYNAMIC_DRAW);

    const auto stride = static_cast<GLsizei>(FVertexLayout::kStride);
    auto attrib = [&](u32 loc, int size, GLenum type, bool normalised, u32 offset) {
        EnableVertexAttribArray(loc);
        VertexAttribPointer(loc, size, type, normalised ? GL_TRUE : GL_FALSE, stride,
                            reinterpret_cast<const void*>(static_cast<uintptr_t>(offset)));
    };
    attrib(0, 4, GL_FLOAT, false, FVertexLayout::kPos);
    attrib(1, 3, GL_FLOAT, false, FVertexLayout::kNormal);
    attrib(2, 2, GL_FLOAT, false, FVertexLayout::kTexCoord);
    attrib(3, 3, GL_FLOAT, false, FVertexLayout::kFaceNormal);
    attrib(4, 1, GL_FLOAT, false, FVertexLayout::kFixedShade);
    attrib(5, 4, GL_UNSIGNED_BYTE, true, FVertexLayout::kFaceColour);
    attrib(6, 1, GL_FLOAT, false, FVertexLayout::kTextureNP);
    BindVertexArray(0);

    GenBuffers(4, m_ubo);
    return m_vbo != 0;
}

void Model3Pass::upload_textures(TextureSheets& sheets)
{
    if (sheets.uploads().empty()) {
        return;
    }
    PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    PixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(TextureSheets::kSize));
    const u16* texels = sheets.texels().data();
    BindTexture(GL_TEXTURE_2D_ARRAY, m_bank);
    for (const TextureUpload& u : sheets.uploads()) {
        TexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(u.level), static_cast<GLint>(u.dst_x),
                      static_cast<GLint>(u.dst_y), static_cast<GLint>(u.page), static_cast<GLsizei>(u.width),
                      static_cast<GLsizei>(u.height), 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                      texels + static_cast<usize>(u.src_y) * TextureSheets::kSize + u.src_x);
    }
    PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    sheets.clear_uploads();
}

void Model3Pass::sync_atlas()
{
    const bool wanted = m_replacements != nullptr && !m_replacements->empty() && m_replacements->layer_count() != 0;
    const u64  generation = wanted ? m_replacements->generation() : 0;
    if (m_atlas != 0 && generation == m_atlas_generation) {
        return;
    }
    m_atlas_generation = generation;

    bool have_pixels = generation != 0;
    for (u32 layer = 0; have_pixels && layer < m_replacements->layer_count(); ++layer) {
        have_pixels = !m_replacements->layer_pixels(layer).empty();
    }
    if (m_atlas != 0) {
        DeleteTextures(1, &m_atlas);
    }
    GenTextures(1, &m_atlas);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_atlas);
    m_atlas_live = have_pixels;
    if (!have_pixels) {
        TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
        return;
    }
    const auto size   = static_cast<GLsizei>(kReplacementLayerSize);
    const u32  layers = m_replacements->layer_count();
    TexStorage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLsizei>(TextureReplacements::mip_levels()), GL_RGBA8, size, size,
                 static_cast<GLsizei>(layers));
    for (u32 layer = 0; layer < layers; ++layer) {
        TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(layer), size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                      m_replacements->layer_pixels(layer).data());
    }
    GenerateMipmap(GL_TEXTURE_2D_ARRAY);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_replacements->release_pixels();
}

void Model3Pass::upload_vertices(const Scene& scene)
{
    BindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // ROM vertices are append-only between cache resets: send what is new.
    const auto rom = scene.rom_vertices();
    if (scene.rom_generation() != m_rom_generation || rom.size() < m_rom_uploaded) {
        m_rom_generation = scene.rom_generation();
        m_rom_uploaded   = 0;
    }
    const usize rom_count = std::min<usize>(rom.size(), kMaxRomVerts);
    if (rom_count > m_rom_uploaded) {
        BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(m_rom_uploaded * FVertexLayout::kStride),
                      static_cast<GLsizeiptr>((rom_count - m_rom_uploaded) * FVertexLayout::kStride),
                      rom.data() + m_rom_uploaded);
        m_rom_uploaded = rom_count;
    }

    const auto  ram       = scene.ram_vertices();
    const usize ram_count = std::min<usize>(ram.size(), kMaxRamVerts);
    if (ram_count != 0) {
        BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(static_cast<usize>(kMaxRomVerts) * FVertexLayout::kStride),
                      static_cast<GLsizeiptr>(ram_count * FVertexLayout::kStride), ram.data());
    }
}

void Model3Pass::upload_uniforms(const FramePlan& plan)
{
    auto upload = [&](UboSlot slot, const void* data, usize count, usize size, u32 stride) {
        const usize bytes = std::max<usize>(count, 1) * stride;
        std::vector<u8> staging(bytes, 0);
        for (usize i = 0; i < count; i++) {
            std::memcpy(staging.data() + i * stride, static_cast<const u8*>(data) + i * size, size);
            if (slot == kUboR3D && !m_atlas_live) {
                std::memset(staging.data() + i * stride + offsetof(R3DUniforms, replaceInfo), 0, sizeof(u32));
            }
        }
        BindBuffer(GL_UNIFORM_BUFFER, m_ubo[slot]);
        if (bytes > m_ubo_size[slot]) {
            m_ubo_size[slot] = bytes * 2;
        }
        // Orphan, then fill: the previous frame's draws may still read the old store.
        BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(m_ubo_size[slot]), nullptr, GL_STREAM_DRAW);
        BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(bytes), staging.data());
    };
    if (m_aniso_taps > 1) {
        std::vector<R3DUniforms> uniforms = plan.uniforms;
        for (R3DUniforms& u : uniforms) u.anisoTaps = m_aniso_taps;
        upload(kUboR3D, uniforms.data(), uniforms.size(), sizeof(R3DUniforms), m_uniform_stride);
    } else {
        upload(kUboR3D, plan.uniforms.data(), plan.uniforms.size(), sizeof(R3DUniforms), m_uniform_stride);
    }
    upload(kUboModel, plan.models.data(), plan.models.size(), sizeof(R3DModelUniforms), m_model_stride);
    std::vector<ScrollFogUniforms> fogs;
    for (const FogDraw& f : plan.fogs) {
        fogs.push_back(f.uniforms);
    }
    upload(kUboFog, fogs.data(), fogs.size(), sizeof(ScrollFogUniforms), m_fog_stride);
}

void Model3Pass::draw_layer(u32 texture, const LayerDraw& d)
{
    struct {
        float rect[4];
        float uv_scale[2];
        float clip[2];
    } block{};
    block.rect[0]     = static_cast<float>(d.rect.x);
    block.rect[1]     = static_cast<float>(d.rect.y);
    block.rect[2]     = static_cast<float>(d.rect.width);
    block.rect[3]     = static_cast<float>(d.rect.height);
    block.uv_scale[0] = d.uv_scale;
    block.uv_scale[1] = 1.0f;
    block.clip[0]     = d.clip_lo;
    block.clip[1]     = d.clip_hi;

    BindBuffer(GL_UNIFORM_BUFFER, m_ubo[kUboLayer]);
    BufferData(GL_UNIFORM_BUFFER, sizeof(block), &block, GL_STREAM_DRAW);
    BindBufferBase(GL_UNIFORM_BUFFER, kBindLayer, m_ubo[kUboLayer]);

    UseProgram(m_layer_program);
    ActiveTexture(GL_TEXTURE0);
    BindTexture(GL_TEXTURE_2D, texture);
    const GLint filter = m_smooth_2d ? GL_LINEAR : GL_NEAREST;
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    BindVertexArray(m_empty_vao);
    DrawArrays(GL_TRIANGLES, 0, 3);
}

void Model3Pass::draw_range(const FramePlan& plan, DrawRange range, bool colour_pass)
{
    static_cast<void>(colour_pass);
    s32 last_layered = -1;
    s32 last_no_los  = -1;
    u32 last_viewport = ~0u;
    u32 last_uniforms = ~0u;
    u32 last_model    = ~0u;
    for (u32 i = range.begin; i < range.end; i++) {
        const R3DDraw& d = plan.draws[i];

        if (d.viewport != last_viewport) {
            const ViewportRect& vp = plan.viewports[d.viewport];
            set_rect(Viewport, vp.viewport);
            set_rect(Scissor, vp.scissor);
            last_viewport = d.viewport;
        }
        if (d.uniforms != last_uniforms) {
            BindBufferRange(GL_UNIFORM_BUFFER, kBindR3D, m_ubo[kUboR3D],
                            static_cast<GLintptr>(static_cast<usize>(d.uniforms) * m_uniform_stride),
                            sizeof(R3DUniforms));
            last_uniforms = d.uniforms;
        }
        if (d.model != last_model) {
            BindBufferRange(GL_UNIFORM_BUFFER, kBindModel, m_ubo[kUboModel],
                            static_cast<GLintptr>(static_cast<usize>(d.model) * m_model_stride),
                            sizeof(R3DModelUniforms));
            last_model = d.model;
        }
        // Layered meshes draw where the stencil is still zero and count up;
        // the others write their no-LOS-return flag into the top bit.
        if (static_cast<s32>(d.layered) != last_layered || (!d.layered && static_cast<s32>(d.no_los_return) != last_no_los)) {
            if (d.layered) {
                StencilFunc(GL_EQUAL, 0, 0x7F);
                StencilOp(GL_KEEP, GL_INCR, GL_INCR);
                StencilMask(0x7F);
            } else {
                StencilFunc(GL_ALWAYS, d.no_los_return ? 0x80 : 0x00, 0x80);
                StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                StencilMask(0x80);
            }
            last_layered = d.layered;
            last_no_los  = d.no_los_return;
        }
        const u32 base = d.dynamic ? kMaxRomVerts : 0;
        DrawArrays(GL_TRIANGLES, static_cast<GLint>(base + d.first_vertex), static_cast<GLsizei>(d.vertex_count));
    }
}

void Model3Pass::draw_3d(const FramePlan& plan, u64 frame_number)
{
    const GLenum all[4]    = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    const GLenum fog[4]    = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_NONE};
    const GLenum trans1[4] = {GL_NONE, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE};
    const GLenum trans2[4] = {GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_NONE};
    const GLfloat zero_f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint  zero_u[4] = {0, 0, 0, 0};

    BindFramebuffer(GL_FRAMEBUFFER, m_r3d_fbo);
    DrawBuffers(4, all);
    Viewport(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height));
    Disable(GL_SCISSOR_TEST);
    Disable(GL_BLEND);
    ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int i = 0; i < 3; i++) {
        ClearBufferfv(GL_COLOR, i, zero_f);
    }
    ClearBufferuiv(GL_COLOR, 3, zero_u);

    // Ambient and scroll fog, straight into the colour target.
    Enable(GL_SCISSOR_TEST);
    Disable(GL_DEPTH_TEST);
    Disable(GL_STENCIL_TEST);
    if (!plan.fogs.empty()) {
        DrawBuffers(4, fog);
        UseProgram(m_fog_program);
        BindVertexArray(m_empty_vao);
        for (usize i = 0; i < plan.fogs.size(); i++) {
            const ViewportRect& vp = plan.viewports[plan.fogs[i].viewport];
            set_rect(Viewport, vp.viewport);
            set_rect(Scissor, vp.scissor);
            BindBufferRange(GL_UNIFORM_BUFFER, kBindFog, m_ubo[kUboFog],
                            static_cast<GLintptr>(i * m_fog_stride), sizeof(ScrollFogUniforms));
            DrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // Anisotropic filtering gets its own program so the default one stays small.
    if (m_aniso_taps > 1 && m_r3d_aniso_program == 0) {
        m_r3d_aniso_program = program(shaders::kModel3R3dVertGlsl, shaders::kModel3R3dFragGlsl, "SM2_ANISO");
    }
    UseProgram(m_aniso_taps > 1 && m_r3d_aniso_program != 0 ? m_r3d_aniso_program : m_r3d_program);
    BindVertexArray(m_vao);
    ActiveTexture(GL_TEXTURE1);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_bank);
    ActiveTexture(GL_TEXTURE2);
    BindTexture(GL_TEXTURE_2D_ARRAY, m_atlas);
    ActiveTexture(GL_TEXTURE0);

    LineOfSight los;
    los.frame_number = frame_number;
    bool any_los     = false;

    for (const LayerPass& pass : plan.passes) {
        DrawBuffers(4, all);
        Disable(GL_SCISSOR_TEST);
        DepthMask(GL_TRUE);
        StencilMask(0xFF);
        ClearBufferfi(GL_DEPTH_STENCIL, 0, 0.0f, 0);  // reversed Z: clear to 0
        ClearBufferuiv(GL_COLOR, 3, zero_u);
        Enable(GL_SCISSOR_TEST);
        Enable(GL_DEPTH_TEST);
        Enable(GL_STENCIL_TEST);

        DepthFunc(GL_GEQUAL);
        draw_range(plan, pass.colour, true);

        if (pass.los) {
            // Read back this layer's line-of-sight sample.
            GLuint texel[4] = {0, 0, 0, 0};
            ReadBuffer(GL_COLOR_ATTACHMENT3);
            ReadPixels(pass.los_x, pass.los_y, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, texel);
            ReadBuffer(GL_COLOR_ATTACHMENT0);
            los.range[static_cast<usize>(pass.priority)] = decode_line_of_sight(texel[0], texel[1] != 0);
            any_los = true;
        }

        DepthFunc(GL_GREATER);

        // An empty layer changes nothing, and the depth only needs saving
        // when both layers draw.
        const bool both = !pass.trans1.empty() && !pass.trans2.empty();
        if (both) {
            // Save the depth and stencil for the second translucent layer.
            Disable(GL_SCISSOR_TEST);
            BindFramebuffer(GL_READ_FRAMEBUFFER, m_r3d_fbo);
            BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_copy_fbo);
            BlitFramebuffer(0, 0, static_cast<GLint>(m_width), static_cast<GLint>(m_height), 0, 0,
                            static_cast<GLint>(m_width), static_cast<GLint>(m_height),
                            GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
            BindFramebuffer(GL_FRAMEBUFFER, m_r3d_fbo);
            Enable(GL_SCISSOR_TEST);
        }
        if (!pass.trans1.empty()) {
            DrawBuffers(4, trans1);
            draw_range(plan, pass.trans1, false);
        }
        if (both) {
            // Restore them.
            Disable(GL_SCISSOR_TEST);
            BindFramebuffer(GL_READ_FRAMEBUFFER, m_copy_fbo);
            BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_r3d_fbo);
            BlitFramebuffer(0, 0, static_cast<GLint>(m_width), static_cast<GLint>(m_height), 0, 0,
                            static_cast<GLint>(m_width), static_cast<GLint>(m_height),
                            GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
            BindFramebuffer(GL_FRAMEBUFFER, m_r3d_fbo);
            Enable(GL_SCISSOR_TEST);
        }
        if (!pass.trans2.empty()) {
            DrawBuffers(4, trans2);
            draw_range(plan, pass.trans2, false);
        }
    }

    Disable(GL_DEPTH_TEST);
    Disable(GL_STENCIL_TEST);
    DrawBuffers(4, all);
    // A layer not sampled this frame reads zero. Results are published as many
    // frames late as the Vulkan pass reads its copies back, so a game sees the
    // same results on either renderer.
    static_cast<void>(any_los);
    const u64 r    = m_los_records++;
    LosSlot&  read = m_los_slots[(r + 1) % m_los_slots.size()];
    if (read.set) {
        m_los.publish(read.los);
    }
    m_los_slots[r % m_los_slots.size()] = LosSlot{los, true};
}

void Model3Pass::render(FrameBuilder& frame, u32 native_fbo)
{
    if (!m_ready || !frame.has_frame()) {
        return;
    }
    const FramePlan& plan = frame.plan();

    sync_atlas();
    upload_textures(frame.textures());
    for (int i = 0; i < 2; i++) {
        if (frame.layer(i).size() != static_cast<usize>(m_layer_width[i]) * TileGen::kHeight) {
            continue;
        }
        BindTexture(GL_TEXTURE_2D, m_layers[i]);
        TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(m_layer_width[i]), TileGen::kHeight, GL_RGBA,
                      GL_UNSIGNED_BYTE, frame.layer(i).data());
    }

    if (!plan.white_flash) {
        upload_vertices(frame.scene());
        upload_uniforms(plan);
        draw_3d(plan, frame.frame_number());
    }

    // The whole frame cleared, then the bottom tile layer.
    BindFramebuffer(GL_FRAMEBUFFER, native_fbo);
    Viewport(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height));
    Disable(GL_DEPTH_TEST);
    Disable(GL_STENCIL_TEST);
    Disable(GL_SCISSOR_TEST);
    ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    Clear(GL_COLOR_BUFFER_BIT);

    Enable(GL_SCISSOR_TEST);
    set_rect(Scissor, plan.display);
    Enable(GL_BLEND);
    BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_layer(m_layers[0], plan.layers[0]);

    if (plan.white_flash) {
        // Block culling: the 3D clears to white inside the display box.
        ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        Clear(GL_COLOR_BUFFER_BIT);
    } else {
        // The 3D: base layer, then the translucency layers.
        BindVertexArray(m_empty_vao);
        UseProgram(m_base_program);
        ActiveTexture(GL_TEXTURE0);
        BindTexture(GL_TEXTURE_2D, m_colour[0]);
        DrawArrays(GL_TRIANGLES, 0, 3);

        UseProgram(m_trans_program);
        ActiveTexture(GL_TEXTURE0);
        BindTexture(GL_TEXTURE_2D, m_colour[1]);
        ActiveTexture(GL_TEXTURE1);
        BindTexture(GL_TEXTURE_2D, m_colour[2]);
        ActiveTexture(GL_TEXTURE0);
        DrawArrays(GL_TRIANGLES, 0, 3);
    }

    // The top tile layer.
    draw_layer(m_layers[1], plan.layers[1]);

    Disable(GL_BLEND);
    Disable(GL_SCISSOR_TEST);
    BindVertexArray(0);
    UseProgram(0);
}

}  // namespace sm2::render::gl
