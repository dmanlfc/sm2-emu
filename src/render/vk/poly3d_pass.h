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

namespace sm2::render::vk {

/// Draws the emulated 3D output.
///
/// The geometry engine has already produced this frame's polygons in screen
/// space, clipped and in drawing order, so there is no vertex work left to do.
/// What remains is genuinely a fill: turn each polygon into triangles, give it its
/// one colour, and honour the hardware's rule about which polygon owns a pixel.
///
/// Two properties of the hardware decide the shape of this pass.
///
/// It has no depth buffer. Polygons arrive nearest first and a one-bit fill mask
/// stops anything drawing over a pixel already claimed. That is reproduced with a
/// stencil attachment tested for equality against zero and incremented on pass,
/// which is the same rule stated in Vulkan's terms. A depth test would be the
/// obvious substitute and would be wrong: games lean on the drawing order, with
/// per-polygon sort overrides and priority windows that ignore depth altogether.
///
/// It rasterises at 496 by 384 and nothing else. So does this pass, into an
/// offscreen image that is then scaled to the window in one step. Rasterising at
/// window resolution instead would be visibly wrong rather than merely sharper:
/// the hardware's translucency is a checkerboard stipple locked to the raster
/// grid, and its texture level of detail is chosen from raster-pixel
/// derivatives, so both would come out at the wrong scale.
class Poly3DPass {
public:
    static constexpr u32 kWidth  = 496;
    static constexpr u32 kHeight = 384;

    /// Words in one texture sheet. Only the low half of the two-megabyte texture
    /// RAM window is ever written, and only that half is addressable by the texel
    /// arithmetic, so only that half is uploaded.
    static constexpr u32 kSheetWords = 0x40000;

    Poly3DPass() = default;
    ~Poly3DPass();

    Poly3DPass(const Poly3DPass&)            = delete;
    Poly3DPass& operator=(const Poly3DPass&) = delete;

    /// Physical texel containers per sheet: half the 1024x2048 physical texel
    /// grid in each axis, since the decode pass packs a 2x2 block of texels into
    /// one image element (see decode_texel.comp).
    static constexpr u32 kDecodedWidth  = 512;
    static constexpr u32 kDecodedHeight = 1024;

    [[nodiscard]] bool init(Context& context);
    void shutdown();

    /// Triangulate this frame's polygons, unpack their texture headers, and refresh
    /// whatever machine memory has changed since this frame last ran.
    ///
    /// The machine argument is what the pass reads texture RAM, luminance RAM and
    /// the colour tables from; it also carries the change counters that decide
    /// whether any of that needs copying again.
    void build(const hw::Model2MachineBase* machine, const hw::Model2Video& video);

    /// Record the fill-mask stencil's transition into an attachment layout,
    /// which must happen outside any rendering scope. Call with a command buffer
    /// open and no rendering scope active; stencil_attachment() then hands the
    /// attachment to the tilemap pass so the 3D draws into the native frame.
    void prepare_stencil();

    /// The fill-mask stencil attachment for the native-frame scope, valid after
    /// prepare_stencil() and until the scope ends. Cleared to zero on load.
    [[nodiscard]] VkRenderingAttachmentInfo stencil_attachment() const;
    [[nodiscard]] bool stencil_has_depth() const { return m_stencil_has_depth; }

    /// Draw what build() prepared directly into the native frame, inside the
    /// scope the tilemap pass opened (between record_below() and record_above())
    /// with stencil_attachment() attached. Blends over the below-tilemap with
    /// premultiplied-over -- the same result the old composite step produced.
    void draw_polygons();

    // -- diagnostics -------------------------------------------------------

    [[nodiscard]] u32 triangles() const { return m_vertex_count / 3; }
    [[nodiscard]] u32 drawn_polygons() const { return m_frame_geometry.drawn_polygons; }

