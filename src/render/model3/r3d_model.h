// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real3D scene structures and polygon header decoder.
//
// Derived from Supermodel's Src/Graphics/New3D/Model.h, Model.cpp,
// PolyHeader.h, PolyHeader.cpp and R3DData.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2003-2026 The Supermodel Team (New3D by Ian Curtis).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
#pragma once

#include "core/types.h"
#include "render/model3/r3d_math.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace sm2::render::model3 {

struct Vertex {  // half vertex
    float pos[4];
    float normal[3];
    float texcoords[2];
    float fixedShade;

    static bool Equal(const Vertex& p1, const Vertex& p2)
    {
        return (p1.pos[0] == p2.pos[0] && p1.pos[1] == p2.pos[1] && p1.pos[2] == p2.pos[2]);
    }

    static void Average(const Vertex& p1, const Vertex& p2, Vertex& p3)
    {
        p3.pos[3]     = 1.0f;  // always 1
        p3.fixedShade = (p1.fixedShade + p2.fixedShade) * 0.5f;

        for (int i = 0; i < 3; i++) { p3.pos[i] = (p1.pos[i] + p2.pos[i]) * 0.5f; }
        for (int i = 0; i < 3; i++) { p3.normal[i] = (p1.normal[i] + p2.normal[i]) * 0.5f; }
        for (int i = 0; i < 2; i++) { p3.texcoords[i] = (p1.texcoords[i] + p2.texcoords[i]) * 0.5f; }
    }
};

struct R3DPoly {
    Vertex v[4];
    float  faceNormal[3];  // helps work out polygon winding; the hardware presumably uses it rather than calculating normals
    u8     faceColour[4];  // per face colour
    float  textureNP;
    int    number = 4;
};

struct FVertex {  // full vertex including face attributes
    Vertex base;  // composition keeps FVertex standard-layout, for offsetof
    float  faceNormal[3];
    u8     faceColour[4];
    float  textureNP;

    FVertex& operator=(const Vertex& vertex)
    {
        base = vertex;
        return *this;
    }

    FVertex() = default;
    FVertex(const R3DPoly& r3dPoly, int index)
    {
        for (int i = 0; i < 4; i++) { faceColour[i] = r3dPoly.faceColour[i]; }
        for (int i = 0; i < 3; i++) { faceNormal[i] = r3dPoly.faceNormal[i]; }
        textureNP = r3dPoly.textureNP;

        *this = r3dPoly.v[index];
    }

    FVertex(const R3DPoly& r3dPoly, int index1, int index2)  // average of 2 points
    {
        Vertex::Average(r3dPoly.v[index1], r3dPoly.v[index2], base);

        // copy face attributes
        for (int i = 0; i < 4; i++) { faceColour[i] = r3dPoly.faceColour[i]; }
        for (int i = 0; i < 3; i++) { faceNormal[i] = r3dPoly.faceNormal[i]; }
        textureNP = r3dPoly.textureNP;
    }

    static bool Equal(const FVertex& p1, const FVertex& p2) { return Vertex::Equal(p1.base, p2.base); }

    static void Average(const FVertex& p1, const FVertex& p2, FVertex& p3)
    {
        Vertex::Average(p1.base, p2.base, p3.base);
        for (int i = 0; i < 4; i++) { p3.faceColour[i] = p1.faceColour[i]; }
        for (int i = 0; i < 3; i++) { p3.faceNormal[i] = p1.faceNormal[i]; }
    }
};

struct FVertexLayout {
    static constexpr u32 kStride      = 60;
    static constexpr u32 kPos         = 0;
    static constexpr u32 kNormal      = 16;
    static constexpr u32 kTexCoord    = 28;
    static constexpr u32 kFixedShade  = 36;
    static constexpr u32 kFaceNormal  = 40;
    static constexpr u32 kFaceColour  = 52;
    static constexpr u32 kTextureNP   = 56;
};
static_assert(sizeof(FVertex) == FVertexLayout::kStride);
static_assert(offsetof(FVertex, base) + offsetof(Vertex, normal) == FVertexLayout::kNormal);
static_assert(offsetof(FVertex, base) + offsetof(Vertex, texcoords) == FVertexLayout::kTexCoord);
static_assert(offsetof(FVertex, base) + offsetof(Vertex, fixedShade) == FVertexLayout::kFixedShade);
static_assert(offsetof(FVertex, faceNormal) == FVertexLayout::kFaceNormal);
static_assert(offsetof(FVertex, faceColour) == FVertexLayout::kFaceColour);
static_assert(offsetof(FVertex, textureNP) == FVertexLayout::kTextureNP);

