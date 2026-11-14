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
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp, Copyright (c)
// 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.

#include "render/vk/model3_pass.h"

#include "core/log.h"
#include "render/texture_replace.h"

#include "shaders/fullscreen_quad_vert.h"
#include "shaders/model3_composite_base_frag.h"
#include "shaders/model3_composite_trans_frag.h"
#include "shaders/model3_layer_frag.h"
#include "shaders/model3_r3d_frag.h"
#include "shaders/model3_r3d_frag_encode.h"
#include "shaders/model3_r3d_vert.h"
#include "shaders/model3_scroll_fog_frag.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace sm2::render::vk {

using namespace render::model3;

namespace {

constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kLosFormat    = VK_FORMAT_R32G32_UINT;
constexpr VkFormat kBankFormat   = VK_FORMAT_R16_UINT;
constexpr VkDeviceSize kLosSlotBytes = 16;

constexpr VkColorComponentFlags kWriteAll = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

VkDeviceSize round_up(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) / a * a;
}

bool create_module(VkDevice device, const u32* code, u32 words, VkShaderModule* out)
{
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<usize>(words) * sizeof(u32);
    info.pCode    = code;
    SM2_VK_TRY(vkCreateShaderModule(device, &info, nullptr, out));
    return true;
}

VkRect2D to_rect(const PixelRect& r)
{
    VkRect2D rect{};
    rect.offset = {std::max(r.x, 0), std::max(r.y, 0)};
    rect.extent = {static_cast<u32>(std::max(r.width - (rect.offset.x - r.x), 0)),
                   static_cast<u32>(std::max(r.height - (rect.offset.y - r.y), 0))};
    return rect;
}

/// GL clip space (Y up) into a top-left target: a negative-height viewport.
VkViewport flipped_viewport(const PixelRect& r)
{
    return VkViewport{static_cast<float>(r.x), static_cast<float>(r.y + r.height), static_cast<float>(r.width),
                      -static_cast<float>(r.height), 0.0f, 1.0f};
}

VkViewport plain_viewport(const PixelRect& r)
{
    return VkViewport{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width),
                      static_cast<float>(r.height), 0.0f, 1.0f};
}

VkRenderingAttachmentInfo colour_attachment(VkImageView view, VkAttachmentLoadOp load)
{
    VkRenderingAttachmentInfo a{};
    a.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView   = view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp      = load;
    a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    return a;
}

}  // namespace

Model3Pass::~Model3Pass()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

bool Model3Pass::create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo result{};
    SM2_VK_TRY(vmaCreateBuffer(m_context->allocator(), &info, &alloc, &b.buffer, &b.allocation, &result));
    b.mapped = result.pMappedData;
    b.size   = size;
    b.serial = ++m_buffer_serial;
    return b.mapped != nullptr;
}

void Model3Pass::destroy_buffer(Buffer& b)
{
    if (b.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->allocator(), b.buffer, b.allocation);
    }
    b = Buffer{};
}

bool Model3Pass::ensure_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (b.size >= size) {
        return true;
    }
    // Only ever called on this frame slot's buffers, which its fence has retired.
    destroy_buffer(b);
    return create_buffer(b, std::max<VkDeviceSize>(size + size / 2, 64 * 1024), usage);
}

bool Model3Pass::create_image(Image& img, VkFormat format, u32 w, u32 h, u32 levels, VkImageUsageFlags usage,
                              VkImageAspectFlags aspect, u32 layers)
{
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = format;
    info.extent        = {w, h, 1};
    info.mipLevels     = levels;
    info.arrayLayers   = layers;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = usage;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    SM2_VK_TRY(vmaCreateImage(m_context->allocator(), &info, &alloc, &img.image, &img.allocation, nullptr));

    VkImageViewCreateInfo view{};
    view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image                       = img.image;
    view.viewType                    = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format                      = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = levels;
    view.subresourceRange.layerCount = layers;
    SM2_VK_TRY(vkCreateImageView(m_context->device(), &view, nullptr, &img.view));
    return true;
}

void Model3Pass::destroy_image(Image& img)
{
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->device(), img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context->allocator(), img.image, img.allocation);
    }
    img = Image{};
}

bool Model3Pass::init(Context& context, u32 render_scale, u32 width)
{
    shutdown(true);
    if (context.device() == VK_NULL_HANDLE) {
        return false;
    }
    m_context      = &context;
    m_render_scale = render_scale;
    m_width        = width * render_scale;
    m_height       = 384 * render_scale;
    m_layer_width.fill(width > TileGen::kVisibleWidth ? width : TileGen::kWidth);
    // Tile-aligned targets: a render area that is not a whole number of
    // tiles costs tilers a slow clear path and partial tile stores.
    m_target_width  = round_up(m_width, 64);
    m_target_height = round_up(m_height, 64);
    m_ubo_alignment = static_cast<u32>(
        std::max<VkDeviceSize>(context.device_properties().limits.minUniformBufferOffsetAlignment, 16));

    // A float depth buffer (D32_SFLOAT_S8_UINT); devices without one get D24S8
    // and the shader's depth remap.
    m_depth_format = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context.physical_device(), f, &props);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((props.optimalTilingFeatures & need) == need) {
            m_depth_format = f;
            break;
        }
    }
    if (m_depth_format == VK_FORMAT_UNDEFINED) {
        SM2_ERROR("vk: no depth/stencil format for the Model 3 renderer");
        return false;
    }
    m_float_depth_format = VK_FORMAT_UNDEFINED;
    if (m_depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context.physical_device(), VK_FORMAT_D32_SFLOAT, &props);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((props.optimalTilingFeatures & need) == need) {
            m_float_depth_format = VK_FORMAT_D32_SFLOAT;
            SM2_INFO("vk: model3 depth %s, D32_SFLOAT for passes without stencil draws",
                     m_depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
        }
    }

    m_ready = create_resources() && create_descriptors() && create_pipelines();
    if (!m_ready) {
        SM2_ERROR("vk: could not create the Model 3 pass");
        shutdown();
        return false;
    }
    m_rom_generation    = 0;
    m_rom_uploaded      = 0;
    m_banks_initialised = false;
    m_atlas.bound       = false;
    return true;
}

