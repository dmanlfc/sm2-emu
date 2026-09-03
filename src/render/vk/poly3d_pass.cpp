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
//
// The texture, colour and fill-mask rules reproduced here are those of MAME's
// src/mame/sega/model2_v.cpp and model2rd.ipp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
#include "render/vk/poly3d_pass.h"

#include "core/log.h"
#include "hw/geometrizer.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <vk_mem_alloc.h>

#include "shaders/polygon_frag.h"
#include "shaders/polygon_vert.h"
#include "shaders/texel_decode_comp.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace sm2::render::vk {
namespace {

/// Push constants of the polygon pipeline.
struct PolygonPush {
    float inv_raster[2];
};

/// Bytes of luminance RAM, which is one tone curve per 128 entries.
constexpr usize kLumaBytes = 0x8000;

/// Format of the decoded texture sheets: four independent 8-bit nibble values,
/// one per texel of a 2x2 container, addressed by uint so the fragment shader
/// can pick a channel exactly rather than have a sampler interpolate one.
/// Guaranteed by Vulkan 1.3 core for both storage and sampled use, unlike a
/// single-channel format, which several drivers only support for one or the
/// other.
constexpr VkFormat kDecodedFormat = VK_FORMAT_R8G8B8A8_UINT;

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

/// Destroys a pair of shader modules however the caller leaves.
struct ModuleGuard {
    VkDevice       device;
    VkShaderModule vertex   = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;

    ~ModuleGuard()
    {
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertex, nullptr);
        }
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fragment, nullptr);
        }
    }
};

}  // namespace

Poly3DPass::~Poly3DPass()
{
    shutdown();
}

bool Poly3DPass::init(Context& context)
{
    m_context           = &context;
    m_stencil_format    = context.stencil_format();
    m_stencil_has_depth = context.stencil_format_has_depth();

    if (m_stencil_format == VK_FORMAT_UNDEFINED) {
        SM2_ERROR("3d: no stencil format, so the fill mask cannot be built");
        return false;
    }

    // Nearest, and only ever read with texelFetch: the tone curve is a lookup
    // table, so interpolating between its entries would be meaningless.
    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_NEAREST;
    sampler.minFilter    = VK_FILTER_NEAREST;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SM2_VK_TRY(vkCreateSampler(context.device(), &sampler, nullptr, &m_sampler));

    // Same rule for the decoded texture image: the fragment shader's own filter
    // does the bilinear blend between a container's neighbours, so this sampler
    // must return the exact stored value, never one the hardware has already
    // mixed.
    SM2_VK_TRY(vkCreateSampler(context.device(), &sampler, nullptr, &m_decoded_sampler));

    m_frame_geometry.vertices.reserve(1 << 14);
    m_frame_geometry.polygons.reserve(1 << 12);
    m_tone_curve.assign(static_cast<usize>(hw::Model2Video::kToneShades)
                            * hw::Model2Video::kToneComponents,
                        0);

    return create_frames() && create_descriptors() && create_polygon_pipeline()
        && create_decode_pipeline();
}

void Poly3DPass::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    for (Frame& target : m_frames) {
        destroy_host_buffer(&target.vertices);
        destroy_host_buffer(&target.polygons);
        destroy_host_buffer(&target.sheets);
        destroy_host_buffer(&target.luma);
        destroy_host_buffer(&target.tone_staging);

        if (target.tone_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.tone_view, nullptr);
            target.tone_view = VK_NULL_HANDLE;
        }
        if (target.tone != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, target.tone, target.tone_alloc);
            target.tone       = VK_NULL_HANDLE;
            target.tone_alloc = nullptr;
        }
        if (target.decoded_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.decoded_view, nullptr);
            target.decoded_view = VK_NULL_HANDLE;
        }
        if (target.decoded != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, target.decoded, target.decoded_alloc);
            target.decoded       = VK_NULL_HANDLE;
            target.decoded_alloc = nullptr;
        }
        if (target.stencil_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.stencil_view, nullptr);
            target.stencil_view = VK_NULL_HANDLE;
        }
        if (target.stencil != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, target.stencil, target.stencil_alloc);
            target.stencil       = VK_NULL_HANDLE;
            target.stencil_alloc = nullptr;
        }
        target.polygon_set   = VK_NULL_HANDLE;
        target.decode_set    = VK_NULL_HANDLE;
    }

    if (m_decode_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_decode_pipeline, nullptr);
        m_decode_pipeline = VK_NULL_HANDLE;
    }
    if (m_decode_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_decode_layout, nullptr);
        m_decode_layout = VK_NULL_HANDLE;
    }
    if (m_decode_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_decode_set_layout, nullptr);
        m_decode_set_layout = VK_NULL_HANDLE;
    }
    if (m_decoded_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_decoded_sampler, nullptr);
        m_decoded_sampler = VK_NULL_HANDLE;
    }

    if (m_polygon_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_polygon_pipeline, nullptr);
        m_polygon_pipeline = VK_NULL_HANDLE;
    }
    if (m_polygon_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_polygon_layout, nullptr);
        m_polygon_layout = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_polygon_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_polygon_set_layout, nullptr);
        m_polygon_set_layout = VK_NULL_HANDLE;
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

