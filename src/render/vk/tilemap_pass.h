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

#include "hw/model2_machine_base.h"
#include "render/panorama.h"
#include "render/vk/context.h"
#include "render/vk/vk_common.h"

#include <array>
#include <span>

// Forward-declared so VMA's header stays out of the public interface.
using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::render::vk {

/// Draws the emulated 2D output into the native frame.
///
/// The System 24 tilemap layers are composited into two RGBA8 surfaces at the
/// machine's native 496x384: one for the layers that belong behind the 3D
/// output and one for the layers in front of it. This pass draws both, one
/// either side of the 3D, at one texel per pixel, composed either on the GPU
/// (compute()) or by the CPU (upload()).
///
/// Nothing here scales: the draws and the 3D's blend between them happen on the
/// hardware's own pixels, and PresentPass magnifies once afterwards.
class TilemapPass {
public:
    /// Native resolution of the Model 2 raster.
    static constexpr u32 kSourceWidth  = kNativeWidth;
    static constexpr u32 kSourceHeight = kNativeHeight;

    TilemapPass() = default;
    ~TilemapPass();

    TilemapPass(const TilemapPass&)            = delete;
    TilemapPass& operator=(const TilemapPass&) = delete;

    /// The band surfaces stay native; only the composite scope is N*native,
    /// with N = `render_scale`. `columns` widens the frame on each side for
    /// widescreen.
    [[nodiscard]] bool init(Context& context, u32 render_scale, u32 columns);

    /// Width of the band surfaces and the native frame, in native pixels.
    [[nodiscard]] u32 width() const { return frame_width(m_columns); }

    /// Draw the layers behind the 3D across the widescreen side columns.
    /// 0: plain side bars; 1: fill the sides, mirroring the edge as a last
    /// resort; 2: fill the sides, black where only a mirror would do; 3: as 2,
    /// but only for a menu panel (a row-scroll layer's foreground tile).
    void set_wide_below(u32 mode) { m_wide_below = mode; }
    void set_hud_bands(const std::array<u32, 4>& bands) { m_hud_bands = bands; }
    void shutdown();

    /// Refresh this frame's copies of tile RAM, character RAM and the pen table
    /// if their generation counters changed, then dispatch the compose shader
    /// into the staging buffers and schedule the transfer into the sampled
    /// images.
    ///
    /// Replaces only the tilemap half of hw::Model2Video::compose(), so it does
    /// not apply in render test mode or with no machine loaded.
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope.
    void compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// Copy this frame's surfaces into the staging buffers and schedule the
    /// transfer into the sampled images: the CPU-composited path.
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope. Both spans must hold kSourceWidth * kSourceHeight
    /// premultiplied RGBA8 pixels; in widescreen they land in the middle.
    ///
    /// Invalidates this frame-in-flight slot's compute() cache, since the two
    /// share the staging buffers.
    void upload(std::span<const u32> below, std::span<const u32> above);

    /// Begin drawing into the native frame: resolve the below layers against the
    /// background colour, covering every pixel.
    ///
    /// `target` is the view PresentPass::begin_frame() returned. Opens a rendering
    /// scope that record_above() closes. A non-null `stencil` is attached as the
    /// scope's depth/stencil for the 3D pass's fill mask; `stencil_depth` says
    /// whether that format also carries a depth aspect. Null leaves the scope
    /// colour-only (render test mode, no 3D).
    void record_below(VkImageView                      target,
                      u32                              background_rgba,
                      const VkRenderingAttachmentInfo* stencil,
                      bool                             stencil_depth);

    /// Draw the above layers over whatever is in the frame, and end rendering.
    void record_above();

    /// 2D upscale: 0 faithful, 1 xBR, 2 ScaleFX. Applied in the composite
    /// fragment shader; live, no reallocation.
    void set_upscale_2d(u32 mode) { m_upscale_2d = mode; }

private:
    struct Surface {
        VkBuffer        staging    = VK_NULL_HANDLE;
        VmaAllocation   allocation = nullptr;
        void*           mapped     = nullptr;
        VkImage         image      = VK_NULL_HANDLE;
        VmaAllocation   image_alloc = nullptr;
        VkImageView     view       = VK_NULL_HANDLE;
        VkDescriptorSet set        = VK_NULL_HANDLE;
    };

    /// A host-visible buffer the CPU writes and the GPU reads directly, as
    /// Poly3DPass::HostBuffer.
    struct HostBuffer {
        VkBuffer      handle     = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void*         mapped     = nullptr;
        VkDeviceSize  size       = 0;
    };

    /// The compute dispatch's own per-frame resources: copies of the machine
    /// memory it reads, and the generation counters those copies were last
    /// refreshed from.
    struct ComputeFrame {
        HostBuffer tile_ram;
        HostBuffer char_ram;
        HostBuffer pens;

        VkDescriptorSet set = VK_NULL_HANDLE;

        /// Zero means never copied; the machine's counters start at one.
        u64 tile_generation  = 0;
        u64 char_generation  = 0;
        u64 table_generation = 0;
        bool window_mask_a_hidden = false;
        u32  composed_wide        = 0;
    };

    /// Two surfaces per frame in flight: recording frame N+1 must not overwrite
    /// what frame N is still reading.
    static constexpr u32 kSurfacesPerFrame = 2;

    /// Sizes of tile RAM, character RAM and the pen table, the same on every
    /// board.
    static constexpr usize kTileRamBytes = 0x10000;
    static constexpr usize kCharRamBytes = 0x80000;
    static constexpr usize kPenCount     = 0x1000;

    [[nodiscard]] bool create_surfaces();
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_pipelines();
    [[nodiscard]] bool create_pipeline(bool blend, VkPipeline* out_pipeline);
    [[nodiscard]] bool create_compute_resources();
    [[nodiscard]] bool create_host_buffer(VkDeviceSize       size,
                                          VkBufferUsageFlags usage,
                                          HostBuffer*        out);
    void destroy_host_buffer(HostBuffer* buffer);

    [[nodiscard]] Surface&      surface(u32 index);
    [[nodiscard]] ComputeFrame& compute_frame();

    void transition_for_transfer(const Surface& surface);
    void transition_for_sampling(const Surface& surface);
    void copy_to_image(const Surface& surface);
    void dispatch_compose();

    Context* m_context = nullptr;

    VkSampler             m_sampler        = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool           = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set_layout     = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeline_layout = VK_NULL_HANDLE;

    /// Blending disabled: writes the below layers resolved against the
    /// background, replacing whatever the clear left.
    VkPipeline m_pipeline_opaque = VK_NULL_HANDLE;

    /// Blending enabled, premultiplied: draws the above layers over the 3D.
    VkPipeline m_pipeline_blend = VK_NULL_HANDLE;

    std::array<Surface, Context::kFramesInFlight * kSurfacesPerFrame> m_surfaces{};

    /// Index of the first surface belonging to the frame being recorded.
    u32 m_frame_surface_base = 0;

    /// Separate from m_pool, which is sized for the fragment pass's sets.
    VkDescriptorPool      m_compute_pool        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compute_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_compute_layout      = VK_NULL_HANDLE;
    VkPipeline            m_compute_pipeline    = VK_NULL_HANDLE;

    std::array<ComputeFrame, Context::kFramesInFlight> m_compute_frames{};

    /// Internal 3D render scale; the composite scope is N*native.
    u32 m_render_scale = 1;

    u32  m_columns    = 0;
    u32  m_wide_below = 0;
    std::array<u32, 4> m_hud_bands{};

    /// Widescreen side columns for the layers behind the 3D; see PanoramaTracker.
    PanoramaTracker m_panorama;
    PanoramaTracker::State m_panorama_state;
    u64             m_panorama_generation = 0;
    VkBuffer        m_panorama_buffer     = VK_NULL_HANDLE;
    VmaAllocation   m_panorama_allocation = nullptr;
    bool            m_panorama_cleared    = false;
    /// This frame's sky panorama, and the names generation last uploaded.
    const hw::Model2MachineBase::SkyPanorama* m_sky = nullptr;
    u64             m_sky_generation      = 0;

    u32 m_upscale_2d = 0;  ///< 0 faithful, 1 xBR, 2 ScaleFX

    [[nodiscard]] usize surface_bytes() const
    {
        return static_cast<usize>(width()) * kSourceHeight * sizeof(u32);
    }
};

}  // namespace sm2::render::vk
