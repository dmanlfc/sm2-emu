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

#include "render/vk/context.h"
#include "render/vk/vk_common.h"

#include <array>
#include <vector>

// Forward-declared so VMA's header stays out of the public interface.
using VmaAllocation = struct VmaAllocation_T*;

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
struct RenderPolygon;
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

    /// Room for far more geometry than the hardware could draw in a frame. Virtua
    /// Fighter 2 in a match submits about six thousand vertices; the geometry
    /// engine's pool would allow ninety times this in the worst case, but a real
    /// board had nowhere near the fill rate to reach it. Anything beyond this is
    /// dropped with a warning rather than silently.
    static constexpr u32 kMaxVertices = 1 << 17;

    /// Polygons whose parameters can be held at once, which bounds the same thing
    /// from the other direction.
    static constexpr u32 kMaxPolygons = 1 << 15;

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

    /// Draw what build() prepared into the offscreen target.
    ///
    /// Must be called with a command buffer open and outside any dynamic
    /// rendering scope, because it opens one of its own.
    void render();

    /// Blend the offscreen result over the swapchain.
    ///
    /// Must be called inside the swapchain's rendering scope, between the tilemap
    /// layers that belong behind the 3D and those that belong in front of it.
    void composite();

    // -- diagnostics -------------------------------------------------------

    [[nodiscard]] u32 triangles() const { return m_vertex_count / 3; }
    [[nodiscard]] u32 drawn_polygons() const { return m_drawn_polygons; }

    /// Polygons on the one pixel path that draws nothing at all: untextured and
    /// translucent. Not a gap, but worth counting, because a game asking for many
    /// of them would mean the path selection is being read wrongly.
    [[nodiscard]] u32 blank_polygons() const { return m_blank_polygons; }



private:
    /// Twenty-four bytes: where the vertex is, what part of the texture it
    /// names, how far away it is, and which polygon it belongs to.
    ///
    /// The depth is here rather than derived because the fragment stage needs it
    /// both for perspective-correct texturing and for the mipmap level, and the
    /// polygon index is here because everything else about a polygon is the same
    /// for all of its vertices and belongs in a buffer indexed once.
    struct Vertex {
        float x;
        float y;
        /// Texture point from the polygon ROM, in eighths of a texel. Fractional
        /// after clipping, so it cannot be kept as an integer.
        float u;
        float v;
        float depth;
        u32   polygon;
    };

    /// Everything the fragment shader needs to know about one polygon. Must match
    /// the PolyParams declaration in polygon.frag exactly.
    struct PolyParams {
        u32 colour;
        u32 luma_base;
        u32 luma_scale;
        s32 tex_lod;
        u32 tex_width;
        u32 tex_height;
        u32 tex_x;
        u32 tex_y;
        u32 flags;
        u32 micro_x;
        u32 micro_y;
        u32 micro_min_lod;
    };

    /// Bits of PolyParams::flags, mirroring the constants in polygon.frag.
    enum : u32 {
        kFlagTextured  = 1u << 0,
        kFlagChecker   = 1u << 1,
        kFlagWrapX     = 1u << 2,
        kFlagWrapY     = 1u << 3,
        kFlagMirrorX   = 1u << 4,
        kFlagMirrorY   = 1u << 5,
        kFlagSheet     = 1u << 6,
        kFlagMicro     = 1u << 7,
        /// Deepest mipmap level, in bits 11:8.
        kMaxLevelShift    = 8,
        kFlagTranslucent  = 1u << 12,
    };

    /// A run of triangles sharing one scissor rectangle.
    ///
    /// The scissor is the polygon's priority window, which can be smaller than
    /// the screen. Polygons come grouped by window, so a frame needs only a
    /// handful of these however many polygons it holds.
    struct Batch {
        VkRect2D scissor;
        u32      first_vertex;
        u32      vertex_count;
    };

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

        VkImage       colour        = VK_NULL_HANDLE;
        VmaAllocation colour_alloc  = nullptr;
        VkImageView   colour_view   = VK_NULL_HANDLE;

        VkImage       stencil       = VK_NULL_HANDLE;
        VmaAllocation stencil_alloc = nullptr;
        VkImageView   stencil_view  = VK_NULL_HANDLE;

        VkDescriptorSet polygon_set   = VK_NULL_HANDLE;
        VkDescriptorSet composite_set = VK_NULL_HANDLE;

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
    [[nodiscard]] bool create_polygon_pipeline();
    [[nodiscard]] bool create_composite_pipeline();
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

    /// Unpack one polygon's texture header into the parameter buffer.
    [[nodiscard]] PolyParams describe(const hw::RenderPolygon& poly,
                                      const hw::Model2Video&   video) const;

    [[nodiscard]] Frame& frame();

    Context* m_context = nullptr;

    VkFormat m_colour_format  = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;
    bool     m_stencil_has_depth = false;

    VkPipelineLayout m_polygon_layout   = VK_NULL_HANDLE;
    VkPipeline       m_polygon_pipeline = VK_NULL_HANDLE;

    VkSampler             m_sampler            = VK_NULL_HANDLE;
    /// Nearest-filtering, non-normalized-adjacent sampler for the decoded texture
    /// image: the fragment shader still does its own bilinear blend between the
    /// four taps a container's neighbours provide, so this only ever wants the
    /// exact texel, never an interpolated one.
    VkSampler             m_decoded_sampler    = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool               = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_polygon_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_composite_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_composite_layout   = VK_NULL_HANDLE;
    VkPipeline            m_composite_pipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_decode_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_decode_layout     = VK_NULL_HANDLE;
    VkPipeline            m_decode_pipeline   = VK_NULL_HANDLE;

    std::array<Frame, Context::kFramesInFlight> m_frames{};

    /// Built by build(), consumed by render(). Kept between frames so the vectors
    /// stop reallocating after the first busy frame.
    std::vector<Vertex>     m_vertices;
    std::vector<PolyParams> m_polygons;
    std::vector<Batch>      m_batches;
    std::vector<u32>        m_tone_curve;

    u32  m_vertex_count    = 0;
    u32  m_drawn_polygons  = 0;
    u32  m_blank_polygons  = 0;
    bool m_capacity_warned = false;
};

}  // namespace sm2::render::vk
