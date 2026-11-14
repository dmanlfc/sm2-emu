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
// One tile generator layer (512x384, 496 visible, or wider in widescreen)
// drawn at `rect`, blended.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uLayer;

#ifdef SM2_TARGET_GL
layout(std140, binding = 4) uniform Model3Layer {
#else
layout(push_constant) uniform Model3Layer {
#endif
    vec4 rect;      // x, y, width, height in target pixels
    vec2 uv_scale;  // visible fraction of the layer texture (496/512, 1)
    vec2 clip;      // uv.x range drawn
} pc;

layout(location = 0) out vec4 fragColour;

void main()
{
    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;
    if (uv.x < pc.clip.x || uv.y < 0.0 || uv.x >= pc.clip.y || uv.y >= 1.0) {
        discard;
    }
    fragColour = texture(uLayer, uv * pc.uv_scale);
}
