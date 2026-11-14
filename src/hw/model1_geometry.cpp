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
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "hw/model1_geometry.h"

#include "core/archive.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sm2::hw {
namespace {

constexpr float kNear = 1.0e-3F;

[[nodiscard]] inline float u2f(u32 v)
{
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

[[nodiscard]] inline u32 pal5bit(u32 v)
{
    return (v << 3) | (v >> 2);
}

[[nodiscard]] inline float min4(float a, float b, float c, float d)
{
    return std::min(std::min(a, b), std::min(c, d));
}

[[nodiscard]] inline float max4(float a, float b, float c, float d)
{
    return std::max(std::max(a, b), std::max(c, d));
}

}  // namespace

Model1Geometry::Model1Geometry()
    : m_colour_ram(kColourWords, 0)
    , m_poly_ram(kPolyRamWords, 0)
{
    m_points.reserve(1 << 16);
    m_quads.reserve(1 << 15);
}

void Model1Geometry::reset()
{
    std::fill(m_colour_ram.begin(), m_colour_ram.end(), u16{0});
    std::fill(m_poly_ram.begin(), m_poly_ram.end(), 0u);
    m_lightparams.fill(LightParam{});
    m_view = View{};
    m_points.clear();
    m_quads.clear();
}

void Model1Geometry::serialize(Archive& ar)
{
    ar.bytes(m_colour_ram.data(), m_colour_ram.size());
    ar.bytes(m_poly_ram.data(), m_poly_ram.size());
    ar.bytes(m_lightparams.data(), m_lightparams.size());
    ar.raw(m_view);
}

// -- list and polygon data access ------------------------------------------------

u32 Model1Geometry::readi(u32 adr) const
{
    return m_list[adr & (kListWords - 1)] | (static_cast<u32>(m_list[(adr + 1) & (kListWords - 1)]) << 16);
}

s16 Model1Geometry::readi16(u32 adr) const
{
    return static_cast<s16>(m_list[adr & (kListWords - 1)]);
}

float Model1Geometry::readf(u32 adr) const
{
    return u2f(readi(adr));
}

u32 Model1Geometry::poly_word(u32 adr) const
{
    if ((adr & 0x800000) != 0) return m_poly_ram[adr & (kPolyRamWords - 1)];
    adr &= 0x7fffff;
    return adr < m_poly_rom.size() ? m_poly_rom[adr] : 0;
}

float Model1Geometry::poly_float(u32 adr) const
{
    return u2f(poly_word(adr));
}

// -- view --------------------------------------------------------------------------

void Model1Geometry::transform_point(Point& p) const
{
    const float* t = m_view.translation;
    const float  x = p.x, y = p.y, z = p.z;
    p.x = t[0] * x + t[3] * y + t[6] * z + t[9];
    p.y = t[1] * x + t[4] * y + t[7] * z + t[10];
    p.z = t[2] * x + t[5] * y + t[8] * z + t[11];
}

void Model1Geometry::project(Point& p, const Lens& lens) const
{
    p.sx = m_view.xc + (p.x / p.z * lens.zoomx + lens.viewx);
    p.sy = m_view.yc - (p.y / p.z * lens.zoomy + lens.viewy);
}

u32 Model1Geometry::current_lens()
{
    if (m_lens_changed || m_lenses.empty()) {
        const View& v = m_view;
        m_lenses.push_back({v.zoomx, v.zoomy, v.viewx, v.viewy, v.a_bottom, v.a_top, v.a_left, v.a_right});
        m_lens_changed = false;
    }
    return static_cast<u32>(m_lenses.size() - 1);
}

void Model1Geometry::recompute_frustum()
{
    View& v = m_view;
    m_lens_changed = true;
    if (v.zoomx == 0 || v.zoomy == 0) return;
    v.a_left   = (static_cast<float>(v.x1 - wide_left()) - v.xc - v.viewx) / v.zoomx;
    v.a_right  = (static_cast<float>(v.x2 + wide_right()) - v.xc - v.viewx) / v.zoomx;
    v.a_bottom = (-static_cast<float>(v.y1) + v.yc - v.viewy) / v.zoomy;
    v.a_top    = (-static_cast<float>(v.y2) + v.yc - v.viewy) / v.zoomy;
}

// -- colour ------------------------------------------------------------------------

u32 Model1Geometry::shade(u32 tex_adr, u32 lumval, bool flat_allowed) const
{
    const u16 tex_data = m_colour_ram[(tex_adr - 0x40000) % kColourWords];
    const u32 entry    = 0x1000 | (tex_data & 0x3ff);
    const u16 colour   = entry < m_palette.size() ? m_palette[entry] : 0;
    u32       r        = (colour >> 0) & 0x1f;
    u32       g        = (colour >> 5) & 0x1f;
    u32       b        = (colour >> 10) & 0x1f;

    if (flat_allowed) {
        // Bits 11:10 of the colour word: bit 10 is a flat element drawn unlit, and
        // mode 01 also blinks by rotating the channels on alternate frames.
        if (((tex_data >> 10) & 3) == 1 && (m_frame_number & 1) != 0) {
            const u32 tmp = b;
            b             = r;
            r             = g;
            g             = tmp;
        }
        if ((tex_data & 0x400) != 0) lumval = 0x3f;
    }

    const auto xlat = [this](u32 index) -> u32 {
        return index < m_xlat.size() ? (m_xlat[index] >> 3) & 0x1f : 0;
    };
    r = xlat((r << 8) | lumval | 0x0000);
    g = xlat((g << 8) | lumval | 0x2000);
    b = xlat((b << 8) | lumval | 0x4000);
    return (pal5bit(r) << 16) | (pal5bit(g) << 8) | pal5bit(b);
}

float Model1Geometry::specular(const float* n, float diffuse, int lightmode) const
{
    if (!m_view.spec_enable) return 0.0F;
    const LightParam& lp = m_lightparams[static_cast<usize>(lightmode) & 0xff];
    if (lp.p == 0 || lp.s <= 0.0F) return 0.0F;

    // z of the reflected light vector, R.z = 2(N.L)N.z - L.z, raised to a power
    // of two picked by the power field.
    float s = 2.0F * diffuse * n[2] - m_view.light[2];
    if (s <= 0.0F) return 0.0F;
    if (lp.p >= 2) s *= s;
    if (lp.p >= 4) s *= s;
    if (lp.p >= 7) s *= s;
    return std::min(s * lp.s, 1.0F);
}

// -- objects -----------------------------------------------------------------------

void Model1Geometry::push_object(u32 tex_adr, u32 poly_adr, u32 size, float& old_z)
{
    const usize first_point = m_points.size();
    const usize first_quad  = m_quads.size();
    push_object_quads(tex_adr, poly_adr, size, old_z);

    // A spread HUD: what the game draws with a view offset is the HUD, and an
    // object wholly on one side of the screen follows it out to that edge.
    const Lens& lens = m_lenses.empty() ? Lens{} : m_lenses.back();
    if (m_wide == 0 || m_spread_right == 0 || first_quad == m_quads.size() || lens.viewx == 0) return;
    float lo = 1e9F, hi = -1e9F;
    for (usize i = first_point; i < m_points.size(); ++i) {
        Point p = m_points[i];
        if (p.z <= kNear) continue;
        project(p, lens);
        lo = std::min(lo, p.sx);
        hi = std::max(hi, p.sx);
    }
    const float shift = hi < m_spread_left   ? -static_cast<float>(m_wide)
                      : lo >= m_spread_right ? static_cast<float>(m_wide)
                                             : 0.0F;
    if (shift == 0) return;
    for (usize i = first_quad; i < m_quads.size(); ++i) m_quads[i].shift = m_hud_hold != 0 ? shift : 0.0F;
    m_hud_sides |= shift < 0 ? 1 : 2;
}

void Model1Geometry::push_object_quads(u32 tex_adr, u32 poly_adr, u32 size, float& old_z)
{
    if (tex_adr == 0xffffffffu || size >= 0x1000000) return;
    if (size == 0) size = 0xffffffffu;

    const auto new_point = [this](float x, float y, float z) {
        Point p;
        p.x = x;
        p.y = y;
        p.z = z;
        transform_point(p);
        m_points.push_back(p);
        return static_cast<u32>(m_points.size() - 1);
    };

    u32 old_p0 = new_point(poly_float(poly_adr + 0), poly_float(poly_adr + 1), poly_float(poly_adr + 2));
    u32 old_p1 = new_point(poly_float(poly_adr + 3), poly_float(poly_adr + 4), poly_float(poly_adr + 5));
    poly_adr += 6;

    for (u32 i = 0; i < size; ++i) {
        const u32 flags = poly_word(poly_adr);
        const u32 type  = flags & 3;
        if (type == 0) break;
        if (m_points.size() > (1u << 22)) break;  // runaway list

        if ((flags & 0x00001000) != 0) ++tex_adr;
        // Bit 22 selects the second light parameter bank (netmerc).
        const int lightmode = static_cast<int>(((flags >> 17) & 15) | ((flags & 0x00400000) != 0 ? 0x80 : 0));

        float vn[3] = {poly_float(poly_adr + 1), poly_float(poly_adr + 2), poly_float(poly_adr + 3)};
        u32   p0    = new_point(poly_float(poly_adr + 4), poly_float(poly_adr + 5), poly_float(poly_adr + 6));
        u32   p1    = type == 2 ? new_point(poly_float(poly_adr + 4), poly_float(poly_adr + 5), poly_float(poly_adr + 6))
                                : new_point(poly_float(poly_adr + 7), poly_float(poly_adr + 8), poly_float(poly_adr + 9));
        const u32 link = (flags >> 8) & 3;

        if (link != 0) {
            const Point& a = m_points[old_p1];
            const Point& b = m_points[old_p0];
            const Point& c = m_points[p0];
            const float  x1 = b.x - a.x, y1 = b.y - a.y, z1 = b.z - a.z;
            const float  x2 = c.x - a.x, y2 = c.y - a.y, z2 = c.z - a.z;
            const float  det = a.x * (y1 * z2 - y2 * z1) + a.y * (z1 * x2 - z2 * x1) + a.z * (x1 * y2 - x2 * y1);

            if ((flags & 0x00004000) != 0 || det <= 0) {
                const float* t = m_view.translation;
                float n[3] = {t[0] * vn[0] + t[3] * vn[1] + t[6] * vn[2],
                              t[1] * vn[0] + t[4] * vn[1] + t[7] * vn[2],
                              t[2] * vn[0] + t[5] * vn[1] + t[8] * vn[2]};
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 0) {
                    n[0] /= len;
                    n[1] /= len;
                    n[2] /= len;
                }

                Quad q;
                q.p[0] = old_p1;
                q.p[1] = old_p0;
                q.p[2] = p0;
                q.p[3] = p1;
                const Point& d = m_points[p1];
                switch ((flags >> 10) & 3) {
                    case 0: q.z = old_z; break;
                    case 1: q.z = old_z = min4(a.z, b.z, c.z, d.z); break;
                    case 2: q.z = old_z = max4(a.z, b.z, c.z, d.z); break;
                    default: q.z = 0; break;
                }

                const float dif = n[0] * m_view.light[0] + n[1] * m_view.light[1] + n[2] * m_view.light[2];
                const LightParam& lp = m_lightparams[static_cast<usize>(lightmode) & 0xff];
                const float ln = lp.a + lp.d * std::max(0.0F, dif) + specular(n, dif, lightmode);
                const int   lum = std::clamp(static_cast<int>(255.0F * std::min(1.0F, ln)) >> 2, 0, 0x3f);
                q.colour = shade(tex_adr, static_cast<u32>(lum), true);
                q.moire  = (flags & 0x00002000) != 0;
                q.lens   = current_lens();
                m_quads.push_back(q);
            }
        }

        poly_adr += 10;
        switch (link) {
            case 0:
            case 2:
                old_p0 = p0;
                old_p1 = p1;
                break;
            case 1: old_p1 = p0; break;
            case 3: old_p0 = p1; break;
        }
    }
}

