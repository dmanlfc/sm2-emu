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
#include "render/vk/tilemap_pass.h"

#include "core/log.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <vk_mem_alloc.h>

#include "shaders/fullscreen_quad_vert.h"
#include "shaders/tilemap_compose_comp.h"
#include "shaders/tilemap_composite_frag.h"

#include <algorithm>
#include <cstring>

namespace sm2::render::vk {
namespace {

/// Bytes in one surface.
constexpr usize kSurfaceBytes =
    static_cast<usize>(TilemapPass::kSourceWidth) * TilemapPass::kSourceHeight * sizeof(u32);

struct PushConstants {
    float background[4];
    u32   mode;
};

/// mode values understood by the fragment shader.
constexpr u32 kModeResolveOverBackground = 0;
constexpr u32 kModeBlendOver             = 1;

/// Where the two draws go. One texel per pixel, so no filtering is involved and
/// the sampler's filter choice is irrelevant.
constexpr VkViewport kNativeViewport{0.0F, 0.0F,
                                    static_cast<float>(TilemapPass::kSourceWidth),
                                    static_cast<float>(TilemapPass::kSourceHeight),
                                    0.0F, 1.0F};
constexpr VkRect2D kNativeScissor{{0, 0},
                                 {TilemapPass::kSourceWidth, TilemapPass::kSourceHeight}};

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

/// As record_image_barrier (vk_common.h), for a buffer. Only this file needs
/// one so far; promote it if a second caller appears.
void record_buffer_barrier(VkCommandBuffer      cmd,
                          VkBuffer              buffer,
                          VkDeviceSize          size,
                          VkPipelineStageFlags2 src_stage,
                          VkAccessFlags2        src_access,
                          VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2        dst_access)
{
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask        = src_stage;
    barrier.srcAccessMask       = src_access;
    barrier.dstStageMask        = dst_stage;
    barrier.dstAccessMask       = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = buffer;
    barrier.offset              = 0;
    barrier.size                = size;

    VkDependencyInfo dependency{};
    dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

TilemapPass::~TilemapPass()
{
    shutdown();
}

bool TilemapPass::init(Context& context)
{
    m_context = &context;

    // NEAREST, and it never matters: these surfaces are drawn at exactly their own
    // size. Magnification happens once, later, in PresentPass.
    VkSamplerCreateInfo sampler{};
    sampler.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Clamping matters: the fullscreen triangle's texture coordinates reach
    // exactly 1.0 at the right and bottom edges, and repeating would wrap the
    // filter kernel round to the opposite side.
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    SM2_VK_TRY(vkCreateSampler(context.device(), &sampler, nullptr, &m_sampler));

    return create_surfaces() && create_descriptors() && create_pipelines()
        && create_compute_resources();
}

void TilemapPass::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    for (Surface& surface : m_surfaces) {
        if (surface.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, surface.view, nullptr);
            surface.view = VK_NULL_HANDLE;
        }
        if (surface.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, surface.image, surface.image_alloc);
            surface.image       = VK_NULL_HANDLE;
            surface.image_alloc = nullptr;
        }
        if (surface.staging != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, surface.staging, surface.allocation);
            surface.staging    = VK_NULL_HANDLE;
            surface.allocation = nullptr;
            surface.mapped     = nullptr;
        }
        surface.set = VK_NULL_HANDLE;
    }

    for (ComputeFrame& target : m_compute_frames) {
        destroy_host_buffer(&target.tile_ram);
        destroy_host_buffer(&target.char_ram);
        destroy_host_buffer(&target.pens);
        target.set = VK_NULL_HANDLE;
    }

    if (m_compute_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_compute_pipeline, nullptr);
        m_compute_pipeline = VK_NULL_HANDLE;
    }
    if (m_compute_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_compute_layout, nullptr);
        m_compute_layout = VK_NULL_HANDLE;
    }
    if (m_compute_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_compute_pool, nullptr);
        m_compute_pool = VK_NULL_HANDLE;
    }
    if (m_compute_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_compute_set_layout, nullptr);
        m_compute_set_layout = VK_NULL_HANDLE;
    }

    if (m_pipeline_blend != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline_blend, nullptr);
        m_pipeline_blend = VK_NULL_HANDLE;
    }
    if (m_pipeline_opaque != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline_opaque, nullptr);
        m_pipeline_opaque = VK_NULL_HANDLE;
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