    /// Polygons on the one pixel path that draws nothing at all: untextured and
    /// translucent. Not a gap, but worth counting, because a game asking for many
    /// of them would mean the path selection is being read wrongly.
    [[nodiscard]] u32 blank_polygons() const { return m_frame_geometry.blank_polygons; }



private:
    /// Vertex, PolyParams, kFlag*/kMaxLevelShift and Batch's shape all live in
    /// render/geometry.h now -- see that header's own doc comment for why.
    /// scissor_to_vk() below is the one-line conversion at the boundary the
    /// design calls for, from render::ScissorRect to this API's VkRect2D.
    using Vertex     = render::Vertex;
    using PolyParams = render::PolyParams;

    [[nodiscard]] static VkRect2D scissor_to_vk(const render::ScissorRect& scissor)
    {
        return VkRect2D{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
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
    /// The machine's memory is duplicated per frame rather than shared because
    /// three frames are in flight and the emulated program keeps writing: a shared
    /// copy would have to be fenced against frames still reading it. Duplicating
    /// costs a few megabytes and the copies only happen when the data actually
    /// changed, which for textures is usually once at startup.
    struct Frame {
        HostBuffer vertices;
        HostBuffer polygons;
        HostBuffer sheets;
        HostBuffer luma;

        VkImage       tone       = VK_NULL_HANDLE;
        VmaAllocation tone_alloc = nullptr;
        VkImageView   tone_view  = VK_NULL_HANDLE;
        HostBuffer    tone_staging;
        bool          tone_uploaded = false;

        /// The two texture sheets, decoded from their packed 4-bit form into one
        /// pixel per 2x2 texel container (see texel_decode.comp). Sampled by the
        /// fragment shader in place of the packed buffer directly; rebuilt only
        /// when texture_generation changes, which is almost always once. One view
        /// serves both the compute pass's writes and the fragment pass's reads,
        /// since both name the same uint format.
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

    /// Copy whatever this frame's copies are missing, and record the transfer of
    /// the tone curve into its image.
    void refresh_machine_data(const hw::Model2MachineBase& machine, const hw::Model2Video& video);

    /// Dispatch the decode pass over the current frame's texture sheets.
    ///
    /// Unconditional: refresh_machine_data() calls this only when the sheets
    /// buffer it just copied is new, so there is no separate dirty check here.
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope, and after the sheets buffer's host writes -- there is no
    /// memory barrier between them because both are host-coherent CPU writes
    /// completed before this command is even recorded, and the compute shader's
    /// own execution dependency (waiting for the copy to finish) is unnecessary
    /// since there was never a GPU-side write to wait on.
    void decode_textures();

    [[nodiscard]] Frame& frame();

    Context* m_context = nullptr;

    VkFormat m_colour_format  = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;
    bool     m_stencil_has_depth = false;

    VkPipelineLayout m_polygon_layout   = VK_NULL_HANDLE;
    VkPipeline       m_polygon_pipeline = VK_NULL_HANDLE;
    VkPipeline       m_polygon_pipeline_early = VK_NULL_HANDLE;  ///< early-test variant

    VkSampler             m_sampler            = VK_NULL_HANDLE;
    /// Nearest-filtering, non-normalized-adjacent sampler for the decoded texture
    /// image: the fragment shader still does its own bilinear blend between the
    /// four taps a container's neighbours provide, so this only ever wants the
    /// exact texel, never an interpolated one.
    VkSampler             m_decoded_sampler    = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool               = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_polygon_set_layout = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_decode_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_decode_layout     = VK_NULL_HANDLE;
    VkPipeline            m_decode_pipeline   = VK_NULL_HANDLE;

    std::array<Frame, Context::kFramesInFlight> m_frames{};

    /// Built by build() via render::triangulate(), consumed by render().
    render::TriangulatedFrame m_frame_geometry;
    std::vector<u32>          m_tone_curve;

    u32  m_vertex_count    = 0;
    bool m_capacity_warned = false;
};

}  // namespace sm2::render::vk
