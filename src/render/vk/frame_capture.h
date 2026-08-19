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

#include "render/vk/vk_common.h"

#include <string>

using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::render::vk {

class Context;

/// Reads a rendered image back off the GPU and writes it as a PPM.
///
/// This exists so that "does it look right?" is a question the build can answer on
/// its own. Without a readback the only check is a human looking at a window,
/// which cannot run unattended and cannot be diffed against a previous result.
///
/// What it is pointed at is the caller's choice, but in practice it is the native
/// 496x384 frame rather than the swapchain. That is the frame the hardware
/// produced, at the size it produced it: independent of the window, small enough
/// to keep, and directly comparable against the same frame from another emulator.
/// The cost is that the final magnification to the window is then the one stage a
/// capture does not cover.
///
/// The copy is recorded into the frame's own command buffer, so it captures what
/// was rendered rather than a re-render.
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
    /// must be in COLOR_ATTACHMENT_OPTIMAL and is left in it, so whatever the
    /// caller does with it next is unaffected.
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
