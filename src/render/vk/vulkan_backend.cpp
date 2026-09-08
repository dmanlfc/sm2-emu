//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
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
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

    m_render_scale = std::clamp(config.render_scale, 1U, kMaxRenderScale);

    if (!m_context.init(window, context_config)) {
        return false;
    }

    // Clamp to what the device can actually allocate. maxImageDimension2D bounds
    // a 2D image's width and height; N*496 or N*384 beyond it would fail image
    // creation, so reduce N to the largest value that fits.
    u32 max_dimension = m_context.device_properties().limits.maxImageDimension2D;
    if (const char* env = std::getenv("SM2_TEST_MAX_IMAGE_DIM")) {  // test-only override
        max_dimension = static_cast<u32>(std::strtoul(env, nullptr, 10));
    }
    const u32 fitted = clamp_scale_to_max_dimension(m_render_scale, max_dimension);
    if (fitted != m_render_scale) {
        SM2_WARN("render scale %ux (%ux%u) exceeds device maxImageDimension2D %u; "
                 "using %ux",
                 m_render_scale, scaled_width(m_render_scale),
                 scaled_height(m_render_scale), max_dimension, fitted);
        m_render_scale = fitted;
    }
    if (m_render_scale != 1) {
        SM2_INFO("render scale %ux: 3D and composite targets at %ux%u",
                 m_render_scale, scaled_width(m_render_scale),
                 scaled_height(m_render_scale));
    }

    if (!m_tilemaps.init(m_context, m_render_scale)) {
        SM2_ERROR("could not create the 2D pipeline");
        return false;
    }
    if (!m_polygons.init(m_context, m_render_scale)) {
        SM2_ERROR("could not create the 3D pipeline");
        return false;
    }
    if (!m_present.init(m_context, m_render_scale)) {
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
    // begin_frame() fenced the reused slot, so anything retired long enough ago
    // is now unreferenced.
    reclaim_retired_textures();
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
    return m_capture.record(m_present.native_image(), m_present.composite_extent(),
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

    // ImGui's own font set plus one AddTexture set per picker tile's art. 256
    // covers a screen of tiles with headroom.
    constexpr u32 kOverlayDescriptorSets = 256;
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kOverlayDescriptorSets},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = kOverlayDescriptorSets;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(m_context.device(), &pool_info, nullptr, &m_overlay_pool)
        != VK_SUCCESS) {
        SM2_ERROR("gui: failed to create descriptor pool");
        return false;
    }

    // One LINEAR clamp sampler shared by every picker texture.
    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_LINEAR;
    sampler.minFilter    = VK_FILTER_LINEAR;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.minLod       = 0.0F;
    sampler.maxLod       = 0.0F;
    if (vkCreateSampler(m_context.device(), &sampler, nullptr, &m_overlay_sampler)
        != VK_SUCCESS) {
        SM2_ERROR("gui: failed to create overlay sampler");
        vkDestroyDescriptorPool(m_context.device(), m_overlay_pool, nullptr);
        m_overlay_pool = VK_NULL_HANDLE;
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
    // Device is idle here (shutdown() waits), so free every texture outright;
    // the ImGui descriptor sets go with the pool.
    m_context.wait_idle();
    for (auto& [handle, texture] : m_textures) {
        static_cast<void>(handle);
        vkDestroyImageView(m_context.device(), texture.view, nullptr);
        vmaDestroyImage(m_context.allocator(), texture.image, texture.allocation);
    }
    m_textures.clear();
    for (RetiredTexture& retired : m_texture_graveyard) {
        vkDestroyImageView(m_context.device(), retired.texture.view, nullptr);
        vmaDestroyImage(m_context.allocator(), retired.texture.image,
                        retired.texture.allocation);
    }
    m_texture_graveyard.clear();

    ImGui_ImplVulkan_Shutdown();
    if (m_overlay_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context.device(), m_overlay_sampler, nullptr);
        m_overlay_sampler = VK_NULL_HANDLE;
    }
    if (m_overlay_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context.device(), m_overlay_pool, nullptr);
        m_overlay_pool = VK_NULL_HANDLE;
    }
    m_overlay_renderer_ready = false;
}

// ---------------------------------------------------------------------------
// Overlay textures (game-picker box art)
// ---------------------------------------------------------------------------