bool Poly3DPass::create_host_buffer(VkDeviceSize       size,
                                    VkBufferUsageFlags usage,
                                    HostBuffer*        out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size        = size;
    buffer.usage       = usage;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // Persistently mapped and host-coherent. Every one of these is written whole
    // by the CPU and read once by the GPU, so there is nothing for an explicit
    // flush or a device-local copy to gain.
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
        SM2_ERROR("3d: a host buffer was not mapped");
        return false;
    }
    return true;
}

void Poly3DPass::destroy_host_buffer(HostBuffer* buffer)
{
    if (buffer->handle != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), buffer->handle, buffer->allocation);
        buffer->handle     = VK_NULL_HANDLE;
        buffer->allocation = nullptr;
        buffer->mapped     = nullptr;
        buffer->size       = 0;
    }
}

bool Poly3DPass::create_frames()
{
    const VkDevice     device    = m_context->device();
    const VmaAllocator allocator = m_context->allocator();

    const VkImageAspectFlags stencil_aspect =
        m_stencil_has_depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_STENCIL_BIT;

    const VkDeviceSize tone_bytes =
        static_cast<VkDeviceSize>(m_tone_curve.size()) * sizeof(u32);

    for (Frame& target : m_frames) {
        if (!create_host_buffer(static_cast<VkDeviceSize>(kMaxVertices) * sizeof(Vertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &target.vertices)
            || !create_host_buffer(
                static_cast<VkDeviceSize>(kMaxPolygons) * sizeof(PolyParams),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &target.polygons)
            || !create_host_buffer(static_cast<VkDeviceSize>(kSheetWords) * 2 * sizeof(u32),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &target.sheets)
            || !create_host_buffer(kLumaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   &target.luma)
            || !create_host_buffer(tone_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   &target.tone_staging)) {
            return false;
        }

        VkImageCreateInfo image{};
        image.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType     = VK_IMAGE_TYPE_2D;
        image.format        = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent        = VkExtent3D{hw::Model2Video::kToneShades,
                                  hw::Model2Video::kToneComponents, 1};
        image.mipLevels     = 1;
        image.arrayLayers   = 1;
        image.samples       = VK_SAMPLE_COUNT_1_BIT;
        image.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo image_allocation{};
        image_allocation.usage = VMA_MEMORY_USAGE_AUTO;
        SM2_VK_TRY(vmaCreateImage(allocator, &image, &image_allocation, &target.tone,
                                  &target.tone_alloc, nullptr));

        VkImageViewCreateInfo view{};
        view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image    = target.tone;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        SM2_VK_TRY(vkCreateImageView(device, &view, nullptr, &target.tone_view));

        // The fill mask never leaves the GPU and never survives a frame.
        image.format = m_stencil_format;
        image.extent = VkExtent3D{kWidth, kHeight, 1};
        image.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        SM2_VK_TRY(vmaCreateImage(allocator, &image, &image_allocation, &target.stencil,
                                  &target.stencil_alloc, nullptr));

        view.image                       = target.stencil;
        view.format                      = m_stencil_format;
        view.subresourceRange.aspectMask = stencil_aspect;
        SM2_VK_TRY(vkCreateImageView(device, &view, nullptr, &target.stencil_view));

        // The decoded texture sheets: two array layers, one per sheet, each
        // holding a 2x2 texel container's four nibbles in its four channels. Used
        // for both the compute pass's writes and the fragment pass's reads, which
        // is why the usage carries both STORAGE and SAMPLED.
        image.format         = kDecodedFormat;
        image.extent         = VkExtent3D{kDecodedWidth, kDecodedHeight, 1};
        image.arrayLayers    = 2;
        image.usage          = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        SM2_VK_TRY(vmaCreateImage(allocator, &image, &image_allocation, &target.decoded,
                                  &target.decoded_alloc, nullptr));

        view.image                       = target.decoded;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format                      = kDecodedFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.layerCount = 2;
        SM2_VK_TRY(vkCreateImageView(device, &view, nullptr, &target.decoded_view));
    }
    return true;
}

bool Poly3DPass::create_descriptors()
{
    const VkDevice device = m_context->device();
    const u32      frames = static_cast<u32>(m_frames.size());

    // Set 0 of the polygon pipeline: the decoded texture sheets, luminance RAM,
    // the tone curve and the per-polygon parameters. Binding 0 used to be the
    // packed sheets as a storage buffer; the decode pass now does the unpacking,
    // so the fragment shader instead samples the image it produces.
    VkDescriptorSetLayoutBinding polygon_bindings[4]{};
    for (u32 index = 0; index < 4; ++index) {
        polygon_bindings[index].binding         = index;
        polygon_bindings[index].descriptorCount = 1;
        polygon_bindings[index].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        polygon_bindings[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    polygon_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    polygon_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 4;
    layout.pBindings    = polygon_bindings;
    SM2_VK_TRY(vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_polygon_set_layout));

    // The decode pass's own set: the packed sheets it reads, and the decoded
    // image it writes.
    VkDescriptorSetLayoutBinding decode_bindings[2]{};
    decode_bindings[0].binding         = 0;
    decode_bindings[0].descriptorCount = 1;
    decode_bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    decode_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    decode_bindings[1].binding         = 1;
    decode_bindings[1].descriptorCount = 1;
    decode_bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    decode_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    layout.bindingCount = 2;
    layout.pBindings    = decode_bindings;
    SM2_VK_TRY(vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_decode_set_layout));

    // Storage buffers: luma, polygons (per polygon-set) and the sheets, once per
    // frame each for the polygon set's use and once more per frame for the decode
    // set's own read of the same buffer. Combined image samplers: the decoded
    // sheets and the tone curve per polygon set. Storage images: the decoded
    // sheets, once per decode set. The 3D now draws straight into the native
    // frame, so there is no offscreen colour image to sample and no composite
    // set for it.
    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // Per frame: luma + polygons (polygon set) + sheets (decode set).
    sizes[0].descriptorCount = frames * 3;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // Per frame: decoded sheets + tone curve (polygon set).
    sizes[1].descriptorCount = frames * 2;
    sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    // Per frame: decoded sheets (decode set).
    sizes[2].descriptorCount = frames;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = frames * 2;  // polygon + decode, per frame
    pool.poolSizeCount = 3;
    pool.pPoolSizes    = sizes;
    SM2_VK_TRY(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool));

    // Every set names fixed resources, so nothing is rewritten during a frame and
    // there is no risk of updating a set the GPU is reading.
    for (Frame& target : m_frames) {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = m_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts        = &m_polygon_set_layout;
        SM2_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &target.polygon_set));

        allocate.pSetLayouts = &m_decode_set_layout;
        SM2_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &target.decode_set));

        const VkDescriptorBufferInfo buffers[2] = {
            {target.luma.handle, 0, target.luma.size},
            {target.polygons.handle, 0, target.polygons.size},
        };
        VkDescriptorImageInfo decoded{};
        decoded.sampler     = m_decoded_sampler;
        decoded.imageView   = target.decoded_view;
        decoded.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo tone{};
        tone.sampler     = m_sampler;
        tone.imageView   = target.tone_view;
        tone.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const VkDescriptorBufferInfo decode_sheets{target.sheets.handle, 0,
                                                    target.sheets.size};
        VkDescriptorImageInfo decode_target{};
        decode_target.imageView   = target.decoded_view;
        decode_target.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[6]{};
        for (VkWriteDescriptorSet& write : writes) {
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        writes[0].dstSet         = target.polygon_set;
        writes[0].dstBinding     = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo     = &decoded;

        writes[1].dstSet         = target.polygon_set;
        writes[1].dstBinding     = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo    = &buffers[0];

        writes[2].dstSet         = target.polygon_set;
        writes[2].dstBinding     = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo     = &tone;

        writes[3].dstSet         = target.polygon_set;
        writes[3].dstBinding     = 3;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo    = &buffers[1];

        writes[4].dstSet         = target.decode_set;
        writes[4].dstBinding     = 0;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo    = &decode_sheets;

        writes[5].dstSet         = target.decode_set;
        writes[5].dstBinding     = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[5].pImageInfo     = &decode_target;

        vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
    }
    return true;
}

