// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/model2rd.ipp, src/mame/sega/model2_v.cpp and
// src/devices/video/poly.h (BSD-3-Clause, copyright-holders R. Belmont, Olivier
// Galibert, ElSemi, Angelo Salese, Matthew Daniels, Aaron Giles).
//
// The arithmetic below is deliberately MAME's, in MAME's order, including the
// parts that look like rounding accidents: the point of this file is that any
// difference from MAME is a difference in sm2-emu, not in the port.

#include "hw/model2_softrender.h"

#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace sm2::hw {
namespace {

/// poly.h's round_coordinate. Rounds the midpoint towards negative infinity,
/// which MAME's own comment flags as questionable and which has to be kept
/// because it decides which pixel an edge lands on.
[[nodiscard]] inline s32 round_coordinate(float value)
{
    if (value >= static_cast<float>(std::numeric_limits<s32>::max())) {
        return std::numeric_limits<s32>::max();
    }
    const float ipart = std::floor(value);
    if (ipart < static_cast<float>(std::numeric_limits<s32>::min())) {
        return std::numeric_limits<s32>::min();
    }
    const float fpart = value - ipart;
    return static_cast<s32>(ipart) + ((fpart > 0.5F) ? 1 : 0);
}

/// model2rd.ipp's LERP, on two eight-bit lanes packed sixteen bits apart.
[[nodiscard]] constexpr u32 lerp(u32 x, u32 y, unsigned a)
{
    return (x + (((y - x) * a) >> 8)) & 0x00ff00ffu;
}

/// model2rd.ipp's fast_log2, taken there from the Voodoo renderer. Reproducing
/// the table rather than calling log2 keeps the mipmap level and the blend
/// between levels identical at every boundary.
[[nodiscard]] inline s32 fast_log2(float value)
{
    if (value < 0.0F) {
        return 0;
    }
    static constexpr u8 kTable[128] = {
        0,   2,   5,   8,   11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,
        43,  46,  48,  51,  53,  56,  58,  61,  63,  65,  68,  70,  73,  75,  77,  80,
        82,  84,  87,  89,  91,  93,  96,  98,  100, 102, 104, 106, 109, 111, 113, 115,
        117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147,
        149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177,
        179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205,
        206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230,
        232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};
    u32       ival = std::bit_cast<u32>(value) >> 16;
    const s32 exp  = static_cast<s32>(ival >> 7) - 127;
    return (exp << 8) | static_cast<s32>(kTable[ival & 127]);
}

/// model2rd.ipp's get_texel. The sheets are addressed as 2048x1024 but stored as
/// 1024x2048, so the right half of the logical sheet is the bottom half of the
/// stored one.
[[nodiscard]] inline u16 get_texel(u32 base_x, u32 base_y, s32 x, s32 y, const u32* sheet,
                                   usize words)
{
    s32 x2 = static_cast<s32>(base_x) + x;
    s32 y2 = static_cast<s32>(base_y) + y;
    if (x2 >= 1024) {
        x2 -= 1024;
        y2 ^= 1024;
    }
    const u32 offset = ((static_cast<u32>(y2) / 2) * 512) + (static_cast<u32>(x2) / 2);
    const u32 index  = offset >> 1;
    u32       texel  = index < words ? sheet[index] : 0u;

    if ((offset & 1) != 0) {
        texel >>= 16;
    }
    if ((y & 1) == 0) {
        texel >>= 8;
    }
    if ((x & 1) == 0) {
        texel >>= 4;
    }
    return static_cast<u16>(texel & 0x0f);
}

[[nodiscard]] inline u32 pack_rgba(u32 red, u32 green, u32 blue)
{
    return red | (green << 8) | (blue << 16) | 0xff000000u;
}

}  // namespace

SoftRenderer::SoftRenderer()
    : m_gamma(256, 0)
    , m_destmap(static_cast<usize>(kTargetWidth) * kTargetHeight, 0)
    , m_fillmap(static_cast<usize>(kTargetWidth) * kTargetHeight, 0)
{
    for (u32 index = 0; index < 256; ++index) {
        m_gamma[index] = static_cast<u8>(
            std::max((static_cast<double>(index) - 64.0) * 255.0 / 191.0, 0.0));
    }
}

