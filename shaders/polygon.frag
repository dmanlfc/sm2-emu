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
// Model 2's pixel stage: fetch a texel, then turn it into a colour.
//
// Ported from MAME's src/mame/sega/model2rd.ipp and model2_v.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels). The integer arithmetic below is deliberately the same
// arithmetic, in the same order, because none of it is standard: this hardware has
// no RGB textures, no per-vertex shading and no alpha blending, and it reaches its
// colours by a route with no modern equivalent.
//
// A texel is four bits of *intensity*, not colour. Colour arrives afterwards:
//
//   1. Filter the texel. Bilinear within a mipmap level, then linear between two
//      levels, all in fixed point on the four-bit values scaled to eight bits.
//   2. Look the filtered intensity up in the polygon's own tone curve, a 128-entry
//      table in luminance RAM chosen by the texture header, and scale the result
//      by the polygon's lighting term. That gives six bits of shade.
//   3. Index the master colour translation table with that shade and the polygon's
//      five-bit red, green and blue components, one lookup per channel, then run
//      each through the monitor gamma ramp.
//
// Step 2 is why none of this can be precomputed on the host: the shade depends on
// the texel, and the tone curve is applied after filtering, so it cannot be baked
// into the texture. Steps 1 to 3 collapse here into three lookups from one image
// that the host has already flattened.

// Early tests are only safe for runs that never discard: a discarding polygon
// leaves its stencil unclaimed, which an early test would wrongly hand to a
// farther polygon.
#ifdef SM2_EARLY_FRAGMENT_TESTS
layout(early_fragment_tests) in;
#endif

layout(location = 0) in vec2      vTexel;
layout(location = 1) in flat uint vPolygon;

layout(location = 0) out vec4 fragColour;

// ---------------------------------------------------------------------------
// Machine memory
// ---------------------------------------------------------------------------

/// Everything the pixel stage needs to know about one polygon.
///
/// Unpacked by the host from the polygon's texture header, so the bit twiddling
/// that decoding needs happens once per polygon rather than once per pixel.
struct PolyParams {
    uint colour;       ///< five-bit red, green, blue in successive bytes
    uint lumaBase;     ///< start of this polygon's tone curve in luminance RAM
    uint lumaScale;    ///< the geometry engine's lighting term, 0..255
    int  texLod;       ///< level-of-detail bias, 8.7 fixed point
    uint texWidth;
    uint texHeight;
    uint texX;         ///< position of level 0 in the texture sheet
    uint texY;
    uint flags;
    uint microX;       ///< position of the microtexture, always 128 by 128
    uint microY;
    uint microMinLod;
};

/// Both texture sheets, decoded ahead of time by texel_decode.comp from the
/// hardware's packed 4-bit form into one pixel per 2x2 texel container: R is the
/// texel at even y, even x; G at even y, odd x; B at odd y, even x; A at odd y,
/// odd x. One array layer per sheet. Decoding on the host would defeat the point
/// -- the mipmap fold below depends on the polygon being drawn, not just the
/// physical address -- so what moved to the decode pass is only the word/byte/
/// nibble split, which does not. Sampled with texelFetch, never filtered: this
/// shader's own bilinear blend below is what combines a container's neighbours,
/// and a hardware filter would double it.
layout(set = 0, binding = 0) uniform usampler2DArray uSheets;

/// Luminance RAM, thirty-two kilobytes of tone curves, four bytes to a word.
layout(std430, set = 0, binding = 1) readonly buffer Luma { uint uLuma[]; };

/// The colour translation table and the gamma ramp, flattened by the host: shade
/// across, colour component down, one channel per component of the texel.
layout(set = 0, binding = 2) uniform sampler2D uTone;

layout(std430, set = 0, binding = 3) readonly buffer Polys { PolyParams uPolygon[]; };

const uint kFlagTextured  = 1u << 0;
const uint kFlagChecker   = 1u << 1;
const uint kFlagWrapX     = 1u << 2;
const uint kFlagWrapY     = 1u << 3;
const uint kFlagMirrorX   = 1u << 4;
const uint kFlagMirrorY   = 1u << 5;
const uint kFlagSheet     = 1u << 6;
const uint kFlagMicro     = 1u << 7;
/// Deepest mipmap level, in bits 11:8. Levels stop at two by two.
const uint kMaxLevelShift = 8u;
const uint kFlagTranslucent = 1u << 12;

// ---------------------------------------------------------------------------
// Texel fetch
// ---------------------------------------------------------------------------

