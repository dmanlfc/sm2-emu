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
// The present-stage magnification of the finished native frame onto the window,
// with the user's scaling method and the optional CRT filter.
//
// The input is the already-composited opaque frame. The sampler is nearest for
// Nearest/Integer and linear for Bilinear/SharpBilinear.

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 fragColour;

layout(set = 0, binding = 0) uniform sampler2D uLayer;

// GL has no push constants, so it takes a uniform block at a free binding.
#ifdef SM2_TARGET_GL
layout(binding = 1) uniform Present {
#else
layout(push_constant) uniform Present {
#endif
    // Native source size and destination rectangle size in window pixels.
    vec2 source_size;
    vec2 target_size;
    // 0 nearest, 1 bilinear, 2 sharp-bilinear, 3 integer (sm2::ScalingMethod).
    uint method;
    // CRT filter strengths, 0..1; curvature 0 is flat.
    uint  crt_enabled;
    float crt_scanline;
    float crt_mask;
    float crt_glow;
    float crt_curvature;
    // Model 3 picture brightness; 1 is the hardware's own. Model 1 and 2 apply
    // it to their colour ramp instead.
    float gain;
} pc;

const uint kSharpBilinear = 2u;

#ifdef SM2_TARGET_GL
const bool kCrtVariant = true;
#else
// A pipeline without the CRT block keeps the plain path small enough to run at
// the full thread count on tilers; the CRT code costs V3D half its threads.
layout(constant_id = 0) const bool kCrtVariant = true;
#endif

// Sample coordinate for the current scaling method (before any CRT warp).
vec2 sample_uv(vec2 uv)
{
    if (pc.method == kSharpBilinear) {
        // Hold the texel centre across each magnified texel and interpolate
        // only in the pixel straddling a boundary: sharp, without the shimmer
        // of nearest at non-integer scales.
        vec2 scale  = pc.target_size / pc.source_size;   // >= 1 in practice
        vec2 src    = uv * pc.source_size;               // texel space
        vec2 centre = floor(src) + 0.5;
        vec2 offset = clamp((src - centre) * scale, -0.5, 0.5);
        uv = (centre + offset) / pc.source_size;
    }
    return uv;
}

// Barrel warp of the [0,1] frame coordinate; amount 0 is identity.
vec2 curve(vec2 uv, float amount)
{
    vec2 c = uv * 2.0 - 1.0;                 // -1..1, centre origin
    c *= 1.0 + amount * dot(c.yx, c.yx) * 0.25;
    return c * 0.5 + 0.5;
}

void main()
{
    vec2 frame = vTexCoord;

    if (!kCrtVariant || pc.crt_enabled == 0u) {
        fragColour = vec4(texture(uLayer, sample_uv(frame)).rgb * pc.gain, 1.0);
        return;
    }

    // Outside the warped frame is black rather than a smeared border texel.
    if (pc.crt_curvature > 0.0) {
        frame = curve(frame, pc.crt_curvature);
        if (frame.x < 0.0 || frame.x > 1.0 || frame.y < 0.0 || frame.y > 1.0) {
            fragColour = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }

    vec3 colour = texture(uLayer, sample_uv(frame)).rgb;

    // Glow: a 4-tap box of one native texel radius.
    if (pc.crt_glow > 0.0) {
        vec2 r = 1.0 / pc.source_size;
        vec3 blur = texture(uLayer, sample_uv(frame) + vec2( r.x, 0.0)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(-r.x, 0.0)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(0.0,  r.y)).rgb
                  + texture(uLayer, sample_uv(frame) + vec2(0.0, -r.y)).rgb;
        colour = mix(colour, max(colour, blur * 0.25), pc.crt_glow);
    }

    // Scanlines follow native rows, not window pixels.
    if (pc.crt_scanline > 0.0) {
        float line = frame.y * pc.source_size.y;
        float s    = 0.5 + 0.5 * cos(6.2831853 * line);
        colour *= 1.0 - pc.crt_scanline * s;
    }

    // Aperture-grille mask follows window pixel columns.
    if (pc.crt_mask > 0.0) {
        float col = frame.x * pc.target_size.x;
        int   phase = int(mod(col, 3.0));
        vec3  tint = phase == 0 ? vec3(1.0, 0.7, 0.7)
                   : phase == 1 ? vec3(0.7, 1.0, 0.7)
                                : vec3(0.7, 0.7, 1.0);
        colour *= mix(vec3(1.0), tint, pc.crt_mask);
    }

    fragColour = vec4(colour * pc.gain, 1.0);
}
