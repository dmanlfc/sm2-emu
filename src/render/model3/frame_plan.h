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
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp and
// R3DShader.cpp, Copyright (c) 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// A Model 3 frame as backend-neutral GPU work for the GL and Vulkan passes.
// Uniform blocks are std140; rectangles are target pixels, origin top left.
#pragma once

#include "core/types.h"
#include "render/model3/scene.h"

#include <array>
#include <cstddef>
#include <vector>

namespace sm2::render::model3 {

inline constexpr s32 kLayerColour = 0;
inline constexpr s32 kLayerTrans0 = 1;
inline constexpr s32 kLayerTrans1 = 2;

struct alignas(16) R3DUniforms {
    float projMat[16];            // 0
    float fogColour[4];           // 64
    float spotEllipse[4];         // 80
    float spotColor[4];           // 96
    float spotFogColor[4];        // 112
    float lighting[2][4];         // 128: [0].xyz sun direction, [1].x diffuse, [1].y ambient
    s32   baseTexInfo[4];         // 160
    float spotRange[2];           // 176
    s32   textureWrapMode[2];     // 184
    float fragCoordYTransform[2]; // 192
    float cota;                   // 200
    float fogIntensity;
    float fogDensity;
    float fogStart;
    float fogAttenuation;
    float fogAmbient;
    float specularValue;
    float shininess;
    float microTextureMinLOD;     // 232
    s32   baseTexType;
    s32   microTextureID;
    s32   texturePage;
    s32   hardwareStep;
    s32   colourLayer;            // 252
    u32   textureEnabled;         // 256
    u32   microTexture;
    u32   textureInverted;
    u32   textureAlpha;
    u32   alphaTest;
    u32   discardAlpha;
    u32   lightEnabled;
    u32   sunClamp;
    u32   intensityClamp;
    u32   specularEnabled;
    u32   fixedShading;
    u32   smoothShading;
    u32   polyAlpha;
    u32   noLosReturn;
    u32   encodeDepth;            // 312
    u32   texAlphaInvert;         // 316: alpha 1 becomes 0, else 1
    u32   texShift[4];            // 320: per-channel bit position in the texel
    u32   texMask[4];             // 336
    float texScale[4];            // 352: multiplied, or divided when texDivide
    u32   texDivide;              // 368
    u32   replaceInfo;            // 372: custom texture atlas layer + 1 (0 = none), max LOD << 8
    u32   replaceXY;              // its rectangle in the layer, 16 bits each
    u32   replaceWH;
    u32   anisoTaps;              // 384: anisotropic taps, 1 is the hardware filter
    u32   pad[3];
};
static_assert(sizeof(R3DUniforms) == 400);
static_assert(offsetof(R3DUniforms, baseTexInfo) == 160);
static_assert(offsetof(R3DUniforms, spotRange) == 176);
static_assert(offsetof(R3DUniforms, fragCoordYTransform) == 192);
static_assert(offsetof(R3DUniforms, cota) == 200);
static_assert(offsetof(R3DUniforms, microTextureMinLOD) == 232);
static_assert(offsetof(R3DUniforms, colourLayer) == 252);
static_assert(offsetof(R3DUniforms, textureEnabled) == 256);
static_assert(offsetof(R3DUniforms, encodeDepth) == 312);

struct alignas(16) R3DModelUniforms {
    float modelMat[16];
    float modelScale;
    float nodeAlpha;
    u32   translatorMap;
    u32   pad0;
};
static_assert(sizeof(R3DModelUniforms) == 80);

struct alignas(16) ScrollFogUniforms {
    float fogColour[4];      // .rgb colour, .a density
    float spotEllipse[4];
    float spotFogColor[4];
    float fragCoordYTransform[2];
    float fogAttenuation;
    float fogAmbient;
};
static_assert(sizeof(ScrollFogUniforms) == 64);

struct PixelRect {
    s32 x      = 0;
    s32 y      = 0;
    s32 width  = 0;
    s32 height = 0;
};

struct ViewportRect {
    PixelRect viewport;  ///< glViewport() / VkViewport.
    PixelRect scissor;   ///< Clamped to the target and the display box.
};

// Fragment shader specialisation bits, in constant_id order.
constexpr u32 kR3DVariantCount = 128;
constexpr u32 kR3DVariantReplaced = 1u << 6;
[[nodiscard]] constexpr u32 r3d_variant(const R3DUniforms& u)
{
    return u.textureEnabled | (u.microTexture << 1) | (u.textureAlpha << 2) | (u.alphaTest << 3)
           | (u.lightEnabled << 4) | (u.specularEnabled << 5) | (u.replaceInfo != 0 ? kR3DVariantReplaced : 0u);
}

struct R3DDraw {
    u32  uniforms     = 0;  ///< Index into FramePlan::uniforms.
    u32  variant      = 0;  ///< r3d_variant() of those uniforms.
    u32  model        = 0;  ///< Index into FramePlan::models.
    u32  viewport     = 0;  ///< Index into FramePlan::viewports.
    u32  first_vertex = 0;  ///< Within its vertex region.
    u32  vertex_count = 0;
    bool dynamic      = false;  ///< RAM region (Scene::ram_vertices()), else ROM.
    bool layered       = false;  // EQUAL 0 / INCR on 0x7F, else ALWAYS no_los<<7 / REPLACE on 0x80
    bool no_los_return = false;
};

struct DrawRange {
    u32 begin = 0;
    u32 end   = 0;
    [[nodiscard]] bool empty() const { return begin == end; }
};

// One (priority, overlay) iteration of the frame: clear depth and
// stencil to 0 and the LOS target; draw `colour` (GEQUAL); sample LOS; store
// depth; draw `trans1` (GREATER); restore depth; draw `trans2` (GREATER).
struct LayerPass {
    int       priority = 0;
    bool      overlay  = false;
    DrawRange colour;
    DrawRange trans1;
    DrawRange trans2;
    bool      los   = false;
    s32       los_x = 0;  ///< Target pixel, origin top left.
    s32       los_y = 0;
};

struct FogDraw {
    u32               viewport = 0;
    ScrollFogUniforms uniforms{};
};

// A tile layer's texture drawn at `rect`: uv.x scaled by uv_scale, kept
// within [clip_lo, clip_hi).
struct LayerDraw {
    PixelRect rect;
    float     uv_scale = 1.0f;
    float     clip_lo  = 0.0f;
    float     clip_hi  = 1.0f;
};

struct FramePlan {
    u32 target_width  = 0;
    u32 target_height = 0;

    bool white_flash = false;

    PixelRect display;   // the scissor: display inset by the 2D/3D correction
    PixelRect layer_2d;  // tile layers, shifted left/up by the correction
    LayerDraw layers[2];  // bottom, top; set by FrameBuilder
    bool      widened = false;  // a viewport reaches into the widescreen columns

    std::vector<FogDraw>          fogs;
    std::vector<LayerPass>        passes;
    std::vector<R3DDraw>          draws;
    std::vector<R3DUniforms>      uniforms;
    std::vector<R3DModelUniforms> models;
    std::vector<ViewportRect>     viewports;

    [[nodiscard]] bool los_active() const;
};

// blank_pages: texture pages no non-zero texel has reached yet.
void build_frame_plan(Scene& scene, bool encode_depth, std::array<bool, 2> blank_pages, FramePlan& plan);

// Decodes one line-of-sight sample.
[[nodiscard]] float decode_line_of_sight(u32 depth_bits, bool no_los_return);

}  // namespace sm2::render::model3