/// One four-bit texel.
///
/// The sheets are addressed as 2048 by 1024 but stored as 1024 by 2048, hence the
/// fold: the right half of the logical sheet lives in the bottom half of the
/// stored one. Texels are then grouped two by two into each halfword, and the
/// parity that picks between them is that of the *logical* coordinate rather than
/// the stored one, which is how the hardware behaves and what makes a mipmap
/// level's odd base position work out.
///
/// `sheet` is the array layer, 0 or 1, replacing the packed buffer's byte offset
/// of a sheet's own base; the decode pass has already turned that into the array
/// dimension. What is left here is exactly the fold and the four-way channel pick
/// texel_decode.comp's own header describes, now against a decoded pixel instead
/// of a raw word.
uint fetchTexel(uint sheet, uint baseX, uint baseY, uint x, uint y)
{
    uint x2 = baseX + x;
    uint y2 = baseY + y;
    if (x2 >= 1024u) {
        x2 -= 1024u;
        y2 ^= 1024u;
    }

    const uvec4 nibbles = texelFetch(uSheets, ivec3(int(x2 >> 1), int(y2 >> 1), int(sheet)), 0);

    // R/G/B/A hold (even y, even x) / (even y, odd x) / (odd y, even x) /
    // (odd y, odd x), matching texel_decode.comp's layout exactly.
    const uint row    = ((y2 & 1u) == 0u) ? nibbles.x : nibbles.z;  // even-x column
    const uint rowOdd = ((y2 & 1u) == 0u) ? nibbles.y : nibbles.w;  // odd-x column
    return ((x2 & 1u) == 0u) ? row : rowOdd;
}

/// Fixed-point interpolation on one eight-bit lane, as the hardware model does it.
int lerp8(int x, int y, int a)
{
    return (x + (((y - x) * a) >> 8)) & 0xff;
}

/// The same on a texel's two lanes at once.
///
/// MAME packs intensity and alpha into one word sixteen bits apart and
/// interpolates them with a single masked add. Splitting them into a pair is the
/// same arithmetic with the lanes named.
ivec2 lerp2(ivec2 x, ivec2 y, int a)
{
    return ivec2(lerp8(x.x, y.x, a), lerp8(x.y, y.y, a));
}

/// The brightest intensity is the transparent one. There is no gradation: a texel
/// is either wholly there or wholly absent, so alpha is one bit, widened to eight
/// for the filter to have something to interpolate.
const int kTransparent    = 0xf0;
const int kOpaque         = 0x80;
/// Half of kOpaque. Below this the pixel is dropped.
const int kAlphaThreshold = 0x40;

bool isTransparent(ivec2 texel)
{
    return texel.x == kTransparent && texel.y == 0;
}