bool TilemapPass::create_surfaces()
{
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    for (Surface& surface : m_surfaces) {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size  = kSurfaceBytes;
        // STORAGE_BUFFER so the compose shader can write into this buffer
        // directly; TRANSFER_SRC is unchanged, for the copy into the sampled
        // image right after.
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Persistently mapped and host-coherent: the CPU writes a whole surface
        // every frame, so there is nothing to gain from a device-local staging
        // dance and no need for explicit flushes.
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo info{};
        SM2_VK_TRY(vmaCreateBuffer(allocator, &buffer, &allocation, &surface.staging,
                                   &surface.allocation, &info));
        surface.mapped = info.pMappedData;
        if (surface.mapped == nullptr) {
            SM2_ERROR("tilemap: staging buffer was not mapped");
            return false;
        }

        VkImageCreateInfo image{};
        image.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType     = VK_IMAGE_TYPE_2D;
        image.format        = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent        = VkExtent3D{kSourceWidth, kSourceHeight, 1};
        image.mipLevels     = 1;
        image.arrayLayers   = 1;
        image.samples       = VK_SAMPLE_COUNT_1_BIT;
        image.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo image_allocation{};
        image_allocation.usage = VMA_MEMORY_USAGE_AUTO;
        SM2_VK_TRY(vmaCreateImage(allocator, &image, &image_allocation, &surface.image,
                                  &surface.image_alloc, nullptr));

        VkImageViewCreateInfo view{};
        view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image    = surface.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        SM2_VK_TRY(vkCreateImageView(device, &view, nullptr, &surface.view));
    }
    return true;
}

bool TilemapPass::create_descriptors()
{
    const VkDevice device = m_context->device();
    const u32      count  = static_cast<u32>(m_surfaces.size());

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

    // One set per surface. Each names a fixed image, so nothing is rewritten
    // during a frame and there is no risk of updating a set the GPU is reading.
    for (Surface& surface : m_surfaces) {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = m_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts        = &m_set_layout;
        SM2_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &surface.set));

        VkDescriptorImageInfo image{};
        image.sampler     = m_sampler;
        image.imageView   = surface.view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = surface.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    return true;
}

bool TilemapPass::create_pipelines()
{
    const VkDevice device = m_context->device();

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset     = 0;
    range.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layout{};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount         = 1;
    layout.pSetLayouts            = &m_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &range;
    SM2_VK_TRY(vkCreatePipelineLayout(device, &layout, nullptr, &m_pipeline_layout));

    return create_pipeline(false, &m_pipeline_opaque)
        && create_pipeline(true, &m_pipeline_blend);
}

bool TilemapPass::create_pipeline(bool blend, VkPipeline* out_pipeline)
{
    const VkDevice device = m_context->device();

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

    // Premultiplied source, so the source factor is ONE rather than SRC_ALPHA.
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable         = blend ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp        = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
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
    const VkFormat colour_format      = kNativeColourFormat;
    rendering.pColorAttachmentFormats = &colour_format;

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
                                         out_pipeline));
    return true;
}

bool TilemapPass::create_host_buffer(VkDeviceSize       size,
                                    VkBufferUsageFlags usage,
                                    HostBuffer*        out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size        = size;
    buffer.usage       = usage;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // Persistently mapped and host-coherent, as Poly3DPass::create_host_buffer:
    // these are written whole by the CPU and read once by the GPU per refresh,
    // which is rare after the first frame.
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo info{};
    SM2_VK_TRY(vmaCreateBuffer(m_context->allocator(), &buffer, &allocation, &out->handle,
                               &out->allocation, &info));
    out->mapped = info.pMappedData;
    out->size   = size;
    if (out->mapped == nullptr) {
        SM2_ERROR("tilemap: a host buffer was not mapped");
        return false;
    }
    return true;
}

void TilemapPass::destroy_host_buffer(HostBuffer* buffer)
{
    if (buffer->handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), buffer->handle, buffer->allocation);
        buffer->handle     = VK_NULL_HANDLE;
        buffer->allocation = nullptr;
        buffer->mapped     = nullptr;
        buffer->size       = 0;
    }
}

