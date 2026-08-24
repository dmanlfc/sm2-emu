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

#include "render/geometry.h"
#include "render/vk/context.h"
#include "render/vk/vk_common.h"

#include <array>
#include <span>

// Forward-declared so VMA's header stays out of the public interface.
using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::render::vk {

/// Owns the native-resolution frame and puts it on the screen.
///
/// Everything the machine draws goes into one 496x384 image: the tilemap layers
/// behind the 3D, then the 3D, then the layers in front of it. This pass hands
/// that image to those passes and then scales the finished result to the window
/// in a single step, letterboxed to 4:3.
///
/// Compositing before magnifying rather than after is the point. Each of the
/// three stages used to be scaled to the window separately and blended there, so
/// the blends ran on colours a linear filter had already mixed with their
/// neighbours; the difference showed as a halo at the edge of every tilemap glyph
/// where it met the 3D. It also means a screenshot is the frame the hardware
/// produced, at the size the hardware produced it, which is what makes a
/// comparison against another emulator possible at all.
class PresentPass {
public:
    static constexpr u32 kWidth  = kNativeWidth;
    static constexpr u32 kHeight = kNativeHeight;

    PresentPass() = default;
    ~PresentPass();

    PresentPass(const PresentPass&)            = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    [[nodiscard]] bool init(Context& context);
    void shutdown();

    /// Claim this frame's native image and return the view to draw into.
    ///
    /// Records the transition into a colour attachment, so it must be called with
    /// a command buffer open and outside any rendering scope. The returned view
    /// stays valid until the next call.
    [[nodiscard]] VkImageView begin_frame();

    /// The image begin_frame() handed out, for reading back a capture. Valid in
    /// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL between the end of the emulated
    /// passes and record().
    [[nodiscard]] VkImage native_image() const;

    /// Replace the native frame with pixels rendered on the CPU, in place of the
    /// tilemap and 3D passes.
    ///
    /// Same present and capture path either way, so a screenshot or the windowed
    /// view is directly comparable to the Vulkan path regardless of which one
    /// produced the pixels. `pixels` must hold kWidth * kHeight RGBA8 texels.
    /// Call after begin_frame() and before record().
    void upload_from_host(std::span<const u32> pixels);

    static constexpr VkExtent2D native_extent() { return VkExtent2D{kWidth, kHeight}; }
    static constexpr VkFormat   native_format() { return kNativeColourFormat; }

    /// Scale the finished native frame onto the swapchain.
    ///
    /// Clears the whole swapchain image first so the letterbox bars are defined,
    /// then draws into the largest 4:3 rectangle that fits. Opens and closes its
    /// own rendering scope; the swapchain image must already be in
    /// COLOR_ATTACHMENT_OPTIMAL.
    void record();

    /// Largest 4:3 rectangle centred in the swapchain.
    [[nodiscard]] VkViewport letterbox() const;

private:
    struct Target {
        VkImage         image      = VK_NULL_HANDLE;
        VmaAllocation   allocation = nullptr;
        VkImageView     view       = VK_NULL_HANDLE;
        VkDescriptorSet set        = VK_NULL_HANDLE;

        /// Staging for upload_from_host(), one per frame in flight like every
        /// other per-frame resource here.
        VkBuffer      host_staging    = VK_NULL_HANDLE;
        VmaAllocation host_allocation = nullptr;
        void*         host_mapped     = nullptr;
    };

    [[nodiscard]] bool create_targets();
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_pipeline();

    Context* m_context = nullptr;

    VkSampler             m_sampler         = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool            = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set_layout      = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;

    std::array<Target, Context::kFramesInFlight> m_targets{};

    /// Which target begin_frame() handed out.
    u32 m_current = 0;
};

}  // namespace sm2::render::vk
