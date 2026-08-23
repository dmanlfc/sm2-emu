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

#include <array>
#include <string>
#include <vector>

// Forward-declared so that VMA's header stays out of the public interface.
using VmaAllocator = struct VmaAllocator_T*;

namespace sm2::osd {
class Window;
}

namespace sm2::render::vk {

struct ContextConfig {
    bool        enable_validation = false;
    bool        vsync             = true;
    /// Exact VkPhysicalDeviceProperties::deviceName to prefer. Empty means
    /// "pick the best scoring device".
    std::string preferred_device;
};

/// Vulkan instance, device, swapchain and per-frame synchronisation.
///
/// Baseline is Vulkan 1.3 core. That buys dynamic rendering and
/// synchronization2 without extension juggling, which removes VkRenderPass and
/// VkFramebuffer from the codebase entirely. Model 2 needs nothing beyond
/// this: no geometry shaders (all geometry work is on the CPU, as it was on the
/// real hardware), no bindless, no sampler LOD bias.
class Context {
public:
    /// Three rather than two so that recording frame N+1 does not wait on the
    /// GPU finishing frame N. Costs one frame of latency and turns a frame into
    /// max(CPU, GPU) instead of CPU + GPU.
    static constexpr u32 kFramesInFlight = 3;

    Context() = default;
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    [[nodiscard]] bool init(osd::Window& window, const ContextConfig& config);
    void shutdown();

    // -- frame lifecycle ---------------------------------------------------

    /// Wait for this frame's fence, acquire a swapchain image and begin
    /// recording. Returns false when there is nothing to draw into (window
    /// minimised, or the swapchain needed rebuilding); the caller should skip
    /// the frame rather than treat it as an error.
    [[nodiscard]] bool begin_frame();

    /// Finish recording, submit and present.
    [[nodiscard]] bool end_frame();

    /// Rebuild the swapchain against the window's current drawable size.
    [[nodiscard]] bool recreate_swapchain();

    /// Block until the device is idle.
    ///
    /// Required before destroying anything a submitted command buffer still
    /// refers to. Renderers own their pipelines and are torn down before the
    /// context, so they cannot rely on the context's own shutdown wait.
    void wait_idle();

    // -- GPU stage timing (phase 8 benchmark) -------------------------------

    /// Whether this device can report GPU timestamps at all. False makes
    /// write_timestamp() and stage_times() both no-ops rather than crash, so a
    /// benchmark run says "not supported" instead of reporting zeros as if they
    /// were measured -- see design.md requirement 1 and the BRIEF's warning
    /// against exactly that failure mode.
    [[nodiscard]] bool supports_gpu_timing() const { return m_timestamp_period > 0.0F; }

    /// Record a timestamp for `stage`'s start or end into the current frame's
    /// query slot. No-op if supports_gpu_timing() is false. `is_end` selects
    /// which of the stage's two queries this writes; the caller is responsible
    /// for calling both once per frame for a stage it wants timed, in program
    /// order, since a stage that is skipped some frames (the decode dispatch)
    /// must still write both or leave both unwritten, never one.
    void write_timestamp(VkPipelineStageFlags2 stage_mask, GpuStage stage, bool is_end);

    /// GPU times from the oldest still-in-flight slot, which by construction
    /// finished at least kFramesInFlight-1 frames ago and so is always safe to
    /// read without a wait. Call once per frame, after begin_frame(); the result
    /// lags real time by a couple of frames, which is fine for a benchmark
    /// averaged over thousands of them.
    [[nodiscard]] GpuStageTimes read_stage_times();

    // -- accessors ---------------------------------------------------------

    [[nodiscard]] VkDevice         device()          const { return m_device; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return m_physical_device; }
    [[nodiscard]] VkInstance       instance()        const { return m_instance; }
    [[nodiscard]] VmaAllocator     allocator()       const { return m_allocator; }
    [[nodiscard]] VkQueue          graphics_queue()  const { return m_graphics_queue; }
    [[nodiscard]] u32              graphics_family() const { return m_graphics_family; }

    /// Command buffer for the frame currently being recorded.
    [[nodiscard]] VkCommandBuffer cmd() const { return m_command_buffers[m_frame_index]; }

    /// Index into the per-frame resource rings, in [0, kFramesInFlight).
    [[nodiscard]] u32 frame_index() const { return m_frame_index; }

    [[nodiscard]] VkImage     swapchain_image() const { return m_swapchain_images[m_image_index]; }
    [[nodiscard]] VkImageView swapchain_view()  const { return m_swapchain_views[m_image_index]; }
    [[nodiscard]] VkFormat    swapchain_format() const { return m_swapchain_format; }
    [[nodiscard]] VkExtent2D  swapchain_extent() const { return m_swapchain_extent; }