enum class Layer { colour, trans1, trans2, trans12 /*both 1&2*/, all, none };

struct Mesh {
    // helper funcs
    [[nodiscard]] bool Render(Layer layer, float nodeAlpha) const
    {
        const bool nAlpha = nodeAlpha < 1.0f;

        switch (layer) {
            case Layer::colour:
                if (polyAlpha || nAlpha) {
                    return false;
                }
                break;
            case Layer::trans1:
                if ((!textureAlpha && !polyAlpha && !nAlpha) || transLSelect) {
                    return false;
                }
                break;
            case Layer::trans2:
                if ((!textureAlpha && !polyAlpha && !nAlpha) || !transLSelect) {
                    return false;
                }
                break;
            default:  // not using these types
                return false;
        }

        return true;
    }

    enum TexWrapMode : int { repeat = 0, repeatClamp, mirror, mirrorClamp };

    // texture
    int  format   = 0;
    int  x        = 0;
    int  y        = 0;
    int  width    = 0;
    int  height   = 0;
    int  page     = 0;
    bool inverted = false;

    TexWrapMode wrapModeU = TexWrapMode::repeat;
    TexWrapMode wrapModeV = TexWrapMode::repeat;

    // microtexture
    bool  microTexture       = false;
    int   microTextureID     = 0;
    float microTextureMinLOD = 0;

    // attributes
    bool textured      = false;
    bool polyAlpha     = false;  // specified in the rgba colour
    bool textureAlpha  = false;  // use alpha in texture
    bool alphaTest     = false;  // discard fragment based on alpha
    bool layered       = false;  // stencil poly
    bool highPriority  = false;  // rendered over the top
    bool transLSelect  = false;  // translucency layer: false = layer 0, true = layer 1
    bool translatorMap = false;  // colours are multiplied by 16
    bool noLosReturn   = false;  // line of sight test

    // lighting
    bool  fixedShading  = false;
    bool  smoothShading = false;
    bool  lighting      = false;
    bool  specular      = false;
    float shininess     = 0;
    float specularValue = 0;

    // fog
    float fogIntensity = 1.0f;

    // vertex buffer placement
    int vboOffset   = 0;
    int vertexCount = 0;  // /3 for triangles /4 for quads
};

struct SortingMesh : public Mesh {  // holds the model data until it is copied to the main buffer
    std::vector<FVertex> verts;
};

struct Model {
    std::shared_ptr<std::vector<Mesh>> meshes;  // shared, as several models may use the same meshes

    // which memory are we in
    bool dynamic = true;

    // texture offsets for model
    int textureOffsetX = 0;
    int textureOffsetY = 0;
    int page           = 0;

    // matrices
    float modelMat[16];

    // model scale step 1.5+
    float scale = 1.0f;

    // node transparency
    float alpha = 1.0f;
};

struct Viewport {
    float vpX = 0;  // raw hardware values
    float vpY = 0;
    float vpWidth = 0;
    float vpHeight = 0;
    float angle_left = 0;
    float angle_right = 0;
    float angle_top = 0;
    float angle_bottom = 0;
    float cota = 0;

    Mat4 projectionMatrix;  // calculated once the scene near/far values are known

    float lightingParams[6] = {};  // lighting parameters (see RenderViewport() and vertex shader)
    bool  sunClamp = false;        // unknown how this is set
    bool  intensityClamp = false;  // unknown how this is set
    float spotEllipse[4] = {};     // spotlight ellipse (see RenderViewport())
    float spotRange[2] = {};       // Z range
    float spotColor[3] = {};       // colour
    float fogColour[3] = {};
    float fogStart = 0;
    float fogDensity = 0;
    float fogAttenuation = 0;
    float fogAmbient = 0;
    float scrollFog = 0;         // a transparency value that determines if fog is blended over the bottom 2D layer
    int   losPosX = 0, losPosY = 0;  // line of sight position
    int   x = 0, y = 0;          // viewport coordinates (scaled and in OpenGL format)
    int   width = 0, height = 0; // viewport dimensions (scaled for display surface size)
    int   priority = 0;          // priority
    int   select = 0;            // viewport select?
    int   number = 0;            // viewport number
    float spotFogColor[3] = {};  // spotlight colour on fog
    float scrollAtt = 0;

    int hardwareStep = 0;
};

class NodeAttributes {
public:
    NodeAttributes() { Reset(); }

    bool Push();
    bool Pop();
    [[nodiscard]] bool StackLimit() const { return m_depth >= 1024; }
    void Reset();

