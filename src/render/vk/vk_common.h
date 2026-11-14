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

#include "core/log.h"
#include "core/types.h"
#include "render/backend.h"

#include <vulkan/vulkan.h>

#include <array>

namespace sm2::render::vk {

/// Human-readable form of a VkResult, for diagnostics.
[[nodiscard]] const char* result_string(VkResult result);

/// Log and return false when `expr` does not succeed, rather than aborting.
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

// The native frame, from render/backend.h.
using sm2::render::kDisplayAspect;
using sm2::render::kNativeHeight;
using sm2::render::kNativeWidth;

/// Format of the native frame. UNORM, as the colour chain has already applied the
/// hardware's gamma ramp; an sRGB encode would wash it out.
constexpr VkFormat kNativeColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

// GPU stage timing, from render/backend.h.
using GpuStage      = sm2::render::GpuStage;
using GpuStageTime  = sm2::render::GpuStageTime;
using GpuStageTimes = sm2::render::GpuStageTimes;

// ---------------------------------------------------------------------------
// Entry points past Vulkan 1.0
// ---------------------------------------------------------------------------

/// Core on a 1.3 device; on an older one the same functions come from the
/// KHR/EXT extensions, under their own names.
struct DeviceFunctions {
    PFN_vkCmdBeginRendering         cmd_begin_rendering         = nullptr;
    PFN_vkCmdEndRendering           cmd_end_rendering           = nullptr;
    PFN_vkCmdPipelineBarrier2       cmd_pipeline_barrier2       = nullptr;
    PFN_vkCmdWriteTimestamp2        cmd_write_timestamp2        = nullptr;
    PFN_vkQueueSubmit2              queue_submit2               = nullptr;
    PFN_vkCmdSetDepthTestEnable     cmd_set_depth_test_enable   = nullptr;
    PFN_vkCmdSetDepthWriteEnable    cmd_set_depth_write_enable  = nullptr;
    PFN_vkCmdSetDepthCompareOp      cmd_set_depth_compare_op    = nullptr;
    PFN_vkCmdSetStencilTestEnable   cmd_set_stencil_test_enable = nullptr;
};

/// Fill the table for `device`; false if any entry point is missing.
[[nodiscard]] bool load_device_functions(VkDevice device);

/// The table load_device_functions() filled.
[[nodiscard]] const DeviceFunctions& device_functions();

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Record a layout transition using synchronization2, with explicit stage and
/// access masks.
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
