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
#include "render/gl/gl_backend.h"

#include "core/log.h"
#include "osd/window.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace sm2::render::gl {

GlBackend::~GlBackend()
{
    shutdown();
}

bool GlBackend::init(osd::Window& window, const BackendConfig& config)
{
    m_window = &window;

    m_render_scale = std::clamp(config.render_scale, 1U, kMaxRenderScale);
    m_present_options = config.present;
    m_enhancement_options = config.enhancement;

    ContextConfig context_config;
    context_config.vsync = config.vsync;
#if defined(SM2_FORCE_GLES)
    context_config.es_mode = true;
#endif

    if (!m_context.init(window, context_config)) {
        return false;
    }

    set_version_directive(m_context.is_es()         ? kEsVersionDirective
                          : m_context.has_compute() ? kDesktopVersionDirective
                                                    : kGl41VersionDirective);

    // The composite target is a texture and its fill mask a renderbuffer, so
    // bound N by the smaller of the two size limits.
    s32 max_texture      = 0;
    s32 max_renderbuffer = 0;
    GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    u32 max_dimension = std::min(static_cast<u32>(max_texture),
                                 static_cast<u32>(max_renderbuffer));
    if (const char* env = std::getenv("SM2_TEST_MAX_IMAGE_DIM")) {  // test-only override
        max_dimension = static_cast<u32>(std::strtoul(env, nullptr, 10));
    }
    const u32 fitted = clamp_scale_to_max_dimension(m_render_scale, max_dimension);
    if (fitted != m_render_scale) {
        SM2_WARN("gl: render scale %ux (%ux%u) exceeds device limit %u "
                 "(GL_MAX_TEXTURE_SIZE %d, GL_MAX_RENDERBUFFER_SIZE %d); using %ux",
                 m_render_scale, scaled_width(m_render_scale),
                 scaled_height(m_render_scale), max_dimension, max_texture,
                 max_renderbuffer, fitted);
        m_render_scale = fitted;
    }
    if (m_render_scale != 1) {
        SM2_INFO("gl: render scale %ux: 3D and composite targets at %ux%u",
                 m_render_scale, scaled_width(m_render_scale),
                 scaled_height(m_render_scale));
    }

    // GL_MAX_TEXTURE_MAX_ANISOTROPY (0x84FF) is a float; truncating it is fine
    // for a tap ceiling. 1.0 means unavailable.
    constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;
    if (has_gl_extension("GL_ARB_texture_filter_anisotropic")
        || has_gl_extension("GL_EXT_texture_filter_anisotropic")) {
        s32 aniso = 1;
        GetIntegerv(kMaxTextureMaxAnisotropy, &aniso);
        m_max_anisotropy = std::max(1.0F, static_cast<float>(aniso));
    }

    if (!m_tilemaps.init()) {
        SM2_ERROR("gl: could not create the 2D pipeline");
        return false;
    }
    if (!m_polygons.init(m_render_scale)) {
        SM2_ERROR("gl: could not create the 3D pipeline");
        return false;
    }
    if (!m_present.init(m_render_scale)) {
        SM2_ERROR("gl: could not create the presentation pipeline");
        return false;
    }
    m_present.set_options(m_present_options);
    m_polygons.set_texture_quality(effective_texture_quality());
    m_tilemaps.set_upscale_2d(static_cast<u32>(m_enhancement_options.upscale_2d));
    return true;
}

void GlBackend::set_widescreen(u32 columns, u32 wide_background)
{
    m_model3_wide = wide_background;
    m_tilemaps.set_wide_below(wide_background);
    if (columns == m_columns) {
        return;
    }
    // GL is synchronous, so the 2D surfaces and the native frame can be
    // recreated at their new width straight away.
    m_present.shutdown();
    m_tilemaps.shutdown();
    m_columns = columns;
    if (!m_tilemaps.init(m_columns) || !m_present.init(m_render_scale, frame_width(m_columns))) {
        SM2_ERROR("gl: could not resize the native frame for widescreen");
    }
    m_tilemaps.set_wide_below(wide_background);
    m_tilemaps.set_upscale_2d(static_cast<u32>(m_enhancement_options.upscale_2d));
    m_present.set_options(m_present_options);
    m_polygons.set_width(frame_width(m_columns));
}