bool TilemapPass::create_compute_resources()
{
    const VkDevice device = m_context->device();
    const u32      frames = static_cast<u32>(m_compute_frames.size());

    for (ComputeFrame& target : m_compute_frames) {
        if (!create_host_buffer(kTileRamBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                &target.tile_ram)
            || !create_host_buffer(kCharRamBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   &target.char_ram)
            || !create_host_buffer(static_cast<VkDeviceSize>(kPenCount) * sizeof(u32),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &target.pens)) {
            return false;
        }
    }

    // Five storage buffers: tile RAM, character RAM and pens (read), below and
    // above (written -- the same staging buffers create_surfaces() made, now
    // also bound here as this frame's compose target).
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (u32 index = 0; index < 5; ++index) {
        bindings[index].binding         = index;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 5;
    layout.pBindings    = bindings;
    SM2_VK_TRY(
        vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_compute_set_layout));

    VkDescriptorPoolSize size{};
    size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = frames * 5;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = frames;
    pool.poolSizeCount = 1;
    pool.pPoolSizes    = &size;
    SM2_VK_TRY(vkCreateDescriptorPool(device, &pool, nullptr, &m_compute_pool));

    for (u32 index = 0; index < frames; ++index) {
        ComputeFrame& target = m_compute_frames[index];

        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = m_compute_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts        = &m_compute_set_layout;
        SM2_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &target.set));

        // Below/above bindings 3/4 name the same surfaces upload() used to
        // memcpy into: surface 0 of this frame index is below, surface 1 is
        // above (see kSurfacesPerFrame's layout in surface()/upload()).
        const u32 below_index = index * kSurfacesPerFrame + 0;
        const u32 above_index = index * kSurfacesPerFrame + 1;

        const VkDescriptorBufferInfo buffers[5] = {
            {target.tile_ram.handle, 0, target.tile_ram.size},
            {target.char_ram.handle, 0, target.char_ram.size},
            {target.pens.handle, 0, target.pens.size},
            {m_surfaces[below_index].staging, 0, kSurfaceBytes},
            {m_surfaces[above_index].staging, 0, kSurfaceBytes},
        };

        VkWriteDescriptorSet writes[5]{};
        for (u32 binding = 0; binding < 5; ++binding) {
            writes[binding].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet          = target.set;
            writes[binding].dstBinding      = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo     = &buffers[binding];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }

    VkPipelineLayoutCreateInfo pipeline_layout{};
    pipeline_layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts    = &m_compute_set_layout;
    SM2_VK_TRY(
        vkCreatePipelineLayout(device, &pipeline_layout, nullptr, &m_compute_layout));

    VkShaderModuleCreateInfo module_info{};
    module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = static_cast<usize>(shaders::kTilemapComposeCompWordCount)
                         * sizeof(u32);
    module_info.pCode    = shaders::kTilemapComposeComp;
    VkShaderModule module = VK_NULL_HANDLE;
    SM2_VK_TRY(vkCreateShaderModule(device, &module_info, nullptr, &module));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "main";

    VkComputePipelineCreateInfo compute_info{};
    compute_info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_info.stage  = stage;
    compute_info.layout = m_compute_layout;

    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                      &compute_info, nullptr,
                                                      &m_compute_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        SM2_ERROR("tilemap: vkCreateComputePipelines failed: %s", result_string(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame recording
// ---------------------------------------------------------------------------

TilemapPass::Surface& TilemapPass::surface(u32 index)
{
    return m_surfaces[m_frame_surface_base + index];
}

TilemapPass::ComputeFrame& TilemapPass::compute_frame()
{
    return m_compute_frames[m_context->frame_index()];
}

void TilemapPass::upload(std::span<const u32> below, std::span<const u32> above)
{
    m_frame_surface_base = m_context->frame_index() * kSurfacesPerFrame;

    const std::span<const u32> sources[kSurfacesPerFrame] = {below, above};
    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        Surface& target = surface(index);
        const usize bytes =
            std::min(kSurfaceBytes, sources[index].size_bytes());
        std::memcpy(target.mapped, sources[index].data(), bytes);
        if (bytes < kSurfaceBytes) {
            std::memset(static_cast<u8*>(target.mapped) + bytes, 0, kSurfaceBytes - bytes);
        }
    }

    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        transition_for_transfer(surface(index));
    }
    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        copy_to_image(surface(index));
    }
    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        transition_for_sampling(surface(index));
    }

    // This slot's staging buffers now hold a CPU-composited frame, not
    // whatever compute() last wrote -- force its next call to redo the
    // dispatch regardless of whether the generation counters moved. Only the
    // counters reset, not the whole struct: the buffers and descriptor set are
    // live GPU resources that must survive to be reused, not recreated.
    ComputeFrame& stale     = compute_frame();
    stale.tile_generation   = 0;
    stale.char_generation   = 0;
    stale.table_generation  = 0;
}