u32 Model1Geometry::push_direct(u32 list_offset)
{
    u32 tex_adr = readi(list_offset + 2);

    // Screen-space already: the viewport centre is the only transform.
    const auto new_point = [this](float x, float y, float z) {
        Point p;
        p.x  = x;
        p.y  = y;
        p.z  = z;
        p.sx = m_view.xc + x;
        p.sy = m_view.yc - y;
        m_points.push_back(p);
        return static_cast<u32>(m_points.size() - 1);
    };

    u32 old_p0 = new_point(readf(list_offset + 6), readf(list_offset + 8), readf(list_offset + 10));
    u32 old_p1 = new_point(readf(list_offset + 14), readf(list_offset + 16), readf(list_offset + 18));
    list_offset += 18;

    for (u32 guard = 0; guard < kListWords; ++guard) {
        const u32 flags = readi(list_offset + 2);
        const u32 type  = flags & 3;
        if (type == 0) break;
        if ((flags & 0x00001000) != 0) ++tex_adr;

        const u32 lum = readi(list_offset + 4);
        u32       p0, p1;
        float     z;
        if (type == 2) {
            p0 = new_point(readf(list_offset + 8), readf(list_offset + 10), readf(list_offset + 12));
            p1 = p0;
            z  = m_points[p0].z;
            list_offset += 12;
        } else {
            p0 = new_point(readf(list_offset + 8), readf(list_offset + 10), readf(list_offset + 12));
            p1 = new_point(readf(list_offset + 16), readf(list_offset + 18), readf(list_offset + 20));
            z  = readf(list_offset + 14);
            list_offset += 20;
        }

        const u32 link = (flags >> 8) & 3;
        if (link != 0) {
            Quad q;
            q.p[0]   = old_p1;
            q.p[1]   = old_p0;
            q.p[2]   = p0;
            q.p[3]   = p1;
            q.z      = z;
            q.direct = true;
            const int lumval = std::clamp(static_cast<int>(static_cast<float>(lum >> 24) * 2.0F) >> 2, 0, 0x3f);
            q.colour = shade(tex_adr, static_cast<u32>(lumval), false);
            q.moire  = (flags & 0x00002000) != 0;

            bool keep = true;
            if (q.moire) {
                // A stipple over the transparent pen is a fade toward the backdrop,
                // not a black fill.
                if ((m_colour_ram[(tex_adr - 0x40000) % kColourWords] & 0x3ff) == 0) {
                    const u16 pen = m_palette.empty() ? 0 : m_palette[0];
                    u32       r = pal5bit(pen & 0x1f), g = pal5bit((pen >> 5) & 0x1f), b = pal5bit((pen >> 10) & 0x1f);
                    if ((pen & 0x8000) == 0) {
                        r >>= 1;
                        g >>= 1;
                        b >>= 1;
                    }
                    q.colour = (r << 16) | (g << 8) | b;
                }
                // A record with all four corners on one point draws nothing.
                const Point& a = m_points[q.p[0]];
                bool         same = true;
                for (u32 k = 1; k < 4; ++k) {
                    const Point& o = m_points[q.p[k]];
                    same = same && o.sx == a.sx && o.sy == a.sy;
                }
                keep = !same;
            }
            if (keep) m_quads.push_back(q);
        }

        switch (link) {
            case 0:
            case 2:
                old_p0 = p0;
                old_p1 = p1;
                break;
            case 1: old_p1 = p0; break;
            case 3: old_p0 = p1; break;
        }
    }
    return list_offset + 4;
}

