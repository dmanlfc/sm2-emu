// SPDX-License-Identifier: BSD-3-Clause
//
// A CPU rasteriser for the 3D pass, ported line for line from MAME.
//
// Derived from MAME's src/mame/sega/model2rd.ipp and the scan conversion in
// src/devices/video/poly.h (BSD-3-Clause, copyright-holders R. Belmont, Olivier
// Galibert, ElSemi, Angelo Salese, Matthew Daniels, Aaron Giles).
//
// This is not how sm2-emu draws. The real pixel stage is Vulkan, which matches
// MAME's *output* rather than its implementation. This exists to make that claim
// testable: with a software path that is a faithful port, a frame can be compared
// three ways -- MAME against this, and this against Vulkan -- and any difference
// attributed to one stage instead of being argued about. It also runs headless,
// so a pixel comparison needs no GPU.
#pragma once

#include "core/types.h"
#include "hw/geometrizer.h"

#include <span>
#include <vector>

namespace sm2::hw {

class Model2MachineBase;

/// Everything the pixel stage needs about one polygon, unpacked once per polygon.
/// MAME's m2_poly_extra_data.
struct SoftPolyExtra {
    u32 lumabase  = 0;
    u32 colorbase = 0;
    u8  checker   = 0;
    const u32* texsheet[2] = {nullptr, nullptr};
    usize      texwords[2] = {0, 0};
    u32 texwidth   = 0;
    u32 texheight  = 0;
    u32 texx       = 0;
    u32 texy       = 0;
    u8  texwrapx   = 0;
    u8  texwrapy   = 0;
    u8  texmirrorx = 0;
    u8  texmirrory = 0;
    u8  utex       = 0;
    u8  utexminlod = 0;
    u32 utexx      = 0;
    u32 utexy      = 0;
    s32 texlod     = 0;
    u8  luma       = 0;
};

/// The software 3D pass, plus the same three-way composition MAME's screen_update
/// performs around it.
class SoftRenderer {
public:
    static constexpr u32 kWidth  = 496;
    static constexpr u32 kHeight = 384;

    /// MAME draws the 3D into a 512x512 bitmap and copies the visible window out
    /// of its top left corner, so a polygon whose viewport reaches past the raster
    /// still lands where the hardware puts it.
    static constexpr u32 kTargetWidth  = 512;
    static constexpr u32 kTargetHeight = 512;

    SoftRenderer();

    /// Produce one complete frame as RGBA8, kWidth by kHeight.
    ///
    /// `out` must hold kWidth * kHeight entries. The composition is MAME's: the
    /// background pen, then the tilemap layers of priority category zero, then the
    /// 3D output (or the framebuffer in render test mode), then category one.
    void render(const Model2MachineBase& machine, const RenderList& list,
                std::span<u32> out);

    /// Pixels the 3D pass wrote this frame, for reporting.
    [[nodiscard]] u32 pixels_drawn() const { return m_pixels; }

private:
    struct Extent {
        s32   startx = 0;
        s32   stopx  = 0;
        float start[3]{};
        float dpdx[3]{};
    };

    struct Rect {
        s32 left = 0, top = 0, right = 0, bottom = 0;  // right/bottom inclusive
        [[nodiscard]] bool empty() const { return right < left || bottom < top; }
    };

    void draw_polygon(const RenderPolygon& poly, const Rect& cliprect);

    void render_triangle(const Rect& cliprect, const PolyVertex& v1, const PolyVertex& v2,
                         const PolyVertex& v3, u32 renderer, const SoftPolyExtra& extra);
    void render_convex(const Rect& cliprect, const PolyVertex* v, u32 count, u32 renderer,
                       const SoftPolyExtra& extra);

    void draw_scanline_solid(s32 scanline, const Extent& extent, const SoftPolyExtra& object,
                             bool translucent);
    void draw_scanline_tex(s32 scanline, const Extent& extent, const SoftPolyExtra& object,
                           bool translucent);

    [[nodiscard]] u32 fetch_bilinear_texel(const SoftPolyExtra& object, s32 miplevel, s32 u,
                                           s32 v, bool translucent) const;

    // Machine memory for the frame being drawn.
    std::span<const u16> m_palram;
    std::span<const u16> m_colorxlat;
    std::span<const u8>  m_lumaram;
    std::span<const u32> m_texture0;
    std::span<const u32> m_texture1;

    std::vector<u8>  m_gamma;
    std::vector<u32> m_destmap;
    std::vector<u8>  m_fillmap;

    u32 m_pixels = 0;
};

}  // namespace sm2::hw
