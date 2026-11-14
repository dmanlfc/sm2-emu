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

namespace sm2::render {
class TextureReplacements;
}

namespace sm2::render::vk {

class Model3Pass {
public:
    Model3Pass() = default;
    ~Model3Pass();

    Model3Pass(const Model3Pass&)            = delete;
    Model3Pass& operator=(const Model3Pass&) = delete;

    [[nodiscard]] bool init(Context& context, u32 render_scale, u32 width);
    /// keep_atlas holds on to the custom texture atlas, whose pixels are gone
    /// once uploaded.
    void shutdown(bool keep_atlas = false);
    [[nodiscard]] bool ready() const { return m_ready; }

    [[nodiscard]] bool encode_depth() const { return m_depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT; }

    void set_smooth_2d(bool smooth) { m_smooth_2d = smooth; }
    /// Anisotropic taps in the 3D pass; 1 is the hardware's own filter.
    void set_aniso_taps(u32 taps) { m_aniso_taps = taps; }
    void set_texture_replacements(TextureReplacements* replacements) { m_replacements = replacements; }

    // The buffer writes run on the frame's worker while the commands are
    // recorded: wait(take_upload_ticket()) on it before submitting.
    void record(model3::FrameBuilder& frame, VkImage native_image, VkImageView native_view);
    /// On the frame's worker, after a build: the 3D pass for the next
    /// record(), which otherwise records it itself.
    void prepare_next(model3::FrameBuilder& frame);
    [[nodiscard]] double prepare_ms() const { return m_prepare_ms; }
    [[nodiscard]] u64 take_upload_ticket()
    {
        const u64 ticket = m_upload_ticket;
        m_upload_ticket  = 0;
        return ticket;
    }

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
        Buffer staging;  ///< Texture and tile layer uploads.
    };
    // The 3D pass of one record(): record r uses slot r % kSlots, which the
    // GPU finished with when the record before it began its frame.
    struct BuildSlot {
        Buffer          ram_vertices;
        Buffer          uniforms;   ///< R3D blocks then fog blocks.
        Buffer          los;        ///< 4 readback slots.
        VkDescriptorSet r3d_set = VK_NULL_HANDLE;
        VkDescriptorSet fog_set = VK_NULL_HANDLE;
        u64             bound_uniforms = 0;  ///< serial of the buffer the sets point at
        VkCommandBuffer cmd = VK_NULL_HANDLE;  ///< secondary
        u64             build      = 0;  ///< FrameBuilder::build_serial() recorded, 0 for none
        u64             sets_epoch = 0;
        bool            has_3d     = false;
        std::array<bool, 4> los_pending{};
        u64             frame_number = 0;
    };
    static constexpr u32 kSlots = Context::kFramesInFlight + 1;

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
    void update_frame_sets(BuildSlot& f);
    void sync_atlas(VkCommandBuffer cmd);

    struct Copy {
        void*       dst   = nullptr;
        const void* src   = nullptr;
        usize       bytes = 0;
    };

    void read_back_los(BuildSlot& f);
    void upload(model3::FrameBuilder& frame, PerFrame& f, VkCommandBuffer cmd);
    [[nodiscard]] bool prepare_3d(model3::FrameBuilder& frame, BuildSlot& f, bool on_worker);
    void draw_3d(const model3::FramePlan& plan, BuildSlot& f, VkCommandBuffer cmd);
    void draw_range(const model3::FramePlan& plan, model3::DrawRange range, int layer, BuildSlot& f,
                    VkCommandBuffer cmd, bool float_depth);
    void composite(const model3::FramePlan& plan, VkImageView native_view, VkCommandBuffer cmd);

    Context* m_context      = nullptr;
    u64      m_buffer_serial = 0;
    bool     m_ready        = false;
    bool     m_smooth_2d    = false;
    u32      m_aniso_taps   = 1;
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
    std::array<u32, 2>   m_layer_width{};
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
    std::array<BuildSlot, kSlots>                  m_slots;
    u64           m_records    = 0;
    u64           m_sets_epoch = 1;  // bumped when the slots' sets are rewritten
    VkCommandPool m_slot_pool  = VK_NULL_HANDLE;
    double        m_prepare_ms = 0.0;

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
    std::array<VkPipeline, 2 * 2 * 3 * 2 * model3::kR3DVariantCount> m_r3d_pipelines{};
    VkShaderModule m_r3d_vs = VK_NULL_HANDLE;
    VkShaderModule m_r3d_fs = VK_NULL_HANDLE;
    VkShaderModule m_r3d_fs_float = VK_NULL_HANDLE;
    VkPipeline m_fog_pipeline   = VK_NULL_HANDLE;
    // The fog quads drawn inside the first layer pass, by depth format (D24S8, float).
    std::array<VkPipeline, 2> m_layer_fog_pipelines{};
    VkPipeline m_base_pipeline  = VK_NULL_HANDLE;
    VkPipeline m_trans_pipeline = VK_NULL_HANDLE;
    VkPipeline m_layer_pipeline = VK_NULL_HANDLE;

    // Custom textures: RGBA8 layers with mips, or a 1x1 stand-in.
    struct Atlas {
        Image  image;
        Buffer staging;
        u32    staging_age = 0;  ///< records left before staging can go
        u32    mips        = 1;
        u32    layers      = 1;
        u64    generation  = 0;
        bool   live        = false;
        bool   bound       = false;  ///< written into the current r3d sets
    };
    TextureReplacements* m_replacements  = nullptr;
    Atlas                m_atlas;
    VkSampler            m_atlas_sampler = VK_NULL_HANDLE;

    model3::LineOfSightMailbox m_los;

    std::vector<Copy> m_copies;
    u64               m_upload_ticket = 0;
};

}  // namespace sm2::render::vk
