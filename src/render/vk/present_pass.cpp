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
#include "render/vk/present_pass.h"

#include "core/log.h"

#include <vk_mem_alloc.h>

#include "shaders/fullscreen_quad_vert.h"
#include "shaders/tilemap_composite_frag.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::vk {
namespace {

/// Magnification filter.
///
/// Linear because the raster is scaled by a non-integer factor at almost every
/// window size, where nearest sampling makes glyph stems alternate between one
/// and two pixels wide. Switch to NEAREST for a blockier presentation.
constexpr VkFilter kMagnifyFilter = VK_FILTER_LINEAR;

/// Matches the push constants in tilemap_composite.frag.
struct PushConstants {
    float background[4];
    u32   mode;
};

/// Mode 2: copy an already-finished opaque frame.
constexpr u32 kModeCopy = 2;

[[nodiscard]] bool create_shader_module(VkDevice        device,
                                        const u32*      code,
                                        u32             word_count,
                                        VkShaderModule* out_module)
{
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<usize>(word_count) * sizeof(u32);
    info.pCode    = code;
    SM2_VK_TRY(vkCreateShaderModule(device, &info, nullptr, out_module));
    return true;
}

}  // namespace

PresentPass::~PresentPass()
{
    shutdown();
}

bool PresentPass::init(Context& context)
{
    m_context = &context;

    VkSamplerCreateInfo sampler{};
    sampler.sType      = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter  = kMagnifyFilter;
    sampler.minFilter  = kMagnifyFilter;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Clamping matters: the fullscreen triangle's texture coordinates reach
    // exactly 1.0 at the right and bottom edges, and repeating would wrap the
    // filter kernel round to the opposite side.
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SM2_VK_TRY(vkCreateSampler(context.device(), &sampler, nullptr, &m_sampler));

    return create_targets() && create_descriptors() && create_pipeline();
}

void PresentPass::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    for (Target& target : m_targets) {
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.view, nullptr);
            target.view = VK_NULL_HANDLE;
        }
        if (target.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, target.image, target.allocation);
            target.image      = VK_NULL_HANDLE;
            target.allocation = nullptr;
        }
        if (target.host_staging != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, target.host_staging, target.host_allocation);
            target.host_staging    = VK_NULL_HANDLE;
            target.host_allocation = nullptr;
            target.host_mapped     = nullptr;
        }
        target.set = VK_NULL_HANDLE;
    }

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipeline_layout, nullptr);
        m_pipeline_layout = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_set_layout, nullptr);
        m_set_layout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    m_context = nullptr;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

bool PresentPass::create_targets()
{
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    for (Target& target : m_targets) {
        VkImageCreateInfo image{};
        image.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType   = VK_IMAGE_TYPE_2D;
        image.format      = native_format();
        image.extent      = VkExtent3D{kWidth, kHeight, 1};
        image.mipLevels   = 1;
        image.arrayLayers = 1;
        image.samples     = VK_SAMPLE_COUNT_1_BIT;
        image.tiling      = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC so a screenshot can be read straight out of it, which is
        // the whole reason captures are now at the native size. TRANSFER_DST so
        // upload_from_host() can put the software renderer's output here by the
        // same path a screenshot reads it back from.
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        SM2_VK_TRY(vmaCreateImage(allocator, &image, &allocation, &target.image,
                                  &target.allocation, nullptr));

        VkImageViewCreateInfo view{};
        view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image    = target.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = native_format();
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        SM2_VK_TRY(vkCreateImageView(device, &view, nullptr, &target.view));

        // Staging for upload_from_host(). Host-coherent and persistently mapped
        // like every other per-frame CPU->GPU path in this renderer (see
        // Poly3DPass::create_host_buffer / TilemapPass::create_surfaces): the
        // whole point of this path is the software renderer's frame, written
        // once by the CPU and read once by the GPU, so there is nothing an
        // explicit flush or a device-local copy would buy.
        VkBufferCreateInfo staging{};
        staging.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging.size        = static_cast<VkDeviceSize>(kWidth) * kHeight * sizeof(u32);
        staging.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo host_allocation{};
        host_allocation.usage = VMA_MEMORY_USAGE_AUTO;
        host_allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        host_allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo host_info{};
        SM2_VK_TRY(vmaCreateBuffer(allocator, &staging, &host_allocation,
                                   &target.host_staging, &target.host_allocation,
                                   &host_info));
        target.host_mapped = host_info.pMappedData;
        if (target.host_mapped == nullptr) {
            SM2_ERROR("present: host staging buffer was not mapped");
            return false;
        }
    }
    return true;
}

bool PresentPass::create_descriptors()
{
    const VkDevice device = m_context->device();
    const u32      count  = static_cast<u32>(m_targets.size());

    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings    = &binding;
    SM2_VK_TRY(vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_set_layout));

    VkDescriptorPoolSize size{};
    size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = count;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = count;
    pool.poolSizeCount = 1;
    pool.pPoolSizes    = &size;
    SM2_VK_TRY(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool));

    // One set per target, each naming a fixed image, so nothing is rewritten
    // during a frame.
    for (Target& target : m_targets) {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = m_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts        = &m_set_layout;
        SM2_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &target.set));

        VkDescriptorImageInfo image{};
        image.sampler     = m_sampler;
        image.imageView   = target.view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = target.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    return true;
}