u32 SoftRenderer::fetch_bilinear_texel(const SoftPolyExtra& object, s32 miplevel, s32 u, s32 v,
                                       bool translucent) const
{
    u32        tex_width  = 0;
    u32        tex_height = 0;
    u32        tex_x      = 0;
    u32        tex_y      = 0;
    const u32* sheet      = nullptr;
    usize      words      = 0;

    if (miplevel == -1) {
        tex_width  = 128;
        tex_height = 128;
        tex_x      = object.utexx;
        tex_y      = object.utexy;
        sheet      = object.texsheet[1];
        words      = object.texwords[1];
        u <<= 1 << object.utexminlod;
        v <<= 1 << object.utexminlod;
    } else {
        tex_width  = object.texwidth >> miplevel;
        tex_height = object.texheight >> miplevel;
        tex_x      = ((object.texx - 2048) >> miplevel) & 2047;
        tex_y      = ((object.texy - 1024) >> miplevel) & 1023;
        sheet      = object.texsheet[miplevel & 1];
        words      = object.texwords[miplevel & 1];
        u >>= miplevel;
        v >>= miplevel;
    }
    if (sheet == nullptr) {
        return 0;
    }

    if (object.texmirrorx != 0 && (u & static_cast<s32>(tex_width << 8)) != 0) {
        u = ~u;
    }
    if (object.texmirrory != 0 && (v & static_cast<s32>(tex_height << 8)) != 0) {
        v = ~v;
    }

    u -= 0x80;
    v -= 0x80;

    u32 ufrac = static_cast<u32>(u) & 0xff;
    u32 vfrac = static_cast<u32>(v) & 0xff;

    u32 u0 = static_cast<u32>(u >> 8) & (tex_width - 1);
    u32 u1 = (u0 + 1) & (tex_width - 1);
    u32 v0 = static_cast<u32>(v >> 8) & (tex_height - 1);
    u32 v1 = (v0 + 1) & (tex_height - 1);

    if (object.texwrapx == 0 && u1 == 0) {
        if (ufrac >= 0x80) {
            u0    = u1;
            u1++;
            ufrac = 0;
        } else {
            u1    = u0;
            u0--;
            ufrac = 0x100;
        }
    }
    if (object.texwrapy == 0 && v1 == 0) {
        if (vfrac >= 0x80) {
            v0    = 0;
            v1++;
            vfrac = 0;
        } else {
            v1    = v0;
            v0--;
            vfrac = 0x100;
        }
    }

    u32 tex00 = static_cast<u32>(get_texel(tex_x, tex_y, static_cast<s32>(u0),
                                           static_cast<s32>(v0), sheet, words))
              << 4;
    u32 tex01 = static_cast<u32>(get_texel(tex_x, tex_y, static_cast<s32>(u1),
                                           static_cast<s32>(v0), sheet, words))
              << 4;
    u32 tex10 = static_cast<u32>(get_texel(tex_x, tex_y, static_cast<s32>(u0),
                                           static_cast<s32>(v1), sheet, words))
              << 4;
    u32 tex11 = static_cast<u32>(get_texel(tex_x, tex_y, static_cast<s32>(u1),
                                           static_cast<s32>(v1), sheet, words))
              << 4;

    if (translucent) {
        if (tex00 != 0xf0) tex00 |= 0x00800000u;
        if (tex01 != 0xf0) tex01 |= 0x00800000u;
        if (tex10 != 0xf0) tex10 |= 0x00800000u;
        if (tex11 != 0xf0) tex11 |= 0x00800000u;

        if (tex00 == 0x000000f0) tex00 = tex01 & 0xff;
        if (tex01 == 0x000000f0) tex01 = tex00 & 0xff;
        if (tex10 == 0x000000f0) tex10 = tex11 & 0xff;
        if (tex11 == 0x000000f0) tex11 = tex10 & 0xff;
    }

    u32 tex0x = lerp(tex00, tex01, ufrac);
    u32 tex1x = lerp(tex10, tex11, ufrac);

    if (translucent) {
        if (tex0x == 0x000000f0) tex0x = tex1x & 0xff;
        if (tex1x == 0x000000f0) tex1x = tex0x & 0xff;
    }

    return lerp(tex0x, tex1x, vfrac);
}