u32 Model1Geometry::skip_direct(u32 list_offset) const
{
    list_offset += 18;
    for (u32 guard = 0; guard < kListWords; ++guard) {
        const u32 type = readi(list_offset + 2) & 3;
        if (type == 0) break;
        list_offset += type == 2 ? 12 : 20;
    }
    return list_offset + 4;
}

// -- output ------------------------------------------------------------------------

void Model1Geometry::emit_screen(const Quad& q, const Point* points, u32 count, std::vector<RenderPolygon>* out)
{
    const float wide = static_cast<float>(m_wide);
    const auto  fill = [&](RenderPolygon& poly) {
        poly.texheader[0] = q.moire ? 0x8000 : 0;
        poly.direct_rgb   = 0x80000000u | q.colour;
        const int cols    = static_cast<int>(m_wide);
        poly.scissor[0]   = static_cast<s16>(std::clamp(m_view.x1 - wide_left() + cols, 0, 496 + 2 * cols));
        poly.scissor[1]   = static_cast<s16>(std::clamp(m_view.y1, 0, 384));
        poly.scissor[2]   = static_cast<s16>(std::clamp(m_view.x2 + 1 + wide_right() + cols, 0, 496 + 2 * cols));
        poly.scissor[3]   = static_cast<s16>(std::clamp(m_view.y2 + 1, 0, 384));
        if (poly.scissor[2] < poly.scissor[0]) poly.scissor[2] = poly.scissor[0];
        if (poly.scissor[3] < poly.scissor[1]) poly.scissor[3] = poly.scissor[1];
    };
    const auto vertex = [&](RenderPolygon& poly, u32 slot, const Point& p) {
        poly.v[slot].x    = p.sx + wide;
        poly.v[slot].y    = p.sy;
        poly.v[slot].p[0] = 1.0F;
    };

    // Two distinct corners is a wireframe edge; give it one native pixel of width.
    u32 distinct[2] = {0, 0};
    u32 ndist       = 1;
    for (u32 i = 1; i < count && ndist <= 2; ++i) {
        const Point& p    = points[i];
        bool         seen = false;
        for (u32 k = 0; k < ndist && k < 2; ++k) {
            const Point& o = points[distinct[k]];
            seen = seen || (std::fabs(o.sx - p.sx) < 0.5F && std::fabs(o.sy - p.sy) < 0.5F);
        }
        if (!seen) {
            if (ndist < 2) distinct[ndist] = i;
            ++ndist;
        }
    }
    if (ndist < 2) {
        // A single point is still one pixel.
        Point c[4] = {points[0], points[0], points[0], points[0]};
        c[0].sx -= 0.5F; c[0].sy -= 0.5F;
        c[1].sx += 0.5F; c[1].sy -= 0.5F;
        c[2].sx += 0.5F; c[2].sy += 0.5F;
        c[3].sx -= 0.5F; c[3].sy += 0.5F;
        RenderPolygon& poly = out->emplace_back();
        fill(poly);
        poly.num_vertices = 4;
        for (u32 i = 0; i < 4; ++i) vertex(poly, i, c[i]);
        return;
    }
    if (ndist == 2) {
        const Point& a  = points[distinct[0]];
        const Point& b  = points[distinct[1]];
        float        dx = b.sx - a.sx, dy = b.sy - a.sy;
        const float  len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) return;
        // Half a pixel either side, and half a pixel past each end.
        dx = dx / len * 0.5F;
        dy = dy / len * 0.5F;
        Point c[4] = {a, a, b, b};
        c[0].sx += -dy - dx; c[0].sy += dx - dy;
        c[1].sx += dy - dx;  c[1].sy += -dx - dy;
        c[2].sx += dy + dx;  c[2].sy += -dx + dy;
        c[3].sx += -dy + dx; c[3].sy += dx + dy;
        RenderPolygon& poly = out->emplace_back();
        fill(poly);
        poly.num_vertices = 4;
        for (u32 i = 0; i < 4; ++i) vertex(poly, i, c[i]);
        return;
    }

    // The board fills whole pixels, both ends of every span included, so a
    // shape covers half a pixel more on each side than its outline.
    Point grown[16];
    u32   n_grown = 0;
    for (u32 i = 0; i < count && n_grown < 16; ++i) {
        const Point& p = points[i];
        const Point& prev = n_grown > 0 ? grown[n_grown - 1] : points[count - 1];
        if (std::fabs(p.sx - prev.sx) < 1e-3F && std::fabs(p.sy - prev.sy) < 1e-3F) continue;
        grown[n_grown++] = p;
    }
    if (n_grown >= 3) {
        float cx = 0, cy = 0;
        for (u32 i = 0; i < n_grown; ++i) {
            cx += grown[i].sx;
            cy += grown[i].sy;
        }
        cx /= static_cast<float>(n_grown);
        cy /= static_cast<float>(n_grown);
        const auto outward = [&](const Point& a, const Point& b, float* nx, float* ny) {
            const float dx = b.sx - a.sx, dy = b.sy - a.sy;
            const float len = std::sqrt(dx * dx + dy * dy);
            *nx = dy / len;
            *ny = -dx / len;
            if (*nx * ((a.sx + b.sx) * 0.5F - cx) + *ny * ((a.sy + b.sy) * 0.5F - cy) < 0) {
                *nx = -*nx;
                *ny = -*ny;
            }
        };
        Point moved[16];
        for (u32 i = 0; i < n_grown; ++i) {
            const Point& prev = grown[(i + n_grown - 1) % n_grown];
            const Point& next = grown[(i + 1) % n_grown];
            float ax, ay, bx, by;
            outward(prev, grown[i], &ax, &ay);
            outward(grown[i], next, &bx, &by);
            float mx = ax + bx, my = ay + by;
            const float denom = 1.0F + ax * bx + ay * by;
            mx = denom > 1e-3F ? mx * 0.5F / denom : 0.0F;
            my = denom > 1e-3F ? my * 0.5F / denom : 0.0F;
            const float ml = std::sqrt(mx * mx + my * my);
            if (ml > 1.0F) {
                mx /= ml;
                my /= ml;
            }
            moved[i]     = grown[i];
            moved[i].sx += mx;
            moved[i].sy += my;
        }
        std::copy(moved, moved + n_grown, grown);
        points = grown;
        count  = n_grown;
    }

    // RenderPolygon holds eight corners; a ninth from clipping goes in a second fan.
    for (u32 start = 1; start + 1 < count;) {
        const u32      n    = std::min<u32>(count - start, 7);
        RenderPolygon& poly = out->emplace_back();
        fill(poly);
        poly.num_vertices = static_cast<u8>(n + 1);
        vertex(poly, 0, points[0]);
        for (u32 i = 0; i < n; ++i) vertex(poly, i + 1, points[start + i]);
        start += n - 1;
    }
}

