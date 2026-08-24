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
// Backend-neutral render-list processing: turning the geometry engine's output
// into triangles and per-polygon draw parameters, and the one piece of
// presentation math (the 4:3 letterbox) that has nothing to do with any
// graphics API either.
//
// This is what phase 8's design.md originally proposed moving out of
// render/vk/Poly3DPass and PresentPass, so a second backend would not need to
// reimplement the triangulation and texture-header unpacking by hand. That
// move did not happen during phase 8; this file is where it happens instead,
// so both the Vulkan backend and the OpenGL/OpenGL ES backends this file's own
// phase adds call one copy of this logic rather than two.
#pragma once

#include "core/types.h"
#include "render/backend.h"

#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
struct RenderPolygon;
}  // namespace sm2::hw

namespace sm2::render {

/// One triangle-list vertex. Twenty-four bytes: where it is, what part of the
/// texture it names, how far away it is, and which polygon it belongs to.
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

/// Everything the fragment stage needs to know about one polygon. Must match
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
    kMaxLevelShift   = 8,
    kFlagTranslucent = 1u << 12,
};

/// A backend-neutral screen-space rectangle: a Vulkan `VkRect2D`'s twin, so a
/// backend converts to its own API's type in one line at the call site instead
/// of this header naming one.
struct ScissorRect {
    s32 x = 0;
    s32 y = 0;
    u32 width  = 0;
    u32 height = 0;
};

/// A run of triangles sharing one scissor rectangle.
///
/// The scissor is the polygon's priority window, which can be smaller than the
/// screen. Polygons come grouped by window, so a frame needs only a handful of
/// these however many polygons it holds.
struct Batch {
    ScissorRect scissor;
    u32         first_vertex  = 0;
    u32         vertex_count  = 0;
};

/// One frame's worth of triangulated geometry, ready for a backend to upload.
struct TriangulatedFrame {
    std::vector<Vertex>     vertices;
    std::vector<PolyParams> polygons;
    std::vector<Batch>      batches;

    /// Polygons that produced at least one triangle.
    u32 drawn_polygons = 0;

    /// Polygons on the one pixel path that draws nothing at all: untextured
    /// and translucent. Not a gap, but worth counting, because a game asking
    /// for many of them would mean the path selection is being read wrongly.
    u32 blank_polygons = 0;
};

/// Room for far more geometry than the hardware could draw in a frame. Virtua
/// Fighter 2 in a match submits about six thousand vertices; the geometry
/// engine's pool would allow ninety times this in the worst case, but a real
/// board had nowhere near the fill rate to reach it.
constexpr u32 kMaxVertices = 1 << 17;

/// Polygons whose parameters can be held at once, which bounds the same thing
/// from the other direction.
constexpr u32 kMaxPolygons = 1 << 15;

/// Unpack one polygon's texture header into the fragment stage's parameters.
///
/// `video` supplies the colour and tone-curve lookups this polygon's header
/// selects; nothing here touches a graphics API.
[[nodiscard]] PolyParams describe_polygon(const hw::RenderPolygon& poly,
                                          const hw::Model2Video&   video);

/// Deepest mipmap level for a texture of these dimensions. MAME derives it
/// from the smaller side and stops at two by two.
[[nodiscard]] u32 deepest_mip_level(u32 width, u32 height);

/// Triangulate this frame's render list: fan every polygon from its first
/// vertex (the clipper only ever produces convex polygons, so this cannot fold
/// over itself), unpack each polygon's texture header, and batch runs of
/// triangles that share a scissor rectangle.
///
/// `machine` may be null, in which case the result is empty (the idle
/// bring-up display has nothing to draw). Polygons that are both untextured
/// and translucent are counted in `blank_polygons` and skipped outright:
/// `draw_scanline_solid` in MAME's model2rd.ipp returns immediately for that
/// combination, because with no texel there is nothing to alpha-test. The
/// *textured* translucent path does draw and must not be culled here — Virtua
/// Fighter 2's sky is 181 textured translucent polygons.
///
/// A frame with more geometry than kMaxVertices/kMaxPolygons hold is
/// truncated; the caller is responsible for warning about that once rather
/// than every frame; `warned` is set true the first time this happens across
/// calls sharing it, so a caller can log only on the transition to true.
[[nodiscard]] TriangulatedFrame triangulate(const hw::Model2MachineBase* machine,
                                            const hw::Model2Video&       video,
                                            bool*                        warned);

/// A backend-neutral rectangle in window pixels: a Vulkan `VkViewport`'s twin.
struct Letterbox {
    float x = 0.0F;
    float y = 0.0F;
    float width  = 0.0F;
    float height = 0.0F;
};

/// Largest 4:3 rectangle centred in a `window_width` by `window_height` target.
[[nodiscard]] Letterbox compute_letterbox(u32 window_width, u32 window_height);

}  // namespace sm2::render
