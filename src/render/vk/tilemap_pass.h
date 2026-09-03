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
/// either side of the 3D, at one texel per pixel.
///
/// Composition itself happens on the GPU: compute() dispatches
/// shaders/tilemap_compose.comp against tile RAM, character RAM and the pen
/// table, gated on the machine's tile_generation()/char_generation()/
/// table_generation() counters exactly as Poly3DPass gates its own texture and
/// tone-curve uploads. The shader writes the same packed-RGBA8 layout the CPU
/// path (hw::Model2Video::compose(), still used by the software renderer) used
/// to produce, into the same staging buffers, so everything downstream of that
/// -- the transfer into the sampled images and both draws -- is unchanged.
///
/// Nothing here scales. The frame is composited at the native size and magnified
/// once afterwards by PresentPass, because these two draws and the 3D's blend
/// between them have to happen on the hardware's own pixels.
class TilemapPass {
public:
    /// Native resolution of the Model 2 raster.
    static constexpr u32 kSourceWidth  = kNativeWidth;
    static constexpr u32 kSourceHeight = kNativeHeight;

    TilemapPass() = default;
    ~TilemapPass();

    TilemapPass(const TilemapPass&)            = delete;
    TilemapPass& operator=(const TilemapPass&) = delete;

    [[nodiscard]] bool init(Context& context);
    void shutdown();

    /// Refresh this frame's copies of tile RAM, character RAM and the pen table
    /// if their generation counters changed, then dispatch the compose shader
    /// into the staging buffers and schedule the transfer into the sampled
    /// images.
    ///
    /// The caller decides between this and upload(): compute() replaces the
    /// tilemap-compositing half of hw::Model2Video::compose(), not the whole of
    /// what a board's compose_video() does. It is not correct in render test
    /// mode, where the framebuffer draw over `below` has no GPU equivalent, or
    /// with no machine loaded, where there is nothing to read tile RAM from --
    /// see main.cpp's call site for the choice between this and upload() in
    /// those two cases.
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope: the copy is a transfer and the dispatch is compute, and
    /// neither can run inside a rendering scope.
    void compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// Copy this frame's surfaces into the staging buffers and schedule the
    /// transfer into the sampled images -- the CPU-composited path, for when
    /// compute() does not apply (see its own comment).
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope. Both spans must hold kSourceWidth * kSourceHeight
    /// premultiplied RGBA8 pixels.
    ///
    /// Invalidates this frame-in-flight slot's compute() cache: the two share
    /// the same staging buffers, and a slot upload() just wrote into must not
    /// have compute() later assume its own generation counters still describe
    /// what is in them.
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

        /// Zero means never copied, and the machine's own counters start at
        /// one, so the first frame always refreshes -- see Poly3DPass::Frame's
        /// texture_generation for the same convention.
        u64 tile_generation  = 0;
        u64 char_generation  = 0;
        u64 table_generation = 0;
    };

    /// Two surfaces per frame in flight: recording frame N+1 must not overwrite
    /// what frame N is still reading.
    static constexpr u32 kSurfacesPerFrame = 2;

    /// Bytes of tile RAM, character RAM and the pen table respectively, sized to
    /// the largest a board can present: model2.h/model2b.h/model2c.h/
    /// model2_original.h all `assign` these to the same sizes.
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

    /// Separate from m_pool: that pool's sizing is specific to the fragment
    /// pass's combined-image-sampler sets and this keeps the two independent.
    VkDescriptorPool      m_compute_pool        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compute_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_compute_layout      = VK_NULL_HANDLE;
    VkPipeline            m_compute_pipeline    = VK_NULL_HANDLE;

    std::array<ComputeFrame, Context::kFramesInFlight> m_compute_frames{};
};

}  // namespace sm2::render::vk
