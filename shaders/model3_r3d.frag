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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DShaderTriangles.h
// and R3DShaderCommon.h, Copyright (c) 2003-2026 The Supermodel Team,
// GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Real3D fragment shader. Line of sight goes to an
// RG32UI target; fragCoordYTransform restores GL's gl_FragCoord origin.
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

#define textureInverted bool(b_textureInverted)
#define discardAlpha    bool(b_discardAlpha)
#define sunClamp        bool(b_sunClamp)
#define intensityClamp  bool(b_intensityClamp)
#ifdef SM2_TARGET_GL
#define textureEnabled  bool(b_textureEnabled)
#define microTexture    bool(b_microTexture)
#define textureAlpha    bool(b_textureAlpha)
#define alphaTest       bool(b_alphaTest)
#define lightEnabled    bool(b_lightEnabled)
#define specularEnabled bool(b_specularEnabled)
#define textureReplaced (replaceInfo != 0u)
#ifdef SM2_ANISO
#define anisotropic     true
#else
#define anisotropic     false
#endif
#else
// Pipeline specialisation: the bits of R3DDraw::variant (frame_plan.h).
layout(constant_id = 0) const bool textureEnabled  = false;
layout(constant_id = 1) const bool microTexture    = false;
layout(constant_id = 2) const bool textureAlpha    = false;
layout(constant_id = 3) const bool alphaTest       = false;
layout(constant_id = 4) const bool lightEnabled    = false;
layout(constant_id = 5) const bool specularEnabled = false;
layout(constant_id = 6) const bool textureReplaced = false;
layout(constant_id = 7) const bool anisotropic     = false;
#endif
#define fixedShading    bool(b_fixedShading)
#define smoothShading   bool(b_smoothShading)
#define noLosReturn     bool(b_noLosReturn)

layout(set = 0, binding = 1) uniform usampler2DArray textureBank;  // layer = page
// Custom textures (render/texture_replace.cpp), in place of textureBank.
layout(set = 0, binding = 2) uniform sampler2DArray replaceAtlas;
const float kReplaceLayerSize = 4096.0;

layout(location = 0) in vec3  fsViewVertex;
layout(location = 1) in vec3  fsViewNormal;  // per vertex normal vector
layout(location = 2) in vec2  fsTexCoord;
layout(location = 3) in vec4  fsColor;
layout(location = 4) in float fsFixedShade;
layout(location = 5) in float fsDiscardPoly;
layout(location = 6) in float fsLODBase;

layout(location = 0) out vec4  out0;    // opaque
layout(location = 1) out vec4  out1;    // trans layer 1
layout(location = 2) out vec4  out2;    // trans layer 2
layout(location = 3) out uvec2 outLos;  // line-of-sight depth bits and no-return flag

#define LayerColour 0x0
#define LayerTrans0 0x1
#define LayerTrans1 0x2

vec2 ScreenFragCoord()
{
    return vec2(gl_FragCoord.x, fragCoordYTransform.y + fragCoordYTransform.x * gl_FragCoord.y);
}

vec4 ExtractColour(uint value)
{
    uvec4 bits = (uvec4(value) >> texShift) & texMask;
    vec4  c    = vec4(bits);
    c = (texDivide != 0u) ? c / texScale : c * texScale;
    if (texAlphaInvert != 0u) {
        // On the raw bits: a GPU may divide as multiply-by-reciprocal, so
        // 255.0 / 255.0 need not be exactly 1.0.
        c.a = (bits.a == texMask.a) ? 0.0 : 1.0;
    }
    return c;
}

ivec2 WrapTexCoords(ivec2 pos, ivec2 coordinate, int level)
{
    ivec2 newCoord;
    newCoord.x = coordinate.x & (2047 >> level);
    newCoord.y = coordinate.y & (1023 >> level);

    return newCoord;
}

ivec2 GetTextureSize(int level, ivec2 size)
{
    return size >> level;
}

ivec2 GetTexturePosition(int level, ivec2 pos)
{
    return pos >> level;
}

ivec2 GetMicroTexturePos(int id)
{
    int xCoords[8] = int[](0, 0, 128, 128, 0, 0, 128, 128);
    int yCoords[8] = int[](0, 128, 0, 128, 256, 384, 256, 384);

    return ivec2(xCoords[id], yCoords[id]);
}

