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
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The texture-header decoding reproduced here follows MAME's
// src/mame/sega/model2_v.cpp and model2rd.ipp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
#include "render/geometry.h"

#include "core/log.h"
#include "hw/geometrizer.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"
#include "render/texture_replace.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace sm2::render {
namespace {

/// Texture header bits that select the pixel path. MAME reads bits 14 and 13
/// together as a renderer index: bit 14 chooses textured over solid and bit 13
/// chooses translucent over opaque.
constexpr u32 kHeaderTextured    = 1u << 14;
constexpr u32 kHeaderTranslucent = 1u << 13;
constexpr u32 kHeaderChecker     = 1u << 15;

}  // namespace

u32 deepest_mip_level(u32 width, u32 height)
{
    u32 smaller = std::min(width, height);
    u32 level   = 0;
    while (smaller > 2 && level < 15) {
        smaller >>= 1;
        ++level;
    }
    return level;
}

PolyParams describe_polygon(const hw::RenderPolygon& poly, const hw::Model2Video& video)
{
    PolyParams params{};

    if ((poly.direct_rgb & 0x80000000u) != 0) {
        const u32 rgb = poly.direct_rgb;
        params.colour = ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16);
        params.flags  = kFlagDirect;
        if ((poly.texheader[0] & kHeaderChecker) != 0) {
            params.flags |= kFlagChecker;
        }
        return params;
    }

    // Ten bits of colour base into the 3D half of palette RAM, kept as
    // components because shading is per pixel.
    params.colour     = video.polygon_colour_components((poly.texheader[3] >> 6) & 0x3ff);
    params.luma_base  = static_cast<u32>(poly.texheader[1] & 0xff) << 7;
    params.luma_scale = poly.luma;
    params.tex_lod    = poly.texlod;

    if ((poly.texheader[0] & kHeaderChecker) != 0) {
        params.flags |= kFlagChecker;
    }
    if ((poly.texheader[0] & kHeaderTranslucent) != 0) {
        params.flags |= kFlagTranslucent;
    }
    if ((poly.texheader[0] & kHeaderTextured) == 0) {
        return params;
    }
    params.flags |= kFlagTextured;

    // Dimensions are powers of two from 32 up, three bits each.
    params.tex_width  = 32u << (poly.texheader[0] & 0x7);
    params.tex_height = 32u << ((poly.texheader[0] >> 3) & 0x7);
    params.tex_x      = 32u * (poly.texheader[2] & 0x3f);
    params.tex_y      = 32u * ((poly.texheader[2] >> 6) & 0x1f);

    const bool mirror_x = ((poly.texheader[0] >> 8) & 1) != 0;
    const bool mirror_y = ((poly.texheader[0] >> 9) & 1) != 0;
    if (mirror_x) {
        params.flags |= kFlagMirrorX;
    }
    if (mirror_y) {
        params.flags |= kFlagMirrorY;
    }
    // Smooth wrapping and mirroring are mutually exclusive, as MAME masks them.
    if (((poly.texheader[0] >> 6) & 1) != 0 && !mirror_x) {
        params.flags |= kFlagWrapX;
    }
    if (((poly.texheader[0] >> 7) & 1) != 0 && !mirror_y) {
        params.flags |= kFlagWrapY;
    }
    if ((poly.texheader[2] & 0x1000) != 0) {
        params.flags |= kFlagSheet;
    }
    if (((poly.texheader[0] >> 12) & 1) != 0) {
        params.flags |= kFlagMicro;
    }

    params.micro_min_lod = (poly.texheader[0] >> 10) & 3;
    params.micro_x       = ((poly.texheader[2] >> 13) & 1) * 128;
    params.micro_y       = ((poly.texheader[2] >> 14) & 3) * 128;

    params.flags |= deepest_mip_level(params.tex_width, params.tex_height) << kMaxLevelShift;
    return params;
}

