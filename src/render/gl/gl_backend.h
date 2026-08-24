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
#pragma once

#include "render/backend.h"
#include "render/gl/gl_context.h"
#include "render/gl/gl_frame_capture.h"
#include "render/gl/gl_poly3d_pass.h"
#include "render/gl/gl_present_pass.h"
#include "render/gl/gl_tilemap_pass.h"

namespace sm2::render::gl {

/// The OpenGL 4.3 core implementation of render::Backend.
///
/// Mirrors render::vk::VulkanBackend's shape (one Context, one TilemapPass,
/// one Poly3DPass, one PresentPass, one FrameCapture, wired to Backend's
/// per-frame sequence exactly as documented in backend.h) -- see that
/// class's own doc comment for why the shape is shared even though the API
/// underneath differs. Also owns ImGui's OpenGL3 renderer backend, the same
/// role VulkanBackend plays for imgui_impl_vulkan.
class GlBackend final : public Backend {
public:
    GlBackend()  = default;
    ~GlBackend() override;

    GlBackend(const GlBackend&)            = delete;
    GlBackend& operator=(const GlBackend&) = delete;

    [[nodiscard]] bool init(osd::Window& window, const BackendConfig& config) override;
    void               shutdown() override;

    [[nodiscard]] bool init_overlay(osd::Gui& gui) override;
    void               shutdown_overlay() override;

    [[nodiscard]] Capabilities capabilities() const override;

    [[nodiscard]] bool begin_frame() override;
    void compute_tilemap(const hw::Model2MachineBase& machine,
                        const hw::Model2Video&       video) override;
    void upload_tilemap(std::span<const u32> below, std::span<const u32> above) override;
    void submit_polygons(const hw::Model2MachineBase* machine,
                        const hw::Model2Video&       video) override;
    void render_polygons() override;
    void composite_native_frame(u32 background_rgba, bool skip_3d) override;
    void submit_native_frame(std::span<const u32> pixels) override;
    [[nodiscard]] bool request_capture() override;
    [[nodiscard]] bool save_capture(const std::string& path) const override;
    void blit_to_swapchain() override;
    void begin_overlay_frame() override;
    void draw_overlay(bool active) override;
    [[nodiscard]] bool end_frame() override;
    void wait_idle() override;

    [[nodiscard]] bool          supports_gpu_timing() const override { return false; }
    [[nodiscard]] GpuStageTimes read_stage_times() override { return GpuStageTimes{}; }

    [[nodiscard]] u32  drawn_polygons() const override { return m_polygons.drawn_polygons(); }
    [[nodiscard]] u32  triangles() const override { return m_polygons.triangles(); }
    [[nodiscard]] u32  blank_polygons() const override { return m_polygons.blank_polygons(); }
    [[nodiscard]] const char* device_name() const override
    {
        return m_context.device_name().c_str();
    }

    [[nodiscard]] u32          native_width() const override { return PresentPass::kWidth; }
    [[nodiscard]] u32          native_height() const override { return PresentPass::kHeight; }
    [[nodiscard]] NativeFormat native_format() const override
    {
        return NativeFormat::Rgba8Unorm;
    }

private:
    osd::Window* m_window = nullptr;

    Context      m_context;
    TilemapPass  m_tilemaps;
    Poly3DPass   m_polygons;
    PresentPass  m_present;
    FrameCapture m_capture;

    bool m_capture_requested = false;
    bool m_overlay_ready     = false;
};

}  // namespace sm2::render::gl