float LinearTexLocations(int wrapMode, float size, float u, out float u0, out float u1)
{
    float texelSize     = 1.0 / size;
    float halfTexelSize = 0.5 / size;

    if (wrapMode == 0) {  // repeat
        u  = u * size - 0.5;
        u0 = (floor(u) + 0.5) / size;  // + 0.5 moves to the texel centre, avoiding rounding errors
        u0 = fract(u0);
        u1 = u0 + texelSize;
        u1 = fract(u1);

        return fract(u);  // return weight
    }
    else if (wrapMode == 1) {  // repeat + clamp
        u  = fract(u);  // force into 0-1 first
        u  = u * size - 0.5;
        u0 = (floor(u) + 0.5) / size;
        u1 = u0 + texelSize;

        if (u0 < 0.0)  u0 = 0.0;
        if (u1 >= 1.0) u1 = 1.0 - halfTexelSize;

        return fract(u);  // return weight
    }
    else {  // mirror and mirror clamp: the same, as the edge texels repeat anyway
        float odd = floor(mod(u, 2.0));  // odd values are mirrored

        if (odd > 0.0) {
            u = 1.0 - fract(u);
        }
        else {
            u = fract(u);
        }

        u  = u * size - 0.5;
        u0 = (floor(u) + 0.5) / size;
        u1 = u0 + texelSize;

        if (u0 < 0.0)  u0 = 0.0;
        if (u1 >= 1.0) u1 = 1.0 - halfTexelSize;

        return fract(u);  // return weight
    }
}

vec4 texBiLinear(int page, ivec2 wrapMode, vec2 texSize, ivec2 texPos, vec2 texCoord, int level)
{
    float tx[2], ty[2];
    float a = LinearTexLocations(wrapMode.s, texSize.x, texCoord.x, tx[0], tx[1]);
    float b = LinearTexLocations(wrapMode.t, texSize.y, texCoord.y, ty[0], ty[1]);

    vec4 p0q0 = ExtractColour(texelFetch(textureBank, ivec3(WrapTexCoords(texPos, ivec2(vec2(tx[0], ty[0]) * texSize + vec2(texPos)), level), page), level).r);
    vec4 p1q0 = ExtractColour(texelFetch(textureBank, ivec3(WrapTexCoords(texPos, ivec2(vec2(tx[1], ty[0]) * texSize + vec2(texPos)), level), page), level).r);
    vec4 p0q1 = ExtractColour(texelFetch(textureBank, ivec3(WrapTexCoords(texPos, ivec2(vec2(tx[0], ty[1]) * texSize + vec2(texPos)), level), page), level).r);
    vec4 p1q1 = ExtractColour(texelFetch(textureBank, ivec3(WrapTexCoords(texPos, ivec2(vec2(tx[1], ty[1]) * texSize + vec2(texPos)), level), page), level).r);

    if (alphaTest) {
        if (p0q0.a > p1q0.a) { p1q0.rgb = p0q0.rgb; }
        if (p0q0.a > p0q1.a) { p0q1.rgb = p0q0.rgb; }

        if (p1q0.a > p0q0.a) { p0q0.rgb = p1q0.rgb; }
        if (p1q0.a > p1q1.a) { p1q1.rgb = p1q0.rgb; }

        if (p0q1.a > p0q0.a) { p0q0.rgb = p0q1.rgb; }
        if (p0q1.a > p1q1.a) { p1q1.rgb = p0q1.rgb; }

        if (p1q1.a > p0q1.a) { p0q1.rgb = p1q1.rgb; }
        if (p1q1.a > p1q0.a) { p1q0.rgb = p1q1.rgb; }
    }

    vec4 pInterp_q0 = mix(p0q0, p1q0, a);  // top row in X
    vec4 pInterp_q1 = mix(p0q1, p1q1, a);  // bottom row in X

    return mix(pInterp_q0, pInterp_q1, b);  // then in Y
}

vec4 texBiLinearBank(int page, ivec2 wrapMode, vec2 texSize, ivec2 texPos, vec2 texCoord, int level)
{
    return texBiLinear(page, wrapMode, texSize, texPos, texCoord, level);
}

float ReplaceWrap(int wrapMode, float u, float margin)
{
    if (wrapMode == 0) {
        return fract(u);  // the atlas gutter repeats the image
    }
    if (wrapMode >= 2 && floor(mod(u, 2.0)) > 0.0) {
        u = 1.0 - fract(u);
    }
    else {
        u = fract(u);
    }
    return clamp(u, margin, 1.0 - margin);
}