void SoftRenderer::draw_scanline_solid(s32 scanline, const Extent& extent,
                                       const SoftPolyExtra& object, bool translucent)
{
    // A translucent untextured polygon has nothing to draw: there is no texel to
    // take an alpha from, so the hardware writes nothing at all.
    if (translucent) {
        return;
    }

    u32* const p    = &m_destmap[static_cast<usize>(scanline) * kTargetWidth];
    u8* const  fill = &m_fillmap[static_cast<usize>(scanline) * kTargetWidth];

    const u8 luma = static_cast<u8>(object.luma >> 2);

    u32 color = object.colorbase;
    color     = (color + 0x1000) < m_palram.size() ? (m_palram[color + 0x1000] & 0xffffu) : 0u;

    const usize red_base   = 0x0000 / 2 + ((static_cast<usize>(color >> 0) & 0x1f) << 8);
    const usize green_base = 0x4000 / 2 + ((static_cast<usize>(color >> 5) & 0x1f) << 8);
    const usize blue_base  = 0x8000 / 2 + ((static_cast<usize>(color >> 10) & 0x1f) << 8);

    const auto table = [this](usize base, u32 shade) -> u32 {
        const usize index = base + shade;
        return index < m_colorxlat.size() ? (m_colorxlat[index] & 0xffu) : 0u;
    };

    const u32 resolved = pack_rgba(m_gamma[table(red_base, luma)],
                                   m_gamma[table(green_base, luma)],
                                   m_gamma[table(blue_base, luma)]);

    s32       x  = extent.startx;
    const s32 dx = object.checker != 0 ? 2 : 1;
    if (object.checker != 0 && ((x ^ scanline) & 1) == 0) {
        x++;
    }

    for (; x < extent.stopx; x += dx) {
        if (fill[x] == 0) {
            p[x]    = resolved;
            fill[x] = 0xff;
            ++m_pixels;
        }
    }
}

