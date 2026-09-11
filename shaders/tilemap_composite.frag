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
// Composites one Model 2 tilemap result over what is already in the target.
//
// The emulator rasterises the System 24 layers on the CPU into two RGBA8
// surfaces: the layers whose priority category is zero, which belong behind the
// 3D output, and the layers whose category is one, which belong in front of it.
// This shader draws either of them.
//
// Colours arrive premultiplied by alpha: a pixel nothing wrote is all zeroes, a
// drawn pixel is its colour with alpha one. That matters because the surface is
// magnified with linear filtering, and filtering a straight-alpha edge pulls the
// colour towards black and leaves a dark fringe around every glyph.

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 fragColour;

layout(set = 0, binding = 0) uniform sampler2D uLayer;

// See polygon.vert's SM2_TARGET_GL comment: GL/GLES have no push-constant
// concept, so the GL compile takes a small uniform block instead, at a
// binding this shader has room for (0 is uLayer); the Vulkan compile keeps
// the push constant it already had, unchanged.
#ifdef SM2_TARGET_GL
layout(binding = 1) uniform Push {
#else
layout(push_constant) uniform Push {
#endif
    // Palette entry zero. The hardware shows this wherever nothing is drawn.
    vec4 background;
    // 0: resolve against the background, replacing the target.
    // 1: blend over the target, which by then holds the 3D output.
    // 2: copy, for putting the finished native frame on screen.
    uint mode;
    // 2D upscale: 0 faithful (plain sample), 1 xBR, 2 ScaleFX. Only applies to
    // the tilemap layers (modes 0/1), not the mode-2 copy. source_size is the
    // native surface size for texelFetch.
    uint upscale;
    vec2 source_size;
} pc;

// Edge-directed pixel-art upscaling of the 2D tilemap layer.
//
// Own implementation of the published xBR (Hyllian) and ScaleFX edge rules: at
// each output position, look at the source texel neighbourhood, measure colour
// distance across the two diagonals, and bend the sampled colour toward the
// stronger edge so a stairstepped diagonal reads as a clean slope. The tilemap
// surfaces are PREMULTIPLIED (a transparent texel is all zeros), so the edge
// distance is measured on un-premultiplied colour and a fully transparent texel
// is treated as "no contribution" rather than black, which would otherwise pull
// a dark halo along every glyph edge (the same trap polygon.frag solves for 3D
// cutouts).

// Colour distance in a perceptual-ish YUV space, the standard xBR metric.
float colourDist(vec4 a, vec4 b)
{
    // Compare on un-premultiplied colour; a zero-alpha texel contributes only
    // its alpha difference, never a spurious black.
    vec3 ca = a.a > 0.0 ? a.rgb / a.a : vec3(0.0);
    vec3 cb = b.a > 0.0 ? b.rgb / b.a : vec3(0.0);
    vec3 d  = ca - cb;
    const vec3 w = vec3(0.299, 0.587, 0.114);
    return abs(dot(d, w)) * 48.0 + abs(a.a - b.a) * 8.0;
}

vec4 fetchSrc(ivec2 p)
{
    ivec2 m = clamp(p, ivec2(0), ivec2(pc.source_size) - 1);
    return texelFetch(uLayer, m, 0);
}

// One edge-directed output sample. `kind` 1 = xBR, 2 = ScaleFX (a wider
// neighbourhood test, so it resolves thin features xBR misses). The sub-texel
// position `f` in [0,1) selects which output quadrant of the source texel we
// are reconstructing.
vec4 edgeSample(vec2 uv, uint kind)
{
    vec2  src   = uv * pc.source_size - 0.5;
    ivec2 base  = ivec2(floor(src));
    vec2  f     = src - vec2(base);

    // The 3x3 core, named as the compass around the centre texel E.
    vec4 A = fetchSrc(base + ivec2(-1, -1));
    vec4 B = fetchSrc(base + ivec2( 0, -1));
    vec4 C = fetchSrc(base + ivec2( 1, -1));
    vec4 D = fetchSrc(base + ivec2(-1,  0));
    vec4 E = fetchSrc(base + ivec2( 0,  0));
    vec4 F = fetchSrc(base + ivec2( 1,  0));
    vec4 G = fetchSrc(base + ivec2(-1,  1));
    vec4 H = fetchSrc(base + ivec2( 0,  1));
    vec4 I = fetchSrc(base + ivec2( 1,  1));

    // Pick the near corner of the quadrant `f` falls in, and the two edge
    // neighbours that bracket that corner's diagonal.
    vec4 corner = (f.x < 0.5)
                    ? ((f.y < 0.5) ? A : G)
                    : ((f.y < 0.5) ? C : I);
    vec4 hn = (f.x < 0.5) ? D : F;   // horizontal neighbour toward the corner
    vec4 vn = (f.y < 0.5) ? B : H;   // vertical neighbour toward the corner

    // The xBR decision: if the diagonal through the two edge neighbours is a
    // stronger match than the anti-diagonal through the centre and the corner,
    // the corner belongs to the edge and the output bends toward the neighbours.
    float edge   = colourDist(hn, vn);
    float noedge = colourDist(E, corner);

    // ScaleFX widens the test with the second ring so a one-pixel line is not
    // mistaken for an edge to be rounded off.
    if (kind == 2u) {
        vec4 hn2 = (f.x < 0.5) ? fetchSrc(base + ivec2(-2, 0))
                               : fetchSrc(base + ivec2( 2, 0));
        vec4 vn2 = (f.y < 0.5) ? fetchSrc(base + ivec2(0, -2))
                               : fetchSrc(base + ivec2(0,  2));
        noedge += 0.5 * (colourDist(hn, hn2) + colourDist(vn, vn2));
    }

    // How far to blend toward the edge, from how decisively the edge test wins.
    // A tie leaves the centre texel (faithful), so flat areas are untouched.
    float w = clamp((noedge - edge) * 0.5, 0.0, 1.0);
    // Distance of the sample point from the corner within the quadrant scales
    // the effect: the bend is strongest right at the diagonal.
    float d = 1.0 - min(1.0, length(f - step(0.5, f)) * 2.0);
    vec4  edgeColour = 0.5 * (hn + vn);
    return mix(E, edgeColour, w * d);
}

void main()
{
    vec4 layer;
    if (pc.upscale != 0u && pc.mode != 2u) {
        layer = edgeSample(vTexCoord, pc.upscale);
    } else {
        layer = texture(uLayer, vTexCoord);
    }

    if (pc.mode == 0u) {
        fragColour = vec4(pc.background.rgb * (1.0 - layer.a) + layer.rgb, 1.0);
    } else if (pc.mode == 1u) {
        fragColour = layer;
    } else {
        // The source is already a finished opaque frame. Forcing alpha to one
        // rather than passing it through means a stray zero in the alpha channel
        // cannot make the presented image transparent on a compositing window
        // system.
        fragColour = vec4(layer.rgb, 1.0);
    }
}