/// Bilinear filter within one mipmap level, or within the microtexture at -1.
///
/// `u` and `v` are texel coordinates with eight fractional bits. Returns intensity
/// in x and alpha in y; alpha is meaningful only when `translucent`.
ivec2 filterLevel(PolyParams p, int level, int u, int v, bool translucent)
{
    uint texWidth;
    uint texHeight;
    uint texX;
    uint texY;
    uint sheet = ((p.flags & kFlagSheet) != 0u) ? 1u : 0u;

    if (level < 0) {
        // The microtexture is a fixed 128 by 128 patch on the other sheet, sampled
        // at a coarser rate so it can add detail closer than level zero resolves.
        texWidth  = 128u;
        texHeight = 128u;
        texX      = p.microX;
        texY      = p.microY;
        sheet    ^= 1u;
        u <<= int(1u << p.microMinLod);
        v <<= int(1u << p.microMinLod);
    } else {
        texWidth  = p.texWidth >> level;
        texHeight = p.texHeight >> level;
        // Deliberately unsigned and deliberately wrapping: the subtraction
        // underflows and the shift then places each level where the hardware keeps
        // it, which is not simply the base position scaled down.
        texX   = ((p.texX - 2048u) >> level) & 2047u;
        texY   = ((p.texY - 1024u) >> level) & 1023u;
        sheet ^= uint(level & 1);
        u >>= level;
        v >>= level;
    }

    // Mirroring folds every other repeat of the texture back on itself.
    if ((p.flags & kFlagMirrorX) != 0u && (u & int(texWidth << 8)) != 0) {
        u = ~u;
    }
    if ((p.flags & kFlagMirrorY) != 0u && (v & int(texHeight << 8)) != 0) {
        v = ~v;
    }

    // Half a texel, so the four samples straddle the sample point.
    u -= 0x80;
    v -= 0x80;

    int ufrac = u & 0xff;
    int vfrac = v & 0xff;

    uint u0 = uint(u >> 8) & (texWidth - 1u);
    uint u1 = (u0 + 1u) & (texWidth - 1u);
    uint v0 = uint(v >> 8) & (texHeight - 1u);
    uint v1 = (v0 + 1u) & (texHeight - 1u);

    // Without smooth wrapping the filter must not reach across the seam, so the
    // pair collapses onto whichever edge the sample point is nearer and the
    // fraction is forced to pick it outright.
    if ((p.flags & kFlagWrapX) == 0u && u1 == 0u) {
        if (ufrac >= 0x80) {
            u0    = u1;
            u1    = u1 + 1u;
            ufrac = 0;
        } else {
            u1    = u0;
            u0    = u0 - 1u;
            ufrac = 0x100;
        }
    }
    if ((p.flags & kFlagWrapY) == 0u && v1 == 0u) {
        if (vfrac >= 0x80) {
            v0    = 0u;
            v1    = v1 + 1u;
            vfrac = 0;
        } else {
            v1    = v0;
            v0    = v0 - 1u;
            vfrac = 0x100;
        }
    }

    // Four bits scaled to eight, so the filter has somewhere to interpolate.
    ivec2 tex00 = ivec2(int(fetchTexel(sheet, texX, texY, u0, v0)) << 4, 0);
    ivec2 tex01 = ivec2(int(fetchTexel(sheet, texX, texY, u1, v0)) << 4, 0);
    ivec2 tex10 = ivec2(int(fetchTexel(sheet, texX, texY, u0, v1)) << 4, 0);
    ivec2 tex11 = ivec2(int(fetchTexel(sheet, texX, texY, u1, v1)) << 4, 0);

    if (translucent) {
        tex00.y = (tex00.x != kTransparent) ? kOpaque : 0;
        tex01.y = (tex01.x != kTransparent) ? kOpaque : 0;
        tex10.y = (tex10.x != kTransparent) ? kOpaque : 0;
        tex11.y = (tex11.x != kTransparent) ? kOpaque : 0;

        // A transparent texel borrows its neighbour's intensity while staying
        // transparent itself. Without this the filter would fade a cutout's edge
        // towards whatever the unused transparent entry happens to mean, which
        // shows up as a bright or dark halo. The order matters: each substitution
        // can see the previous one's result.
        if (isTransparent(tex00)) { tex00 = ivec2(tex01.x, 0); }
        if (isTransparent(tex01)) { tex01 = ivec2(tex00.x, 0); }
        if (isTransparent(tex10)) { tex10 = ivec2(tex11.x, 0); }
        if (isTransparent(tex11)) { tex11 = ivec2(tex10.x, 0); }
    }

    ivec2 tex0x = lerp2(tex00, tex01, ufrac);
    ivec2 tex1x = lerp2(tex10, tex11, ufrac);

    if (translucent) {
        // Again after the horizontal pass, because a pair of transparent texels
        // interpolates to a value that is still transparent.
        if (isTransparent(tex0x)) { tex0x = ivec2(tex1x.x, 0); }
        if (isTransparent(tex1x)) { tex1x = ivec2(tex0x.x, 0); }
    }

    return lerp2(tex0x, tex1x, vfrac);
}

/// Fixed-point base-two logarithm, 8.8, from the exponent and the top seven
/// mantissa bits. MAME takes it from its 3dfx Voodoo renderer; reproducing the
/// table rather than calling log2 keeps the mipmap level and the blend between
/// levels identical at every boundary.
// Module-scope so the compiler materialises it once, not once per fragment:
// some GLSL compilers (the V3D tiler among them) rebuild a function-local const
// array on every invocation. Same table, none of the per-pixel cost.
const uint kLog2Table[128] = uint[128](
      0u,   2u,   5u,   8u,  11u,  14u,  16u,  19u,  22u,  25u,  27u,  30u,
     33u,  35u,  38u,  40u,  43u,  46u,  48u,  51u,  53u,  56u,  58u,  61u,
     63u,  65u,  68u,  70u,  73u,  75u,  77u,  80u,  82u,  84u,  87u,  89u,
     91u,  93u,  96u,  98u, 100u, 102u, 104u, 106u, 109u, 111u, 113u, 115u,
    117u, 119u, 121u, 123u, 125u, 127u, 129u, 132u, 134u, 136u, 138u, 140u,
    141u, 143u, 145u, 147u, 149u, 151u, 153u, 155u, 157u, 159u, 161u, 162u,
    164u, 166u, 168u, 170u, 172u, 173u, 175u, 177u, 179u, 181u, 182u, 184u,
    186u, 188u, 189u, 191u, 193u, 194u, 196u, 198u, 200u, 201u, 203u, 205u,
    206u, 208u, 209u, 211u, 213u, 214u, 216u, 218u, 219u, 221u, 222u, 224u,
    225u, 227u, 229u, 230u, 232u, 233u, 235u, 236u, 238u, 239u, 241u, 242u,
    244u, 245u, 247u, 248u, 250u, 251u, 253u, 254u);

