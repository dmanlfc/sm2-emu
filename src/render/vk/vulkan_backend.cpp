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
#include "render/vk/vulkan_backend.h"

#include "core/log.h"
#include "osd/window.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace sm2::render::vk {

VulkanBackend::~VulkanBackend()
{
    shutdown();
}

bool VulkanBackend::init(osd::Window& window, const BackendConfig& config)
{
    ContextConfig context_config;
    context_config.enable_validation = config.enable_validation;
    context_config.vsync             = config.vsync;
    context_config.preferred_device  = config.preferred_device;

    if (!m_context.init(window, context_config)) {
        return false;
    }
    if (!m_tilemaps.init(m_context)) {
        SM2_ERROR("could not create the 2D pipeline");
        return false;
    }
    if (!m_polygons.init(m_context)) {
        SM2_ERROR("could not create the 3D pipeline");
        return false;
    }
    if (!m_present.init(m_context)) {
        SM2_ERROR("could not create the presentation pipeline");
        return false;
    }
    if (!m_capture.init(m_context)) {
        SM2_ERROR("could not set up frame capture");
        return false;
    }
    return true;
}

void VulkanBackend::shutdown()
{
    // Nothing may be destroyed while a submitted command buffer still refers
    // to it; the context itself waits on shutdown too, but the passes below
    // are torn down first and cannot rely on that happening before they run.
    m_context.wait_idle();

    m_capture.shutdown();
    m_present.shutdown();
    m_polygons.shutdown();
    m_tilemaps.shutdown();
    m_context.shutdown();
}

Capabilities VulkanBackend::capabilities() const
{
    Capabilities caps;
    caps.compute_shaders = true;
    caps.gpu_timing      = m_context.supports_gpu_timing();
    return caps;
}

// ---------------------------------------------------------------------------
// Per-frame sequence
// ---------------------------------------------------------------------------

bool VulkanBackend::begin_frame()
{
    m_capture_requested = false;
    if (!m_context.begin_frame()) {
        return false;
    }
    m_native_view = m_present.begin_frame();
    return true;
}

void VulkanBackend::compute_tilemap(const hw::Model2MachineBase& machine,
                                    const hw::Model2Video&       video)
{
    m_tilemaps.compute(machine, video);
}

void VulkanBackend::upload_tilemap(std::span<const u32> below, std::span<const u32> above)
{
    m_tilemaps.upload(below, above);
}

void VulkanBackend::submit_polygons(const hw::Model2MachineBase* machine,
                                    const hw::Model2Video&       video)
{
    m_polygons.build(machine, video);
}

void VulkanBackend::render_polygons()
{
    // The 3D draws inside the native-frame scope (composite_native_frame); all
    // that remains here is the fill-mask stencil transition, which cannot be
    // recorded inside a rendering scope.
    m_polygons.prepare_stencil();
}

void VulkanBackend::composite_native_frame(u32 background_rgba, bool skip_3d)
{
    // One native-frame scope: below layers, the 3D straight onto them, then the
    // above layers, with no colour image stored and re-sampled between them. The
    // stencil is attached even when skip_3d draws no 3D, because the tilemap
    // pipelines declare the scope's stencil format (dynamic rendering's
    // format-match rule, see TilemapPass::create_pipeline).
    const VkRenderingAttachmentInfo stencil = m_polygons.stencil_attachment();
    m_tilemaps.record_below(m_native_view, background_rgba, &stencil,
                            m_polygons.stencil_has_depth());
    // Render test mode cuts the DSP out: the framebuffer bank the host has been
    // drawing into is shown instead of the 3D pass, and has already been composed
    // into the layers below by the caller.
    if (!skip_3d) {
        m_polygons.draw_polygons();
    }
    m_tilemaps.record_above();
}

void VulkanBackend::submit_native_frame(std::span<const u32> pixels)
{
    m_present.upload_from_host(pixels);
}

bool VulkanBackend::request_capture()
{
    m_capture_requested = true;
    return m_capture.record(m_present.native_image(), m_present.native_extent(),
                            m_present.native_format());
}

bool VulkanBackend::save_capture(const std::string& path) const
{
    return m_capture.save(path);
}

