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
// Model 1 3D: the display list the TGP and V60 leave in list RAM, walked into a
// RenderList of flat-coloured screen-space polygons. Command set, lighting and
// colour from MAME's sega/model1_v.cpp (BSD-3-Clause, Olivier Galibert).
#pragma once

#include "core/types.h"
#include "hw/geometrizer.h"

#include <array>
#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class Model1Geometry {
public:
    static constexpr u32 kListWords   = 0x8000;
    static constexpr u32 kColourWords = 0xc0000;
    static constexpr u32 kPolyRamWords = 0x40000;  // what MAME saves of it

    Model1Geometry();

    void attach(std::span<const u32> poly_rom) { m_poly_rom = poly_rom; }
    void reset();

    struct Frame {
        std::span<const u16> list;
        std::span<const u16> palette;
        std::span<const u16> colour_xlat;
        u64                  frame_number = 0;
        u32                  wide_columns = 0;
        /// Widescreen HUD split columns (GameSpec::hud_spread_*), 0 for none.
        u16                  hud_spread_left  = 0;
        u16                  hud_spread_right = 0;
    };

    /// Walk the list and produce this frame's polygons, nearest first. Objects
    /// the hardware draws over the HUD tilemaps go to `overlay`.
    void render(const Frame& frame, RenderList* out, RenderList* overlay);

    /// Apply only the list's uploads (colours, polygon RAM, light parameters),
    /// for a frame the display does not draw.
    void scan(const Frame& frame);

    void serialize(Archive& ar);

    /// Whether the last render moved HUD objects to the widescreen edges.
    [[nodiscard]] bool hud_spread() const { return m_hud_hold != 0; }

private:
    struct Point {
        float x = 0, y = 0, z = 0;
        float sx = 0, sy = 0;
    };

    struct Quad {
        u32   p[4]{};
        float z      = 0;
        u32   colour = 0;  ///< 0x00rrggbb
        bool  moire  = false;
        bool  direct = false;
        float shift  = 0;  ///< widescreen HUD shift, in columns
        u32   lens   = 0;  ///< m_lenses entry it was pushed under
    };

    /// The zoom, view offset and frustum a quad is clipped and projected with.
    /// They can change between objects inside one viewport.
    struct Lens {
        float zoomx = 0, zoomy = 0, viewx = 0, viewy = 0;
        float a_bottom = 0, a_top = 0, a_left = 0, a_right = 0;
    };

    struct LightParam {
        float a = 0, d = 0, s = 0;
        int   p = 0;
    };

    struct View {
        float xc = 0, yc = 0;
        int   x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        float zoomx = 0, zoomy = 0, viewx = 0, viewy = 0;
        float a_bottom = 0, a_top = 0, a_left = 0, a_right = 0;
        float translation[12]{};
        float light[3]{};
        bool  spec_enable = false;
    };

    enum class Pass { Main, Overlay, ScanOnly };

    void walk(const Frame& frame, Pass pass, std::vector<RenderPolygon>* out);
    void flush(std::vector<RenderPolygon>* out);

    [[nodiscard]] u32   readi(u32 adr) const;
    [[nodiscard]] s16   readi16(u32 adr) const;
    [[nodiscard]] float readf(u32 adr) const;
    [[nodiscard]] float poly_float(u32 adr) const;
    [[nodiscard]] u32   poly_word(u32 adr) const;

    void transform_point(Point& p) const;
    void project(Point& p, const Lens& lens) const;
    [[nodiscard]] u32 current_lens();
    void recompute_frustum();

    /// In widescreen a viewport that reaches a screen edge carries on into the
    /// added columns on that side.
    [[nodiscard]] int wide_left() const { return m_view.x1 <= 0 ? static_cast<int>(m_wide) : 0; }
    [[nodiscard]] int wide_right() const { return m_view.x2 >= 495 ? static_cast<int>(m_wide) : 0; }

    void push_object(u32 tex_adr, u32 poly_adr, u32 size, float& old_z);
    void push_object_quads(u32 tex_adr, u32 poly_adr, u32 size, float& old_z);
    u32  push_direct(u32 list_offset);
    [[nodiscard]] u32 skip_direct(u32 list_offset) const;

    [[nodiscard]] u32 shade(u32 tex_adr, u32 lumval, bool blink) const;
    [[nodiscard]] float specular(const float* normal, float diffuse, int lightmode) const;

    void emit_clipped(const Quad& q, std::vector<RenderPolygon>* out);
    void emit_screen(const Quad& q, const Point* points, u32 count, std::vector<RenderPolygon>* out);

    std::span<const u32> m_poly_rom;
    std::vector<u16>     m_colour_ram;  ///< MAME's m_tgp_ram, addressed from 0x40000
    std::vector<u32>     m_poly_ram;    ///< addressed from 0x800000

    std::array<LightParam, 256> m_lightparams{};
    View                        m_view;

    std::vector<Point> m_points;
    std::vector<Quad>  m_quads;
    std::vector<Lens>  m_lenses;
    u16                m_spread_left  = 0;
    u16                m_spread_right = 0;
    u8                 m_hud_sides    = 0;  ///< bit 0 a HUD object moved left, bit 1 right
    u32                m_hud_hold     = 0;  ///< frames the spread HUD stays on
    bool               m_lens_changed = true;
    std::vector<u32>   m_order;

    // Per-render inputs.
    std::span<const u16> m_list;
    std::span<const u16> m_palette;
    std::span<const u16> m_xlat;
    u64                  m_frame_number = 0;
    u32                  m_wide         = 0;
};

}  // namespace sm2::hw