// The LOD is the polygon's own, shifted by the replacement's scale. A micro
// texture fades in only once the replacement too runs out of detail.
vec4 ReplacementTexel(float lod)
{
    vec2  rectXY = vec2(float(replaceXY & 0xffffu), float(replaceXY >> 16));
    vec2  rectWH = vec2(float(replaceWH & 0xffffu), float(replaceWH >> 16));
    float scale  = min(rectWH.x / float(baseTexInfo.z), rectWH.y / float(baseTexInfo.w));
    float rLod   = lod + log2(scale);
    float level  = clamp(rLod, 0.0, float((replaceInfo >> 8) & 0xfu));
    vec2  margin = 0.5 * exp2(level) / rectWH;
    vec2  st     = vec2(ReplaceWrap(textureWrapMode.s, fsTexCoord.x, margin.x),
                        ReplaceWrap(textureWrapMode.t, fsTexCoord.y, margin.y));
    vec4  texel  = textureLod(replaceAtlas, vec3((rectXY + st * rectWH) / kReplaceLayerSize, float((replaceInfo & 0xffu) - 1u)), level);

    if (microTexture && rLod < 0.0) {
        vec4  scaleIndex = vec4(2.0, 4.0, 16.0, 256.0);
        vec2  mscale     = (vec2(baseTexInfo.zw) / 128.0) * scaleIndex[int(microTextureMinLOD)];
        vec4  micro      = texBiLinearBank((texturePage + 1) & 1, ivec2(0), vec2(128.0), GetMicroTexturePos(microTextureID), fsTexCoord * mscale, 0);
        float blend      = min(-rLod * exp2(-microTextureMinLOD) * 0.5, 0.5);
        texel = mix(texel, micro, blend);
    }
    return texel;
}

vec4 SampleBank(vec2 texCoord, float lod)
{
    float numLevels = floor(log2(min(float(baseTexInfo.z), float(baseTexInfo.w)))) - 1.0;  // the Real3D only generates down to 2x2 for square textures, otherwise to the min dimension
    float fLevel    = clamp(lod, 0.0, numLevels);
    int   iLevel    = int(fLevel);

    ivec2 tex1Pos  = GetTexturePosition(iLevel, baseTexInfo.xy);
    ivec2 tex1Size = GetTextureSize(iLevel, baseTexInfo.zw);
    vec4  tex1Data = texBiLinearBank(texturePage, textureWrapMode, vec2(tex1Size), tex1Pos, texCoord, iLevel);

    vec4 tex2Data = vec4(0.0);

    float blendFactor = 0.0;

    // Blend with the next mip level only when the level has a fraction.
    float ffL = fract(fLevel);
    if (ffL > 0.0) {
        ivec2 tex2Pos  = GetTexturePosition(iLevel + 1, baseTexInfo.xy);
        ivec2 tex2Size = GetTextureSize(iLevel + 1, baseTexInfo.zw);
        tex2Data = texBiLinearBank(texturePage, textureWrapMode, vec2(tex2Size), tex2Pos, texCoord, iLevel + 1);

        blendFactor = ffL;
    }
    else if (microTexture && lod < 0.0) {
        vec4 scaleIndex = vec4(2.0, 4.0, 16.0, 256.0);  // the last min LOD's 256x is a guess; no game seems to use it
        vec2 scale      = (vec2(baseTexInfo.zw) / 128.0) * scaleIndex[int(microTextureMinLOD)];

        // microtextures are always 128x128 and only use LOD 0 mipmap
        ivec2 tex2Pos = GetMicroTexturePos(microTextureID);
        tex2Data = texBiLinearBank((texturePage + 1) & 1, ivec2(0), vec2(128.0), tex2Pos, texCoord * scale, 0);

        blendFactor = -lod * exp2(-microTextureMinLOD) * 0.5;
        blendFactor = min(blendFactor, 0.5);
    }

    return mix(tex1Data, tex2Data, blendFactor);
}