void Model1Geometry::emit_clipped(const Quad& q, std::vector<RenderPolygon>* out)
{
    Point in[16];
    Point tmp[16];
    u32   count = 4;
    for (u32 i = 0; i < 4; ++i) in[i] = m_points[q.p[i]];

    if (q.direct) {
        emit_screen(q, in, count, out);
        return;
    }

    // Near plane first, then the four frustum sides, all in view space so
    // nothing is projected from behind the eye.
    const Lens& v = m_lenses[q.lens];
    const auto  plane = [&v](u32 which, const Point& p) -> float {
        switch (which) {
            case 0: return p.z - kNear;
            case 1: return p.z * v.a_bottom - p.y;
            case 2: return p.y - p.z * v.a_top;
            case 3: return p.x - p.z * v.a_left;
            default: return p.z * v.a_right - p.x;
        }
    };

    for (u32 which = 0; which < 5 && count >= 2; ++which) {
        u32 n = 0;
        for (u32 i = 0; i < count; ++i) {
            const Point& a  = in[i];
            const Point& b  = in[(i + 1) % count];
            const float  fa = plane(which, a);
            const float  fb = plane(which, b);
            if (fa >= 0) tmp[n++] = a;
            if ((fa >= 0) != (fb >= 0) && n < 16) {
                const float t = fa / (fa - fb);
                Point       c;
                c.x = a.x + (b.x - a.x) * t;
                c.y = a.y + (b.y - a.y) * t;
                c.z = a.z + (b.z - a.z) * t;
                tmp[n++] = c;
            }
            if (n >= 15) break;
        }
        count = n;
        std::copy(tmp, tmp + n, in);
    }
    if (count < 2) return;
    for (u32 i = 0; i < count; ++i) project(in[i], v);

    for (u32 i = 0; i < count; ++i) in[i].sx += q.shift;
    emit_screen(q, in, count, out);
}

