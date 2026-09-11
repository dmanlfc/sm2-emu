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
// The present-stage magnification of the finished native frame onto the window,
// with the user's scaling method and the optional CRT filter.
//
// Split out from tilemap_composite.frag, which still does the pre-magnification
// three-way composite; this shader only ever runs on the one already-composited
// opaque frame. The sampler bound alongside it is nearest for Nearest/Integer
// and linear for Bilinear/SharpBilinear.

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 fragColour;

layout(set = 0, binding = 0) uniform sampler2D uLayer;

// GL/GLES have no push constants; the GL compile takes a uniform block at a
// free binding (0 is uLayer) exactly as tilemap_composite.frag does, while the
// Vulkan compile keeps a push constant.
#ifdef SM2_TARGET_GL
layout(binding = 1) uniform Present {
#else
layout(push_constant) uniform Present {
#endif
    // The native source size (496x384) and the destination rectangle size in
    // window pixels, both needed for the sharp-bilinear texel snap.
    vec2 source_size;
    vec2 target_size;
    // 0 nearest, 1 bilinear, 2 sharp-bilinear, 3 integer. Matches
    // sm2::ScalingMethod. Nearest/Bilinear/Integer take the plain path (the
    // sampler does the work); only SharpBilinear needs the snap below.
    uint method;
    // CRT cosmetic filter. crt_enabled 0 skips the whole block (zero cost);
    // the four strengths are 0..1 (the config's 0..100 divided by 100), and
    // curvature 0 is flat.
    uint  crt_enabled;
    float crt_scanline;
    float crt_mask;
    float crt_glow;
    float crt_curvature;
} pc;

const uint kSharpBilinear = 2u;

// Sample coordinate for the current scaling method (before any CRT warp).
vec2 sample_uv(vec2 uv)
{
    if (pc.method == kSharpBilinear) {
        // Sharp-bilinear: keep the sample at the source texel centre across the
        // flat interior of each magnified texel, and let the linear sampler
        // interpolate only within the one destination pixel that straddles a
        // texel boundary. Sharp like nearest, but without the shimmer nearest
        // shows when the magnification is not a whole number.
        vec2 scale  = pc.target_size / pc.source_size;   // >= 1 in practice
        vec2 src    = uv * pc.source_size;               // texel space
        vec2 centre = floor(src) + 0.5;
        vec2 offset = clamp((src - centre) * scale, -0.5, 0.5);
        uv = (centre + offset) / pc.source_size;
    }
    return uv;
}

// A gentle barrel warp of the [0,1] frame coordinate; amount 0 is identity.
// Returns the warped coordinate; the caller blacks out anything that leaves
// the frame so the curved edges do not smear the border texel.
vec2 curve(vec2 uv, float amount)
{
    vec2 c = uv * 2.0 - 1.0;                 // -1..1, centre origin
    c *= 1.0 + amount * dot(c.yx, c.yx) * 0.25;
    return c * 0.5 + 0.5;
}

void main()
{
    vec2 frame = vTexCoord;

    if (pc.crt_enabled == 0u) {
        // Plain present path -- no added work when the filter is off.
        fragColour = vec4(texture(uLayer, sample_uv(frame)).rgb, 1.0);
        return;
    }

    // Curvature warps the frame coordinate; outside the frame is the black
    // border a real tube shows past the phosphor.
    if (pc.crt_curvature > 0.0) {
        frame = curve(frame, pc.crt_curvature);
        if (frame.x < 0.0 || frame.x > 1.0 || frame.y < 0.0 || frame.y > 1.0) {
            fragColour = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }

    vec3 colour = texture(uLayer, sample_uv(frame)).rgb;

    // Glow: a cheap 4-tap box around the sample, mixed back in, for the soft
    // bloom a tube's beam spreads. Radius is one native texel.
    if (pc.crt_glow > 0.0) {
        vec2 r = 1.0 / pc.source_size;
        vec3 blur = texture(uLayer, sample_uv(frame) + vec2( r.x, 0.0)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(-r.x, 0.0)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(0.0,  r.y)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(0.0, -r.y)).rgb;
        colour = mix(colour, max(colour, blur * 0.25), pc.crt_glow);
    }

    // Scanlines, keyed to the native raster rows (not window pixels) so they
    // track the emulated 384-line image at any window scale. A raised-cosine
    // darkening between lines; crt_scanline sets the depth.
    if (pc.crt_scanline > 0.0) {
        float line = frame.y * pc.source_size.y;
        float s    = 0.5 + 0.5 * cos(6.2831853 * line);
        colour *= 1.0 - pc.crt_scanline * s;
    }

    // Aperture-grille mask, keyed to the destination pixel column so the RGB
    // stripes are a fixed physical size on screen. Every third column favours
    // one channel; crt_mask sets how strongly.
    if (pc.crt_mask > 0.0) {
        float col = frame.x * pc.target_size.x;
        int   phase = int(mod(col, 3.0));
        vec3  tint = phase == 0 ? vec3(1.0, 0.7, 0.7)
                   : phase == 1 ? vec3(0.7, 1.0, 0.7)
                                : vec3(0.7, 0.7, 1.0);
        colour *= mix(vec3(1.0), tint, pc.crt_mask);
    }

    fragColour = vec4(colour, 1.0);
}
