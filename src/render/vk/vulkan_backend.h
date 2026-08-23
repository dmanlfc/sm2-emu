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
#include "render/vk/context.h"
#include "render/vk/frame_capture.h"
#include "render/vk/poly3d_pass.h"
#include "render/vk/present_pass.h"
#include "render/vk/tilemap_pass.h"

namespace sm2::render::vk {

/// The Vulkan implementation of render::Backend.
///
/// Everything here already existed in 0.7.0's main.cpp as a set of concrete
/// objects (Context, TilemapPass, Poly3DPass, PresentPass, FrameCapture) that
/// main.cpp called directly; this class owns the same five objects and wraps
/// their existing call sequence behind render::Backend's interface. Nothing
/// about how a frame is drawn changes -- see Backend's own doc comment for
/// the exact sequence, which is unchanged from what main.cpp did itself.
///
/// It also owns the ImGui Vulkan renderer backend (the descriptor pool and
/// `ImGui_ImplVulkan_*` calls), which used to live inside osd::Gui. Gui now
/// owns only ImGui's context and its SDL3 platform backend; which GPU API
/// draws the widgets it builds is exactly the kind of decision a render
/// backend should own.
class VulkanBackend final : public Backend {
public:
    VulkanBackend()  = default;
    ~VulkanBackend() override;

    VulkanBackend(const VulkanBackend&)            = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

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

    [[nodiscard]] bool          supports_gpu_timing() const override;
    [[nodiscard]] GpuStageTimes read_stage_times() override;

    [[nodiscard]] u32  drawn_polygons() const override { return m_polygons.drawn_polygons(); }
    [[nodiscard]] u32  triangles() const override { return m_polygons.triangles(); }
    [[nodiscard]] u32  blank_polygons() const override { return m_polygons.blank_polygons(); }
    [[nodiscard]] const char* device_name() const override
    {
        return m_context.device_properties().deviceName;
    }

    [[nodiscard]] u32          native_width() const override { return PresentPass::kWidth; }
    [[nodiscard]] u32          native_height() const override { return PresentPass::kHeight; }
    [[nodiscard]] NativeFormat native_format() const override
    {
        return NativeFormat::Rgba8Unorm;
    }

private:
    Context      m_context;
    TilemapPass  m_tilemaps;
    Poly3DPass   m_polygons;
    PresentPass  m_present;
    FrameCapture m_capture;

    /// The view begin_frame() returned this frame, for the tilemap/3D calls
    /// that draw into it.
    VkImageView m_native_view = VK_NULL_HANDLE;

    bool m_capture_requested = false;

    // -- ImGui's Vulkan renderer backend -------------------------------------
    VkDescriptorPool m_overlay_pool             = VK_NULL_HANDLE;
    VkFormat         m_overlay_target_format    = VK_FORMAT_UNDEFINED;
    bool             m_overlay_renderer_ready    = false;
};

}  // namespace sm2::render::vk