void Model3Pass::shutdown(bool keep_atlas)
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    vkDeviceWaitIdle(device);
    if (!keep_atlas) {
        destroy_image(m_atlas.image);
        destroy_buffer(m_atlas.staging);
        m_atlas = Atlas{};
    }

    for (VkPipeline& p : m_r3d_pipelines) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    for (VkShaderModule* m : {&m_r3d_vs, &m_r3d_fs, &m_r3d_fs_float}) {
        if (*m != VK_NULL_HANDLE) vkDestroyShaderModule(device, *m, nullptr);
        *m = VK_NULL_HANDLE;
    }
    for (VkPipeline* p : {&m_fog_pipeline, &m_layer_fog_pipelines[0], &m_layer_fog_pipelines[1], &m_base_pipeline,
                          &m_trans_pipeline, &m_layer_pipeline}) {
        if (*p != VK_NULL_HANDLE) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&m_r3d_layout, &m_fog_layout, &m_one_tex_pl_layout, &m_two_tex_pl_layout, &m_layer_layout}) {
        if (*l != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : {&m_r3d_set_layout, &m_ubo_set_layout, &m_one_tex_layout, &m_two_tex_layout}) {
        if (*l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    for (VkSampler* s : {&m_nearest, &m_linear, &m_atlas_sampler}) {
        if (*s != VK_NULL_HANDLE) vkDestroySampler(device, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (auto& f : m_frames) {
        destroy_buffer(f.staging);
        f = PerFrame{};
    }
    for (BuildSlot& s : m_slots) {
        destroy_buffer(s.ram_vertices);
        destroy_buffer(s.uniforms);
        destroy_buffer(s.los);
        s = BuildSlot{};
    }
    if (m_slot_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, m_slot_pool, nullptr);
        m_slot_pool = VK_NULL_HANDLE;
    }
    m_records = 0;
    destroy_buffer(m_rom_vertices);
    destroy_image(m_bank);
    for (Image& i : m_layers) destroy_image(i);
    for (Image& i : m_colour) destroy_image(i);
    destroy_image(m_los_target);
    destroy_image(m_depth);
    destroy_image(m_depth_copy);
    destroy_image(m_depth_f);
    destroy_image(m_depth_f_copy);
    m_ready   = false;
    m_context = nullptr;
}

bool Model3Pass::create_resources()
{
    const VkImageUsageFlags sampled_dst = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Both texture pages as the layers of one array image.
    if (!create_image(m_bank, kBankFormat, TextureSheets::kPageWidth, TextureSheets::kPageHeight, TextureSheets::kLevels,
                      sampled_dst, VK_IMAGE_ASPECT_COLOR_BIT, 2)) {
        return false;
    }
    for (usize l = 0; l < m_layers.size(); l++) {
        if (!create_image(m_layers[l], kColourFormat, m_layer_width[l], TileGen::kHeight, 1, sampled_dst,
                          VK_IMAGE_ASPECT_COLOR_BIT)) {
            return false;
        }
    }
    for (Image& c : m_colour) {
        if (!create_image(c, kColourFormat, m_target_width, m_target_height, 1,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
            return false;
        }
    }
    if (!create_image(m_los_target, kLosFormat, m_target_width, m_target_height, 1,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }
    const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!create_image(m_depth, m_depth_format, m_target_width, m_target_height, 1, depth_usage, ds)
        || !create_image(m_depth_copy, m_depth_format, m_target_width, m_target_height, 1, depth_usage, ds)
        || (m_float_depth_format != VK_FORMAT_UNDEFINED
            && (!create_image(m_depth_f, m_float_depth_format, m_target_width, m_target_height, 1, depth_usage,
                              VK_IMAGE_ASPECT_DEPTH_BIT)
                || !create_image(m_depth_f_copy, m_float_depth_format, m_target_width, m_target_height, 1, depth_usage,
                                 VK_IMAGE_ASPECT_DEPTH_BIT)))) {
        return false;
    }

    // Vertex buffer: the ROM region shared and append-only, the RAM region one
    // per frame in flight.
    if (!create_buffer(m_rom_vertices, static_cast<VkDeviceSize>(kMaxRomVerts) * FVertexLayout::kStride,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        return false;
    }
    for (BuildSlot& s : m_slots) {
        if (!create_buffer(s.los, kLosSlotBytes * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            return false;
        }
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = m_context->graphics_family();
    SM2_VK_TRY(vkCreateCommandPool(m_context->device(), &pool, nullptr, &m_slot_pool));
    for (BuildSlot& s : m_slots) {
        VkCommandBufferAllocateInfo info{};
        info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool        = m_slot_pool;
        info.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        info.commandBufferCount = 1;
        SM2_VK_TRY(vkAllocateCommandBuffers(m_context->device(), &info, &s.cmd));
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_NEAREST;
    sampler.minFilter    = VK_FILTER_NEAREST;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod       = VK_LOD_CLAMP_NONE;
    SM2_VK_TRY(vkCreateSampler(m_context->device(), &sampler, nullptr, &m_nearest));
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    SM2_VK_TRY(vkCreateSampler(m_context->device(), &sampler, nullptr, &m_linear));
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    SM2_VK_TRY(vkCreateSampler(m_context->device(), &sampler, nullptr, &m_atlas_sampler));
    return true;
}

bool Model3Pass::create_descriptors()
{
    const VkDevice device = m_context->device();

    auto make_layout = [&](std::span<const VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<u32>(bindings.size());
        info.pBindings    = bindings.data();
        return vkCreateDescriptorSetLayout(device, &info, nullptr, out) == VK_SUCCESS;
    };

    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding r3d[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding ubo[1] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding one[1] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding two[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    if (!make_layout(r3d, &m_r3d_set_layout) || !make_layout(ubo, &m_ubo_set_layout)
        || !make_layout(one, &m_one_tex_layout) || !make_layout(two, &m_two_tex_layout)) {
        return false;
    }

    const u32 frames = kSlots;
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, frames * 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frames * 3 + 1 + 2 + 4}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = frames * 2 + 2 + 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes    = sizes;
    SM2_VK_TRY(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool));

    auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo info{};
        info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool     = m_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts        = &layout;
        return vkAllocateDescriptorSets(device, &info, out) == VK_SUCCESS;
    };
    for (BuildSlot& f : m_slots) {
        if (!allocate(m_r3d_set_layout, &f.r3d_set) || !allocate(m_ubo_set_layout, &f.fog_set)) {
            return false;
        }
    }
    if (!allocate(m_one_tex_layout, &m_base_set) || !allocate(m_two_tex_layout, &m_trans_set)) {
        return false;
    }
    for (auto& per_layer : m_layer_sets) {
        for (VkDescriptorSet& s : per_layer) {
            if (!allocate(m_one_tex_layout, &s)) {
                return false;
            }
        }
    }

    // Fixed image descriptors.
    auto image_info = [](VkSampler s, VkImageView v) {
        return VkDescriptorImageInfo{s, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(16);
    auto write = [&](VkDescriptorSet set, u32 binding, VkDescriptorImageInfo info) {
        infos.push_back(info);
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = set;
        w.dstBinding      = binding;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &infos.back();
        writes.push_back(w);
    };
    write(m_base_set, 0, image_info(m_nearest, m_colour[0].view));
    write(m_trans_set, 0, image_info(m_nearest, m_colour[1].view));
    write(m_trans_set, 1, image_info(m_nearest, m_colour[2].view));
    for (int l = 0; l < 2; l++) {
        write(m_layer_sets[static_cast<usize>(l)][0], 0, image_info(m_nearest, m_layers[static_cast<usize>(l)].view));
        write(m_layer_sets[static_cast<usize>(l)][1], 0, image_info(m_linear, m_layers[static_cast<usize>(l)].view));
    }
    for (BuildSlot& f : m_slots) {
        const VkDescriptorImageInfo bank = image_info(m_nearest, m_bank.view);
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = f.r3d_set;
        w.dstBinding      = 1;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &bank;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    // Pipeline layouts.
    auto pipeline_layout = [&](VkDescriptorSetLayout set, const VkPushConstantRange* push, VkPipelineLayout* out) {
        VkPipelineLayoutCreateInfo info{};
        info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount         = 1;
        info.pSetLayouts            = &set;
        info.pushConstantRangeCount = push != nullptr ? 1 : 0;
        info.pPushConstantRanges    = push;
        return vkCreatePipelineLayout(device, &info, nullptr, out) == VK_SUCCESS;
    };
    const VkPushConstantRange model_push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(R3DModelUniforms)};
    const VkPushConstantRange layer_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    return pipeline_layout(m_r3d_set_layout, &model_push, &m_r3d_layout)
           && pipeline_layout(m_ubo_set_layout, nullptr, &m_fog_layout)
           && pipeline_layout(m_one_tex_layout, nullptr, &m_one_tex_pl_layout)
           && pipeline_layout(m_two_tex_layout, nullptr, &m_two_tex_pl_layout)
           && pipeline_layout(m_one_tex_layout, &layer_push, &m_layer_layout);
}

// depth_mode: 0 none, 1 GEQUAL (colour layer), 2 GREATER (translucency).
VkPipeline Model3Pass::make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                                     std::span<const VkFormat> colour_formats, VkFormat depth_format,
                                     bool vertex_input, bool blend, int depth_mode, bool layered,
                                     const VkSpecializationInfo* spec)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";
    stages[1].pSpecializationInfo = spec;

    const VkVertexInputBindingDescription binding{0, FVertexLayout::kStride, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[7] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, FVertexLayout::kPos},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, FVertexLayout::kNormal},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, FVertexLayout::kTexCoord},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, FVertexLayout::kFaceNormal},
        {4, 0, VK_FORMAT_R32_SFLOAT, FVertexLayout::kFixedShade},
        {5, 0, VK_FORMAT_R8G8B8A8_UNORM, FVertexLayout::kFaceColour},
        {6, 0, VK_FORMAT_R32_SFLOAT, FVertexLayout::kTextureNP}};
    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (vertex_input) {
        vertex.vertexBindingDescriptionCount   = 1;
        vertex.pVertexBindingDescriptions      = &binding;
        vertex.vertexAttributeDescriptionCount = 7;
        vertex.pVertexAttributeDescriptions    = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;  // the Real3D backface rule lives in the shader
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // The two stencil configurations, layered and not; the non-layered
    // reference (noLosReturn << 7) is dynamic.
    VkStencilOpState stencil{};
    if (layered) {
        stencil.compareOp   = VK_COMPARE_OP_EQUAL;
        stencil.compareMask = 0x7F;
        stencil.writeMask   = 0x7F;
        stencil.failOp      = VK_STENCIL_OP_KEEP;
        stencil.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        stencil.passOp      = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    } else {
        stencil.compareOp   = VK_COMPARE_OP_ALWAYS;
        stencil.compareMask = 0x80;
        stencil.writeMask   = 0x80;
        stencil.failOp      = VK_STENCIL_OP_KEEP;
        stencil.depthFailOp = VK_STENCIL_OP_KEEP;
        stencil.passOp      = VK_STENCIL_OP_REPLACE;
    }
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (depth_mode != 0) {
        depth.depthTestEnable   = VK_TRUE;
        depth.depthWriteEnable  = VK_TRUE;
        depth.depthCompareOp    = depth_mode == 1 ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER;
        depth.stencilTestEnable = depth_format == VK_FORMAT_D32_SFLOAT ? VK_FALSE : VK_TRUE;
        depth.front             = stencil;
        depth.back              = stencil;
    }

    std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
    for (auto& a : attachments) {
        a.blendEnable         = blend ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        a.colorBlendOp        = VK_BLEND_OP_ADD;
        a.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        a.alphaBlendOp        = VK_BLEND_OP_ADD;
        a.colorWriteMask      = kWriteAll;
    }
    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = static_cast<u32>(colour_formats.size());
    blend_state.pAttachments    = attachments.data();

    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = depth_mode != 0 ? 3 : 2;
    dynamic_state.pDynamicStates    = dynamic;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = static_cast<u32>(colour_formats.size());
    rendering.pColorAttachmentFormats = colour_formats.data();
    rendering.depthAttachmentFormat   = depth_format;
    rendering.stencilAttachmentFormat = depth_format == VK_FORMAT_D32_SFLOAT ? VK_FORMAT_UNDEFINED : depth_format;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend_state;
    info.pDynamicState       = &dynamic_state;
    info.layout              = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        SM2_ERROR("vk: Model 3 pipeline creation failed: %s", result_string(result));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Model3Pass::create_pipelines()
{
    const VkDevice device = m_context->device();
    struct Modules {
        VkDevice       device;
        VkShaderModule m[7] = {};
        ~Modules()
        {
            for (VkShaderModule s : m) {
                if (s != VK_NULL_HANDLE) vkDestroyShaderModule(device, s, nullptr);
            }
        }
    } mods{device};
    if (!create_module(device, shaders::kModel3R3dVert, shaders::kModel3R3dVertWordCount, &m_r3d_vs)
        || !(encode_depth()
                 ? create_module(device, shaders::kModel3R3dFragEncode, shaders::kModel3R3dFragEncodeWordCount, &m_r3d_fs)
                 : create_module(device, shaders::kModel3R3dFrag, shaders::kModel3R3dFragWordCount, &m_r3d_fs))
        || (m_float_depth_format != VK_FORMAT_UNDEFINED
            && !create_module(device, shaders::kModel3R3dFrag, shaders::kModel3R3dFragWordCount,
                                    &m_r3d_fs_float))
        || !create_module(device, shaders::kFullscreenQuadVert, shaders::kFullscreenQuadVertWordCount, &mods.m[2])
        || !create_module(device, shaders::kModel3ScrollFogFrag, shaders::kModel3ScrollFogFragWordCount, &mods.m[3])
        || !create_module(device, shaders::kModel3CompositeBaseFrag, shaders::kModel3CompositeBaseFragWordCount, &mods.m[4])
        || !create_module(device, shaders::kModel3CompositeTransFrag, shaders::kModel3CompositeTransFragWordCount, &mods.m[5])
        || !create_module(device, shaders::kModel3LayerFrag, shaders::kModel3LayerFragWordCount, &mods.m[6])) {
        return false;
    }

    const VkFormat fog_pass[3]    = {kColourFormat, kColourFormat, kColourFormat};
    const VkFormat native[1]      = {kNativeColourFormat};

    m_fog_pipeline   = make_pipeline(mods.m[2], mods.m[3], m_fog_layout, fog_pass, VK_FORMAT_UNDEFINED, false, false, 0, false);
    // The same quads drawn inside a layer pass, which also has the line of sight.
    const VkFormat layer_pass[4] = {kColourFormat, kColourFormat, kColourFormat, kLosFormat};
    m_layer_fog_pipelines[0] =
        make_pipeline(mods.m[2], mods.m[3], m_fog_layout, layer_pass, m_depth_format, false, false, 0, false);
    if (m_float_depth_format != VK_FORMAT_UNDEFINED) {
        m_layer_fog_pipelines[1] = make_pipeline(mods.m[2], mods.m[3], m_fog_layout, layer_pass, m_float_depth_format,
                                                 false, false, 0, false);
    }
    m_base_pipeline  = make_pipeline(mods.m[2], mods.m[4], m_one_tex_pl_layout, native, VK_FORMAT_UNDEFINED, false, true, 0, false);
    m_trans_pipeline = make_pipeline(mods.m[2], mods.m[5], m_two_tex_pl_layout, native, VK_FORMAT_UNDEFINED, false, true, 0, false);
    m_layer_pipeline = make_pipeline(mods.m[2], mods.m[6], m_layer_layout, native, VK_FORMAT_UNDEFINED, false, true, 0, false);

    // The textured, lit colour pipeline nearly every game starts with; the
    // other R3D variants are created on first use.
    if (r3d_pipeline(0, false, 0x11, false) == VK_NULL_HANDLE) {
        return false;
    }
    return m_fog_pipeline != VK_NULL_HANDLE && m_layer_fog_pipelines[0] != VK_NULL_HANDLE
           && (m_float_depth_format == VK_FORMAT_UNDEFINED || m_layer_fog_pipelines[1] != VK_NULL_HANDLE)
           && m_base_pipeline != VK_NULL_HANDLE && m_trans_pipeline != VK_NULL_HANDLE
           && m_layer_pipeline != VK_NULL_HANDLE;
}

VkPipeline Model3Pass::r3d_pipeline(int layer, bool layered, u32 variant, bool float_depth)
{
    const bool  aniso = m_aniso_taps > 1;
    const usize slot =
        (((static_cast<usize>(aniso ? 1 : 0) * 2 + static_cast<usize>(float_depth ? 1 : 0)) * 3 + static_cast<usize>(layer)) * 2
         + (layered ? 1 : 0)) * kR3DVariantCount
        + variant;
    VkPipeline& pipeline = m_r3d_pipelines[slot];
    if (pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }
    VkBool32                 values[8];
    VkSpecializationMapEntry entries[8];
    for (u32 i = 0; i < 8; i++) {
        values[i]  = i < 7 ? (variant >> i) & 1u : aniso;
        entries[i] = {i, i * static_cast<u32>(sizeof(VkBool32)), sizeof(VkBool32)};
    }
    VkSpecializationInfo spec{};
    spec.mapEntryCount = 8;
    spec.pMapEntries   = entries;
    spec.dataSize      = sizeof(values);
    spec.pData         = values;

    // Draw buffers: the colour layer writes all four targets, each
    // translucency layer only its own.
    const VkFormat colour_pass[4] = {kColourFormat, kColourFormat, kColourFormat, kLosFormat};
    const VkFormat trans1_pass[4] = {VK_FORMAT_UNDEFINED, kColourFormat, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    const VkFormat trans2_pass[4] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, kColourFormat, VK_FORMAT_UNDEFINED};
    const std::span<const VkFormat> formats = layer == 0 ? std::span<const VkFormat>(colour_pass)
                                              : layer == 1 ? std::span<const VkFormat>(trans1_pass)
                                                           : std::span<const VkFormat>(trans2_pass);
    pipeline = make_pipeline(m_r3d_vs, float_depth ? m_r3d_fs_float : m_r3d_fs, m_r3d_layout, formats,
                             float_depth ? m_float_depth_format : m_depth_format, true, false,
                             layer == 0 ? 1 : 2, layered, &spec);
    return pipeline;
}

void Model3Pass::update_frame_sets(BuildSlot& f)
{
    if (f.bound_uniforms == f.uniforms.serial) {
        return;
    }
    VkDescriptorBufferInfo r3d{f.uniforms.buffer, 0, sizeof(R3DUniforms)};
    VkDescriptorBufferInfo fog{f.uniforms.buffer, 0, sizeof(ScrollFogUniforms)};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = f.r3d_set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo     = &r3d;
    writes[1]                 = writes[0];
    writes[1].dstSet          = f.fog_set;
    writes[1].pBufferInfo     = &fog;
    vkUpdateDescriptorSets(m_context->device(), 2, writes, 0, nullptr);
    f.bound_uniforms = f.uniforms.serial;
}

void Model3Pass::sync_atlas(VkCommandBuffer cmd)
{
    if (m_atlas.staging_age != 0 && --m_atlas.staging_age == 0) {
        destroy_buffer(m_atlas.staging);
    }
    const bool wanted = m_replacements != nullptr && !m_replacements->empty() && m_replacements->layer_count() != 0;
    const u64  generation = wanted ? m_replacements->generation() : 0;
    if (m_atlas.image.image == VK_NULL_HANDLE || generation != m_atlas.generation) {
        // Rare (a game or pack load), so the sets are rewritten with the device idle.
        if (m_atlas.image.image != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_context->device());
            destroy_image(m_atlas.image);
            destroy_buffer(m_atlas.staging);
            m_atlas.staging_age = 0;
        }
        bool have_pixels = generation != 0;
        for (u32 layer = 0; have_pixels && layer < m_replacements->layer_count(); ++layer) {
            have_pixels = !m_replacements->layer_pixels(layer).empty();
        }
        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const u32          size        = kReplacementLayerSize;
        const VkDeviceSize layer_bytes = static_cast<VkDeviceSize>(size) * size * 4;
        if (have_pixels) {
            const u32 layers = m_replacements->layer_count();
            const u32 mips   = TextureReplacements::mip_levels();
            if (!create_image(m_atlas.image, kColourFormat, size, size, mips, usage, VK_IMAGE_ASPECT_COLOR_BIT, 2 > layers ? 2 : layers)
                || !create_buffer(m_atlas.staging, layer_bytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
                SM2_ERROR("vk: could not allocate the Model 3 custom texture atlas; drawing without it");
                destroy_image(m_atlas.image);
                destroy_buffer(m_atlas.staging);
                have_pixels = false;
            } else {
                for (u32 layer = 0; layer < layers; ++layer) {
                    std::memcpy(static_cast<u8*>(m_atlas.staging.mapped) + layer_bytes * layer,
                                m_replacements->layer_pixels(layer).data(), layer_bytes);
                }
                m_replacements->release_pixels();
                m_atlas.layers      = layers;
                m_atlas.mips        = mips;
                m_atlas.staging_age = Context::kFramesInFlight + 1;
            }
        }
        if (!have_pixels) {
            // Two layers, so the view is an array as the shader declares.
            if (!create_image(m_atlas.image, kColourFormat, 1, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT, 2)) {
                return;
            }
            m_atlas.layers = 2;
            m_atlas.mips   = 1;
        }
        m_atlas.generation = generation;
        m_atlas.live       = have_pixels;
        m_atlas.bound      = false;

        const VkImage image = m_atlas.image.image;
        auto barrier = [&](u32 level, u32 levels, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
            VkImageMemoryBarrier2 b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask        = src_stage;
            b.srcAccessMask       = src_access;
            b.dstStageMask        = dst_stage;
            b.dstAccessMask       = dst_access;
            b.oldLayout           = from;
            b.newLayout           = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, VK_REMAINING_ARRAY_LAYERS};
            VkDependencyInfo dep{};
            dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers    = &b;
            device_functions().cmd_pipeline_barrier2(cmd, &dep);
        };
        if (!have_pixels) {
            barrier(0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        } else {
            barrier(0, m_atlas.mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m_atlas.layers};
            region.imageExtent      = {size, size, 1};
            vkCmdCopyBufferToImage(cmd, m_atlas.staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            for (u32 level = 1; level < m_atlas.mips; ++level) {
                barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
                const s32 from = static_cast<s32>(std::max(size >> (level - 1), 1u));
                const s32 to   = static_cast<s32>(std::max(size >> level, 1u));
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, m_atlas.layers};
                blit.srcOffsets[1]  = {from, from, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, m_atlas.layers};
                blit.dstOffsets[1]  = {to, to, 1};
                vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
            }
            if (m_atlas.mips > 1) {
                barrier(0, m_atlas.mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            barrier(m_atlas.mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    if (!m_atlas.bound) {
        const VkDescriptorImageInfo info{m_atlas_sampler, m_atlas.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ++m_sets_epoch;
        for (BuildSlot& f : m_slots) {
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = f.r3d_set;
            w.dstBinding      = 2;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo      = &info;
            vkUpdateDescriptorSets(m_context->device(), 1, &w, 0, nullptr);
        }
        m_atlas.bound = true;
    }
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void Model3Pass::read_back_los(BuildSlot& f)
{
    // This slot's fence has retired, so its earlier copies are complete.
    bool any = false;
    LineOfSight los;
    los.frame_number = f.frame_number;
    for (u32 p = 0; p < 4; p++) {
        if (!f.los_pending[p]) {
            continue;
        }
        u32 texel[2] = {0, 0};
        std::memcpy(texel, static_cast<const u8*>(f.los.mapped) + p * kLosSlotBytes, sizeof(texel));
        los.range[p]      = decode_line_of_sight(texel[0], texel[1] != 0);
        f.los_pending[p]  = false;
        any               = true;
    }
    if (any || f.frame_number != 0) {
        m_los.publish(los);
    }
    f.frame_number = 0;
}

void Model3Pass::upload(FrameBuilder& frame, PerFrame& f, VkCommandBuffer cmd)
{
    TextureSheets& sheets = frame.textures();
    const usize layer_bytes[2] = {static_cast<usize>(m_layer_width[0]) * TileGen::kHeight * 4,
                                  static_cast<usize>(m_layer_width[1]) * TileGen::kHeight * 4};

    // Staging: both tile layers, then every texture rectangle packed tightly.
    usize texture_bytes = 0;
    for (const TextureUpload& u : sheets.uploads()) {
        texture_bytes += round_up(static_cast<VkDeviceSize>(u.width) * u.height * 2, 4);
    }
    if (!ensure_buffer(f.staging, layer_bytes[0] + layer_bytes[1] + texture_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        return;
    }
    if (frame.layer(0).size_bytes() != layer_bytes[0] || frame.layer(1).size_bytes() != layer_bytes[1]) {
        return;
    }
    u8* staging = static_cast<u8*>(f.staging.mapped);
    m_copies.push_back({staging, frame.layer(0).data(), layer_bytes[0]});
    m_copies.push_back({staging + layer_bytes[0], frame.layer(1).data(), layer_bytes[1]});

    // Layers: last frame's composite may still sample them; the barrier orders that.
    for (int l = 0; l < 2; l++) {
        record_image_barrier(cmd, m_layers[static_cast<usize>(l)].image, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset     = l == 0 ? 0 : layer_bytes[0];
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {m_layer_width[static_cast<usize>(l)], TileGen::kHeight, 1};
        vkCmdCopyBufferToImage(cmd, f.staging.buffer, m_layers[static_cast<usize>(l)].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        record_image_barrier(cmd, m_layers[static_cast<usize>(l)].image, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // Texture pages.
    if (!sheets.uploads().empty() || !m_banks_initialised) {
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = layer_bytes[0] + layer_bytes[1];
        const u16*   texels = sheets.texels().data();
        for (const TextureUpload& u : sheets.uploads()) {
            for (u32 row = 0; row < u.height; row++) {
                m_copies.push_back({staging + offset + static_cast<usize>(row) * u.width * 2,
                                    texels + static_cast<usize>(u.src_y + row) * TextureSheets::kSize + u.src_x,
                                    static_cast<usize>(u.width) * 2});
            }
            VkBufferImageCopy region{};
            region.bufferOffset     = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, u.level, u.page, 1};
            region.imageOffset      = {static_cast<s32>(u.dst_x), static_cast<s32>(u.dst_y), 0};
            region.imageExtent      = {u.width, u.height, 1};
            regions.push_back(region);
            offset += round_up(static_cast<VkDeviceSize>(u.width) * u.height * 2, 4);
        }
        const VkImageLayout old_layout =
            m_banks_initialised ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        record_image_barrier(cmd, m_bank.image, VK_IMAGE_ASPECT_COLOR_BIT, old_layout,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!regions.empty()) {
            vkCmdCopyBufferToImage(cmd, f.staging.buffer, m_bank.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<u32>(regions.size()), regions.data());
        }
        record_image_barrier(cmd, m_bank.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_banks_initialised = true;
        sheets.clear_uploads();
    }

}

bool Model3Pass::prepare_3d(FrameBuilder& frame, BuildSlot& f, bool on_worker)
{
    const auto start = std::chrono::steady_clock::now();
    f.build = 0;
    f.los_pending.fill(false);
    const FramePlan& plan = frame.plan();
    f.has_3d              = false;
    if (!plan.white_flash) {
        // Vertices, written in place (host-visible). The ROM region only
        // appends, past anything an earlier frame in flight is reading.
        const Scene& scene = frame.scene();
        const auto   rom   = scene.rom_vertices();
        if (scene.rom_generation() != m_rom_generation || rom.size() < m_rom_uploaded) {
            // A reset may rewrite vertices an in-flight frame reads; wait it
            // out, which only the render thread may do.
            if (m_rom_uploaded != 0) {
                if (on_worker) {
                    return false;
                }
                m_context->wait_idle();
            }
            m_rom_generation = scene.rom_generation();
            m_rom_uploaded   = 0;
        }
        const usize rom_count = std::min<usize>(rom.size(), kMaxRomVerts);
        if (rom_count > m_rom_uploaded) {
            std::memcpy(static_cast<u8*>(m_rom_vertices.mapped) + m_rom_uploaded * FVertexLayout::kStride,
                        rom.data() + m_rom_uploaded, (rom_count - m_rom_uploaded) * FVertexLayout::kStride);
            m_rom_uploaded = rom_count;
        }
        const auto  ram       = scene.ram_vertices();
        const usize ram_count = std::min<usize>(ram.size(), kMaxRamVerts);
        if (ensure_buffer(f.ram_vertices, std::max<usize>(ram_count, 1) * FVertexLayout::kStride,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
            && ram_count != 0) {
            std::memcpy(f.ram_vertices.mapped, ram.data(), ram_count * FVertexLayout::kStride);
        }

        // Uniforms: R3D blocks, then fog blocks, each at the offset alignment.
        const VkDeviceSize r3d_stride = round_up(sizeof(R3DUniforms), m_ubo_alignment);
        const VkDeviceSize fog_stride = round_up(sizeof(ScrollFogUniforms), m_ubo_alignment);
        const VkDeviceSize bytes = r3d_stride * std::max<usize>(plan.uniforms.size(), 1)
                                 + fog_stride * std::max<usize>(plan.fogs.size(), 1);
        if (ensure_buffer(f.uniforms, bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
            u8* u = static_cast<u8*>(f.uniforms.mapped);
            for (usize i = 0; i < plan.uniforms.size(); i++) {
                std::memcpy(u + i * r3d_stride, &plan.uniforms[i], sizeof(R3DUniforms));
                if (m_aniso_taps > 1) reinterpret_cast<R3DUniforms*>(u + i * r3d_stride)->anisoTaps = m_aniso_taps;
            }
            const VkDeviceSize fog_base = r3d_stride * std::max<usize>(plan.uniforms.size(), 1);
            for (usize i = 0; i < plan.fogs.size(); i++) {
                std::memcpy(u + fog_base + i * fog_stride, &plan.fogs[i].uniforms, sizeof(ScrollFogUniforms));
            }
            update_frame_sets(f);
            f.has_3d = true;
        }
    }

    VkCommandBufferInheritanceInfo inherit{};
    inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    VkCommandBufferBeginInfo begin{};
    begin.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin.pInheritanceInfo = &inherit;
    if (vkBeginCommandBuffer(f.cmd, &begin) != VK_SUCCESS) {
        return false;
    }
    if (f.has_3d) {
        draw_3d(plan, f, f.cmd);
    } else {
        // Keep colour targets sampleable even when no 3D was drawn.
        for (Image& c : m_colour) {
            record_image_barrier(f.cmd, c.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    if (vkEndCommandBuffer(f.cmd) != VK_SUCCESS) {
        return false;
    }
    f.frame_number = frame.frame_number();
    f.sets_epoch   = m_sets_epoch;
    f.build        = frame.build_serial();
    m_prepare_ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

void Model3Pass::prepare_next(FrameBuilder& frame)
{
    if (m_ready && frame.has_frame()) {
        static_cast<void>(prepare_3d(frame, m_slots[m_records % kSlots], true));
    }
}

void Model3Pass::draw_range(const FramePlan& plan, DrawRange range, int layer, BuildSlot& f, VkCommandBuffer cmd,
                            bool float_depth)
{
    const VkDeviceSize r3d_stride = round_up(sizeof(R3DUniforms), m_ubo_alignment);

    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    u32        bound_viewport = ~0u;
    u32        bound_uniforms = ~0u;
    u32        bound_model    = ~0u;
    int        bound_region   = -1;
    int        bound_ref      = -1;

    for (u32 i = range.begin; i < range.end; i++) {
        const R3DDraw& d = plan.draws[i];
        const u32 variant = m_atlas.live ? d.variant : d.variant & ~kR3DVariantReplaced;
        const VkPipeline pipeline = r3d_pipeline(layer, d.layered, variant, float_depth);
        if (pipeline == VK_NULL_HANDLE) {
            continue;
        }
        // One layout and the same dynamic states for all: sets, pushes and
        // the stencil reference stay bound across pipelines.
        if (pipeline != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound_pipeline = pipeline;
        }
        if (d.viewport != bound_viewport) {
            const ViewportRect& vp = plan.viewports[d.viewport];
            const VkViewport    v  = flipped_viewport(vp.viewport);
            const VkRect2D      s  = to_rect(vp.scissor);
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            bound_viewport = d.viewport;
        }
        if (d.uniforms != bound_uniforms) {
            const u32 offset = static_cast<u32>(d.uniforms * r3d_stride);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_r3d_layout, 0, 1, &f.r3d_set, 1, &offset);
            bound_uniforms = d.uniforms;
        }
        if (d.model != bound_model) {
            vkCmdPushConstants(cmd, m_r3d_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(R3DModelUniforms),
                               &plan.models[d.model]);
            bound_model = d.model;
        }
        const int ref = d.layered || !d.no_los_return ? 0x00 : 0x80;
        if (!float_depth && ref != bound_ref) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, static_cast<u32>(ref));
            bound_ref = ref;
        }
        const int region = d.dynamic ? 1 : 0;
        if (region != bound_region) {
            const VkBuffer     buffer = d.dynamic ? f.ram_vertices.buffer : m_rom_vertices.buffer;
            const VkDeviceSize zero   = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &zero);
            bound_region = region;
        }
        vkCmdDraw(cmd, d.vertex_count, 1, d.first_vertex, 0);
    }
}

void Model3Pass::draw_3d(const FramePlan& plan, BuildSlot& f, VkCommandBuffer cmd)
{
    const auto& fn = device_functions();
    const VkRect2D whole{{0, 0}, {m_target_width, m_target_height}};
    const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    // Nothing of last frame's targets is kept.
    for (Image& c : m_colour) {
        record_image_barrier(cmd, c.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }
    record_image_barrier(cmd, m_los_target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    record_image_barrier(cmd, m_depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    if (m_float_depth_format != VK_FORMAT_UNDEFINED) {
        record_image_barrier(cmd, m_depth_f.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    }

    // Each rendering instance below loads what the one before wrote. A tiler
    // that renders straight to memory (Turnip's sysmem path) needs the
    // dependency spelled out, or later passes read stale attachments.
    auto attachments_written = [&] {
        VkMemoryBarrier2 mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                          | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb.dstStageMask  = mb.srcStageMask;
        mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkDependencyInfo dep{};
        dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &mb;
        fn.cmd_pipeline_barrier2(cmd, &dep);
    };

    auto draw_fogs = [&](VkPipeline pipeline) {
        if (plan.fogs.empty()) {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkDeviceSize r3d_stride = round_up(sizeof(R3DUniforms), m_ubo_alignment);
        const VkDeviceSize fog_stride = round_up(sizeof(ScrollFogUniforms), m_ubo_alignment);
        const VkDeviceSize fog_base   = r3d_stride * std::max<usize>(plan.uniforms.size(), 1);
        for (usize i = 0; i < plan.fogs.size(); i++) {
            const ViewportRect& vp = plan.viewports[plan.fogs[i].viewport];
            const VkViewport    v  = plain_viewport(vp.viewport);
            const VkRect2D      s  = to_rect(vp.scissor);
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &s);
            const u32 offset = static_cast<u32>(fog_base + i * fog_stride);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fog_layout, 0, 1, &f.fog_set, 1, &offset);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    };

    // Clear the colour targets, then draw the fog quads.
    // With a layer pass to follow, both happen at its start instead: a tiler
    // then skips a store and reload of all three targets.
    if (plan.passes.empty()) {
        VkRenderingAttachmentInfo colour[3];
        for (int i = 0; i < 3; i++) {
            colour[i] = colour_attachment(m_colour[static_cast<usize>(i)].view, VK_ATTACHMENT_LOAD_OP_CLEAR);
        }
        VkRenderingInfo info{};
        info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea           = whole;
        info.layerCount           = 1;
        info.colorAttachmentCount = 3;
        info.pColorAttachments    = colour;
        fn.cmd_begin_rendering(cmd, &info);
        draw_fogs(m_fog_pipeline);
        fn.cmd_end_rendering(cmd);
        attachments_written();
    }

    bool first = true;
    for (const LayerPass& pass : plan.passes) {
        bool needs_stencil = false;
        for (const DrawRange* r : {&pass.colour, &pass.trans1, &pass.trans2}) {
            for (u32 d = r->begin; d < r->end && !needs_stencil; d++) {
                needs_stencil = plan.draws[d].layered;
            }
        }
        const bool               fd        = m_float_depth_format != VK_FORMAT_UNDEFINED && !needs_stencil;
        Image&                   depth_img = fd ? m_depth_f : m_depth;
        Image&                   depth_cpy = fd ? m_depth_f_copy : m_depth_copy;
        const VkImageAspectFlags aspect    = fd ? VK_IMAGE_ASPECT_DEPTH_BIT : ds;

        // Depth and stencil cleared to 0, and the line-of-sight target cleared.
        {
            VkRenderingAttachmentInfo colour[4];
            for (int i = 0; i < 3; i++) {
                colour[i] = colour_attachment(m_colour[static_cast<usize>(i)].view,
                                              first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
            }
            // A tiler writes back only what a later pass reads: the line of
            // sight when it is sampled, the depth when a translucency layer
            // follows.
            colour[3] = colour_attachment(m_los_target.view, VK_ATTACHMENT_LOAD_OP_CLEAR);
            if (!pass.los) colour[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkRenderingAttachmentInfo depth{};
            depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView   = depth_img.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp     = pass.trans1.empty() && pass.trans2.empty() ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                                                           : VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {0.0f, 0};
            VkRenderingInfo info{};
            info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea           = whole;
            info.layerCount           = 1;
            info.colorAttachmentCount = 4;
            info.pColorAttachments    = colour;
            info.pDepthAttachment     = &depth;
            info.pStencilAttachment   = fd ? nullptr : &depth;
            fn.cmd_begin_rendering(cmd, &info);
            if (first) {
                draw_fogs(m_layer_fog_pipelines[fd ? 1 : 0]);
                first = false;
            }
            draw_range(plan, pass.colour, 0, f, cmd, fd);
            fn.cmd_end_rendering(cmd);
            attachments_written();
        }

        if (pass.los) {
            // Line of sight: copy the texel now, decode when this slot retires.
            record_image_barrier(cmd, m_los_target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset     = static_cast<VkDeviceSize>(pass.priority) * kLosSlotBytes;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset      = {pass.los_x, pass.los_y, 0};
            region.imageExtent      = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, m_los_target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.los.buffer, 1, &region);
            record_image_barrier(cmd, m_los_target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            f.los_pending[static_cast<usize>(pass.priority)] = true;
        }

        // Save the depth before the first translucency layer and restore it after.
        auto copy_depth = [&](Image& from, Image& to, VkImageLayout to_old) {
            record_image_barrier(cmd, from.image, aspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                 VK_ACCESS_2_TRANSFER_READ_BIT);
            record_image_barrier(cmd, to.image, aspect, to_old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy region{};
            region.srcSubresource = {aspect, 0, 0, 1};
            region.dstSubresource = {aspect, 0, 0, 1};
            region.extent         = {m_target_width, m_target_height, 1};
            vkCmdCopyImage(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            record_image_barrier(cmd, from.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            record_image_barrier(cmd, to.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        };
        auto trans_pass = [&](int layer, DrawRange range) {
            VkRenderingAttachmentInfo colour[4];
            for (int i = 0; i < 4; i++) {
                colour[i]           = colour_attachment(VK_NULL_HANDLE, VK_ATTACHMENT_LOAD_OP_LOAD);
                colour[i].imageView = VK_NULL_HANDLE;
            }
            colour[layer] = colour_attachment(m_colour[static_cast<usize>(layer)].view, VK_ATTACHMENT_LOAD_OP_LOAD);
            VkRenderingAttachmentInfo depth{};
            depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView   = depth_img.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            // Restored from the copy before the second layer, or never read
            // again: the next pass clears it.
            depth.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkRenderingInfo info{};
            info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea           = whole;
            info.layerCount           = 1;
            info.colorAttachmentCount = 4;
            info.pColorAttachments    = colour;
            info.pDepthAttachment     = &depth;
            info.pStencilAttachment   = fd ? nullptr : &depth;
            fn.cmd_begin_rendering(cmd, &info);
            draw_range(plan, range, layer, f, cmd, fd);
            fn.cmd_end_rendering(cmd);
            attachments_written();
        };

        // An empty layer changes nothing, and the depth only needs saving
        // when both layers draw.
        const bool both = !pass.trans1.empty() && !pass.trans2.empty();
        if (both) {
            copy_depth(depth_img, depth_cpy, VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (!pass.trans1.empty()) {
            trans_pass(1, pass.trans1);
        }
        if (both) {
            copy_depth(depth_cpy, depth_img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        if (!pass.trans2.empty()) {
            trans_pass(2, pass.trans2);
        }
    }

    for (Image& c : m_colour) {
        record_image_barrier(cmd, c.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}

void Model3Pass::composite(const FramePlan& plan, VkImageView native_view, VkCommandBuffer cmd)
{
    const auto& fn = device_functions();
    const VkRect2D whole{{0, 0}, {m_width, m_height}};

    // The whole frame cleared to black, then the bottom tile layer.
    VkRenderingAttachmentInfo target = colour_attachment(native_view, VK_ATTACHMENT_LOAD_OP_CLEAR);
    target.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = whole;
    info.layerCount           = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &target;
    fn.cmd_begin_rendering(cmd, &info);

    const VkViewport full{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    const VkRect2D   display = to_rect(plan.display);
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &display);

    struct {
        float rect[4];
        float uv_scale[2];
        float clip[2];
    } push{};
    const usize filter = m_smooth_2d ? 1 : 0;

    auto draw_layer = [&](int layer) {
        const LayerDraw& d = plan.layers[layer];
        push.rect[0]       = static_cast<float>(d.rect.x);
        push.rect[1]       = static_cast<float>(d.rect.y);
        push.rect[2]       = static_cast<float>(d.rect.width);
        push.rect[3]       = static_cast<float>(d.rect.height);
        push.uv_scale[0]   = d.uv_scale;
        push.uv_scale[1]   = 1.0f;
        push.clip[0]       = d.clip_lo;
        push.clip[1]       = d.clip_hi;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layer_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layer_layout, 0, 1,
                                &m_layer_sets[static_cast<usize>(layer)][filter], 0, nullptr);
        vkCmdPushConstants(cmd, m_layer_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };

    draw_layer(0);

    if (plan.white_flash) {
        // Block culling: white inside the display box, over the bottom layer.
        VkClearAttachment clear{};
        clear.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        clear.clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkClearRect rect{display, 0, 1};
        if (display.extent.width != 0 && display.extent.height != 0) {
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }
    } else {
        // The colour target, then the translucency targets.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_base_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_one_tex_pl_layout, 0, 1, &m_base_set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_trans_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_two_tex_pl_layout, 0, 1, &m_trans_set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    draw_layer(1);
    fn.cmd_end_rendering(cmd);
}

void Model3Pass::record(FrameBuilder& frame, VkImage native_image, VkImageView native_view)
{
    static_cast<void>(native_image);
    if (!m_ready || !frame.has_frame()) {
        return;
    }
    const VkCommandBuffer cmd = m_context->cmd();
    PerFrame&             f   = m_frames[m_context->frame_index()];
    const u64             r   = m_records++;
    BuildSlot&            s   = m_slots[r % kSlots];

    // Line-of-sight from kFramesInFlight records back, whose frame has retired.
    read_back_los(m_slots[(r + kSlots - Context::kFramesInFlight) % kSlots]);
    sync_atlas(cmd);
    if (m_upload_ticket != 0) {
        frame.wait(take_upload_ticket());
    }
    m_copies.clear();
    upload(frame, f, cmd);
    if (!m_copies.empty()) {
        m_upload_ticket = frame.run_async([this] {
            for (const Copy& c : m_copies) {
                std::memcpy(c.dst, c.src, c.bytes);
            }
        });
    }

    if (s.build != frame.build_serial() || s.sets_epoch != m_sets_epoch) {
        if (!prepare_3d(frame, s, false)) {
            return;
        }
    }
    const FramePlan& plan = frame.plan();
    if (s.has_3d) {
        m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, GpuStage::Poly3D, false);
    }
    vkCmdExecuteCommands(cmd, 1, &s.cmd);
    if (s.has_3d) {
        m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, GpuStage::Poly3D, true);
    }
    s.build = 0;  // one submission each
    m_context->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, GpuStage::Composite, false);
    composite(plan, native_view, cmd);
    m_context->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, GpuStage::Composite, true);
}

}  // namespace sm2::render::vk
