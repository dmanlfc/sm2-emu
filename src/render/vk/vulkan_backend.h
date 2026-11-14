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
#pragma once

#include "render/backend.h"
#include "render/vk/context.h"
#include "render/model3/model3_frame.h"
#include "render/vk/frame_capture.h"
#include "render/vk/model3_pass.h"
#include "render/vk/poly3d_pass.h"
#include "render/vk/present_pass.h"
#include "render/vk/tilemap_pass.h"

#include <unordered_map>
#include <vector>

namespace sm2::render::vk {

/// The Vulkan implementation of render::Backend. Owns the context, the render
/// passes, frame capture and ImGui's Vulkan renderer backend.
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

    [[nodiscard]] TextureHandle create_texture(u32 w, u32 h, const u8* rgba) override;
    void                        destroy_texture(TextureHandle handle) override;
    [[nodiscard]] void*         texture_imgui_id(TextureHandle handle) const override;

    [[nodiscard]] bool begin_frame() override;
    [[nodiscard]] bool device_lost() const override { return m_context.device_lost(); }
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
    void set_present_options(const PresentOptions& options) override;
    void set_enhancement_options(const EnhancementOptions& options) override;
    void set_texture_replacements(TextureReplacements* replacements) override;
    void set_widescreen(u32 columns, u32 wide_background) override;
    void set_hud_bands(const std::array<u32, 4>& bands) override { m_tilemaps.set_hud_bands(bands); }
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

    [[nodiscard]] u32          native_width() const override { return m_present.native_width(); }
    [[nodiscard]] u32          native_height() const override { return PresentPass::kHeight; }
    [[nodiscard]] NativeFormat native_format() const override
    {
        return NativeFormat::Rgba8Unorm;
    }

    void overlay_framebuffer_size(u32* width, u32* height) const override
    {
        const VkExtent2D extent = m_context.swapchain_extent();
        *width  = extent.width;
        *height = extent.height;
    }

    [[nodiscard]] bool supports_model3() const override { return true; }
    void submit_model3_frame(const hw::model3::VideoFrame& frame) override;
    void render_model3() override;
    void set_model3_async(bool async) override { m_model3_async = async; }
    void release_model3_frames(const hw::model3::VideoFrame& current) override { m_model3_frame.release(&current); }
    void finish_model3() override { m_model3_frame.wait(); }
    [[nodiscard]] model3::BuildTimes model3_build_times() const override
    {
        model3::BuildTimes times = m_model3_frame.build_times();
        times.record             = m_model3.prepare_ms();
        return times;
    }
    void reset_model3() override { m_model3_frame.reset(); }
    void set_model3_texture_dumper(model3::TextureDumper* dumper) override { m_model3_frame.set_texture_dumper(dumper); }
    [[nodiscard]] model3::LineOfSight model3_line_of_sight() const override
    {
        return m_model3.line_of_sight().latest();
    }

private:
    /// The shader texture-quality value for the current enhancement options,
    /// clamped to the device's anisotropy limit. 0 = faithful single-tap.
    [[nodiscard]] u32 effective_texture_quality() const;
    [[nodiscard]] u32 model3_aniso_taps() const;
    [[nodiscard]] bool ensure_model3();
    [[nodiscard]] model3::SceneGeometry model3_geometry() const
    {
        return {m_render_scale, m_columns, m_model3_wide != 0, m_model3_wide == 1};
    }

    // Model 3, created on the first Model 3 frame.
    model3::FrameBuilder m_model3_frame;
    bool                 m_model3_async = false;
    Model3Pass           m_model3;
    u32                  m_model3_width = 0;
    u32                  m_model3_wide  = 0;  // set_widescreen()'s wide_background

    Context      m_context;
    TilemapPass  m_tilemaps;
    Poly3DPass   m_polygons;
    PresentPass  m_present;
    FrameCapture m_capture;

    /// The view begin_frame() returned this frame, for the tilemap/3D calls
    /// that draw into it.
    VkImageView m_native_view = VK_NULL_HANDLE;

    bool m_capture_requested = false;

    /// Internal 3D render scale (1..kMaxRenderScale); 1 is native.
    u32 m_render_scale = 1;

    u32 m_columns         = 0;
    u32 m_pending_columns = 0;

    PresentOptions     m_present_options;
    EnhancementOptions m_enhancement_options;

    // -- ImGui's Vulkan renderer backend -------------------------------------
    /// Overlay pool size: room for two full sets of picker art, since a rescan
    /// loads the new set while the old one is still retiring.
    static constexpr u32 kOverlayDescriptorSets = 1024;
    /// Kept back for ImGui's font atlas.
    static constexpr u32 kOverlayReservedSets = 16;

    VkDescriptorPool m_overlay_pool             = VK_NULL_HANDLE;
    VkFormat         m_overlay_target_format    = VK_FORMAT_UNDEFINED;
    bool             m_overlay_renderer_ready    = false;

    // -- overlay textures (game-picker box art) ------------------------------
    // destroy_texture() retires resources here and begin_frame() reclaims them
    // after kFramesInFlight frames, as a submitted frame may still sample them.
    struct OverlayTexture {
        VkImage         image      = VK_NULL_HANDLE;
        VkImageView     view       = VK_NULL_HANDLE;
        VmaAllocation   allocation = nullptr;
        VkDescriptorSet set        = VK_NULL_HANDLE;  ///< the ImTextureID
    };
    struct RetiredTexture {
        OverlayTexture texture;
        u32            frames_remaining = 0;
    };
    /// A create_texture() staging buffer, retired the same way: its copy is
    /// recorded into the current frame, so it must outlive the frames in flight.
    struct RetiredStaging {
        VkBuffer      buffer           = VK_NULL_HANDLE;
        VmaAllocation allocation       = nullptr;
        u32           frames_remaining = 0;
    };

    VkSampler                                            m_overlay_sampler = VK_NULL_HANDLE;
    Backend::TextureHandle                               m_next_texture_handle = 1;
    std::unordered_map<Backend::TextureHandle, OverlayTexture> m_textures;
    std::vector<RetiredTexture>                          m_texture_graveyard;
    std::vector<RetiredStaging>                          m_staging_graveyard;
    bool                                                 m_warned_overlay_pool_full = false;

    void reclaim_retired_textures();
    void free_overlay_texture(OverlayTexture& texture);
};

}  // namespace sm2::render::vk
