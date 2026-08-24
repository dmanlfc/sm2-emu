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
#include "render/gl/gl_backend.h"

#include "core/log.h"
#include "osd/window.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

namespace sm2::render::gl {

GlBackend::~GlBackend()
{
    shutdown();
}

bool GlBackend::init(osd::Window& window, const BackendConfig& config)
{
    m_window = &window;

    ContextConfig context_config;
    context_config.vsync = config.vsync;

    if (!m_context.init(window, context_config)) {
        return false;
    }
    if (!m_tilemaps.init()) {
        SM2_ERROR("gl: could not create the 2D pipeline");
        return false;
    }
    if (!m_polygons.init()) {
        SM2_ERROR("gl: could not create the 3D pipeline");
        return false;
    }
    if (!m_present.init()) {
        SM2_ERROR("gl: could not create the presentation pipeline");
        return false;
    }
    return true;
}

void GlBackend::shutdown()
{
    m_present.shutdown();
    m_polygons.shutdown();
    m_tilemaps.shutdown();
    m_context.shutdown();
    m_window = nullptr;
}

Capabilities GlBackend::capabilities() const
{
    Capabilities caps;
    caps.compute_shaders = true;
    // No GPU timer queries wired up yet -- see backend.h's own
    // GpuStage/GpuStageTime documentation of why "did not run" and "zero"
    // must stay distinguishable, and design.md sec 3 for why this is a
    // stated scope cut rather than an oversight (GL_TIME_ELAPSED query
    // objects exist at this floor and could be added later, once there is a
    // device to benchmark against).
    caps.gpu_timing = false;
    return caps;
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
    m_polygons.render();
}

void GlBackend::composite_native_frame(u32 background_rgba, bool skip_3d)
{
    m_present.begin_frame();
    m_tilemaps.draw_below(background_rgba);
    // Render test mode cuts the DSP out: the framebuffer bank the host has
    // been drawing into is shown instead of the 3D pass, and has already
    // been composed into the layers below by the caller -- matching
    // VulkanBackend::composite_native_frame()'s own skip_3d handling.
    if (!skip_3d) {
        m_polygons.composite();
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
    return m_capture.record(m_present.native_width(), m_present.native_height());
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
    // GL has no explicit device-idle wait; Finish() (glFinish) is the
    // closest equivalent and is only ever needed before a capture is saved
    // or before destroying a resource a submitted draw might still
    // reference -- both of which, on this backend, already happen
    // synchronously (no frames-in-flight ring exists to race against, see
    // gl_context.h's own note on why). Called anyway for parity with the
    // Vulkan path's call sites, which is cheap insurance against a future
    // change here introducing asynchrony this comment did not anticipate.
    Finish();
}

// ---------------------------------------------------------------------------
// ImGui's OpenGL3 renderer backend
// ---------------------------------------------------------------------------

bool GlBackend::init_overlay(osd::Gui& /*gui*/)
{
    if (!ImGui_ImplOpenGL3_Init("#version 430 core")) {
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

}  // namespace sm2::render::gl

namespace sm2::render {

std::unique_ptr<Backend> create_opengl_backend()
{
    return std::make_unique<gl::GlBackend>();
}

}  // namespace sm2::render
