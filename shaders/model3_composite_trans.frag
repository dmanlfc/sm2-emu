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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DFrameBuffers.cpp,
// Copyright (c) 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The Real3D translucency layers, blended over the frame. Pair with fullscreen_quad.vert.
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex1;  // trans layer 1
layout(set = 0, binding = 1) uniform sampler2D tex2;  // trans layer 2

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec2 tc = ivec2(gl_FragCoord.xy);
    vec4 colTrans1 = texelFetch(tex1, tc, 0);
    vec4 colTrans2 = texelFetch(tex2, tc, 0);

    // Where both translucency layers overlap, the result is opaque.
    if (colTrans1.a * colTrans2.a > 0.0) {
        vec3 mixCol = mix(colTrans1.rgb, colTrans2.rgb, (colTrans2.a + (1.0 - colTrans1.a)) / 2.0);
        fragColor = vec4(mixCol, 1.0);
    }
    else if (colTrans1.a > 0.0) {
        fragColor = colTrans1;
    }
    else {
        fragColor = colTrans2;  // zero alpha has no effect anyway
    }
}