bool Poly3DPass::create_polygon_pipeline()
{
    const VkDevice device = m_context->device();

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    range.size       = sizeof(PolygonPush);

    VkPipelineLayoutCreateInfo layout{};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount         = 1;
    layout.pSetLayouts            = &m_polygon_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &range;
    SM2_VK_TRY(vkCreatePipelineLayout(device, &layout, nullptr, &m_polygon_layout));

    ModuleGuard guard{device};
    if (!create_shader_module(device, shaders::kPolygonVert,
                              shaders::kPolygonVertWordCount, &guard.vertex)) {
        return false;
    }
    if (!create_shader_module(device, shaders::kPolygonFrag,
                              shaders::kPolygonFragWordCount, &guard.fragment)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = guard.vertex;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = guard.fragment;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding   = 0;
    vertex_binding.stride    = sizeof(Vertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[4]{};
    attributes[0].location = 0;
    attributes[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset   = offsetof(Vertex, x);
    attributes[1].location = 1;
    attributes[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset   = offsetof(Vertex, u);
    attributes[2].location = 2;
    attributes[2].format   = VK_FORMAT_R32_SFLOAT;
    attributes[2].offset   = offsetof(Vertex, depth);
    attributes[3].location = 3;
    attributes[3].format   = VK_FORMAT_R32_UINT;
    attributes[3].offset   = offsetof(Vertex, polygon);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions    = attributes;

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
    // The geometry engine has already dropped back-facing polygons, using the
    // hardware's own rule: the sign of the dot product of the surface normal with
    // the view vector, with a per-polygon override. Culling again here by winding
    // order would drop a second, different set.
    rasterisation.cullMode  = VK_CULL_MODE_NONE;
    rasterisation.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterisation.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // The fill mask. Polygons arrive nearest first, so a fragment draws only where
    // nothing has drawn yet, and claims the pixel when it does. Incrementing is
    // what claims it: replacing would write the reference value, which has to stay
    // zero for the comparison. A discarded fragment writes nothing, which is what
    // leaves a stippled polygon's gaps for whatever is behind it.
    VkStencilOpState stencil{};
    stencil.failOp      = VK_STENCIL_OP_KEEP;
    stencil.passOp      = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    stencil.depthFailOp = VK_STENCIL_OP_KEEP;
    stencil.compareOp   = VK_COMPARE_OP_EQUAL;
    stencil.compareMask = 0xff;
    stencil.writeMask   = 0xff;
    stencil.reference   = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable   = VK_FALSE;
    depth_stencil.depthWriteEnable  = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_TRUE;
    depth_stencil.front             = stencil;
    depth_stencil.back              = stencil;

    // Premultiplied over, merging the 3D directly onto the below-tilemap in the
    // native frame. The shader writes alpha 1 for every non-discarded pixel, so
    // ONE / ONE_MINUS_SRC_ALPHA overwrites where a polygon draws and leaves the
    // 2D untouched where it discards -- the same result the deleted
    // sample-and-composite step produced, without the offscreen round-trip. (The
    // hardware has no blend of its own; this is only the merge with the 2D.)
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable         = VK_TRUE;
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
    rendering.pColorAttachmentFormats = &m_colour_format;
    rendering.stencilAttachmentFormat = m_stencil_format;
    // Only named when the chosen format actually carries depth. Nothing reads or
    // writes it; it is along for the ride because some drivers offer no
    // stencil-only format.
    rendering.depthAttachmentFormat =
        m_stencil_has_depth ? m_stencil_format : VK_FORMAT_UNDEFINED;

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
    info.layout              = m_polygon_layout;
    info.renderPass          = VK_NULL_HANDLE;

    SM2_VK_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                         &m_polygon_pipeline));
    return true;
}

bool Poly3DPass::create_decode_pipeline()
{
    const VkDevice device = m_context->device();

    VkPipelineLayoutCreateInfo layout{};
    layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts     = &m_decode_set_layout;
    SM2_VK_TRY(vkCreatePipelineLayout(device, &layout, nullptr, &m_decode_layout));

    VkShaderModuleCreateInfo module_info{};
    module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = static_cast<usize>(shaders::kTexelDecodeCompWordCount)
                         * sizeof(u32);
    module_info.pCode    = shaders::kTexelDecodeComp;
    VkShaderModule module = VK_NULL_HANDLE;
    SM2_VK_TRY(vkCreateShaderModule(device, &module_info, nullptr, &module));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "main";

    VkComputePipelineCreateInfo info{};
    info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage  = stage;
    info.layout = m_decode_layout;

    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &m_decode_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        SM2_ERROR("3d: vkCreateComputePipelines failed: %s", result_string(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------

Poly3DPass::Frame& Poly3DPass::frame()
{
    return m_frames[m_context->frame_index()];
}

void Poly3DPass::refresh_machine_data(const hw::Model2MachineBase& machine,
                                      const hw::Model2Video& video)
{
    Frame& target = frame();

    if (target.texture_generation != machine.texture_generation()) {
        // Only the low half of each two-megabyte window is addressable by the
        // texel arithmetic and only that half is ever written, so the rest is left
        // out rather than uploaded as zeroes.
        auto* words = static_cast<u32*>(target.sheets.mapped);
        for (int sheet = 0; sheet < 2; ++sheet) {
            const std::span<const u32> source = machine.texture_ram(sheet);
            const usize count = std::min<usize>(kSheetWords, source.size());
            std::memcpy(words + static_cast<usize>(sheet) * kSheetWords, source.data(),
                        count * sizeof(u32));
        }
        target.texture_generation = machine.texture_generation();
        decode_textures();
    }

    if (target.table_generation == machine.table_generation() && target.tone_uploaded) {
        return;
    }
    target.table_generation = machine.table_generation();

    const std::span<const u8> luma = machine.luma_ram();
    std::memcpy(target.luma.mapped, luma.data(), std::min(kLumaBytes, luma.size()));

    video.build_tone_curve(m_tone_curve);
    std::memcpy(target.tone_staging.mapped, m_tone_curve.data(),
                m_tone_curve.size() * sizeof(u32));

    const VkCommandBuffer cmd = m_context->cmd();

    // From UNDEFINED because the copy replaces the whole image. When nothing
    // changed no barrier is recorded at all and the image simply stays where the
    // last upload left it.
    record_image_barrier(cmd, target.tone, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.bufferRowLength             = hw::Model2Video::kToneShades;
    region.bufferImageHeight           = hw::Model2Video::kToneComponents;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{hw::Model2Video::kToneShades,
                                    hw::Model2Video::kToneComponents, 1};
    vkCmdCopyBufferToImage(cmd, target.tone_staging.handle, target.tone,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    record_image_barrier(cmd, target.tone, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    target.tone_uploaded = true;
}

void Poly3DPass::decode_textures()
{
    Frame& target = frame();
    const VkCommandBuffer cmd = m_context->cmd();

    // Brackets only the dispatch this call always makes when it runs at all --
    // refresh_machine_data() calls this conditionally, so a frame that skips it
    // (texture_generation unchanged, the common case after the first frame)
    // leaves both queries unwritten rather than reporting a free stage.
    m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, GpuStage::TextureDecode,
                               false);

    // From UNDEFINED because the decode pass overwrites every element it can
    // reach; nothing depends on the image's previous contents. The wait is on
    // the previous frame's own fragment reads of this same per-frame image,
    // which that frame's fence has already retired by the time this one reuses
    // the slot.
    record_image_barrier(cmd, target.decoded, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_decode_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_decode_layout, 0, 1,
                            &target.decode_set, 0, nullptr);

    // One invocation per container, two layers (sheets), rounded up to the
    // sixteen-by-sixteen local group the shader declares.
    const u32 groups_x = (kDecodedWidth + 15) / 16;
    const u32 groups_y = (kDecodedHeight + 15) / 16;
    vkCmdDispatch(cmd, groups_x, groups_y, 2);

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, GpuStage::TextureDecode,
                               true);

    // The polygon pass samples this image next, in the fragment shader.
    record_image_barrier(cmd, target.decoded, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void Poly3DPass::build(const hw::Model2MachineBase* machine, const hw::Model2Video& video)
{
    if (machine != nullptr) {
        refresh_machine_data(*machine, video);
    }

    // Triangulation, texture-header unpacking and scissor batching are
    // backend-neutral (render::triangulate()); see render/geometry.h's own
    // doc comment for why this moved out of Poly3DPass rather than staying
    // duplicated per backend.
    m_frame_geometry = render::triangulate(machine, video, &m_capacity_warned);

    m_vertex_count = static_cast<u32>(m_frame_geometry.vertices.size());
    if (m_vertex_count != 0) {
        Frame& target = frame();
        std::memcpy(target.vertices.mapped, m_frame_geometry.vertices.data(),
                    static_cast<usize>(m_vertex_count) * sizeof(Vertex));
        std::memcpy(target.polygons.mapped, m_frame_geometry.polygons.data(),
                    m_frame_geometry.polygons.size() * sizeof(PolyParams));
    }
}

void Poly3DPass::prepare_stencil()
{
    const VkCommandBuffer cmd    = m_context->cmd();
    Frame&                target = frame();

    // From UNDEFINED: the mask is cleared each frame and never read outside the
    // scope. Recorded here, before the tilemap pass opens that scope, because a
    // layout transition cannot happen inside a rendering scope.
    const VkImageAspectFlags stencil_aspect =
        m_stencil_has_depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_STENCIL_BIT;
    record_image_barrier(cmd, target.stencil, stencil_aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

VkRenderingAttachmentInfo Poly3DPass::stencil_attachment() const
{
    const Frame& target = m_frames[m_context->frame_index()];

    VkRenderingAttachmentInfo stencil{};
    stencil.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    stencil.imageView   = target.stencil_view;
    stencil.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    stencil.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Nothing outside the scope reads the fill mask, so it need not be written
    // back to memory -- on a tiler this keeps it entirely in tile memory.
    stencil.storeOp                         = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    stencil.clearValue.depthStencil.depth   = 1.0F;
    stencil.clearValue.depthStencil.stencil = 0;
    return stencil;
}

void Poly3DPass::draw_polygons()
{
    const VkCommandBuffer cmd    = m_context->cmd();
    Frame&                target = frame();

    // Runs inside TilemapPass's open native-frame scope, between record_below()
    // and record_above(): the polygon pipeline's premultiplied-over blend merges
    // the 3D onto the below-tilemap directly, with no offscreen colour image to
    // store and re-sample (the tiler flush+reload this change removes).
    m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, GpuStage::Poly3D, false);

    if (m_vertex_count != 0) {
        PolygonPush push{};
        push.inv_raster[0] = 1.0F / static_cast<float>(kWidth);
        push.inv_raster[1] = 1.0F / static_cast<float>(kHeight);

        const VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_polygon_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &target.vertices.handle, &offset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_polygon_layout, 0,
                                1, &target.polygon_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_polygon_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);

        for (const render::Batch& batch : m_frame_geometry.batches) {
            if (batch.vertex_count == 0) {
                continue;
            }
            const VkRect2D vk_scissor = scissor_to_vk(batch.scissor);
            vkCmdSetScissor(cmd, 0, 1, &vk_scissor);
            vkCmdDraw(cmd, batch.vertex_count, 1, batch.first_vertex, 0);
        }
    }

    m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, GpuStage::Poly3D, true);
}

}  // namespace sm2::render::vk
