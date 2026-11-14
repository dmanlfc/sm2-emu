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

#include "render/vk/vk_common.h"

#include <string>

using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::render::vk {

class Context;

/// Reads a rendered image back off the GPU and writes it as a PPM.
///
/// Usually pointed at the native 496x384 frame rather than the swapchain, so the
/// final magnification is not captured. The copy is recorded into the frame's
/// own command buffer, so it captures what was rendered rather than a re-render.
class FrameCapture {
public:
    FrameCapture() = default;
    ~FrameCapture();

    FrameCapture(const FrameCapture&)            = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    [[nodiscard]] bool init(Context& context);
    void shutdown();

    /// Record a copy of `image` into the readback buffer.
    ///
    /// Call with a command buffer open and outside any rendering scope. The image
    /// must be in COLOR_ATTACHMENT_OPTIMAL and is left in it.
    [[nodiscard]] bool record(VkImage image, VkExtent2D extent, VkFormat format);

    /// Write what was captured. Only valid once the submission has completed, so
    /// the caller must have waited on the frame or on the device.
    [[nodiscard]] bool save(const std::string& path) const;

private:
    [[nodiscard]] bool ensure_buffer(VkExtent2D extent);

    Context*      m_context    = nullptr;
    VkBuffer      m_buffer     = VK_NULL_HANDLE;
    VmaAllocation m_allocation = nullptr;
    void*         m_mapped     = nullptr;
    usize         m_capacity   = 0;

    VkExtent2D m_extent{0, 0};
    VkFormat   m_format = VK_FORMAT_UNDEFINED;
    bool       m_valid  = false;
};

}  // namespace sm2::render::vk