    int   currentTexOffsetX = 0;
    int   currentTexOffsetY = 0;
    int   currentPage = 0;
    float currentModelScale = 1.0f;
    float currentModelAlpha = 1.0f;
    bool  currentDisableCulling = false;

private:
    struct NodeAttribs {
        int   texOffsetX;
        int   texOffsetY;
        int   page;
        float modelScale;
        float modelAlpha;  // from culling node
        bool  disableCulling;
    };
    static constexpr u32 kMaxDepth = 128;
    std::array<NodeAttribs, kMaxDepth> m_vecAttribs{};
    u32 m_depth = 0;
};

struct Node {
    Viewport           viewport;
    std::vector<Model> models;
};

// -- LOD blend table -------------------------------------------------------------

struct LOD {
    float deleteSize;
    float blendFactor;
};

struct LODFeatureType {
    LOD lod[4];
};

struct LODBlendTable {
    LODFeatureType table[128];
};
static_assert(sizeof(LODBlendTable) == 128 * 4 * 8);

// -- PolyHeader -----------------------------------------------------------------

/*
Polygon Data

0x00:
xxxxxx-- -------- -------- --------     Specular
------x- -------- -------- --------     Clockwise winding
-------x xxxxxxxx xxxxxx-- --------     Polygon number (not always present)
-------- -------- ------xx --------     Discard
-------- -------- -------- x-------     Enable specular
-------- -------- -------- -x------     0 = Triangle, 1 = Quad
-------- -------- -------- --x-----     Poly is points
-------- -------- -------- ---x----     Anti-aliasing
-------- -------- -------- ----x---     Vertex 3 shared from previous polygon
-------- -------- -------- -----x--     Vertex 2 shared from previous polygon
-------- -------- -------- ------x-     Vertex 1 shared from previous polygon
-------- -------- -------- -------x     Vertex 0 shared from previous polygon

0x01:
xxxxxxxx xxxxxxxx xxxxxxxx--------      Polygon normal X coordinate(2.22 fixed point)
-------- -------- -------- x-------     Edge on translucency (probably anti-aliasing)
-------- -------- -------- -x------     UV scale (0 = 13.3, 1 = 16.0)
-------- -------- -------- --x-----     Fixed shading (seems to only be enabled if lighting is enabled)
-------- -------- -------- ---x----     1 = Double-sided polygon
-------- -------- -------- ----x---     1 = smooth shading, 0 = flat shading
-------- -------- -------- -----x--     If set, this is the last polygon
-------- -------- -------- ------x-     Poly color, 1 = RGB, 0 = color table
-------- -------- -------- -------x     No los return (line of sight)

0x02:
xxxxxxxx xxxxxxxx xxxxxxxx --------     Polygon normal Y coordinate(2.22 fixed point)
-------- -------- -------- xxx-----     Microtexture texture number 0-7
-------- -------- -------- ---x----     Microtexture enabled
-------- -------- -------- ----xx--     Microtexture min lod
-------- -------- -------- ------x-     Texture U mirror enable
-------- -------- -------- -------x     Texture V mirror enable

0x03:
xxxxxxxx xxxxxxxx xxxxxxxx --------     Polygon normal Z coordinate(2.22 fixed point)
-------- -------- -------- x-------     X wrap smoothing
-------- -------- -------- -x------     Y wrap smoothing
-------- -------- -------- --xxx---     Texture width(in 8 - pixel tiles)
-------- -------- -------- -----xxx     Texture height(in 8 - pixel tiles)

0x04:
xxxxxxxx xxxxxxxx xxxxxxxx --------     Color (RGB888 or two 12-bit indexes, sensor color and color)
-------- -------- -------- x-------     Translator map enable (multiplies colour values by 16)
-------- -------- -------- -x------     Texture page
-------- -------- -------- ---xxxxx     Upper 5 bits of texture U coordinate

0x05 :
xxxxxxxx xxxxxxxx xxxxxxxx --------     Texture NP
-------- -------- -------- x-------     Low bit of texture U coordinate
-------- -------- -------- ---xxxxx     Low 5 bits of texture V coordinate

0x06:
x------- -------- -------- --------     Alpha testing / contour
-xxxxxxx -------- -------- --------     Translator map offset?
-------- xxxxxx-- -------- --------     Polygon transparency 0-32
-------- ------x- -------- --------     Translucency pattern select (stipple alpha) - not sure if used?
-------- -------x -------- --------     1 = disable lighting
-------- -------- xxxxx--- --------     Polygon light modifier(Amount that a luminous polygon will burn through fog. Valid range is 0.0 to 1.0. 0.0 is completely fogged; 1.0 has no fog.)
-------- -------- -----x-- --------     Texture enable
-------- -------- ------xx x-------     Texture format
-------- -------- -------- -xx-----     Shininess
-------- -------- -------- ---x----     High priority
-------- -------- -------- ----x---     Layered polygon
-------- -------- -------- -----xxx     Translucency mode
*/