vec4 GetTextureValue()
{
    float lod = -log2(gl_FragCoord.w * gl_FragCoord.w * fsLODBase);

    // Anisotropic enhancement: a sharper level, sampled along the footprint's
    // long axis. One tap is the hardware's isotropic filter.
    int  taps = 1;
    vec2 axis = vec2(0.0);
    if (anisotropic) {
        vec2  texels = fsTexCoord * vec2(baseTexInfo.zw);
        vec2  dx     = dFdx(texels);
        vec2  dy     = dFdy(texels);
        float lx    = length(dx);
        float ly    = length(dy);
        float ratio = max(lx, ly) / max(min(lx, ly), 1e-6);
        taps        = int(min(ceil(ratio), float(anisoTaps)));
        axis        = (lx > ly ? dx : dy) / vec2(baseTexInfo.zw);
        lod        -= log2(float(taps));
    }

    vec4 tex1Data;
    if (textureReplaced) {
        tex1Data = ReplacementTexel(lod);
    }
    else if (taps <= 1) {
        tex1Data = SampleBank(fsTexCoord, lod);
    }
    else {
        tex1Data = vec4(0.0);
        for (int i = 0; i < taps; i++) {
            tex1Data += SampleBank(fsTexCoord + axis * ((float(i) + 0.5) / float(taps) - 0.5), lod);
        }
        tex1Data /= float(taps);
    }

    if (textureInverted) {
        tex1Data.rgb = vec3(1.0) - tex1Data.rgb;
    }

    if (alphaTest) {
        if (tex1Data.a < (32.0 / 255.0)) {
            discard;
        }
    }

    if (textureAlpha) {
        if (discardAlpha) {  // opaque 1st pass
            if (tex1Data.a < 1.0) {
                discard;
            }
        }
        else {  // transparent 2nd pass
            if ((tex1Data.a * fsColor.a) >= 1.0) {
                discard;
            }
        }
    }

    if (textureAlpha == false && alphaTest == false) {
        tex1Data.a = 1.0;
    }

    return tex1Data;
}

void Step15Luminous(inout vec4 colour)
{
    // On Step 1.5 luminous polygons are modulated by ambient plus the fixed shade,
    // or without fixed shading apparently scaled by 1.5.
    if (hardwareStep == 0x15) {
        if (!lightEnabled) {
            if (fixedShading) {
                colour.rgb *= 1.0 + fsFixedShade + lighting[1].y;
            }
            else {
                colour.rgb *= 1.5;
            }
        }
    }
}

float CalcFog()
{
    float z   = -fsViewVertex.z;
    float fog = fogIntensity * clamp(fogStart + z * fogDensity, 0.0, 1.0);

    return fog;
}

float Sqr(float a)
{
    return a * a;
}

float SqrLength(vec2 a)
{
    return a.x * a.x + a.y * a.y;
}

// Reversed-Z depth sits just above zero; on unorm depth buffers (b_encodeDepth)
// write the biased IEEE-754 bit pattern instead. LOS uses the unencoded value.
float gLinearDepth;

const uint  kDepthEncodeBias  = 796917760u;          // floatBitsToUint(2^-32)
const float kDepthEncodeScale = 1.0 / 268435456.0;   // 1 / (bits(1.0) - bias)

float EncodeDepth(float depth)
{
    gLinearDepth = depth;
    if (b_encodeDepth == 0u) {
        return depth;
    }
    uint bits   = floatBitsToUint(max(depth, 0.0));
    uint biased = bits > kDepthEncodeBias ? bits - kDepthEncodeBias : 0u;
    return min(float(biased) * kDepthEncodeScale, 1.0);
}

void ComputeSpotlight(vec4 ellipseParams, vec2 rangeParams, float viewZ, out float lobeEffect, out float lobeFogEffect)
{
    float ellipse = SqrLength((ScreenFragCoord() - ellipseParams.xy) / ellipseParams.zw);
    ellipse = 1.0 - ellipse;
    ellipse = max(0.0, ellipse);

    float enable = step(rangeParams.x, -viewZ);

    float range = 0.0;

    if (rangeParams.y != 0.0) {
        float absExtent = abs(rangeParams.y);

        float d = rangeParams.x + absExtent + viewZ;
        d = min(d, 0.0);

        float inv_r = 1.0 / (1.0 + absExtent);

        // inverse-linear falloff
        range = 1.0 / Sqr(d * inv_r - 1.0);
        range *= enable;
    }

    lobeEffect    = range * ellipse;
    lobeFogEffect = enable * ellipse;
}

void WriteOutputs(vec4 colour, int layer)
{
    vec4 blank = vec4(0.0);

    if (layer == LayerColour) {
        colour.a = 1.0;
        out0 = colour;
        out1 = blank;
        out2 = blank;
    }
    else if (layer == LayerTrans0) {
        out0 = blank;
        out1 = colour;
        out2 = blank;
    }
    else {
        out0 = blank;
        out1 = blank;
        out2 = colour;
    }
}

