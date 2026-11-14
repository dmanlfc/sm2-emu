#version 450
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
// Positions a Model 2 polygon vertex and sets up perspective-correct texturing.
//
// The geometry engine has already transformed, projected and clipped, so this
// receives a raster-space position, its view depth and the raw texture point.
// The hardware interpolates u/z, v/z and 1/z linearly and divides per pixel,
// which is what a GPU does when clip-space w is the view depth, so the depth
// goes in w and the position is pre-multiplied by it. 1/w reaches the fragment
// stage as gl_FragCoord.w for the level-of-detail calculation.

layout(location = 0) in vec2  inPosition;  // raster space: 0..496 by 0..384
layout(location = 1) in vec2  inTexel;     // texture point, in texels
layout(location = 2) in float inDepth;     // view depth, always positive
layout(location = 3) in uint  inPolygon;   // index into the parameter buffer

layout(location = 0) out vec2      vTexel;
layout(location = 1) out flat uint vPolygon;

// GL/GLES have no push constants, so they use a small uniform block instead;
// see fullscreen_quad.vert.
#ifdef SM2_TARGET_GL
layout(binding = 4) uniform Push {
#else
layout(push_constant) uniform Push {
#endif
    // Reciprocal of the raster size, so the divide happens once per frame on the
    // host rather than twice per vertex here.
    vec2 invRaster;
    // Render scale (1 at native) and anisotropic filter quality, read only by
    // polygon.frag but part of the shared layout.
    uint renderScale;
    uint textureQuality;
} pc;

void main()
{
    vec2 ndc = inPosition * pc.invRaster * 2.0 - 1.0;

    // Clipping already discarded everything behind the eye, so the depth is
    // positive; the floor only guards against a denormal reaching the divide.
    float w = max(inDepth, 1.0e-6);

    // Depth is the polygon's place in the draw order, nearest first. Only the
    // blended translucency pass tests it; the fill mask orders everything else.
    float order = float(inPolygon + 1u) / 32769.0;
#ifdef SM2_TARGET_GL
    order = order * 2.0 - 1.0;
#endif
    gl_Position = vec4(ndc * w, order * w, w);
    vTexel      = inTexel;
    vPolygon    = inPolygon;
}