class PolyHeader {
public:
    PolyHeader() = default;
    explicit PolyHeader(const u32* h) : header(h) {}

    PolyHeader& operator=(const u32* h)
    {
        header = h;
        return *this;
    }

    [[nodiscard]] const u32* StartOfData() const { return header + 7; }  // the header is 7 words; data follows it

    bool NextPoly()
    {
        if (LastPoly()) {
            return false;
        }
        header += 7 + (NumVerts() - NumSharedVerts()) * 4;
        return true;
    }

    [[nodiscard]] int NumTrianglesTotal() const
    {
        PolyHeader walk(header);
        int count = (walk.NumVerts() == 4) ? 2 : 1;
        while (walk.NextPoly()) {
            count += (walk.NumVerts() == 4) ? 2 : 1;
        }
        return count;
    }

    // header 0
    [[nodiscard]] bool  SpecularEnabled() const { return (header[0] & 0x80) > 0; }
    [[nodiscard]] float SpecularValue() const { return static_cast<float>(header[0] >> 26) / 63.f; }  // 63 matches decompiled lib value
    [[nodiscard]] bool  Discard() const { return (header[0] & 0x100) && (header[0] & 0x200); }
    [[nodiscard]] bool  Discard1() const { return (header[0] & 0x200) > 0; }  // bit 1 only
    [[nodiscard]] bool  Discard2() const { return (header[0] & 0x100) > 0; }  // bit 2 only
    [[nodiscard]] int   NumVerts() const { return (header[0] & 0x40) ? 4 : 3; }
    [[nodiscard]] int   NumSharedVerts() const
    {
        static constexpr int sharedVerts[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
        return sharedVerts[header[0] & 0xf];
    }
    [[nodiscard]] bool SharedVertex(int vertex) const { return (header[0] & (1u << vertex)) > 0; }

    // header 1
    void FaceNormal(float n[3]) const
    {
        n[0] = static_cast<float>(static_cast<s32>(header[1]) >> 8) * static_cast<float>(1.0 / 4194304.0);
        n[1] = static_cast<float>(static_cast<s32>(header[2]) >> 8) * static_cast<float>(1.0 / 4194304.0);
        n[2] = static_cast<float>(static_cast<s32>(header[3]) >> 8) * static_cast<float>(1.0 / 4194304.0);
    }
    [[nodiscard]] float UVScale() const { return (header[1] & 0x40) ? 1.0f : (1.0f / 8.0f); }
    [[nodiscard]] bool  DoubleSided() const { return (header[1] & 0x10) != 0; }
    [[nodiscard]] bool  LastPoly() const { return (header[1] & 4) > 0; }
    [[nodiscard]] bool  PolyColor() const { return (header[1] & 2) > 0; }  // if false uses LUT from ram
    [[nodiscard]] bool  FixedShading() const { return (header[1] & 0x20) > 0; }
    [[nodiscard]] bool  SmoothShading() const { return (header[1] & 0x8) > 0; }
    [[nodiscard]] bool  NoLosReturn() const { return (header[1] & 0x1) > 0; }  // no line of sight return

    // header 2
    [[nodiscard]] bool TexUMirror() const { return (header[2] & 2) > 0; }
    [[nodiscard]] bool TexVMirror() const { return (header[2] & 1) > 0; }
    [[nodiscard]] bool MicroTexture() const { return (header[2] & 0x10) > 0; }
    [[nodiscard]] int  MicroTextureID() const { return static_cast<int>((header[2] >> 5) & 7); }
    [[nodiscard]] int  MicroTextureMinLOD() const { return static_cast<int>((header[2] >> 2) & 3); }

    // header 3
    [[nodiscard]] int TexWidth() const
    {
        u32 w = (header[3] >> 3) & 7;
        if (w >= 6) {
            w = 0;
        }
        return 32 << w;
    }
    [[nodiscard]] int TexHeight() const
    {
        u32 h = (header[3] >> 0) & 7;
        if (h >= 6) {
            h = 0;
        }
        return 32 << h;
    }
    [[nodiscard]] bool TexSmoothU() const { return (header[3] & 0x80) > 0; }
    [[nodiscard]] bool TexSmoothV() const { return (header[3] & 0x40) > 0; }

    // header 4
    [[nodiscard]] int  ColorIndex() const { return static_cast<int>((header[4] >> 8) & 0xFFF); }
    [[nodiscard]] bool TranslatorMap() const { return (header[4] & 0x80) > 0; }
    [[nodiscard]] int  Page() const { return static_cast<int>((header[4] & 0x40) >> 6); }

    // header 5
    [[nodiscard]] int X() const
    {
        int x = static_cast<int>(32 * (((header[4] & 0x1F) << 1) | ((header[5] >> 7) & 1)));
        x &= 2047;
        return x;
    }
    [[nodiscard]] int Y() const
    {
        int y = static_cast<int>(32 * (header[5] & 0x1F));  // if we hit 2nd page add 1024 to y coordinate
        y &= 2047;
        return y;
    }
    [[nodiscard]] float TextureNP() const { return static_cast<float>(header[5] >> 8); }

    // header 6
    [[nodiscard]] bool  Layered() const { return (header[6] & 0x8) > 0; }
    [[nodiscard]] float Shininess() const { return static_cast<float>((header[6] >> 5) & 3); }  // input sdk values are float 0-1 output are int 0-3
    [[nodiscard]] int   TexFormat() const { return static_cast<int>((header[6] >> 7) & 7); }
    [[nodiscard]] bool  TexEnabled() const { return (header[6] & 0x400) > 0; }
    [[nodiscard]] bool  LightEnabled() const { return !(header[6] & 0x00010000); }
    [[nodiscard]] bool  AlphaTest() const { return (header[6] & 0x80000000) > 0; }
    [[nodiscard]] u8    Transparency() const
    {
        if (header[6] & 0x800000) {  // Star Wars sets the top bit for opaque, with garbage below
            return 255;
        }
        return static_cast<u8>((((header[6] >> 18) & 0x3F) * 255) / 32);
    }
    [[nodiscard]] bool  PolyAlpha() const { return (header[6] & 0x00800000) == 0; }
    [[nodiscard]] bool  TextureAlpha() const { return (header[6] & 0x7) > 0; }
    [[nodiscard]] float LightModifier() const { return static_cast<float>((header[6] >> 11) & 0x1F) * (1.0f / 16.0f); }
    [[nodiscard]] bool  HighPriority() const { return (header[6] & 0x10) > 0; }
    [[nodiscard]] int   TranslatorMapOffset() const { return static_cast<int>((header[6] >> 24) & 0x7f); }
    [[nodiscard]] bool  TranslucencyPatternSelect() const { return (header[6] & 0x20000) > 0; }

    // misc
    [[nodiscard]] u64 Hash() const  // make a unique hash for sorting by state
    {
        u64 hash = 0;
        hash |= static_cast<u64>(header[3] & 0xFF);                                            // bits 0-7 tex width / height / uv smooth
        hash |= static_cast<u64>(((header[4] & 0x1F) << 1) | ((header[5] >> 7) & 1)) << 8;    // bits 8-13 x offset
        hash |= static_cast<u64>(header[5] & 0x1F) << 14;                                     // bits 14-18 y offset
        hash |= static_cast<u64>((header[4] & 0xC0) >> 6) << 19;                              // bits 19-20 page / translatormap
        hash |= static_cast<u64>(DoubleSided()) << 21;                                        // bits 21 double sided
        hash |= static_cast<u64>(AlphaTest()) << 22;                                          // bits 22 contour processing
        hash |= static_cast<u64>(PolyAlpha()) << 23;                                          // bits 23 poly alpha processing
        hash |= static_cast<u64>(header[2] & 0xFF) << 24;                                     // bits 24-31 microtexture / uv mirror
        hash |= static_cast<u64>(SpecularEnabled()) << 32;                                    // bits 32 enable specular reflection
        hash |= static_cast<u64>(SmoothShading()) << 33;                                      // bits 33 smooth shading
        hash |= static_cast<u64>(FixedShading()) << 34;                                       // bits 34 fixed shading
        hash |= static_cast<u64>(header[0] >> 26) << 35;                                      // bits 35-40 specular coefficient (opacity)
        hash |= static_cast<u64>(header[6] & 0x3FFFF) << 41;                                  // bits 41-58 Translucency pattern select / disable lighting / Polygon light modifier / Texture enable / Texture format / Shininess / High priority / Layered polygon / Translucency mode
        hash |= static_cast<u64>(NoLosReturn()) << 59;                                        // bits 59 no line of sight return
        return hash;
    }

    const u32* header = nullptr;
};

}  // namespace sm2::render::model3