void TilemapPass::compute(const hw::Model2MachineBase& machine, const hw::Model2Video& video)
{
    m_frame_surface_base = m_context->frame_index() * kSurfacesPerFrame;
    ComputeFrame& target = compute_frame();

    const bool tile_changed  = target.tile_generation != machine.tile_generation();
    const bool char_changed  = target.char_generation != machine.char_generation();
    const bool table_changed = target.table_generation != machine.table_generation();
    if (!tile_changed && !char_changed && !table_changed) {
        return;
    }

    if (tile_changed) {
        const std::span<const u8> tile_ram = machine.tile_ram();
        std::memcpy(target.tile_ram.mapped, tile_ram.data(),
                   std::min<usize>(kTileRamBytes, tile_ram.size()));
        target.tile_generation = machine.tile_generation();
    }
    if (char_changed) {
        const std::span<const u8> char_ram = machine.char_ram();
        std::memcpy(target.char_ram.mapped, char_ram.data(),
                   std::min<usize>(kCharRamBytes, char_ram.size()));
        target.char_generation = machine.char_generation();
    }
    if (table_changed) {
        const std::span<const u32> pens = video.pens();
        std::memcpy(target.pens.mapped, pens.data(),
                   std::min<usize>(kPenCount, pens.size()) * sizeof(u32));
        target.table_generation = machine.table_generation();
    }

    dispatch_compose();

    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        transition_for_transfer(surface(index));
    }
    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        copy_to_image(surface(index));
    }
    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        transition_for_sampling(surface(index));
    }
}

void TilemapPass::dispatch_compose()
{
    const VkCommandBuffer cmd    = m_context->cmd();
    const ComputeFrame&   target = compute_frame();

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                               GpuStage::TilemapCompose, false);

    // The shader's writes to below/above are plain host-coherent buffers, not
    // images, so there is no layout to transition -- only a barrier making
    // sure the compute write finishes before the copy that follows reads it.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compute_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compute_layout, 0, 1,
                            &target.set, 0, nullptr);

    const u32 groups_x = (kSourceWidth + 15) / 16;
    const u32 groups_y = (kSourceHeight + 15) / 16;
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                               GpuStage::TilemapCompose, true);

    for (u32 index = 0; index < kSurfacesPerFrame; ++index) {
        record_buffer_barrier(cmd, surface(index).staging, kSurfaceBytes,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
}

void TilemapPass::transition_for_transfer(const Surface& target)
{
    // From UNDEFINED: the previous contents are about to be overwritten in full,
    // so there is nothing to preserve. The wait is on the fragment shader of the
    // frame that last sampled this image, which the frame fence has already
    // retired; the barrier is what makes that ordering explicit to the driver.
    record_image_barrier(m_context->cmd(), target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void TilemapPass::copy_to_image(const Surface& target)
{
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = kSourceWidth;
    region.bufferImageHeight = kSourceHeight;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{kSourceWidth, kSourceHeight, 1};

    vkCmdCopyBufferToImage(m_context->cmd(), target.staging, target.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void TilemapPass::transition_for_sampling(const Surface& target)
{
    record_image_barrier(m_context->cmd(), target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void TilemapPass::record_below(VkImageView target, u32 background_rgba)
{
    const VkCommandBuffer cmd = m_context->cmd();

    VkRenderingAttachmentInfo colour{};
    colour.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView   = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // No clear: this draw has blending off and covers every pixel of the frame,
    // resolving the background colour wherever no layer wrote anything.
    colour.loadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = kNativeScissor;
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &colour;
    vkCmdBeginRendering(cmd, &rendering);

    vkCmdSetViewport(cmd, 0, 1, &kNativeViewport);
    vkCmdSetScissor(cmd, 0, 1, &kNativeScissor);

    PushConstants push{};
    push.background[0] = static_cast<float>(background_rgba & 0xff) / 255.0F;
    push.background[1] = static_cast<float>((background_rgba >> 8) & 0xff) / 255.0F;
    push.background[2] = static_cast<float>((background_rgba >> 16) & 0xff) / 255.0F;
    push.background[3] = 1.0F;
    push.mode          = kModeResolveOverBackground;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_opaque);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1,
                            &surface(0).set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void TilemapPass::record_above()
{
    const VkCommandBuffer cmd = m_context->cmd();

    PushConstants push{};
    push.background[3] = 1.0F;
    push.mode          = kModeBlendOver;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_blend);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1,
                            &surface(1).set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

}  // namespace sm2::render::vk
