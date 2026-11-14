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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DScrollFog.cpp,
// Copyright (c) 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The Real3D scroll / ambient fog quad. Pair with fullscreen_quad.vert.
#version 450

#ifdef SM2_TARGET_GL
layout(std140, binding = 3) uniform ScrollFogUniforms {
#else
layout(std140, set = 0, binding = 0) uniform ScrollFogUniforms {
#endif
    vec4  fogColour;            // .rgb colour, .a density
    vec4  spotEllipse;          // .xy centre, .zw half extents (GL lower-left convention)
    vec4  spotFogColor;         // .rgb used
    vec2  fragCoordYTransform;  // (-1, target height): flips gl_FragCoord.y back to GL's convention
    float fogAttenuation;
    float fogAmbient;
};

layout(location = 0) out vec4 out0;  // opaque
layout(location = 1) out vec4 out1;  // trans layer 1
layout(location = 2) out vec4 out2;  // trans layer 2
layout(location = 3) out uvec2 outLos;  // the clear value, when drawn inside a layer pass

vec2 ScreenFragCoord()
{
    return vec2(gl_FragCoord.x, fragCoordYTransform.y + fragCoordYTransform.x * gl_FragCoord.y);
}

void WriteOutputs(vec4 colour)
{
    vec4 blank = vec4(0.0);

    out0 = colour;
    out1 = blank;
    out2 = blank;
    outLos = uvec2(0u);
}

void main()
{
    vec3 lFogColor = fogColour.rgb * fogAmbient;

    vec2  position = spotEllipse.xy;
    vec2  size     = spotEllipse.zw;
    float ellipse  = length((ScreenFragCoord() - position) / size);
    ellipse = ellipse * ellipse;  // decays with the square of the distance from the centre
    ellipse = 1.0 - ellipse;
    ellipse = max(0.0, ellipse);

    vec3 lSpotFogColor = mix(vec3(0.0), spotFogColor.rgb * ellipse * fogColour.rgb, fogAttenuation);

    vec4 scrollFog = vec4(lFogColor + lSpotFogColor, fogColour.a);

    WriteOutputs(scrollFog);
}