void SoftRenderer::draw_scanline_tex(s32 scanline, const Extent& extent,
                                     const SoftPolyExtra& object, bool translucent)
{
    u32* const p    = &m_destmap[static_cast<usize>(scanline) * kTargetWidth];
    u8* const  fill = &m_fillmap[static_cast<usize>(scanline) * kTargetWidth];

    float ooz  = extent.start[0];
    float uoz  = extent.start[1];
    float voz  = extent.start[2];
    float dooz = extent.dpdx[0];
    float duoz = extent.dpdx[1];
    float dvoz = extent.dpdx[2];

    const s32 max_level =
        30 - std::countl_zero(std::min(object.texwidth, object.texheight));

    u32 colorbase = object.colorbase;
    colorbase     = (colorbase + 0x1000) < m_palram.size()
                      ? (m_palram[colorbase + 0x1000] & 0x7fffu)
                      : 0u;

    const usize red_base   = 0x0000 / 2 + ((static_cast<usize>(colorbase >> 0) & 0x1f) << 8);
    const usize green_base = 0x4000 / 2 + ((static_cast<usize>(colorbase >> 5) & 0x1f) << 8);
    const usize blue_base  = 0x8000 / 2 + ((static_cast<usize>(colorbase >> 10) & 0x1f) << 8);

    const auto table = [this](usize base, u32 shade) -> u32 {
        const usize index = base + shade;
        return index < m_colorxlat.size() ? (m_colorxlat[index] & 0xffu) : 0u;
    };

    s32 x  = extent.startx;
    s32 dx = 1;
    if (object.checker != 0) {
        if (((x ^ scanline) & 1) == 0) {
            x++;
            ooz += dooz;
            uoz += duoz;
            voz += dvoz;
        }
        dx = 2;
        dooz *= 2.0F;
        duoz *= 2.0F;
        dvoz *= 2.0F;
    }

    for (; x < extent.stopx; x += dx, ooz += dooz, uoz += duoz, voz += dvoz) {
        if (fill[x] > 0) {
            continue;
        }

        const float z = 1.0F / ooz;

        const s32 mml   = -object.texlod + fast_log2(z);
        const s32 level = std::clamp(mml >> 7, 0, max_level);

        const s32 u = static_cast<s32>(uoz * z * 256.0F);
        const s32 v = static_cast<s32>(voz * z * 256.0F);

        u32 t = fetch_bilinear_texel(object, level, u, v, translucent);

        if (mml > 0 && level < max_level) {
            const u32 t2   = fetch_bilinear_texel(object, level + 1, u, v, translucent);
            const s32 frac = (mml & 127) << 1;
            t              = lerp(t, t2, static_cast<unsigned>(frac));
        } else if (object.utex != 0 && mml < 0) {
            const u32 t2   = fetch_bilinear_texel(object, -1, u, v, translucent);
            const s32 frac = std::min(-mml >> object.utexminlod, 127);
            t              = lerp(t, t2, static_cast<unsigned>(frac));
        }

        if (translucent) {
            if (t < 0x00400000u) {
                continue;
            }
            t &= 0xff;
        }

        const usize luma_index = object.lumabase + (t >> 1);
        const u32   curve = luma_index < m_lumaram.size() ? m_lumaram[luma_index] : 0u;
        u8          luma  = static_cast<u8>(curve * object.luma / 256);
        luma              = std::min<u8>(luma, 0x3f);

        p[x] = pack_rgba(m_gamma[table(red_base, luma)], m_gamma[table(green_base, luma)],
                         m_gamma[table(blue_base, luma)]);
        fill[x] = 0xff;
        ++m_pixels;
    }
}

void SoftRenderer::render_triangle(const Rect& cliprect, const PolyVertex& in1,
                                   const PolyVertex& in2, const PolyVertex& in3, u32 renderer,
                                   const SoftPolyExtra& extra)
{
    const PolyVertex* v1 = &in1;
    const PolyVertex* v2 = &in2;
    const PolyVertex* v3 = &in3;

    if (v2->y < v1->y) std::swap(v1, v2);
    if (v3->y < v2->y) {
        std::swap(v2, v3);
        if (v2->y < v1->y) std::swap(v1, v2);
    }

    s32 v1yclip = std::max(round_coordinate(v1->y), cliprect.top);
    s32 v3yclip = std::min(round_coordinate(v3->y), cliprect.bottom + 1);
    if (v3yclip - v1yclip <= 0) {
        return;
    }

    const float dxdy_v1v2 = (v2->y == v1->y) ? 0.0F : (v2->x - v1->x) / (v2->y - v1->y);
    const float dxdy_v1v3 = (v3->y == v1->y) ? 0.0F : (v3->x - v1->x) / (v3->y - v1->y);
    const float dxdy_v2v3 = (v3->y == v2->y) ? 0.0F : (v3->x - v2->x) / (v3->y - v2->y);

    // Parameters are solved as a plane over the triangle, exactly as poly.h does.
    float param_start[3]{};
    float param_dpdx[3]{};
    float param_dpdy[3]{};
    {
        const float a00 = v2->y - v3->y;
        const float a01 = v3->x - v2->x;
        const float a02 = v2->x * v3->y - v3->x * v2->y;
        const float a10 = v3->y - v1->y;
        const float a11 = v1->x - v3->x;
        const float a12 = v3->x * v1->y - v1->x * v3->y;
        const float a20 = v1->y - v2->y;
        const float a21 = v2->x - v1->x;
        const float a22 = v1->x * v2->y - v2->x * v1->y;
        const float det = a02 + a12 + a22;

        if (std::abs(det) < 0.00001F) {
            for (int index = 0; index < 3; ++index) {
                param_dpdx[index]  = 0.0F;
                param_dpdy[index]  = 0.0F;
                param_start[index] = v1->p[index];
            }
        } else {
            const float idet = 1.0F / det;
            for (int index = 0; index < 3; ++index) {
                param_dpdx[index] =
                    idet * (v1->p[index] * a00 + v2->p[index] * a10 + v3->p[index] * a20);
                param_dpdy[index] =
                    idet * (v1->p[index] * a01 + v2->p[index] * a11 + v3->p[index] * a21);
                param_start[index] =
                    idet * (v1->p[index] * a02 + v2->p[index] * a12 + v3->p[index] * a22);
            }
        }
    }

    for (s32 curscan = v1yclip; curscan < v3yclip; ++curscan) {
        const float fully  = static_cast<float>(curscan) + 0.5F;
        const float startx = v1->x + (fully - v1->y) * dxdy_v1v3;
        const float stopx  = (fully < v2->y) ? (v1->x + (fully - v1->y) * dxdy_v1v2)
                                             : (v2->x + (fully - v2->y) * dxdy_v2v3);

        s32 istartx = round_coordinate(startx);
        s32 istopx  = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, cliprect.left);
        istopx  = std::min(istopx, cliprect.right + 1);
        if (istartx >= istopx) {
            continue;
        }

        Extent extent;
        extent.startx            = istartx;
        extent.stopx             = istopx;
        const float fullstartx   = static_cast<float>(istartx) + 0.5F;
        for (int index = 0; index < 3; ++index) {
            extent.start[index] =
                param_start[index] + fullstartx * param_dpdx[index] + fully * param_dpdy[index];
            extent.dpdx[index] = param_dpdx[index];
        }

        if ((renderer & 2) != 0) {
            draw_scanline_tex(curscan, extent, extra, (renderer & 1) != 0);
        } else {
            draw_scanline_solid(curscan, extent, extra, (renderer & 1) != 0);
        }
    }
}

