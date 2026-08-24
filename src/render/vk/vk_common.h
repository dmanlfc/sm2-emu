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
#include "render/backend.h"

#include <vulkan/vulkan.h>

#include <array>

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
//
// kNativeWidth/kNativeHeight/kDisplayAspect live in render/backend.h now (none
// names a Vulkan type), aliased here as GpuStage already was, so existing
// render/vk/ call sites need no change.
using sm2::render::kDisplayAspect;
using sm2::render::kNativeHeight;
using sm2::render::kNativeWidth;

/// Format of the native frame. UNORM rather than SRGB because the hardware's own
/// gamma ramp has already been applied by the colour chain; an sRGB encode on top
/// would wash it out. Genuinely Vulkan-typed, unlike the three constants above, so
/// it stays here rather than moving to backend.h.
constexpr VkFormat kNativeColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

// ---------------------------------------------------------------------------
// GPU stage timing (phase 8 benchmark, design.md requirement 1.2)
// ---------------------------------------------------------------------------
//
// Aliased from render::, not redeclared: neither type names a Vulkan type, so
// they live in the backend-neutral header (render/backend.h) and this is the
// name every existing render/vk/ call site already used.
using GpuStage      = sm2::render::GpuStage;
using GpuStageTime  = sm2::render::GpuStageTime;
using GpuStageTimes = sm2::render::GpuStageTimes;

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
