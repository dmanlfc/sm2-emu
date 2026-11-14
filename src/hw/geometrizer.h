// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Sega Model 2 geometry engine and the front end of its 3D rasterizer.
//
// Ported from MAME's src/mame/sega/model2_v.cpp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels), whose
// own header records where the information came from: a disassembly of the
// geometry microcode uploaded by a Model 2B game, the Model 2B-CRX manual, and
// ElSemi's Direct3D implementation.
#pragma once

#include "core/types.h"

#include <memory>
#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// A vertex, in whatever space the current stage works in. The slots are MAME's:
/// p[0] is depth, p[1] and p[2] are texture coordinates.
struct PolyVertex {
    float x = 0.0F;
    float y = 0.0F;
    float p[4]{};
};

/// A clipping plane of the viewing frustum.
struct ClipPlane {
    PolyVertex normal;
    float      distance = 0.0F;
};

/// Lighting parameters, one set per texture slot.
struct TextureParameter {
    float diffuse          = 0.0F;
    float ambient          = 0.0F;
    u32   specular_control = 0;
    float specular_scale   = 0.0F;
};

/// A polygon as the rasterizer receives it: up to eight vertices after clipping,
/// still in view space, with everything the pixel stage will need.
struct Polygon {
    /// Next polygon in the same depth bucket: an intrusive list over a fixed pool.
    void* next = nullptr;

    PolyVertex v[8];
    u8         num_vertices = 3;
    u16        z            = 0;
    u16        texheader[4] = {0, 0, 0, 0};
    u8         luma         = 0;
    s32        texlod       = 0;
    s16        viewport[4]  = {0, 0, 0, 0};
    s16        center[2]    = {0, 0};
    u8         window       = 0;
};

/// A polygon straight off the geometry engine, before clipping.
struct QuadM2 {
    PolyVertex v[4];
    u16        z            = 0;
    u16        texheader[4] = {0, 0, 0, 0};
    u8         luma         = 0;
    s32        texlod       = 0;
};

/// One polygon ready to draw: screen-space positions with view-space depth and
/// texture coordinates. The list is in draw order, front to back, as the hardware
/// uses a first-writer-wins fill mask rather than a depth test.
struct RenderPolygon {
    PolyVertex v[8];
    u8         num_vertices = 3;
    u16        texheader[4] = {0, 0, 0, 0};
    u8         luma         = 0;
    s32        texlod       = 0;

    /// The viewport as a screen-space rectangle (left, top, right, bottom), with
    /// the vertices' y flip and CRTC offset, clamped to the raster. Drawing must
    /// be clipped to it: the frustum planes alone do not bound a small window.
    s16 scissor[4] = {0, 0, 0, 0};

    u8 window = 0;

    /// Bit 31 set: a flat polygon already coloured, 0x00rrggbb in the low bits,
    /// that skips the palette and tone curve. Model 1 resolves its own colours.
    u32 direct_rgb = 0;
};

/// Why the geometry stage discarded what it discarded, and the register state it
/// used to decide.
struct GeometryStats {
    // check_culling, one counter per test, in the order the original tests them.
    u32 cull_backface = 0;
    u32 cull_linktype = 0;
    u32 cull_master_z = 0;
    u32 cull_behind   = 0;

    /// Which frustum plane reduced a polygon below three vertices: left, right,
    /// top, bottom.
    u32 clip_kill[4]{};

    /// Geometry engine opcodes executed, indexed by the five-bit opcode.
    u32 opcodes[32]{};

    /// Words of display list walked, and jumps taken.
    u32 words = 0;
    u32 jumps = 0;

    /// Byte offset the walk started from; the program moves it between frames.
    u32 read_start = 0;

    /// Register state as the last window command left it.
    s16   viewport[4]{};
    s16   center[4][2]{};
    u16   center_sel    = 0;
    float focus[2]{};
    float light[3]{};
    float lod           = 0.0F;
    u32   geo_mode      = 0;
    u8    master_z_clip = 0;
    s32   z_adjust      = 0;

    /// Polygons with non-finite vertices, which every clip test silently rejects.
    u32 non_finite = 0;
};

/// A frame's worth of polygons in draw order.
struct RenderList {
    std::vector<RenderPolygon> polygons;

    /// Polygons produced before clipping and culling, for diagnostics.
    u32 generated = 0;
    u32 culled    = 0;
    u32 clipped_away = 0;