void SoftRenderer::render_convex(const Rect& cliprect, const PolyVertex* v, u32 count,
                                 u32 renderer, const SoftPolyExtra& extra)
{
    // poly.h's render_polygon: two edge walks from the topmost vertex, one each
    // way round, then per-scanline interpolation along whichever pair of edges the
    // scanline crosses. Not a fan of triangles: the parameter interpolation is
    // linear along the edges, which is what the hardware does and what gives the
    // characteristic warp on a large quad.
    u32 minv = 0;
    u32 maxv = 0;
    for (u32 index = 1; index < count; ++index) {
        if (v[index].y < v[minv].y) {
            minv = index;
        } else if (v[index].y > v[maxv].y) {
            maxv = index;
        }
    }

    const s32 minyclip = std::max(round_coordinate(v[minv].y), cliprect.top);
    const s32 maxyclip = std::min(round_coordinate(v[maxv].y), cliprect.bottom + 1);
    if (maxyclip - minyclip <= 0) {
        return;
    }

    struct Edge {
        const PolyVertex* v1   = nullptr;
        const PolyVertex* v2   = nullptr;
        float             dxdy = 0.0F;
        float             dpdy[3]{};
    };

    Edge forward[8];
    Edge backward[8];
    u32  forward_count  = 0;
    u32  backward_count = 0;

    const auto build = [&](Edge* list, u32& written, bool ascending) {
        u32 curv = minv;
        while (curv != maxv) {
            const u32 next = ascending ? ((curv == count - 1) ? 0u : curv + 1)
                                       : ((curv == 0) ? count - 1 : curv - 1);
            Edge&     edge = list[written];
            edge.v1        = &v[curv];
            edge.v2        = &v[next];
            curv           = next;
            if (edge.v1->y == edge.v2->y) {
                continue;  // horizontal edges contribute nothing
            }
            const float ooy = 1.0F / (edge.v2->y - edge.v1->y);
            edge.dxdy       = (edge.v2->x - edge.v1->x) * ooy;
            for (int index = 0; index < 3; ++index) {
                edge.dpdy[index] = (edge.v2->p[index] - edge.v1->p[index]) * ooy;
            }
            ++written;
        }
    };
    build(forward, forward_count, true);
    build(backward, backward_count, false);

    if (forward_count == 0 || backward_count == 0) {
        return;
    }

    const Edge* ledge = nullptr;
    const Edge* redge = nullptr;
    if ((forward[0].v1 == backward[0].v1 && forward[0].dxdy < backward[0].dxdy)
        || (forward[0].v1 != backward[0].v1 && forward[0].v1->x < backward[0].v1->x)) {
        ledge = forward;
        redge = backward;
    } else {
        ledge = backward;
        redge = forward;
    }
    const Edge* lend = ledge + (ledge == forward ? forward_count : backward_count);
    const Edge* rend = redge + (redge == forward ? forward_count : backward_count);

    for (s32 curscan = minyclip; curscan < maxyclip; ++curscan) {
        const float fully = static_cast<float>(curscan) + 0.5F;
        while (fully > ledge->v2->y && fully < v[maxv].y && ledge + 1 < lend) {
            ++ledge;
        }
        while (fully > redge->v2->y && fully < v[maxv].y && redge + 1 < rend) {
            ++redge;
        }

        const float startx = ledge->v1->x + (fully - ledge->v1->y) * ledge->dxdy;
        const float stopx  = redge->v1->x + (fully - redge->v1->y) * redge->dxdy;

        s32 istartx = round_coordinate(startx);
        s32 istopx  = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, cliprect.left);
        istopx  = std::min(istopx, cliprect.right + 1);

        Extent extent;
        const float ldy = fully - ledge->v1->y;
        const float rdy = fully - redge->v1->y;
        const float oox = 1.0F / (stopx - startx);
        for (int index = 0; index < 3; ++index) {
            const float lparam = ledge->v1->p[index] + ldy * ledge->dpdy[index];
            const float rparam = redge->v1->p[index] + rdy * redge->dpdy[index];
            const float dpdx   = (rparam - lparam) * oox;
            extent.start[index] =
                lparam + (static_cast<float>(istartx) + 0.5F - startx) * dpdx;
            extent.dpdx[index] = dpdx;
        }

        if (istartx >= istopx) {
            continue;
        }
        extent.startx = istartx;
        extent.stopx  = istopx;

        if ((renderer & 2) != 0) {
            draw_scanline_tex(curscan, extent, extra, (renderer & 1) != 0);
        } else {
            draw_scanline_solid(curscan, extent, extra, (renderer & 1) != 0);
        }
    }
}

