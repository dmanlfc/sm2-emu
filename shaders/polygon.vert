#version 450
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
// Positions a Model 2 polygon vertex and sets up perspective-correct texturing.
//
// The geometry engine has already done everything a vertex shader would normally
// do: transform, project and clip. What arrives here is a screen-space position in
// the raster's own coordinates, its view depth, and the raw texture point the
// polygon ROM supplied for it.
//
// The one piece of arithmetic left is what makes texturing perspective-correct.
// The hardware interpolates u/z, v/z and 1/z linearly across a scanline and
// divides at every pixel. That is exactly what a GPU does to any varying when the
// clip-space w is the view depth, so this shader puts the depth in w and
// pre-multiplies the position by it. The divide the rasteriser performs then
// cancels the multiplication for the position and supplies it for the texture
// coordinates, and 1/w arrives in the fragment stage as gl_FragCoord.w for the
// level-of-detail calculation to use.
//
// Depth itself is written as zero. There is no depth buffer to write to: which
// polygon owns a pixel is decided by the stencil fill mask instead.

layout(location = 0) in vec2  inPosition;  // raster space: 0..496 by 0..384
layout(location = 1) in vec2  inTexel;     // texture point, in texels
layout(location = 2) in float inDepth;     // view depth, always positive
layout(location = 3) in uint  inPolygon;   // index into the parameter buffer

layout(location = 0) out vec2      vTexel;
layout(location = 1) out flat uint vPolygon;

// Push constants need no backing buffer or descriptor under Vulkan, but GL/GLES
// have no push-constant concept at all -- their nearest equivalent is a small
// uniform block, which does need one. Branching here rather than adding a real
// GL-only descriptor binding to the Vulkan pipeline keeps the working Vulkan
// path untouched; see fullscreen_quad.vert's SM2_TARGET_GL define, set the same
// way for the same reason.
#ifdef SM2_TARGET_GL
layout(binding = 4) uniform Push {
#else
layout(push_constant) uniform Push {
#endif
    // Reciprocal of the raster size, so the divide happens once per frame on the
    // host rather than twice per vertex here.
    vec2 invRaster;
} pc;

void main()
{
    const vec2 ndc = inPosition * pc.invRaster * 2.0 - 1.0;

    // Clipping already discarded everything behind the eye, so the depth is
    // positive; the floor only guards against a denormal reaching the divide.
    const float w = max(inDepth, 1.0e-6);

    gl_Position = vec4(ndc * w, 0.0, w);
    vTexel      = inTexel;
    vPolygon    = inPolygon;
}
