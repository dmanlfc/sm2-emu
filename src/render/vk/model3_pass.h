//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp, Copyright (c)
// 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Model 3 on Vulkan.
// Translucency passes bind only their own target, so no independentBlend.
#pragma once

#include "render/model3/model3_frame.h"
#include "render/vk/context.h"
#include "render/vk/vk_common.h"

#include <array>
#include <vector>

using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::render::vk {

class Model3Pass {
public:
    Model3Pass() = default;
    ~Model3Pass();

    Model3Pass(const Model3Pass&)            = delete;
    Model3Pass& operator=(const Model3Pass&) = delete;

    [[nodiscard]] bool init(Context& context, u32 render_scale, u32 width);
    void shutdown();
    [[nodiscard]] bool ready() const { return m_ready; }

    [[nodiscard]] bool encode_depth() const { return m_depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT; }

    void set_smooth_2d(bool smooth) { m_smooth_2d = smooth; }

    void record(model3::FrameBuilder& frame, VkImage native_image, VkImageView native_view);

    [[nodiscard]] const model3::LineOfSightMailbox& line_of_sight() const { return m_los; }

private:
    struct Buffer {
        VkBuffer      buffer     = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void*         mapped     = nullptr;
        VkDeviceSize  size       = 0;
        u64           serial     = 0;  ///< Distinguishes recreations (handles get reused).
    };
    struct Image {
        VkImage       image      = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView   view       = VK_NULL_HANDLE;
    };
    struct PerFrame {
        Buffer          ram_vertices;
        Buffer          uniforms;   ///< R3D blocks then fog blocks.
        Buffer          staging;    ///< Texture and tile layer uploads.
        Buffer          los;        ///< 4 readback slots.
        VkDescriptorSet r3d_set = VK_NULL_HANDLE;
        VkDescriptorSet fog_set = VK_NULL_HANDLE;
        u64             bound_uniforms = 0;  ///< serial of the buffer the sets point at
        std::array<bool, 4> los_pending{};
        u64             frame_number = 0;
    };

    [[nodiscard]] bool create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& b);
    [[nodiscard]] bool ensure_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
    [[nodiscard]] bool create_image(Image& img, VkFormat format, u32 w, u32 h, u32 levels, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, u32 layers = 1);
    void destroy_image(Image& img);

    [[nodiscard]] bool create_resources();
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_pipelines();
    [[nodiscard]] VkPipeline r3d_pipeline(int layer, bool layered, u32 variant, bool float_depth);
    [[nodiscard]] VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                                           std::span<const VkFormat> colour_formats, VkFormat depth_format,
                                           bool vertex_input, bool blend, int depth_mode, bool layered,
                                           const VkSpecializationInfo* spec = nullptr);
    void update_frame_sets(PerFrame& f);

    void read_back_los(PerFrame& f);
    void upload(model3::FrameBuilder& frame, PerFrame& f, VkCommandBuffer cmd);
    void draw_3d(const model3::FramePlan& plan, PerFrame& f, VkCommandBuffer cmd);
    void draw_range(const model3::FramePlan& plan, model3::DrawRange range, int layer, PerFrame& f,
                    VkCommandBuffer cmd, bool float_depth);
    void composite(const model3::FramePlan& plan, VkImageView native_view, VkCommandBuffer cmd);

    Context* m_context      = nullptr;
    u64      m_buffer_serial = 0;
    bool     m_ready        = false;
    bool     m_smooth_2d    = false;
    u32      m_render_scale = 1;
    u32      m_width        = 0;
    u32      m_height       = 0;
    u32      m_target_width  = 0;
    u32      m_target_height = 0;
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    // When the depth format is D24S8, passes with no stencil draws use
    // D32_SFLOAT where the device has it, with no gl_FragDepth write.
    VkFormat m_float_depth_format = VK_FORMAT_UNDEFINED;
    Image    m_depth_f;
    Image    m_depth_f_copy;
    u32      m_ubo_alignment = 256;

    // Inputs
    Image                m_bank;  // two layers, one per page
    std::array<Image, 2> m_layers;
    bool                 m_banks_initialised = false;
    Buffer               m_rom_vertices;
    u64                  m_rom_generation = 0;
    usize                m_rom_uploaded   = 0;

    // Scene targets
    std::array<Image, 3> m_colour;
    Image                m_los_target;
    Image                m_depth;
    Image                m_depth_copy;

    std::array<PerFrame, Context::kFramesInFlight> m_frames;

    // Descriptors and pipelines
    VkSampler             m_nearest = VK_NULL_HANDLE;
    VkSampler             m_linear  = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_r3d_set_layout    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ubo_set_layout    = VK_NULL_HANDLE;  // fog
    VkDescriptorSetLayout m_one_tex_layout    = VK_NULL_HANDLE;  // base composite, layers
    VkDescriptorSetLayout m_two_tex_layout    = VK_NULL_HANDLE;  // trans composite
    VkPipelineLayout      m_r3d_layout        = VK_NULL_HANDLE;
    VkPipelineLayout      m_fog_layout        = VK_NULL_HANDLE;
    VkPipelineLayout      m_one_tex_pl_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_two_tex_pl_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_layer_layout      = VK_NULL_HANDLE;
    VkDescriptorSet       m_base_set          = VK_NULL_HANDLE;
    VkDescriptorSet       m_trans_set         = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, 2>, 2> m_layer_sets{};  // [layer][nearest, linear]

    // [layer 0 colour, 1 trans1, 2 trans2][layered]
    // [layer][layered][variant], created on first use.
    std::array<VkPipeline, 2 * 3 * 2 * model3::kR3DVariantCount> m_r3d_pipelines{};
    VkShaderModule m_r3d_vs = VK_NULL_HANDLE;
    VkShaderModule m_r3d_fs = VK_NULL_HANDLE;
    VkShaderModule m_r3d_fs_float = VK_NULL_HANDLE;
    VkPipeline m_fog_pipeline   = VK_NULL_HANDLE;
    VkPipeline m_base_pipeline  = VK_NULL_HANDLE;
    VkPipeline m_trans_pipeline = VK_NULL_HANDLE;
    VkPipeline m_layer_pipeline = VK_NULL_HANDLE;

    model3::LineOfSightMailbox m_los;
};

}  // namespace sm2::render::vk