void SoftRenderer::draw_polygon(const RenderPolygon& poly, const Rect& cliprect)
{
    // MAME's model2_3d_render. The renderer index is two bits of the texture
    // header: bit 14 selects textured, bit 13 translucent.
    const u32 renderer = (poly.texheader[0] >> 13) & 3;

    // The geometry stage already applied the y flip and the CRTC offsets, so the
    // scissor is MAME's viewport rectangle with inclusive edges, intersected with
    // the raster the same way MAME's `vp &= cliprect` does.
    Rect vp;
    vp.left   = std::max<s32>(poly.scissor[0], cliprect.left);
    vp.top    = std::max<s32>(poly.scissor[1], cliprect.top);
    vp.right  = std::min<s32>(poly.scissor[2], cliprect.right);
    vp.bottom = std::min<s32>(poly.scissor[3], cliprect.bottom);
    if (vp.empty()) {
        return;
    }

    SoftPolyExtra extra;
    extra.checker   = (poly.texheader[0] >> 15) & 1;
    extra.lumabase  = static_cast<u32>(poly.texheader[1] & 0xff) << 7;
    extra.colorbase = (poly.texheader[3] >> 6) & 0x3ff;
    extra.luma      = poly.luma;
    extra.texlod    = poly.texlod;

    PolyVertex work[8];
    for (u32 index = 0; index < poly.num_vertices; ++index) {
        work[index] = poly.v[index];
    }

    if ((renderer & 2) != 0) {
        extra.texmirrorx = (poly.texheader[0] >> 8) & 1;
        extra.texmirrory = (poly.texheader[0] >> 9) & 1;
        extra.texwrapx   = ((poly.texheader[0] >> 6) & 1) & ~extra.texmirrorx;
        extra.texwrapy   = ((poly.texheader[0] >> 7) & 1) & ~extra.texmirrory;

        const bool second = (poly.texheader[2] & 0x1000) != 0;
        extra.texsheet[0] = second ? m_texture1.data() : m_texture0.data();
        extra.texsheet[1] = second ? m_texture0.data() : m_texture1.data();
        extra.texwords[0] = second ? m_texture1.size() : m_texture0.size();
        extra.texwords[1] = second ? m_texture0.size() : m_texture1.size();

        extra.texwidth  = 32u << ((poly.texheader[0] >> 0) & 0x7);
        extra.texheight = 32u << ((poly.texheader[0] >> 3) & 0x7);
        extra.texx      = 32u * ((poly.texheader[2] >> 0) & 0x3f);
        extra.texy      = 32u * ((poly.texheader[2] >> 6) & 0x1f);

        extra.utex       = (poly.texheader[0] >> 12) & 1;
        extra.utexminlod = (poly.texheader[0] >> 10) & 3;
        extra.utexx      = ((poly.texheader[2] >> 13) & 1) * 128;
        extra.utexy      = ((poly.texheader[2] >> 14) & 3) * 128;

        for (u32 index = 0; index < poly.num_vertices; ++index) {
            work[index].p[0] = 1.0F / (work[index].p[0] + std::numeric_limits<float>::min());
            work[index].p[1] = work[index].p[1] * work[index].p[0] * (1.0F / 8.0F);
            work[index].p[2] = work[index].p[2] * work[index].p[0] * (1.0F / 8.0F);
        }
    }

    if (poly.num_vertices == 3) {
        render_triangle(vp, work[0], work[1], work[2], renderer, extra);
    } else if (poly.num_vertices >= 4 && poly.num_vertices <= 8) {
        render_convex(vp, work, poly.num_vertices, renderer, extra);
    }
}

