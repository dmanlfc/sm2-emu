// SPDX-License-Identifier: BSD-3-Clause
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

namespace sm2::hw {

/// A vertex, in whatever space the current stage works in.
///
/// The generic parameter slots are MAME's: p[0] is depth, p[1] and p[2] are
/// texture coordinates. Keeping that shape rather than naming the fields is what
/// lets the ported geometry code stay comparable with its original, which uses
/// macros to alias exactly these three.
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
    /// Next polygon in the same depth bucket. A raw pointer because the buckets
    /// are intrusive lists over a fixed pool, exactly as in MAME.
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
/// texture coordinates still attached.
///
/// This is the seam between emulation and rendering. Everything above it is the
/// hardware's own arithmetic; everything below is Vulkan's problem. The list is
/// already in draw order, front to back, because the hardware draws that way and
/// relies on a first-writer-wins fill mask rather than a depth test.
struct RenderPolygon {
    PolyVertex v[8];
    u8         num_vertices = 3;
    u16        texheader[4] = {0, 0, 0, 0};
    u8         luma         = 0;
    s32        texlod       = 0;

    /// The polygon's viewport as a screen-space rectangle: left, top, right,
    /// bottom, already through the same y flip and CRTC offset as the vertices
    /// and clamped to the raster. Drawing must be clipped to it, because a window
    /// can be smaller than the screen and the frustum planes only bound the
    /// polygon in the two directions the window's vanishing point defines.
    s16 scissor[4] = {0, 0, 0, 0};

    u8 window = 0;
};

/// A frame's worth of polygons in draw order.
struct RenderList {
    std::vector<RenderPolygon> polygons;

    /// Polygons the geometry engine produced before clipping and culling, for
    /// diagnostics: the ratio to polygons.size() says how much work is being
    /// discarded.
    u32 generated = 0;
    u32 culled    = 0;
    u32 clipped_away = 0;

    void clear()
    {
        polygons.clear();
        generated    = 0;
        culled       = 0;
        clipped_away = 0;
    }
};

/// The geometry engine and the sorting and clipping stage behind it.
///
/// On Model 2 and 2A this hardware is a DSP running microcode held in its own
/// internal ROM, which has never been dumped. MAME reimplements what that
/// microcode does, from a disassembly of the equivalent program that later boards
/// upload, and that reimplementation is what is ported here. It is therefore a
/// high-level model rather than an emulation: the results should match, the
/// timing does not.
///
/// The stage after it is better understood only in outline. Polygons are bucketed
/// by a 16-bit depth ordinal into 65536 lists, clipped against four frustum
/// planes, and drawn from the nearest bucket to the furthest, with later windows
/// always over earlier ones.
///
/// The depth ordinal is a 4.12 float whose exponent is biased by the zsort mode
/// register. A game that leaves that register alone puts every ordinary depth
/// above the top of the range and so into one bucket, which looks like broken
/// sorting but is really a missing register write.
class Geometrizer {
public:
    /// Pool size for one frame's polygons. MAME's figure; a frame that exceeds it
    /// simply stops adding.
    static constexpr u32 kMaxPolygons = 32768;

    /// Depth buckets. The sort ordinal is a 4.12 fixed-point value.
    static constexpr u32 kDepthBuckets = 0x10000;

    /// The visible raster. The projection's vertical flip is measured from the
    /// bottom of it, so it is part of the arithmetic rather than just a size.
    static constexpr u32 kRasterWidth  = 496;
    static constexpr u32 kRasterHeight = 384;

    Geometrizer();
    ~Geometrizer();

    Geometrizer(const Geometrizer&)            = delete;
    Geometrizer& operator=(const Geometrizer&) = delete;

    /// Wire up the ROMs and the display list.
    ///
    /// `polygon_rom` is the model data the geometry engine walks, `texture_rom` is
    /// where texture headers live when they are not in texture RAM, and
    /// `display_list` is the buffer the coprocessor fills. All must outlive this
    /// object.
    void attach(std::span<const u32> polygon_rom,
                std::span<const u16> texture_rom,
                std::span<u32>       display_list);

    void reset();

    /// Walk the display list and produce this frame's polygons.
    ///
    /// Reads from the address last written to the geometry read port. Produces
    /// nothing and reports nothing if the display list is empty, which is the
    /// normal state before a game starts submitting geometry.
    void run(RenderList* out);

    // -- registers ---------------------------------------------------------

    /// 0x0181c000. Writing 0xff disables depth clipping entirely.
    void set_z_clip(u8 value);

    /// Where in the display list the next frame starts.
    void set_read_start_address(u32 address) { m_geo_read_start_address = address; }
    [[nodiscard]] u32 read_start_address() const { return m_geo_read_start_address; }

    /// Raster offsets from the CRTC sync registers. They move the projected image
    /// relative to the monitor.
    void set_crtc_offsets(s16 x, s16 y)
    {
        m_crtc_xoffset = x;
        m_crtc_yoffset = y;
    }

    /// 0x10400000. Sky Target reads this; nothing is known about why.
    [[nodiscard]] u32 polygon_count() const;

private:
    // Everything below keeps MAME's names so the ported body reads the same as
    // its original.

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
    };

    struct GeoState {
        RasterState* raster           = nullptr;
        u32          mode             = 0;
        u32*         polygon_rom      = nullptr;
        u32          polygon_rom_mask = 0;
        float        matrix[12]{};
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
    void model2_3d_zclip_w(u32 data);
    void render_frame_start();

    void geo_parse();
    u32* geo_process_command(GeoState* geo, u32 opcode, u32* input, bool* end_code);

    void geo_parse_np_ns(GeoState* geo, u32* input, u32 count);
    void geo_parse_np_s(GeoState* geo, u32* input, u32 count);
    void geo_parse_nn_ns(GeoState* geo, u32* input, u32 count);
    void geo_parse_nn_s(GeoState* geo, u32* input, u32 count);

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

    /// The display list. Named as MAME names it so the ported geo_parse matches.
    u32* m_bufferram = nullptr;
    u32  m_bufferram_words = 0;

    bool m_render_done = false;

    u32 m_geo_read_start_address = 0;

    s16 m_crtc_xoffset = 0;
    s16 m_crtc_yoffset = 0;

    /// Counters for the frame being built, so build_render_list can report what
    /// the earlier stages discarded.
    u32 m_culled     = 0;
    u32 m_degenerate = 0;

    bool m_pool_exhausted         = false;
    bool m_unknown_command_warned = false;
};

}  // namespace sm2::hw