void Model1Geometry::flush(std::vector<RenderPolygon>* out)
{
    if (out != nullptr && !m_quads.empty()) {
        // Farthest first; on a tie the earlier one first, as MAME's comparator.
        m_order.resize(m_quads.size());
        for (u32 i = 0; i < m_order.size(); ++i) m_order[i] = i;
        std::stable_sort(m_order.begin(), m_order.end(),
                         [this](u32 a, u32 b) { return m_quads[a].z > m_quads[b].z; });
        for (const u32 index : m_order) emit_clipped(m_quads[index], out);
    }
    m_quads.clear();
    m_points.clear();
    m_lenses.clear();
}

void Model1Geometry::walk(const Frame& frame, Pass pass, std::vector<RenderPolygon>* out)
{
    m_list         = frame.list;
    m_palette      = frame.palette;
    m_xlat         = frame.colour_xlat;
    m_frame_number = frame.frame_number;
    m_wide         = frame.wide_columns;
    m_spread_left  = frame.hud_spread_left;
    m_spread_right = frame.hud_spread_right;
    // The race HUD is on while frames move HUD to both edges, and stays on a
    // moment through a frame or two that does not.
    if (pass == Pass::Main) {
        m_hud_hold  = m_hud_sides == 3 ? 30 : m_hud_hold != 0 ? m_hud_hold - 1 : 0;
        m_hud_sides = 0;
    }
    if (m_list.size() < kListWords) return;

    std::fill(std::begin(m_view.translation), std::end(m_view.translation), 0.0F);
    m_view.translation[0] = m_view.translation[4] = m_view.translation[8] = 1.0F;
    float old_z = 0;
    const bool draw = pass != Pass::ScanOnly;

    u32 list_offset = 0;
    for (u32 guard = 0; guard < kListWords; ++guard) {
        const u32 type = readi(list_offset);
        switch (type) {
            case 0: list_offset += 2; break;
            case 1:
            case 0x41:
                if ((pass == Pass::Main && type == 1) || (pass == Pass::Overlay && type == 0x41)) {
                    push_object(readi(list_offset + 2), readi(list_offset + 4), readi(list_offset + 6), old_z);
                }
                list_offset += 8;
                break;
            case 2:
                list_offset = pass == Pass::Main ? push_direct(list_offset) : skip_direct(list_offset);
                break;
            case 3: {
                if (draw) flush(out);
                m_view.xc = readi16(list_offset + 4);
                m_view.yc = static_cast<float>(383 - (readi16(list_offset + 6) - 39));
                m_view.x1 = readi16(list_offset + 8);
                m_view.y2 = 383 - (readi16(list_offset + 10) - 39);
                m_view.x2 = readi16(list_offset + 12);
                m_view.y1 = 383 - (readi16(list_offset + 14) - 39);
                recompute_frustum();
                list_offset += 16;
                break;
            }
            case 4: {
                const u32 adr = readi(list_offset + 2);
                const u32 len = readi(list_offset + 4) + 1;
                if (pass != Pass::Overlay) {
                    for (u32 i = 0; i < len && i < kListWords; ++i) {
                        m_colour_ram[(adr - 0x40000 + i) % kColourWords] = static_cast<u16>(readi16(list_offset + 6 + 2 * i));
                    }
                }
                list_offset += 6 + len * 2;
                break;
            }
            case 5: {
                const u32 adr = readi(list_offset + 2);
                const u32 len = readi(list_offset + 4);
                if (pass != Pass::Overlay) {
                    for (u32 i = 0; i < len && i < kListWords; ++i) {
                        m_poly_ram[(adr - 0x800000 + i) & (kPolyRamWords - 1)] = readi(list_offset + 2 * i + 6);
                    }
                }
                list_offset += 6 + len * 2;
                break;
            }
            case 6: {
                const u32 adr = readi(list_offset + 2);
                const u32 len = readi(list_offset + 4);
                for (u32 i = 0; i < len && i < kListWords; ++i) {
                    const u32   v  = readi(list_offset + 6 + i * 2);
                    LightParam& lp = m_lightparams[(i + adr) & 0xff];
                    lp.d = static_cast<float>(v & 0xff) / 255.0F;
                    lp.a = static_cast<float>((v >> 8) & 0xff) / 255.0F;
                    lp.s = static_cast<float>((v >> 16) & 0xff) / 255.0F;
                    lp.p = static_cast<int>((v >> 24) & 0xff);
                }
                list_offset += 6 + len * 2;
                break;
            }
            case 7:
                m_view.spec_enable = (readi(list_offset + 2) & 1) != 0;
                list_offset += 4;
                break;
            case 8: list_offset += 4; break;
            case 9:
                m_view.zoomx = readf(list_offset + 2) * 4;
                m_view.zoomy = readf(list_offset + 4) * 4;
                recompute_frustum();
                list_offset += 6;
                break;
            case 0xa: {
                float       x = readf(list_offset + 2), y = readf(list_offset + 4), z = readf(list_offset + 6);
                const float len = std::sqrt(x * x + y * y + z * z);
                if (len > 0) {
                    x /= len;
                    y /= len;
                    z /= len;
                }
                m_view.light[0] = x;
                m_view.light[1] = y;
                m_view.light[2] = z;
                list_offset += 8;
                break;
            }
            case 0xb:
                for (u32 i = 0; i < 12; ++i) m_view.translation[i] = readf(list_offset + 2 + 2 * i);
                list_offset += 26;
                break;
            case 0xc:
                m_view.viewx = readf(list_offset + 2);
                m_view.viewy = readf(list_offset + 4);
                recompute_frustum();
                list_offset += 6;
                break;
            default:  // 0xf ends the list; anything else is not a command
                if (draw) flush(out);
                m_quads.clear();
                m_points.clear();
                m_lenses.clear();
                return;
        }
    }
    if (draw) flush(out);
    m_quads.clear();
    m_points.clear();
    m_lenses.clear();
}

void Model1Geometry::render(const Frame& frame, RenderList* out, RenderList* overlay)
{
    // Painter's order per viewport, then reversed: the RenderList is nearest
    // first and the renderers keep the first write to a pixel.
    out->clear();
    walk(frame, Pass::Main, &out->polygons);
    std::reverse(out->polygons.begin(), out->polygons.end());
    if (overlay != nullptr) {
        overlay->clear();
        walk(frame, Pass::Overlay, &overlay->polygons);
        std::reverse(overlay->polygons.begin(), overlay->polygons.end());
    }
}

void Model1Geometry::scan(const Frame& frame)
{
    walk(frame, Pass::ScanOnly, nullptr);
}

}  // namespace sm2::hw