    GeometryStats stats;

    void clear()
    {
        polygons.clear();
        generated    = 0;
        culled       = 0;
        clipped_away = 0;
        stats        = GeometryStats{};
    }
};

/// The geometry engine and the sorting and clipping stage behind it.
///
/// On Model 2 and 2A this is a DSP whose internal microcode ROM was never dumped;
/// this is MAME's high-level reimplementation, so results match but timing does
/// not.
///
/// Polygons are bucketed by a 16-bit depth ordinal, clipped against four frustum
/// planes and drawn nearest bucket first, later windows over earlier ones. The
/// ordinal is a 4.12 float biased by the zsort mode register; a game that never
/// sets it puts every depth into one bucket.
class Geometrizer {
public:
    /// Pool size for one frame's polygons, as MAME; excess polygons are dropped.
    static constexpr u32 kMaxPolygons = 32768;

    /// Depth buckets. The sort ordinal is a 4.12 fixed-point value.
    static constexpr u32 kDepthBuckets = 0x10000;

    /// The visible raster; the projection's vertical flip is measured from its
    /// bottom.
    static constexpr u32 kRasterWidth  = 496;
    static constexpr u32 kRasterHeight = 384;

    Geometrizer();
    ~Geometrizer();

    Geometrizer(const Geometrizer&)            = delete;
    Geometrizer& operator=(const Geometrizer&) = delete;

    /// Wire up the model data, the texture-header ROM and the display list the
    /// coprocessor fills. All must outlive this object.
    void attach(std::span<const u32> polygon_rom,
                std::span<const u16> texture_rom,
                std::span<u32>       display_list);

    void reset();

    /// Walk the display list from the read start address and produce this
    /// frame's polygons.
    void run(RenderList* out);

    // -- registers ---------------------------------------------------------

    /// 0x0181c000. Writing 0xff disables depth clipping entirely.
    void set_z_clip(u8 value);

    /// Where in the display list the next frame starts.
    void set_read_start_address(u32 address) { m_geo_read_start_address = address; }
    [[nodiscard]] u32 read_start_address() const { return m_geo_read_start_address; }

    /// Raster offsets from the CRTC sync registers.
    void set_crtc_offsets(s16 x, s16 y)
    {
        m_crtc_xoffset = x;
        m_crtc_yoffset = y;
    }

    /// Place the 3D in a raster `columns` wider on each side; zero is the
    /// hardware's own. `widen` also grows full-screen windows into those
    /// columns; without it the 3D stays 4:3.
    void set_widescreen(u32 columns, bool widen = true)
    {
        m_wide_columns = static_cast<s32>(columns);
        m_widen        = widen;
    }

    /// 0x10400000. Sky Target reads this; nothing is known about why.
    [[nodiscard]] u32 polygon_count() const;

    /// Wall-clock time of the last run(), in nanoseconds, for the profiler.
    [[nodiscard]] u64 last_run_nanoseconds() const { return m_last_run_ns; }

    /// Save/restore the registers, tables and polygon RAM the display list
    /// sets. The per-frame polygon pools are excluded and rebuilt next vblank.
    void serialize(Archive& ar);

private:
    // Everything below keeps MAME's names.

    struct RasterState {
        RasterState();

        u16*  texture_rom      = nullptr;
        u32   texture_rom_mask = 0;
        s16   viewport[4]      = {0, 0, 0, 0};
        s16   center[4][2]     = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        u16   center_sel       = 0;
        u32   reverse          = 0;
        s32   z_adjust         = 0;
        float polygon_z        = 0.0F;
        u8    master_z_clip    = 0;
        u32   cur_command      = 0;
        u32   command_buffer[32]{};
        u32   command_index    = 0;

        std::vector<Polygon>  poly_list;
        u32                   poly_list_index = 0;
        std::vector<Polygon*> poly_sorted_list;

        u16 min_z = 0;
        u16 max_z = 0;

        std::vector<u16> texture_ram;  ///< 0x10000 words
        std::vector<u8>  log_ram;      ///< 0x8000 bytes

        u8        cur_window = 0;
        ClipPlane clip_plane[4][4];
        /// Widescreen: the left and right planes of the window as the game set
        /// it, before widening, while `widened` says it was widened.
        ClipPlane narrow_plane[4][2];
        bool      widened      = false;
        /// Polygons that reach the window as the game set it.
        u32       narrow_count = 0;
    };

