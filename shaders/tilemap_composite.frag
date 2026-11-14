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

// GL has no push constants, so it takes a uniform block at a free binding.
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
    // 0 plain, 1 xBR, 2 ScaleFX; modes 0 and 1 only.
    uint upscale;
    vec2 source_size;
} pc;

// Edge-directed pixel-art upscaling of the 2D tilemap layer.
//
// Own implementation of the published xBR (Hyllian) and ScaleFX edge rules. As
// the surfaces are premultiplied, edge distance uses un-premultiplied colour so
// transparent texels do not read as black and leave a dark halo.

// Colour distance in a perceptual-ish YUV space, the standard xBR metric.
float colourDist(vec4 a, vec4 b)
{
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

// One edge-directed sample; kind 1 is xBR, 2 ScaleFX. The sub-texel position
// selects the quadrant of the source texel being reconstructed.
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

    // The quadrant's corner and the two neighbours bracketing its diagonal.
    vec4 corner = (f.x < 0.5)
                    ? ((f.y < 0.5) ? A : G)
                    : ((f.y < 0.5) ? C : I);
    vec4 hn = (f.x < 0.5) ? D : F;   // horizontal neighbour toward the corner
    vec4 vn = (f.y < 0.5) ? B : H;   // vertical neighbour toward the corner

    // Bend toward the neighbours when their diagonal matches better than the
    // one through the centre and the corner.
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

    // A tie keeps the centre texel, so flat areas are untouched; the bend is
    // strongest at the diagonal.
    float w = clamp((noedge - edge) * 0.5, 0.0, 1.0);
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
        // Force alpha to one so a compositing window system cannot show the
        // frame as transparent.
        fragColour = vec4(layer.rgb, 1.0);
    }
}