void SoftRenderer::render(const Model2MachineBase& machine, const RenderList& list,
                          std::span<u32> out)
{
    if (out.size() < static_cast<usize>(kWidth) * kHeight) {
        return;
    }

    m_palram    = machine.palette_ram();
    m_colorxlat = machine.colour_translate();
    m_lumaram   = machine.luma_ram();
    m_texture0  = machine.texture_ram(0);
    m_texture1  = machine.texture_ram(1);
    m_pixels    = 0;

    const Model2Video& video = machine.video();

    // The background pen first, as MAME's screen_update fills the bitmap with
    // m_palette->pen(0).
    std::fill(out.begin(), out.begin() + static_cast<usize>(kWidth) * kHeight,
              video.background());

    // Then the tilemap layers of priority category zero.
    const std::span<const u32> below = video.below();
    for (usize index = 0; index < static_cast<usize>(kWidth) * kHeight && index < below.size();
         ++index) {
        if ((below[index] >> 24) != 0) {
            out[index] = below[index];
        }
    }

    // Then the 3D, or the framebuffer in its place.
    if (!machine.render_test_mode()) {
        std::fill(m_destmap.begin(), m_destmap.end(), 0u);
        std::fill(m_fillmap.begin(), m_fillmap.end(), u8{0});

        const Rect cliprect{0, 0, static_cast<s32>(kWidth) - 1, static_cast<s32>(kHeight) - 1};
        for (const RenderPolygon& poly : list.polygons) {
            draw_polygon(poly, cliprect);
        }

        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const u32 pixel = m_destmap[static_cast<usize>(y) * kTargetWidth + x];
                if (pixel != 0) {
                    out[static_cast<usize>(y) * kWidth + x] = pixel;
                }
            }
        }
    }

    // Finally the layers of category one.
    const std::span<const u32> above = video.above();
    for (usize index = 0; index < static_cast<usize>(kWidth) * kHeight && index < above.size();
         ++index) {
        if ((above[index] >> 24) != 0) {
            out[index] = above[index];
        }
    }
}

}  // namespace sm2::hw
