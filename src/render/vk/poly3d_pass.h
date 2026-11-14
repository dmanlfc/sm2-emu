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

#include "render/geometry.h"
#include "render/vk/context.h"
#include "render/vk/vk_common.h"

#include <array>
#include <vector>

// Forward-declared so VMA's header stays out of the public interface.
using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::render {
class TextureReplacements;
}

namespace sm2::render::vk {

/// Draws the emulated 3D output.
///
/// The geometry engine has already produced this frame's polygons in screen
/// space, clipped and in drawing order, so this pass is purely a fill.
///
/// The hardware has no depth buffer: polygons arrive nearest first and a one-bit
/// fill mask stops anything drawing over a claimed pixel. That is reproduced with
/// a stencil tested equal to zero and incremented on pass. A depth test would be
/// wrong, since games rely on drawing order and priority windows that ignore depth.
class Poly3DPass {
public:
    static constexpr u32 kWidth  = 496;
    static constexpr u32 kHeight = 384;

    /// Words in one texture sheet: only the low half of the two-megabyte window
    /// is addressable by the texel arithmetic, so only that half is uploaded.
    static constexpr u32 kSheetWords = 0x40000;

    Poly3DPass() = default;
    ~Poly3DPass();

    Poly3DPass(const Poly3DPass&)            = delete;
    Poly3DPass& operator=(const Poly3DPass&) = delete;

    /// Decoded image size per sheet: half the 1024x2048 texel grid in each axis,
    /// one element per 2x2 block (see texel_decode.comp).
    static constexpr u32 kDecodedWidth  = 512;
    static constexpr u32 kDecodedHeight = 1024;

    /// `render_scale` (N) sizes the fill-mask attachment to N*native. Vertices
    /// stay in native coordinates; the viewport does the scaling.
    [[nodiscard]] bool init(Context& context, u32 render_scale);

    /// Raster width to draw at, up to frame_width(kWideColumns); the fill mask
    /// is allocated that wide, so this is live.
    void set_width(u32 width) { m_width = width; }
    void shutdown();

    /// Anisotropic texture-filter quality (0 = faithful single-tap, else the
    /// tap ceiling). Live.
    void set_texture_quality(u32 quality) { m_texture_quality = quality; }

    /// Draw the stipple as the 50% blend it's trying to achieve. Needs a fill
    /// mask with depth; ignored without one.
    void set_blend_translucency(bool blend) { m_blend_translucency = blend; }

    /// Custom textures to draw in place of the hardware's, or null for none.
    /// The atlas is uploaded by the next build() whenever this or its contents
    /// change.
    void set_texture_replacements(TextureReplacements* replacements)
    {
        m_replacements = replacements;
    }

    /// Triangulate this frame's polygons, unpack their texture headers, and refresh
    /// whatever machine memory has changed since this frame last ran.
    ///
    /// `machine` supplies texture RAM, luminance RAM and the colour tables, and
    /// the change counters that decide what needs copying again.
    void build(const hw::Model2MachineBase* machine, const hw::Model2Video& video);

    /// Record the fill-mask stencil's transition into an attachment layout. Call
    /// with a command buffer open and outside any rendering scope.
    void prepare_stencil();

    /// The fill-mask stencil attachment for the native-frame scope, valid after
    /// prepare_stencil() and until the scope ends. Cleared to zero on load.
    [[nodiscard]] VkRenderingAttachmentInfo stencil_attachment() const;
    [[nodiscard]] bool stencil_has_depth() const { return m_stencil_has_depth; }

    /// Draw what build() prepared into the native frame, inside the tilemap
    /// pass's scope between record_below() and record_above(), with
    /// stencil_attachment() attached. Premultiplied over the below-tilemap.
    void draw_polygons();

    // -- diagnostics -------------------------------------------------------

    [[nodiscard]] u32 triangles() const { return m_vertex_count / 3; }
    [[nodiscard]] u32 drawn_polygons() const { return m_frame_geometry.drawn_polygons; }

    /// Untextured translucent polygons, which draw nothing. Many of them would
    /// suggest the pixel path is being chosen wrongly.
    [[nodiscard]] u32 blank_polygons() const { return m_frame_geometry.blank_polygons; }



private:
    /// Shared with the other backends; see render/geometry.h.
    using Vertex     = render::Vertex;
    using PolyParams = render::PolyParams;

    /// The scissor is in native pixels; the target is N*native.
    [[nodiscard]] VkRect2D scissor_to_vk(const render::ScissorRect& scissor) const
    {
        const int32_t n = static_cast<int32_t>(m_render_scale);
        return VkRect2D{{scissor.x * n, scissor.y * n},
                        {scissor.width * m_render_scale, scissor.height * m_render_scale}};
    }

    /// A host-visible buffer the CPU writes and the GPU reads directly.
    struct HostBuffer {
        VkBuffer      handle     = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void*         mapped     = nullptr;
        VkDeviceSize  size       = 0;
    };

    /// Everything one frame in flight needs of its own.
    ///
    /// Machine memory is copied per frame in flight so a copy is never written
    /// while a frame still reads it. Copies happen only when the data changed.
    struct Frame {
        HostBuffer vertices;
        HostBuffer polygons;
        HostBuffer geometry;
        HostBuffer sheets;
        HostBuffer luma;