void GlBackend::shutdown()
{
    m_model3.shutdown();
    m_model3_width = 0;
    m_present.shutdown();
    m_polygons.shutdown();
    m_tilemaps.shutdown();
    m_context.shutdown();
    m_window = nullptr;
}

Capabilities GlBackend::capabilities() const
{
    Capabilities caps;
    caps.compute_shaders = m_context.has_compute();
    // No GPU timer queries on GL.
    caps.gpu_timing = false;
    caps.anisotropy      = m_max_anisotropy > 1.0F;
    caps.max_anisotropy  = m_max_anisotropy;
    caps.blended_translucency = true;
    caps.widescreen           = true;
    return caps;
}

// GL is synchronous with no frames-in-flight ring, so the handle is just the GL
// texture name and create/destroy are immediate.

Backend::TextureHandle GlBackend::create_texture(u32 w, u32 h, const u8* rgba)
{
    if (w == 0 || h == 0 || rgba == nullptr) {
        return 0;
    }
    GLuint texture = 0;
    GenTextures(1, &texture);
    if (texture == 0) {
        return 0;
    }
    BindTexture(GL_TEXTURE_2D, texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(w),
                 static_cast<GLsizei>(h));
    TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w),
                  static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void GlBackend::destroy_texture(TextureHandle handle)
{
    if (handle == 0) {
        return;
    }
    const GLuint texture = static_cast<GLuint>(handle);
    DeleteTextures(1, &texture);
}

void* GlBackend::texture_imgui_id(TextureHandle handle) const
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));  // GL name is the ID
}

bool GlBackend::begin_frame()
{
    m_capture_requested = false;
    m_present.begin_frame();
    return true;
}

void GlBackend::compute_tilemap(const hw::Model2MachineBase& machine,
                                const hw::Model2Video&       video)
{
    m_tilemaps.compute(machine, video);
}

void GlBackend::upload_tilemap(std::span<const u32> below, std::span<const u32> above)
{
    m_tilemaps.upload(below, above);
}

void GlBackend::submit_polygons(const hw::Model2MachineBase* machine,
                                const hw::Model2Video&       video)
{
    m_polygons.build(machine, video);
}

void GlBackend::render_polygons()
{
    // The 3D draws in composite_native_frame(), between the tilemap layers.
}

void GlBackend::composite_native_frame(u32 background_rgba, bool skip_3d)
{
    m_present.begin_frame();
    m_tilemaps.draw_below(background_rgba);
    // Render test mode shows the host's framebuffer bank instead of the 3D
    // pass; the caller has already composed it into the layers below.
    if (!skip_3d) {
        m_polygons.draw_polygons();
    }
    m_tilemaps.draw_above();
}

void GlBackend::submit_native_frame(std::span<const u32> pixels)
{
    m_present.upload_from_host(pixels);
}

bool GlBackend::request_capture()
{
    m_capture_requested = true;
    m_present.begin_frame();  // rebind the native FBO in case something else was bound
    // Capture the whole composite (N*native), not the native corner, so a
    // screenshot is at the render scale.
    return m_capture.record(m_present.width(), m_present.height());
}

bool GlBackend::save_capture(const std::string& path) const
{
    return m_capture.save(path);
}

void GlBackend::blit_to_swapchain()
{
    u32 width  = 0;
    u32 height = 0;
    m_window->drawable_size(&width, &height);
    m_present.present(width, height);
}

void GlBackend::set_present_options(const PresentOptions& options)
{
    // Present-only state; present() reads it. No target reallocation.
    m_present_options = options;
    m_present.set_options(options);
}

void GlBackend::set_texture_replacements(TextureReplacements* replacements)
{
    m_polygons.set_texture_replacements(replacements);
    m_model3.set_texture_replacements(replacements);
    m_model3_frame.set_texture_replacements(replacements);
}