void main()
{
    vec4 tex1Data;
    vec4 colData;
    vec4 finalData;
    vec4 fogData;

    if (fsDiscardPoly > 0.0) {
        discard;  // emulate back face culling here
    }

#if defined(SM2_ENCODE_DEPTH) || defined(SM2_TARGET_GL)
    // GL's [-1, 1] clip z would cost the precision near / w needs.
    gl_FragDepth = EncodeDepth(projMat[3][2] * gl_FragCoord.w);
#else
    // The projection puts near_val in clip z, so the hardware depth is
    // already near / w; not writing it keeps early depth rejection.
    gLinearDepth = projMat[3][2] * gl_FragCoord.w;
#endif

    fogData  = vec4(fogColour.rgb * fogAmbient, CalcFog());
    tex1Data = vec4(1.0, 1.0, 1.0, 1.0);

    if (textureEnabled) {
        tex1Data = GetTextureValue();
    }

    colData = fsColor;
    Step15Luminous(colData);  // no-op for step 2.0+
    finalData = tex1Data * colData;

    if (finalData.a < (1.0 / 32.0)) {  // discard fully transparent pixels; 1/32 is the hardware's smallest translucency step
        discard;
    }

    float lobeEffect;
    float lobeFogEffect;
    ComputeSpotlight(spotEllipse, spotRange, fsViewVertex.z, lobeEffect, lobeFogEffect);

    if (lightEnabled) {
        vec3  lightIntensity;
        vec3  sunVector;  // sun lighting vector (as reflecting away from vertex)
        float sunFactor;  // sun light projection along vertex normal (0.0 to 1.0)

        sunVector = lighting[0].xyz;

        if (fixedShading) {
            sunFactor = fsFixedShade;
        }
        else {
            sunFactor = dot(sunVector, fsViewNormal);
        }

        // Clamp the ceiling too, for upscaled models without a modelScale.
        sunFactor = clamp(sunFactor, -1.0, 1.0);

        // Optional clamp, as the value may be negative. Translucent polygons
        // seem always clamped (e.g. lasers in Daytona 2).
        if (sunClamp || finalData.a < 0.999) {
            sunFactor = max(sunFactor, 0.0);
        }

        lightIntensity = vec3(sunFactor * lighting[1].x + lighting[1].y);  // diffuse + ambient

        lightIntensity += spotColor.rgb * lobeEffect;

        // Upper clamp is optional: Step 1.5+ games drive brightness beyond 100%
        if (intensityClamp) {
            lightIntensity = min(lightIntensity, 1.0);
        }

        finalData.rgb *= lightIntensity;

        // assume fixed shading doesn't apply to specular
        if (specularEnabled) {

            float specularFactor;

            if (smoothShading) {
                // Always clamp floor to zero
                float NdotL = max(0.0, sunFactor);

                float expIndex[4]  = float[4](8.0, 16.0, 32.0, 64.0);
                float multIndex[4] = float[4](1.6, 1.6, 2.4, 3.2);
                float exponent     = expIndex[int(shininess)];

                specularFactor = pow(NdotL, exponent);
                specularFactor *= multIndex[int(shininess)];
            }
            else {
                // Flat-shaded polygons use Phong reflection (R dot V) with no exponent or multiplier.
                // Model 3 approximates V as (0, 0, 1), so R dot V = R.z.
                vec3 R = reflect(-sunVector, fsViewNormal);
                specularFactor = max(0.0, R.z);
            }

            specularFactor *= specularValue;
            specularFactor *= lighting[1].x;

            if (colData.a < 1.0) {
                // specular highlights affect a translucent polygon's alpha
                finalData.a = max(finalData.a, specularFactor);
            }

            finalData.rgb += specularFactor;
        }
    }

    // Final clamp, needed for correct shading in dimmed light and dark ambients
    finalData.rgb = min(finalData.rgb, vec3(1.0));

    vec3 lSpotFogColor = spotFogColor.rgb * fogAttenuation * fogColour.rgb * lobeFogEffect;

    finalData.rgb = mix(finalData.rgb, fogData.rgb + lSpotFogColor, fogData.a);

    WriteOutputs(finalData, colourLayer);

    outLos = uvec2(floatBitsToUint(gLinearDepth), noLosReturn ? 1u : 0u);
}
