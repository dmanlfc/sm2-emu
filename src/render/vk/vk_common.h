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

#include "core/log.h"
#include "core/types.h"

#include <vulkan/vulkan.h>

namespace sm2::render::vk {

/// Human-readable form of a VkResult, for diagnostics.
[[nodiscard]] const char* result_string(VkResult result);

/// Log and return false when `expr` does not succeed.
///
/// Errors here are reported and propagated rather than aborting: a failed
/// swapchain recreation is recoverable, and a device-lost on someone else's
/// driver should produce a useful message, not a crash.
#define SM2_VK_TRY(expr)                                                    \
    do {                                                                    \
        const VkResult sm2_vk_result = (expr);                              \
        if (sm2_vk_result != VK_SUCCESS) {                                  \
            SM2_ERROR("%s:%d: %s failed: %s", __FILE__, __LINE__, #expr,    \
                      ::sm2::render::vk::result_string(sm2_vk_result));     \
            return false;                                                   \
        }                                                                   \
    } while (0)

/// As SM2_VK_TRY, but for void-returning callers.
#define SM2_VK_WARN_ON_FAIL(expr)                                           \
    do {                                                                    \
        const VkResult sm2_vk_result = (expr);                              \
        if (sm2_vk_result != VK_SUCCESS) {                                  \
            SM2_WARN("%s:%d: %s failed: %s", __FILE__, __LINE__, #expr,     \
                     ::sm2::render::vk::result_string(sm2_vk_result));      \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// The native frame
// ---------------------------------------------------------------------------

/// The Model 2 raster. Every emulated pass draws at exactly this size.
///
/// Nothing is rendered at the window's resolution. The hardware's translucency is
/// a checkerboard stipple locked to the raster grid and its texture level of
/// detail comes from raster-pixel derivatives, so both would come out at the wrong
/// scale; and the three-way composite between the tilemap layers and the 3D has to
/// happen before any magnification or its blends are performed on interpolated
/// colours. The finished frame is scaled to the window once, at the end.
constexpr u32 kNativeWidth  = 496;
constexpr u32 kNativeHeight = 384;

/// Format of that frame. UNORM rather than SRGB because the hardware's own gamma
/// ramp has already been applied by the colour chain; an sRGB encode on top would
/// wash it out.
constexpr VkFormat kNativeColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

/// Aspect ratio the frame is presented at.
///
/// The raster is 496x384, which is 1.29:1, but an arcade monitor stretched it to
/// the usual 4:3, so a square-pixel presentation would be noticeably narrow.
constexpr float kDisplayAspect = 4.0F / 3.0F;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Record a layout transition using synchronization2.
///
/// Stage and access masks are supplied explicitly rather than derived from the
/// layouts: the conservative derivation is easy to get subtly wrong, and every
/// transition in this renderer has a known producer and consumer.
void record_image_barrier(VkCommandBuffer   cmd,
                          VkImage           image,
                          VkImageAspectFlags aspect,
                          VkImageLayout     old_layout,
                          VkImageLayout     new_layout,
                          VkPipelineStageFlags2 src_stage,
                          VkAccessFlags2        src_access,
                          VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2        dst_access);

}  // namespace sm2::render::vk