    /// Stencil-capable format for the fill-mask attachment.
    ///
    /// Model 2 has no depth buffer: polygons are sorted front-to-back on the
    /// CPU and a one-bit fill mask stops anything overwriting a pixel already
    /// drawn. S8_UINT expresses exactly that, but it is optional in Vulkan and
    /// several desktop drivers lack it, so this may name a combined format
    /// whose depth aspect goes unused.
    [[nodiscard]] VkFormat stencil_format()      const { return m_stencil_format; }
    [[nodiscard]] bool     stencil_format_has_depth() const;

    [[nodiscard]] const VkPhysicalDeviceProperties& device_properties() const
    {
        return m_device_properties;
    }

    /// Names of every device that could be selected, for configuration UI.
    [[nodiscard]] static std::vector<std::string> enumerate_device_names();

private:
    [[nodiscard]] bool create_instance(const ContextConfig& config);
    [[nodiscard]] bool create_debug_messenger();
    [[nodiscard]] bool select_physical_device(const ContextConfig& config);
    [[nodiscard]] bool create_device();
    [[nodiscard]] bool create_allocator();
    [[nodiscard]] bool create_command_resources();
    [[nodiscard]] bool create_sync_resources();
    [[nodiscard]] bool create_swapchain(VkSwapchainKHR old_swapchain);
    [[nodiscard]] bool create_present_semaphores();
    [[nodiscard]] bool create_query_pool();
    void destroy_swapchain_views();
    void pick_stencil_format();
    void read_back_stage_times(u32 frame_index);

    osd::Window* m_window = nullptr;

    VkInstance               m_instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface         = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physical_device = VK_NULL_HANDLE;
    VkDevice                 m_device          = VK_NULL_HANDLE;
    VmaAllocator             m_allocator       = nullptr;

    VkPhysicalDeviceProperties m_device_properties{};

    u32     m_graphics_family = VK_QUEUE_FAMILY_IGNORED;
    u32     m_present_family  = VK_QUEUE_FAMILY_IGNORED;
    VkQueue m_graphics_queue  = VK_NULL_HANDLE;
    VkQueue m_present_queue   = VK_NULL_HANDLE;

    VkSwapchainKHR           m_swapchain     = VK_NULL_HANDLE;
    VkFormat                 m_swapchain_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR          m_swapchain_colour_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D               m_swapchain_extent{0, 0};
    VkPresentModeKHR         m_present_mode  = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     m_swapchain_images;
    std::vector<VkImageView> m_swapchain_views;

    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;

    VkCommandPool                                m_command_pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> m_command_buffers{};
    std::array<VkSemaphore,     kFramesInFlight> m_image_available{};
    std::array<VkFence,         kFramesInFlight> m_in_flight{};

    /// One per swapchain image, not per frame in flight: a binary semaphore
    /// must not be signalled again while an earlier present may still be
    /// waiting on it, and presents retire in swapchain-image order.
    std::vector<VkSemaphore> m_render_finished;

    /// Fence of the frame that last used each swapchain image, so a reused
    /// image is waited on even when it comes back out of step with the frame
    /// ring.
    std::vector<VkFence> m_images_in_flight;

    u32  m_frame_index      = 0;
    u32  m_image_index      = 0;
    bool m_frame_active     = false;
    bool m_vsync            = true;
    bool m_needs_recreation = false;
    bool m_portability       = false;

    // -- GPU stage timing ----------------------------------------------------
    // Two queries per stage (begin, end), one set per frame in flight so that
    // recording frame N+1 never overwrites a query frame N's readback has not
    // happened yet. Nanoseconds per tick; zero from vkGetPhysicalDeviceProperties
    // means the device did not report a period, which read_stage_times() treats
    // the same as no support at all rather than dividing by zero.
    static constexpr u32 kQueriesPerStage = 2;
    static constexpr u32 kStageCount      = static_cast<u32>(GpuStage::kCount);
    static constexpr u32 kQueriesPerFrame = kStageCount * kQueriesPerStage;

    VkQueryPool m_query_pool       = VK_NULL_HANDLE;
    float       m_timestamp_period = 0.0F;

    /// Populated by read_back_stage_times() at the start of begin_frame(), the
    /// one point every frame where a slot's queries are both retired and about
    /// to be reset; read_stage_times() hands this back to the caller unchanged.
    /// "Did a stage run" comes from vkGetQueryPoolResults' own availability bit
    /// on each of that stage's begin/end queries -- a stage skipped this frame
    /// (the decode dispatch, when texture_generation did not change) simply
    /// never had its pair written, so both come back unavailable rather than
    /// needing a separate written-flag to track the same fact twice.
    GpuStageTimes m_last_stage_times{};
};

}  // namespace sm2::render::vk