TriangulatedFrame triangulate(const hw::Model2MachineBase* machine,
                              const hw::Model2Video&       video,
                              bool*                        warned,
                              TextureReplacements*         replacements,
                              bool                         blend_translucency)
{
    TriangulatedFrame frame;
    if (machine == nullptr) {
        return frame;
    }
    if (replacements != nullptr && replacements->empty()) {
        replacements = nullptr;
    }
    if (replacements != nullptr) {
        replacements->begin_frame(*machine, video);
    }

    // Blended polygons are held back and appended after everything else in
    // reverse, so each blends over what is behind it.
    struct Deferred {
        ScissorRect scissor;
        u32         first_vertex = 0;
        u32         vertex_count = 0;
    };
    std::vector<Vertex>   deferred_vertices;
    std::vector<Deferred> deferred;

    for (const hw::RenderPolygon& poly : machine->render_list().polygons) {
        // Untextured translucent polygons draw nothing (model2rd.ipp's
        // draw_scanline_solid returns early). Textured translucent ones do
        // draw, alpha-testing each texel, so they must not be culled
        // (Virtua Fighter 2's sky).
        if ((poly.texheader[0] & kHeaderTranslucent) != 0
            && (poly.texheader[0] & kHeaderTextured) == 0) {
            ++frame.blank_polygons;
            continue;
        }
        if (poly.num_vertices < 3) {
            continue;
        }

        const ScissorRect scissor{
            poly.scissor[0], poly.scissor[1],
            static_cast<u32>(poly.scissor[2] - poly.scissor[0]),
            static_cast<u32>(poly.scissor[3] - poly.scissor[1])};
        if (scissor.width == 0 || scissor.height == 0) {
            continue;
        }

        const u32 triangles = static_cast<u32>(poly.num_vertices) - 2u;
        if (frame.vertices.size() + deferred_vertices.size() + triangles * 3u > kMaxVertices
            || frame.polygons.size() >= kMaxPolygons) {
            if (warned != nullptr && !*warned) {
                *warned = true;
                SM2_WARN("3d: more geometry than the buffers hold (%u vertices, %u "
                         "polygons); the rest of this frame is dropped",
                         kMaxVertices, kMaxPolygons);
            }
            break;
        }

        const u32        index  = static_cast<u32>(frame.polygons.size());
        PolyParams params = describe_polygon(poly, video);
        if (replacements != nullptr && (params.flags & kFlagTextured) != 0) {
            replacements->resolve(*machine, video, poly, &params);
        }
        const bool blended = blend_translucency && (params.flags & kFlagChecker) != 0;
        if (blended) {
            params.flags = (params.flags & ~kFlagChecker) | kFlagBlended;
        }
        frame.polygons.push_back(params);

        // The vertex ring for the fragment shader's edge walk (see PolyGeom).
        // No 1/8 on u/v: the shader's int(uv*32) carries the eighths.
        PolyGeom geom{};
        geom.num_vertices = poly.num_vertices;
        for (u32 corner = 0; corner < poly.num_vertices; ++corner) {
            const hw::PolyVertex& pv = poly.v[corner];
            const float           oz = 1.0F / (pv.p[0] + std::numeric_limits<float>::min());
            geom.x[corner]          = pv.x;
            geom.y[corner]          = pv.y;
            geom.one_over_z[corner] = oz;
            geom.u_over_z[corner]   = pv.p[1] * oz;
            geom.v_over_z[corner]   = pv.p[2] * oz;
        }
        frame.geometry.push_back(geom);

        // A fan. The clipper produces convex polygons, so fanning from the
        // first vertex cannot fold over itself.
        const auto emit_fan = [&](std::vector<Vertex>& out) {
            const auto emit = [&](u32 corner) {
                const hw::PolyVertex& v = poly.v[corner];
                out.push_back(Vertex{v.x, v.y, v.p[1], v.p[2], v.p[0], index});
            };
            for (u32 corner = 1; corner + 1 < poly.num_vertices; ++corner) {
                emit(0);
                emit(corner);
                emit(corner + 1);
            }
        };

        if (blended) {
            const u32 first = static_cast<u32>(deferred_vertices.size());
            emit_fan(deferred_vertices);
            deferred.push_back(
                Deferred{scissor, first, static_cast<u32>(deferred_vertices.size()) - first});
            ++frame.drawn_polygons;
            continue;
        }

        const bool can_early = (params.flags & (kFlagChecker | kFlagTranslucent)) == 0;

        // A new batch starts on a scissor or can_early change, so batches stay
        // in list order for the order-based fill mask.
        if (frame.batches.empty() || frame.batches.back().scissor.x != scissor.x
            || frame.batches.back().scissor.y != scissor.y
            || frame.batches.back().scissor.width != scissor.width
            || frame.batches.back().scissor.height != scissor.height
            || frame.batches.back().early != can_early) {
            frame.batches.push_back(
                Batch{scissor, static_cast<u32>(frame.vertices.size()), 0, can_early});
        }

        emit_fan(frame.vertices);

        frame.batches.back().vertex_count =
            static_cast<u32>(frame.vertices.size()) - frame.batches.back().first_vertex;
        ++frame.drawn_polygons;
    }

    for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
        const ScissorRect& scissor = it->scissor;
        if (frame.batches.empty() || !frame.batches.back().blended
            || frame.batches.back().scissor.x != scissor.x
            || frame.batches.back().scissor.y != scissor.y
            || frame.batches.back().scissor.width != scissor.width
            || frame.batches.back().scissor.height != scissor.height) {
            frame.batches.push_back(
                Batch{scissor, static_cast<u32>(frame.vertices.size()), 0, false, true});
        }
        const auto first = deferred_vertices.begin() + static_cast<std::ptrdiff_t>(it->first_vertex);
        frame.vertices.insert(frame.vertices.end(), first,
                              first + static_cast<std::ptrdiff_t>(it->vertex_count));
        frame.batches.back().vertex_count += it->vertex_count;
    }

    return frame;
}