void VulkanBackend::blit_to_swapchain()
{
    record_image_barrier(m_context.cmd(), m_context.swapchain_image(),
                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    m_present.record();
}

void VulkanBackend::begin_overlay_frame()
{
    ImGui_ImplVulkan_NewFrame();
}

void VulkanBackend::draw_overlay(bool active)
{
    const VkCommandBuffer cmd = m_context.cmd();

    if (!active) {
        // Still nothing to submit -- ImGui::Render() already ran (via
        // gui.end_frame(), called by the caller before this), so there is no
        // draw data this frame needs, but no command buffer work either.
        return;
    }

    // Open a dynamic rendering scope on the swapchain for ImGui, loading its
    // existing contents rather than clearing: this draws over the frame
    // blit_to_swapchain() already presented.
    VkRenderingAttachmentInfo colour_att{};
    colour_att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour_att.imageView   = m_context.swapchain_view();
    colour_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour_att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour_att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent    = m_context.swapchain_extent();
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &colour_att;

    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

bool VulkanBackend::end_frame()
{
    record_image_barrier(m_context.cmd(), m_context.swapchain_image(),
                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    return m_context.end_frame();
}

void VulkanBackend::wait_idle()
{
    m_context.wait_idle();
}

// ---------------------------------------------------------------------------
// GPU stage timing
// ---------------------------------------------------------------------------

bool VulkanBackend::supports_gpu_timing() const
{
    return m_context.supports_gpu_timing();
}

GpuStageTimes VulkanBackend::read_stage_times()
{
    return m_context.read_stage_times();
}

// ---------------------------------------------------------------------------
// ImGui's Vulkan renderer backend
// ---------------------------------------------------------------------------
// Ported unchanged from what osd::Gui::init/shutdown/render used to do
// directly; only the ownership moved. See gui.h's comment on why: which GPU
// API draws ImGui's widgets is a render backend's concern, not the widget
// code's.
//
// `gui` itself is unused: what these calls actually need is ImGui's global
// context, which by the caller's contract already exists (init_overlay()) or
// still exists (shutdown_overlay()) by the time these run. The parameter is
// still part of the interface so the ordering requirement is visible at every
// call site rather than only in a comment.

bool VulkanBackend::init_overlay(osd::Gui& /*gui*/)
{
    m_overlay_target_format = m_context.swapchain_format();

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = 16;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(m_context.device(), &pool_info, nullptr, &m_overlay_pool)
        != VK_SUCCESS) {
        SM2_ERROR("gui: failed to create descriptor pool");
        return false;
    }

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion         = VK_API_VERSION_1_3;
    init_info.Instance           = m_context.instance();
    init_info.PhysicalDevice     = m_context.physical_device();
    init_info.Device             = m_context.device();
    init_info.QueueFamily        = m_context.graphics_family();
    init_info.Queue              = m_context.graphics_queue();
    init_info.DescriptorPool     = m_overlay_pool;
    init_info.MinImageCount      = Context::kFramesInFlight;
    init_info.ImageCount         = Context::kFramesInFlight;
    init_info.MSAASamples        = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn     = [](VkResult result) {
        if (result != VK_SUCCESS) {
            SM2_ERROR("gui: Vulkan error %d", static_cast<int>(result));
        }
    };

    // Dynamic rendering format info -- must point to stable storage.
    init_info.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_overlay_target_format;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        SM2_ERROR("gui: ImGui_ImplVulkan_Init failed");
        vkDestroyDescriptorPool(m_context.device(), m_overlay_pool, nullptr);
        m_overlay_pool = VK_NULL_HANDLE;
        return false;
    }

    m_overlay_renderer_ready = true;
    return true;
}

void VulkanBackend::shutdown_overlay()
{
    if (!m_overlay_renderer_ready) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    if (m_overlay_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context.device(), m_overlay_pool, nullptr);
        m_overlay_pool = VK_NULL_HANDLE;
    }
    m_overlay_renderer_ready = false;
}

}  // namespace sm2::render::vk

namespace sm2::render {

std::vector<std::string> enumerate_render_devices()
{
    return vk::Context::enumerate_device_names();
}

std::unique_ptr<Backend> create_vulkan_backend()
{
    return std::make_unique<vk::VulkanBackend>();
}

}  // namespace sm2::render
