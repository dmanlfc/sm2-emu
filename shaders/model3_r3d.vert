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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DShaderTriangles.h,
// Copyright (c) 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Real3D vertex shader. Model block: push constants on Vulkan, UBO on GL;
// GL negates clip Y. R3DUniforms matches render/model3/frame_plan.h (std140).
#version 450

layout(std140, set = 0, binding = 0) uniform R3DUniforms {
    mat4  projMat;
    vec4  fogColour;
    vec4  spotEllipse;
    vec4  spotColor;
    vec4  spotFogColor;
    vec4  lighting[2];
    ivec4 baseTexInfo;
    vec2  spotRange;
    ivec2 textureWrapMode;
    vec2  fragCoordYTransform;
    float cota;
    float fogIntensity;
    float fogDensity;
    float fogStart;
    float fogAttenuation;
    float fogAmbient;
    float specularValue;
    float shininess;
    float microTextureMinLOD;
    int   baseTexType;
    int   microTextureID;
    int   texturePage;
    int   hardwareStep;
    int   colourLayer;
    uint  b_textureEnabled;
    uint  b_microTexture;
    uint  b_textureInverted;
    uint  b_textureAlpha;
    uint  b_alphaTest;
    uint  b_discardAlpha;
    uint  b_lightEnabled;
    uint  b_sunClamp;
    uint  b_intensityClamp;
    uint  b_specularEnabled;
    uint  b_fixedShading;
    uint  b_smoothShading;
    uint  b_polyAlpha;
    uint  b_noLosReturn;
    uint  b_encodeDepth;
    uint  texAlphaInvert;
    uvec4 texShift;
    uvec4 texMask;
    vec4  texScale;
    uint  texDivide;
    uint  replaceInfo;
    uint  replaceXY;
    uint  replaceWH;
    uint  anisoTaps;
};

#ifdef SM2_TARGET_GL
layout(std140, binding = 2) uniform R3DModel {
#else
layout(push_constant) uniform R3DModel {
#endif
    mat4  modelMat;
    float modelScale;
    float nodeAlpha;
    uint  b_translatorMap;
};

#define translatorMap bool(b_translatorMap)

// attributes - see render/model3/r3d_model.h FVertexLayout
layout(location = 0) in vec4  inVertex;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inTexCoord;
layout(location = 3) in vec3  inFaceNormal;  // emulates Real3D culling
layout(location = 4) in float inFixedShade;
layout(location = 5) in vec4  inColour;
layout(location = 6) in float inTextureNP;

layout(location = 0) out vec3  fsViewVertex;
layout(location = 1) out vec3  fsViewNormal;
layout(location = 2) out vec2  fsTexCoord;
layout(location = 3) out vec4  fsColor;
layout(location = 4) out float fsFixedShade;
layout(location = 5) out float fsDiscardPoly;  // GLSL has no bool varyings
layout(location = 6) out float fsLODBase;

vec4 GetColour(vec4 colour)
{
    vec4 c = colour;

    if (translatorMap) {
        c.rgb *= 16.0;
    }

    c.a *= nodeAlpha;

    return c;
}

float CalcBackFace(in vec3 viewVertex)
{
    vec3 vt = viewVertex;
    vec3 vn = mat3(modelMat) * inFaceNormal;

    return dot(vt, vn);
}

void main(void)
{
    fsViewVertex  = (modelMat * inVertex).xyz;
    fsViewNormal  = (mat3(modelMat) / modelScale) * inNormal;
    fsDiscardPoly = CalcBackFace(fsViewVertex);
    fsColor       = GetColour(inColour);
    fsTexCoord    = inTexCoord;
    fsFixedShade  = inFixedShade;
    fsLODBase     = fsDiscardPoly * -cota * inTextureNP;
    gl_Position   = (projMat * modelMat) * inVertex;
#ifdef SM2_TARGET_GL
    gl_Position.y = -gl_Position.y;
#endif
}
