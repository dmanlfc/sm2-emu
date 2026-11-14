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
/// The tilemap layers behind the 3D, the 3D, and the layers in front all draw
/// into one image, which is then scaled to the window in a single step,
/// letterboxed. Compositing before magnifying keeps blends off filtered colours,
/// which would otherwise halo at tilemap edges.
class PresentPass {
public:
    static constexpr u32 kWidth  = kNativeWidth;
    static constexpr u32 kHeight = kNativeHeight;

    PresentPass() = default;
    ~PresentPass();

    PresentPass(const PresentPass&)            = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    /// `render_scale` (1..kMaxRenderScale) sizes the composite target: the 3D
    /// and the two 2D bands are drawn into an N*native image. The software
    /// renderer's upload stays native (native_extent()).
    [[nodiscard]] bool init(Context& context, u32 render_scale, u32 width = kWidth);
    void shutdown();

    /// Size of the composite target, N*native.
    [[nodiscard]] u32 width() const { return m_native_width * m_render_scale; }
    [[nodiscard]] u32 height() const { return scaled_height(m_render_scale); }

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
    /// Same present and capture path either way. `pixels` must hold
    /// kWidth * kHeight RGBA8 texels.
    /// Call after begin_frame() and before record().
    void upload_from_host(std::span<const u32> pixels);

    [[nodiscard]] VkExtent2D native_extent() const { return VkExtent2D{m_native_width, kHeight}; }
    [[nodiscard]] u32        native_width() const { return m_native_width; }
    static constexpr VkFormat   native_format() { return kNativeColourFormat; }

    /// Full size of the composite image, N*native -- what a screenshot reads so
    /// it captures the whole render-scaled frame rather than the native corner.
    [[nodiscard]] VkExtent2D composite_extent() const
    {
        return VkExtent2D{width(), height()};
    }

    /// Adopt new present-stage options (scaling method, aspect mode, CRT).
    /// Reallocates nothing, so it is safe between frames.
    void set_options(const PresentOptions& options) { m_options = options; }

    /// Scale the finished native frame onto the swapchain.
    ///
    /// Clears the whole swapchain image first so the letterbox bars are defined,
    /// then draws into the rectangle the current aspect mode and scaling method
    /// select. Opens and closes its own rendering scope; the swapchain image
    /// must already be in COLOR_ATTACHMENT_OPTIMAL.
    void record();

    /// The rectangle the frame is drawn into for the current aspect mode and
    /// scaling method, centred in the swapchain.
    [[nodiscard]] VkViewport letterbox() const;

private:
    struct Target {
        VkImage         image      = VK_NULL_HANDLE;
        VmaAllocation   allocation = nullptr;
        VkImageView     view       = VK_NULL_HANDLE;
        /// One set per sampler filter: [0] nearest, [1] linear, so switching
        /// the filter live is just a bind.
        std::array<VkDescriptorSet, 2> set{VK_NULL_HANDLE, VK_NULL_HANDLE};

        /// Staging for upload_from_host().
        VkBuffer      host_staging    = VK_NULL_HANDLE;
        VmaAllocation host_allocation = nullptr;
        void*         host_mapped     = nullptr;
    };

    [[nodiscard]] bool create_targets();
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_pipeline();

    Context* m_context = nullptr;

    /// [0] nearest, [1] linear, created up front since a VkSampler's filter is
    /// fixed at creation.
    std::array<VkSampler, 2> m_samplers{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool      m_pool            = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set_layout      = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;
    VkPipeline            m_pipeline_crt    = VK_NULL_HANDLE;

    std::array<Target, Context::kFramesInFlight> m_targets{};

    /// Which target begin_frame() handed out.
    u32 m_current = 0;

    /// Internal 3D render scale; the composite target is N*native.
    u32 m_render_scale = 1;
    u32 m_native_width = kWidth;

    /// Live present-stage options; record()/letterbox() read these each frame.
    PresentOptions m_options;
};

}  // namespace sm2::render::vk