void GlBackend::set_enhancement_options(const EnhancementOptions& options)
{
    m_enhancement_options = options;
    m_polygons.set_texture_quality(effective_texture_quality());
    m_polygons.set_blend_translucency(options.translucency == Translucency::Blended);
    m_tilemaps.set_upscale_2d(static_cast<u32>(options.upscale_2d));
    m_model3.set_aniso_taps(model3_aniso_taps());
}

u32 GlBackend::model3_aniso_taps() const
{
    // The Model 3 shader takes its own taps, so the sampler limit does not apply.
    return m_enhancement_options.texture_filter == TextureFilter::Anisotropic
               ? std::clamp(m_enhancement_options.anisotropy, 1u, 16u)
               : 1u;
}

u32 GlBackend::effective_texture_quality() const
{
    // Faithful is a single tap (0); anisotropic clamps to the device's max.
    if (m_enhancement_options.texture_filter != TextureFilter::Anisotropic) {
        return 0;
    }
    const u32 ceiling = std::max(static_cast<u32>(m_max_anisotropy), 1U);
    return std::min(m_enhancement_options.anisotropy, ceiling);
}

void GlBackend::overlay_framebuffer_size(u32* width, u32* height) const
{
    *width  = 0;
    *height = 0;
    if (m_window != nullptr) {
        m_window->drawable_size(width, height);
    }
}

void GlBackend::begin_overlay_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
}

void GlBackend::draw_overlay(bool active)
{
    if (!active) {
        return;
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool GlBackend::end_frame()
{
    m_context.swap();
    return true;
}

void GlBackend::wait_idle()
{
    // This backend is synchronous, so glFinish is only for parity with Vulkan.
    m_model3_frame.release();
    Finish();
}

// ---------------------------------------------------------------------------
// ImGui's OpenGL3 renderer backend
// ---------------------------------------------------------------------------

bool GlBackend::init_overlay(osd::Gui& /*gui*/)
{
    const char* imgui_version = m_context.is_es()         ? "#version 300 es"
                              : m_context.has_compute() ? "#version 430 core"
                                                        : "#version 410 core";
    if (!ImGui_ImplOpenGL3_Init(imgui_version)) {
        SM2_ERROR("gl: ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    m_overlay_ready = true;
    return true;
}

void GlBackend::shutdown_overlay()
{
    if (!m_overlay_ready) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    m_overlay_ready = false;
}

// ---------------------------------------------------------------------------
// Sega Model 3
// ---------------------------------------------------------------------------

bool GlBackend::ensure_model3()
{
    const u32 width = frame_width(m_columns);
    if (!m_model3.ready() || m_model3_width != width) {
        if (!m_model3.init(m_render_scale, width)) {
            return false;
        }
        m_model3_width = width;
    }
    m_model3_frame.match_geometry(model3_geometry(), false);
    m_model3.set_smooth_2d(m_enhancement_options.upscale_2d != Upscale2D::Faithful);
    m_model3.set_aniso_taps(model3_aniso_taps());
    return true;
}

void GlBackend::submit_model3_frame(const hw::model3::VideoFrame& frame)
{
    m_model3_frame.wait();
    m_model3_frame.set_geometry(model3_geometry());
    // DEPTH32F_STENCIL8 is a float depth buffer: no depth remap needed.
    if (m_model3_async) {
        m_model3_frame.submit_async(frame, false);
    } else {
        m_model3_frame.submit(frame, false);
    }
}

void GlBackend::render_model3()
{
    if (!ensure_model3()) {
        return;
    }
    m_present.begin_frame();
    m_model3.render(m_model3_frame, m_present.fbo());
    m_present.begin_frame();
}

}  // namespace sm2::render::gl

namespace sm2::render {

std::unique_ptr<Backend> create_opengl_backend()
{
    return std::make_unique<gl::GlBackend>();
}

}  // namespace sm2::render
