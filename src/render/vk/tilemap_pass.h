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

namespace sm2::render::vk {

/// Draws the emulated 2D output into the native frame.
///
/// The System 24 tilemap layers are rasterised on the CPU into two RGBA8
/// surfaces at the machine's native 496x384: one for the layers that belong
/// behind the 3D output and one for the layers in front of it. This pass uploads
/// both and draws them, one either side of the 3D, at one texel per pixel.
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

    /// Copy this frame's surfaces into the staging buffers and schedule the
    /// transfer into the sampled images.
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope. Both spans must hold kSourceWidth * kSourceHeight
    /// premultiplied RGBA8 pixels.
    void upload(std::span<const u32> below, std::span<const u32> above);

    /// Begin drawing into the native frame: resolve the below layers against the
    /// background colour, covering every pixel.
    ///
    /// `target` is the view PresentPass::begin_frame() returned. Opens a rendering
    /// scope that record_above() closes.
    void record_below(VkImageView target, u32 background_rgba);

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

    /// Two surfaces per frame in flight: recording frame N+1 must not overwrite
    /// what frame N is still reading.
    static constexpr u32 kSurfacesPerFrame = 2;

    [[nodiscard]] bool create_surfaces();
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_pipelines();
    [[nodiscard]] bool create_pipeline(bool blend, VkPipeline* out_pipeline);

    [[nodiscard]] Surface& surface(u32 index);

    void transition_for_transfer(const Surface& surface);
    void transition_for_sampling(const Surface& surface);
    void copy_to_image(const Surface& surface);

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
};

}  // namespace sm2::render::vk