    struct GeoState {
        RasterState* raster           = nullptr;
        u32          mode             = 0;
        u32*         polygon_rom      = nullptr;
        u32          polygon_rom_mask = 0;
        float        matrix[12]{};
        float        matrix2[12]{};  ///< command 0x1b, for skinned vertices
        PolyVertex   focus;
        PolyVertex   light;
        float        lod = 0.0F;
        float        coef_table[32]{};
        TextureParameter texture_parameters[32];
        std::vector<u32> polygon_ram0;  ///< 0x8000 words, the fast bank
        std::vector<u32> polygon_ram1;  ///< 0x8000 words, the slow bank
    };

    // -- ported implementation ---------------------------------------------

    static void apply_focus(GeoState* geo, PolyVertex* p0);

    [[nodiscard]] u16 float_to_zval(float floatval, s32 z_adjust);
    [[nodiscard]] bool check_culling(RasterState* raster, u32 attr, float min_z, float max_z);

    template <unsigned NumVerts>
    void model2_3d_process_polygon(RasterState* raster, u32 attr);

    void model2_3d_push(RasterState* raster, u32 input);
    void model2_3d_project(Polygon* poly);
    void widen_screen_overlay(Polygon* poly) const;
    void model2_3d_zclip_w(u32 data);
    void render_frame_start();

    void geo_parse();
    u32* geo_process_command(GeoState* geo, u32 opcode, u32* input, bool* end_code);

    void geo_parse_np_ns(GeoState* geo, u32* input, u32 count);
    void geo_parse_np_s(GeoState* geo, u32* input, u32 count);
    void geo_parse_nn_ns(GeoState* geo, u32* input, u32 count, bool compact = false);
    void geo_parse_nn_s(GeoState* geo, u32* input, u32 count, bool compact = false);

    u32* geo_nop(GeoState* geo, u32 opcode, u32* input);
    u32* geo_object_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_direct_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_window_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_texture_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_polygon_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_texture_parameters(GeoState* geo, u32 opcode, u32* input);
    u32* geo_mode(GeoState* geo, u32 opcode, u32* input);
    u32* geo_zsort_mode(GeoState* geo, u32 opcode, u32* input);
    u32* geo_focal_distance(GeoState* geo, u32 opcode, u32* input);
    u32* geo_light_source(GeoState* geo, u32 opcode, u32* input);
    u32* geo_matrix_write(GeoState* geo, u32 opcode, u32* input);
    u32* geo_translate_write(GeoState* geo, u32 opcode, u32* input);
    u32* geo_data_mem_push(GeoState* geo, u32 opcode, u32* input);
    u32* geo_test(GeoState* geo, u32 opcode, u32* input);
    u32* geo_end(GeoState* geo, u32 opcode, u32* input);
    u32* geo_dummy(GeoState* geo, u32 opcode, u32* input);
    u32* geo_log_data(GeoState* geo, u32 opcode, u32* input);
    u32* geo_lod(GeoState* geo, u32 opcode, u32* input);
    u32* geo_code_upload(GeoState* geo, u32 opcode, u32* input);
    u32* geo_code_jump(GeoState* geo, u32 opcode, u32* input);

    /// Walk the depth buckets in drawing order and project into screen space.
    void build_render_list(RenderList* out);

    /// A polygon's viewport as a clamped screen-space rectangle.
    void screen_scissor(const Polygon* poly, s16* out) const;

    std::unique_ptr<RasterState> m_raster;
    std::unique_ptr<GeoState>    m_geo;

    /// The display list, under MAME's name.
    u32* m_bufferram = nullptr;
    u32  m_bufferram_words = 0;

    bool m_render_done = false;

    /// build_render_list's polygons per window, kept to reuse the capacity.
    std::vector<std::vector<Polygon*>> m_window_polygons;

    u32 m_geo_read_start_address = 0;

    s16 m_crtc_xoffset = 0;
    s16 m_crtc_yoffset = 0;

    s32 m_wide_columns = 0;
    bool m_widen        = true;

    /// Discard counters for the frame being built.
    u32 m_culled     = 0;
    u32 m_degenerate = 0;

    GeometryStats m_stats;

    bool m_pool_exhausted         = false;
    bool m_unknown_command_warned = false;

    u64 m_last_run_ns = 0;
};

}  // namespace sm2::hw