Backend::TextureHandle VulkanBackend::create_texture(u32 w, u32 h, const u8* rgba)
{
    if (w == 0 || h == 0 || rgba == nullptr || !m_overlay_renderer_ready) {
        return 0;
    }

    const VmaAllocator allocator = m_context.allocator();
    const VkDevice     device    = m_context.device();
    const VkDeviceSize bytes     = static_cast<VkDeviceSize>(w) * h * 4;

    OverlayTexture texture;

    VkImageCreateInfo image{};
    image.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType     = VK_IMAGE_TYPE_2D;
    image.format        = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent        = VkExtent3D{w, h, 1};
    image.mipLevels     = 1;
    image.arrayLayers   = 1;
    image.samples       = VK_SAMPLE_COUNT_1_BIT;
    image.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo image_alloc{};
    image_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator, &image, &image_alloc, &texture.image, &texture.allocation,
                       nullptr)
        != VK_SUCCESS) {
        SM2_WARN("overlay: texture image allocation failed (%ux%u)", w, h);
        return 0;
    }

    // A throwaway host-visible staging buffer; this is not a hot path, so no
    // persistent ring.
    VkBufferCreateInfo staging{};
    staging.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging.size        = bytes;
    staging.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    staging_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkBuffer          staging_buffer     = VK_NULL_HANDLE;
    VmaAllocation     staging_allocation = nullptr;
    VmaAllocationInfo staging_info{};
    if (vmaCreateBuffer(allocator, &staging, &staging_alloc, &staging_buffer,
                        &staging_allocation, &staging_info)
        != VK_SUCCESS) {
        vmaDestroyImage(allocator, texture.image, texture.allocation);
        SM2_WARN("overlay: texture staging allocation failed");
        return 0;
    }
    std::memcpy(staging_info.pMappedData, rgba, static_cast<usize>(bytes));

    // Recorded into this frame's (open) command buffer: UNDEFINED -> TRANSFER_DST
    // -> copy -> SHADER_READ_ONLY, the same shape the tilemap surfaces use.
    const VkCommandBuffer cmd = m_context.cmd();
    record_image_barrier(cmd, texture.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.bufferRowLength   = w;
    region.bufferImageHeight = h;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    record_image_barrier(cmd, texture.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // The copy is only recorded, so the staging buffer must outlive the frames
    // in flight.
    m_staging_graveyard.push_back({staging_buffer, staging_allocation, Context::kFramesInFlight});

    VkImageViewCreateInfo view{};
    view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image    = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format   = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view, nullptr, &texture.view) != VK_SUCCESS) {
        vmaDestroyImage(allocator, texture.image, texture.allocation);
        SM2_WARN("overlay: texture image view creation failed");
        return 0;
    }

    texture.set = ImGui_ImplVulkan_AddTexture(m_overlay_sampler, texture.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (texture.set == VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.view, nullptr);
        vmaDestroyImage(allocator, texture.image, texture.allocation);
        SM2_WARN("overlay: ImGui_ImplVulkan_AddTexture failed (pool exhausted?)");
        return 0;
    }

    const Backend::TextureHandle handle = m_next_texture_handle++;
    m_textures.emplace(handle, texture);
    return handle;
}

void VulkanBackend::destroy_texture(TextureHandle handle)
{
    if (handle == 0) {
        return;
    }
    const auto it = m_textures.find(handle);
    if (it == m_textures.end()) {
        return;
    }
    // An in-flight frame may still sample it; free after the frames in flight.
    m_texture_graveyard.push_back({it->second, Context::kFramesInFlight});
    m_textures.erase(it);
}

void* VulkanBackend::texture_imgui_id(TextureHandle handle) const
{
    if (handle == 0) {
        return nullptr;
    }
    const auto it = m_textures.find(handle);
    if (it == m_textures.end()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(it->second.set);
}

void VulkanBackend::free_overlay_texture(OverlayTexture& texture)
{
    if (texture.set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture.set);
        texture.set = VK_NULL_HANDLE;
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context.device(), texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context.allocator(), texture.image, texture.allocation);
        texture.image      = VK_NULL_HANDLE;
        texture.allocation = nullptr;
    }
}

void VulkanBackend::reclaim_retired_textures()
{
    for (auto it = m_texture_graveyard.begin(); it != m_texture_graveyard.end();) {
        if (it->frames_remaining > 0) {
            --it->frames_remaining;
            ++it;
            continue;
        }
        free_overlay_texture(it->texture);
        it = m_texture_graveyard.erase(it);
    }
    for (auto it = m_staging_graveyard.begin(); it != m_staging_graveyard.end();) {
        if (it->frames_remaining > 0) {
            --it->frames_remaining;
            ++it;
            continue;
        }
        vmaDestroyBuffer(m_context.allocator(), it->buffer, it->allocation);
        it = m_staging_graveyard.erase(it);
    }
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