bool PresentPass::create_pipeline()
{
    const VkDevice device        = m_context->device();
    const VkFormat target_format = m_context->swapchain_format();

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layout{};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount         = 1;
    layout.pSetLayouts            = &m_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &range;
    SM2_VK_TRY(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipeline_layout));

    VkShaderModule vertex_module   = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    if (!create_shader_module(device, shaders::kFullscreenQuadVert,
                              shaders::kFullscreenQuadVertWordCount, &vertex_module)) {
        return false;
    }
    if (!create_shader_module(device, shaders::kTilemapCompositeFrag,
                              shaders::kTilemapCompositeFragWordCount, &fragment_module)) {
        vkDestroyShaderModule(device, vertex_module, nullptr);
        return false;
    }
    struct ModuleGuard {
        VkDevice       device;
        VkShaderModule vertex;
        VkShaderModule fragment;
        ~ModuleGuard()
        {
            vkDestroyShaderModule(device, vertex, nullptr);
            vkDestroyShaderModule(device, fragment, nullptr);
        }
    } guard{device, vertex_module, fragment_module};

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterisation{};
    rasterisation.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterisation.polygonMode = VK_POLYGON_MODE_FILL;
    rasterisation.cullMode    = VK_CULL_MODE_NONE;
    rasterisation.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterisation.lineWidth   = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    // No blending: this replaces whatever the clear left inside the letterbox.
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable    = VK_FALSE;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                              | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments    = &attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &target_format;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState      = &viewport_state;
    info.pRasterizationState = &rasterisation;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth_stencil;
    info.pColorBlendState    = &blend_state;
    info.pDynamicState       = &dynamic_state;
    info.layout              = m_pipeline_layout;
    info.renderPass          = VK_NULL_HANDLE;

    SM2_VK_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                         &m_pipeline));
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame recording
// ---------------------------------------------------------------------------

VkImageView PresentPass::begin_frame()
{
    m_current = m_context->frame_index();
    const Target& target = m_targets[m_current];

    // From UNDEFINED: the whole image is written every frame, so nothing needs
    // preserving. The wait is on the fragment shader of the frame that last
    // sampled this image, which this frame's fence has already retired.
    record_image_barrier(m_context->cmd(), target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return target.view;
}

VkImage PresentPass::native_image() const
{
    return m_targets[m_current].image;
}

void PresentPass::upload_from_host(std::span<const u32> pixels)
{
    Target&     target = m_targets[m_current];
    const usize bytes  = static_cast<usize>(kWidth) * kHeight * sizeof(u32);

    std::memcpy(target.host_mapped, pixels.data(),
               std::min(bytes, pixels.size_bytes()));

    const VkCommandBuffer cmd = m_context->cmd();

    // From COLOR_ATTACHMENT_OPTIMAL: begin_frame() just put it there, and this
    // replaces the whole image the same way the tilemap and 3D passes would
    // have, so the barrier shape matches theirs rather than a fresh UNDEFINED
    // transition.
    record_image_barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.bufferRowLength   = kWidth;
    region.bufferImageHeight = kHeight;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{kWidth, kHeight, 1};
    vkCmdCopyBufferToImage(cmd, target.host_staging, target.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Back to COLOR_ATTACHMENT_OPTIMAL: record() reads it as a sampled image via
    // its own barrier from that layout, and a capture expects it there too.
    record_image_barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

VkViewport PresentPass::letterbox() const
{
    const VkExtent2D extent = m_context->swapchain_extent();
    const float      width  = static_cast<float>(extent.width);
    const float      height = static_cast<float>(extent.height);

    float target_width  = width;
    float target_height = width / kDisplayAspect;
    if (target_height > height) {
        target_height = height;
        target_width  = height * kDisplayAspect;
    }

    VkViewport viewport{};
    viewport.x        = (width - target_width) * 0.5F;
    viewport.y        = (height - target_height) * 0.5F;
    viewport.width    = target_width;
    viewport.height   = target_height;
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    return viewport;
}

void PresentPass::record()
{
    const VkCommandBuffer cmd    = m_context->cmd();
    const VkExtent2D      extent = m_context->swapchain_extent();
    const Target&         target = m_targets[m_current];

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, GpuStage::Present, false);

    record_image_barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VkRenderingAttachmentInfo colour{};
    colour.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView   = m_context->swapchain_view();
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Cleared so the letterbox bars are defined; the draw fills the rest.
    colour.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    colour.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = VkRect2D{{0, 0}, extent};
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &colour;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport = letterbox();
    const VkRect2D   scissor{
        {static_cast<s32>(viewport.x), static_cast<s32>(viewport.y)},
        {static_cast<u32>(viewport.width), static_cast<u32>(viewport.height)}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants push{};
    push.background[3] = 1.0F;
    push.mode          = kModeCopy;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1,
                            &target.set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, GpuStage::Present, true);
}

}  // namespace sm2::render::vk