int fastLog2(float value)
{
    if (value < 0.0) {
        return 0;
    }
    const uint ival = floatBitsToUint(value) >> 16;
    const int  exp  = int(ival >> 7) - 127;
    return (exp << 8) | int(kLog2Table[ival & 127u]);
}

/// One byte of luminance RAM.
uint fetchLuma(uint index)
{
    const uint word = uLuma[index >> 2];
    return (word >> ((index & 3u) * 8u)) & 0xffu;
}

void main()
{
    const PolyParams p = uPolygon[vPolygon];

    // Translucency by stipple: a screen-locked checkerboard, so it has to be
    // evaluated in raster pixels. This is one of the two reasons the 3D output is
    // rasterised at the machine's own resolution rather than the window's.
    if ((p.flags & kFlagChecker) != 0u
        && ((int(gl_FragCoord.x) ^ int(gl_FragCoord.y)) & 1) == 0) {
        discard;
    }

    uint shade;

    if ((p.flags & kFlagTextured) != 0u) {
        // The rasteriser handed us u/z, v/z and 1/z interpolated linearly; this
        // recovers the depth the hardware would have divided by.
        const float z = 1.0 / gl_FragCoord.w;

        // Texture points carry three fractional bits, and the fixed-point
        // coordinates here carry eight.
        const int u = int(vTexel.x * 32.0);
        const int v = int(vTexel.y * 32.0);

        // Level of detail from the depth against the polygon's own bias. The
        // fractional part blends between two levels; a negative value means the
        // surface is nearer than level zero resolves, which is what the
        // microtexture is for.
        const int mml      = -p.texLod + fastLog2(z);
        const int maxLevel = int((p.flags >> kMaxLevelShift) & 0xfu);
        const int level    = clamp(mml >> 7, 0, maxLevel);

        const bool translucent = (p.flags & kFlagTranslucent) != 0u;

        ivec2 texel = filterLevel(p, level, u, v, translucent);
        if (mml > 0 && level < maxLevel) {
            texel = lerp2(texel, filterLevel(p, level + 1, u, v, translucent),
                          (mml & 127) << 1);
        } else if ((p.flags & kFlagMicro) != 0u && mml < 0) {
            // Blended in to just short of half, so the microtexture adds detail
            // without replacing the surface.
            texel = lerp2(texel, filterLevel(p, -1, u, v, translucent),
                          min(-mml >> int(p.microMinLod), 127));
        }

        // Translucency is an alpha test, not a blend: this hardware cannot mix two
        // colours in one pixel. Discarding rather than writing is also what leaves
        // the stencil fill mask unclaimed, so whatever is further away still gets
        // its chance at the pixel.
        if (translucent && texel.y < kAlphaThreshold) {
            discard;
        }

        // The filtered texel has eight bits but the tone curve has 128 entries, so
        // its low bit is dropped. Six bits of shade come out, and the clamp is
        // MAME's: Virtua Striker sets up a curve that would otherwise overrun.
        const uint curve = fetchLuma(p.lumaBase + (uint(texel.x) >> 1));
        shade = min((curve * p.lumaScale) / 256u, 0x3fu);
    } else {
        // Untextured, so the polygon's lighting term is the shade outright.
        shade = p.lumaScale >> 2;
    }

    // Three lookups, one per channel, each with its own colour component.
    const int  s = int(shade);
    const vec3 colour = vec3(texelFetch(uTone, ivec2(s, (p.colour >> 0) & 0x1fu), 0).r,
                             texelFetch(uTone, ivec2(s, (p.colour >> 8) & 0x1fu), 0).g,
                             texelFetch(uTone, ivec2(s, (p.colour >> 16) & 0x1fu), 0).b);

    // Opaque, and premultiplied trivially, because the compositor blends this
    // against the tilemap layers behind it.
    fragColour = vec4(colour, 1.0);
}