        VkImage       tone       = VK_NULL_HANDLE;
        VmaAllocation tone_alloc = nullptr;
        VkImageView   tone_view  = VK_NULL_HANDLE;
        HostBuffer    tone_staging;
        bool          tone_uploaded = false;

        /// The two texture sheets, one pixel per 2x2 texel block (see
        /// texel_decode.comp), rebuilt when texture_generation changes. One view
        /// serves both the compute writes and the fragment reads.
        VkImage         decoded       = VK_NULL_HANDLE;
        VmaAllocation   decoded_alloc = nullptr;
        VkImageView     decoded_view  = VK_NULL_HANDLE;
        VkDescriptorSet decode_set    = VK_NULL_HANDLE;

        VkImage       stencil       = VK_NULL_HANDLE;
        VmaAllocation stencil_alloc = nullptr;
        VkImageView   stencil_view  = VK_NULL_HANDLE;

        VkDescriptorSet polygon_set   = VK_NULL_HANDLE;

        /// Change counters of the machine data this frame's copies were made from.
        /// Zero means never copied, and the machine's counters start at one.
        u64 texture_generation = 0;
        u64 table_generation   = 0;
    };

    [[nodiscard]] bool create_frames();
    [[nodiscard]] bool create_host_buffer(VkDeviceSize       size,
                                          VkBufferUsageFlags usage,
                                          HostBuffer*        out);
    void destroy_host_buffer(HostBuffer* buffer);
    [[nodiscard]] bool create_descriptors();
    [[nodiscard]] bool create_polygon_pipelines();
    /// Build one polygon pipeline; the two variants differ only by fragment
    /// module (plain vs early-fragment-tests).
    [[nodiscard]] bool create_polygon_pipeline(const u32*  fragment_code,
                                               u32         fragment_word_count,
                                               VkPipeline* out_pipeline);
    [[nodiscard]] bool create_decode_pipeline();

    /// Create the custom-texture atlas, `layers` square layers of `size` with
    /// `mips` levels. A 1x1 placeholder stands in while there is nothing loaded.
    [[nodiscard]] bool create_atlas(u32 size, u32 layers, u32 mips);
    void destroy_atlas();

    /// Bring the atlas in line with m_replacements, recording its upload.
    void sync_replacements();

    /// Copy whatever this frame's copies are missing, and record the transfer of
    /// the tone curve into its image.
    void refresh_machine_data(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// Dispatch the decode pass over the current frame's texture sheets.
    ///
    /// Called only when the sheets are new. Needs a command buffer open and no
    /// rendering scope; the sheets buffer is host-coherent and written before
    /// recording, so no barrier is needed before the dispatch.
    void decode_textures();

    [[nodiscard]] Frame& frame();

    [[nodiscard]] bool blending() const { return m_blend_translucency && m_stencil_has_depth; }

    Context* m_context = nullptr;

    VkFormat m_colour_format  = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;
    bool     m_stencil_has_depth = false;

    VkPipelineLayout m_polygon_layout   = VK_NULL_HANDLE;
    VkPipeline       m_polygon_pipeline = VK_NULL_HANDLE;
    VkPipeline       m_polygon_pipeline_early = VK_NULL_HANDLE;  ///< early-test variant

    VkSampler             m_sampler            = VK_NULL_HANDLE;
    /// Nearest sampler for the decoded sheets: the shader does its own bilinear.
    VkSampler             m_decoded_sampler    = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool               = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_polygon_set_layout = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_decode_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_decode_layout     = VK_NULL_HANDLE;
    VkPipeline            m_decode_pipeline   = VK_NULL_HANDLE;

    std::array<Frame, Context::kFramesInFlight> m_frames{};

    /// Built by build(), consumed by draw_polygons().
    render::TriangulatedFrame m_frame_geometry;
    std::vector<u32>          m_tone_curve;

    u32  m_vertex_count    = 0;
    bool m_capacity_warned = false;

    /// Internal 3D render scale; the fill-mask attachment is N*native.
    u32 m_render_scale = 1;

    /// Raster width in native pixels: kWidth, or wider in widescreen.
    u32 m_width = kWidth;

    u32 m_texture_quality = 0;  ///< 0 = faithful single-tap, else tap ceiling

    bool m_blend_translucency = false;

    // -- custom textures ---------------------------------------------------

    TextureReplacements* m_replacements = nullptr;
    /// Generation of the replacements last synced, 0 for none; live is false
    /// while the 1x1 placeholder stands in.
    u64           m_atlas_generation = 0;
    bool          m_atlas_live       = false;
    bool          m_atlas_ready      = false;
    VkImage       m_atlas            = VK_NULL_HANDLE;
    VmaAllocation m_atlas_alloc      = nullptr;
    VkImageView   m_atlas_view       = VK_NULL_HANDLE;
    u32           m_atlas_size       = 1;
    u32           m_atlas_layers     = 1;
    u32           m_atlas_mips       = 1;
    VkSampler     m_atlas_sampler    = VK_NULL_HANDLE;
    HostBuffer    m_atlas_staging;
};

}  // namespace sm2::render::vk
