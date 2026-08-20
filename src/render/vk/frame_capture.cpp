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
#include "render/vk/frame_capture.h"

#include "core/log.h"
#include "render/vk/context.h"

#include <vk_mem_alloc.h>

#include <cstdio>
#include <vector>

namespace sm2::render::vk {
namespace {

/// True when the format stores blue in the first byte.
///
/// This is a straight byte-order adaptation for whatever image `record()` is
/// handed, not a correction for a bug elsewhere. Today main.cpp captures the
/// renderer's own native R8G8B8A8_UNORM attachment, so the swap below stays
/// off; it fires only if a caller points the capture at a BGRA image (as it
/// did when captures were still read out of the swapchain).
[[nodiscard]] bool is_bgr(VkFormat format)
{
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SNORM:
            return true;
        default:
            return false;
    }
}

/// True when the format holds four 8-bit components, which is all this handles.
[[nodiscard]] bool is_supported(VkFormat format)
{
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SNORM:
            return true;
        default:
            return false;
    }
}

}  // namespace

FrameCapture::~FrameCapture()
{
    shutdown();
}

bool FrameCapture::init(Context& context)
{
    // Nothing is checked here any more: what gets captured is a colour attachment
    // this renderer created with TRANSFER_SRC usage, not the swapchain, so there is
    // no surface capability to depend on. record() validates the format instead,
    // where it knows what it was given.
    m_context = &context;
    return true;
}

void FrameCapture::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), m_buffer, m_allocation);
        m_buffer     = VK_NULL_HANDLE;
        m_allocation = nullptr;
        m_mapped     = nullptr;
        m_capacity   = 0;
    }
    m_context = nullptr;
    m_valid   = false;
}

bool FrameCapture::ensure_buffer(VkExtent2D extent)
{
    const usize needed = static_cast<usize>(extent.width) * extent.height * 4;
    if (m_buffer != VK_NULL_HANDLE && m_capacity >= needed) {
        return true;
    }
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), m_buffer, m_allocation);
        m_buffer     = VK_NULL_HANDLE;
        m_allocation = nullptr;
        m_mapped     = nullptr;
    }

    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size  = needed;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo info{};
    SM2_VK_TRY(vmaCreateBuffer(m_context->allocator(), &buffer, &allocation, &m_buffer,
                               &m_allocation, &info));
    m_mapped   = info.pMappedData;
    m_capacity = needed;
    return m_mapped != nullptr;
}

bool FrameCapture::record(VkImage image, VkExtent2D extent, VkFormat format)
{
    m_valid = false;

    if (image == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0) {
        return false;
    }
    if (!is_supported(format)) {
        SM2_ERROR("frame capture does not handle format %d", static_cast<int>(format));
        return false;
    }
    if (!ensure_buffer(extent)) {
        return false;
    }

    const VkCommandBuffer cmd = m_context->cmd();

    record_image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy region{};
    region.bufferRowLength   = extent.width;
    region.bufferImageHeight = extent.height;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1,
                           &region);

    // Back to the layout the caller expects, so its own present transition is
    // still correct.
    record_image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    m_extent = extent;
    m_format = format;
    m_valid  = true;
    return true;
}

bool FrameCapture::save(const std::string& path) const
{
    if (!m_valid || m_mapped == nullptr) {
        SM2_ERROR("frame capture: nothing was captured");
        return false;
    }

    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("frame capture: could not write '%s'", path.c_str());
        return false;
    }
    std::fprintf(handle, "P6\n%u %u\n255\n", m_extent.width, m_extent.height);

    const bool     swap   = is_bgr(m_format);
    const auto*    source = static_cast<const u8*>(m_mapped);
    std::vector<u8> row(static_cast<usize>(m_extent.width) * 3);

    bool ok = true;
    for (u32 y = 0; y < m_extent.height && ok; ++y) {
        const u8* line = source + static_cast<usize>(y) * m_extent.width * 4;
        for (u32 x = 0; x < m_extent.width; ++x) {
            const u8* pixel = line + static_cast<usize>(x) * 4;
            row[static_cast<usize>(x) * 3 + 0] = swap ? pixel[2] : pixel[0];
            row[static_cast<usize>(x) * 3 + 1] = pixel[1];
            row[static_cast<usize>(x) * 3 + 2] = swap ? pixel[0] : pixel[2];
        }
        ok = std::fwrite(row.data(), 1, row.size(), handle) == row.size();
    }
    std::fclose(handle);

    if (ok) {
        SM2_INFO("captured %ux%u frame to %s", m_extent.width, m_extent.height,
                 path.c_str());
    }
    return ok;
}

}  // namespace sm2::render::vk