Letterbox compute_letterbox(u32           window_width,
                            u32           window_height,
                            AspectMode    aspect,
                            ScalingMethod method,
                            PictureArea   area)
{
    const float width  = static_cast<float>(window_width) * area.width;
    const float height = static_cast<float>(window_height) * area.height;

    float target_width  = width;
    float target_height = height;

    if (aspect != AspectMode::Stretch) {
        // Fit the largest rectangle of the chosen aspect inside the window.
        const float ratio = aspect == AspectMode::SquarePixel
                                ? static_cast<float>(kNativeWidth)
                                      / static_cast<float>(kNativeHeight)
                            : aspect == AspectMode::Widescreen ? 16.0F / 9.0F
                                                               : kDisplayAspect;
        target_width  = width;
        target_height = width / ratio;
        if (target_height > height) {
            target_height = height;
            target_width  = height * ratio;
        }
    }

    // Integer snapping applies to the shaped fits only; stretch fills the
    // window.
    if (method == ScalingMethod::Integer && aspect != AspectMode::Stretch) {
        // Snap the height to a whole native multiple for clean scanlines and
        // take the width from the fitted aspect, accepting a non-integer
        // horizontal scale.
        const float fh     = static_cast<float>(kNativeHeight);
        u32         factor = static_cast<u32>(target_height / fh);
        factor             = std::max(factor, 1u);
        const float scanline_height = fh * static_cast<float>(factor);
        target_width  *= scanline_height / target_height;
        target_height  = scanline_height;
    }

    Letterbox box{};
    box.x      = static_cast<float>(window_width) * area.x + (width - target_width) * 0.5F;
    box.y      = static_cast<float>(window_height) * area.y + (height - target_height) * 0.5F;
    box.width  = target_width;
    box.height = target_height;
    return box;
}

Letterbox compute_game_rect(u32 window_width, u32 window_height, AspectMode aspect,
                            ScalingMethod method, PictureArea area)
{
    Letterbox box = compute_letterbox(window_width, window_height, aspect, method, area);
    if (aspect == AspectMode::Widescreen) {
        const float inner = box.width * static_cast<float>(kNativeWidth)
                          / static_cast<float>(frame_width(kWideColumns));
        box.x += (box.width - inner) * 0.5F;
        box.width = inner;
    }
    return box;
}

}  // namespace sm2::render
