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
// The texture-header decoding reproduced here follows MAME's
// src/mame/sega/model2_v.cpp and model2rd.ipp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
#include "render/geometry.h"

#include "core/log.h"
#include "hw/geometrizer.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <algorithm>

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

    // Ten bits of colour base against the 3D half of palette RAM. The
    // components rather than a resolved colour, because the shade is a
    // per-pixel matter.
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
    // Smooth wrapping and mirroring are mutually exclusive; MAME masks the one
    // out when the other is set rather than letting both apply.
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
                              bool*                        warned)
{
    TriangulatedFrame frame;
    if (machine == nullptr) {
        return frame;
    }

    for (const hw::RenderPolygon& poly : machine->render_list().polygons) {
        // Untextured and translucent is the one combination the hardware
        // draws nothing for: `draw_scanline_solid` in model2rd.ipp begins
        // with `if (Translucent) return;` because with no texel there is
        // nothing to alpha-test. The *textured* translucent path does draw —
        // it alpha-tests each filtered texel and skips the ones below half —
        // so it must not be culled here. Virtua Fighter 2 draws its sky with
        // 181 textured translucent polygons, and culling them leaves a flat
        // band where the sky should be.
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
        if (frame.vertices.size() + triangles * 3u > kMaxVertices
            || frame.polygons.size() >= kMaxPolygons) {
            if (warned != nullptr && !*warned) {
                *warned = true;
                SM2_WARN("3d: more geometry than the buffers hold (%u vertices, %u "
                         "polygons); the rest of this frame is dropped",
                         kMaxVertices, kMaxPolygons);
            }
            break;
        }

        const u32 index = static_cast<u32>(frame.polygons.size());
        frame.polygons.push_back(describe_polygon(poly, video));

        // Polygons come grouped by priority window and the window is what
        // sets the viewport, so a run of them almost always shares a
        // scissor.
        if (frame.batches.empty() || frame.batches.back().scissor.x != scissor.x
            || frame.batches.back().scissor.y != scissor.y
            || frame.batches.back().scissor.width != scissor.width
            || frame.batches.back().scissor.height != scissor.height) {
            frame.batches.push_back(
                Batch{scissor, static_cast<u32>(frame.vertices.size()), 0});
        }

        // A fan. The clipper produces convex polygons, so fanning from the
        // first vertex cannot fold over itself.
        const auto emit = [&](u32 corner) {
            const hw::PolyVertex& v = poly.v[corner];
            frame.vertices.push_back(Vertex{v.x, v.y, v.p[1], v.p[2], v.p[0], index});
        };
        for (u32 corner = 1; corner + 1 < poly.num_vertices; ++corner) {
            emit(0);
            emit(corner);
            emit(corner + 1);
        }

        frame.batches.back().vertex_count =
            static_cast<u32>(frame.vertices.size()) - frame.batches.back().first_vertex;
        ++frame.drawn_polygons;
    }

    return frame;
}

Letterbox compute_letterbox(u32 window_width, u32 window_height)
{
    const float width  = static_cast<float>(window_width);
    const float height = static_cast<float>(window_height);

    float target_width  = width;
    float target_height = width / kDisplayAspect;
    if (target_height > height) {
        target_height = height;
        target_width  = height * kDisplayAspect;
    }

    Letterbox box{};
    box.x      = (width - target_width) * 0.5F;
    box.y      = (height - target_height) * 0.5F;
    box.width  = target_width;
    box.height = target_height;
    return box;
}

}  // namespace sm2::render
