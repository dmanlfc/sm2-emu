// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from MAME's src/mame/sega/model2_v.cpp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
//
// MAME's own notes on this hardware, kept because they record findings that are
// not deducible from the code:
//
//   The 3D system has three main parts. The geometry engine, which on Model 2B
//   and 2C is a DSP the game uploads code to, and on the original and 2A holds
//   that code in an internal ROM. The Z-sort and clip hardware, about which
//   little is known beyond a master clip enable and a sort mode. And the renderer,
//   which is double-buffered and switchable between 60 and 30 Hz output.
//
//   The information came from a disassembly of the geometry code uploaded by a 2B
//   game, the Model 2B-CRX manual, and ElSemi's Direct3D implementation. Because
//   the 2A geometry ROM has never been dumped, the engine below is a
//   reimplementation of what that code does rather than an emulation of it.
//
//   Z-sort is not entirely understood. The manual says the sort works internally
//   in 1.4.8 format, then says the sort mode register value is added to the
//   floating point z and converted to 16 bits, most likely 4.12. A polygon can
//   override its sort value with the previous polygon's, or with the minimum or
//   maximum of its own points, which lets a whole object sit in front of another
//   even where individual coordinates disagree. Polygons from later windows always
//   draw over earlier ones regardless of z: Sega Rally's name entry screen relies
//   on this to put letters both behind and in front of the car.
//
//   Clipping uses four planes for the viewing frustum. A polygon that intersects
//   the origin exactly would produce a vertex at (0,0,0) and NaNs during
//   projection, so a tiny value is added to z.
//
//   Polygons are drawn front to back against a fill buffer that stops a pixel
//   being written twice. The hardware does that so the nearest polygons are drawn
//   first, in case it runs out of time and has to skip to the next frame.

#include "hw/geometrizer.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <limits>

// MAME aliases the generic parameter slots of its vertex type so the geometry
// code can refer to depth and texture coordinates by name. The same aliases are
// defined here, and undefined at the end of the file, so the ported body stays
// textually comparable with its original.
#define pz p[0]
#define pu p[1]
#define pv p[2]

namespace sm2::hw {

Geometrizer::RasterState::RasterState()
    : poly_list(kMaxPolygons)
    , poly_sorted_list(kDepthBuckets, nullptr)
    , texture_ram(0x10000, 0)
    , log_ram(0x8000, 0)
{
}

Geometrizer::Geometrizer()
    : m_raster(std::make_unique<RasterState>())
    , m_geo(std::make_unique<GeoState>())
{
    m_geo->polygon_ram0.assign(0x8000, 0);
    m_geo->polygon_ram1.assign(0x8000, 0);
    m_geo->raster = m_raster.get();
}

Geometrizer::~Geometrizer() = default;

void Geometrizer::attach(std::span<const u32> polygon_rom,
                         std::span<const u16> texture_rom,
                         std::span<u32>       display_list)
{
    // The ported code indexes these through non-const pointers because MAME's
    // memory regions are not const. Nothing in the geometry path writes to
    // either ROM; the display list is genuinely writable on hardware.
    m_geo->polygon_rom      = const_cast<u32*>(polygon_rom.data());
    m_geo->polygon_rom_mask = polygon_rom.empty()
                                ? 0
                                : static_cast<u32>(polygon_rom.size() - 1);

    m_raster->texture_rom      = const_cast<u16*>(texture_rom.data());
    m_raster->texture_rom_mask = texture_rom.empty()
                                   ? 0
                                   : static_cast<u32>(texture_rom.size() - 1);

    m_bufferram       = display_list.data();
    m_bufferram_words = static_cast<u32>(display_list.size());

    if (polygon_rom.empty()) {
        SM2_WARN("geo: no polygon ROM; every model will be empty");
    }
}

void Geometrizer::reset()
{
    m_raster->command_index = 0;
    m_raster->cur_command   = 0;
    m_raster->center_sel    = 0;
    m_raster->reverse       = 0;
    m_raster->z_adjust      = 0;
    m_raster->master_z_clip = 0xff;
    std::fill(std::begin(m_raster->command_buffer), std::end(m_raster->command_buffer), 0u);
    std::fill(std::begin(m_raster->viewport), std::end(m_raster->viewport), s16{0});
    for (auto& centre : m_raster->center) {
        centre[0] = 0;
        centre[1] = 0;
    }
    std::fill(m_raster->texture_ram.begin(), m_raster->texture_ram.end(), u16{0});
    std::fill(m_raster->log_ram.begin(), m_raster->log_ram.end(), u8{0});

    m_geo->mode = 0;
    std::fill(std::begin(m_geo->matrix), std::end(m_geo->matrix), 0.0F);
    std::fill(std::begin(m_geo->coef_table), std::end(m_geo->coef_table), 0.0F);
    for (TextureParameter& parameter : m_geo->texture_parameters) {
        parameter = TextureParameter{};
    }
    m_geo->focus = PolyVertex{};
    m_geo->light = PolyVertex{};
    m_geo->lod   = 0.0F;
    std::fill(m_geo->polygon_ram0.begin(), m_geo->polygon_ram0.end(), 0u);
    std::fill(m_geo->polygon_ram1.begin(), m_geo->polygon_ram1.end(), 0u);

    m_geo_read_start_address = 0;
    m_render_done            = false;
    m_culled                 = 0;
    m_degenerate             = 0;
    m_pool_exhausted         = false;
    m_unknown_command_warned = false;

    render_frame_start();
}

void Geometrizer::set_z_clip(u8 value)
{
    model2_3d_zclip_w(value);
}

u32 Geometrizer::polygon_count() const
{
    return m_raster->poly_list_index;
}

void Geometrizer::run(RenderList* out)
{
    out->clear();

    if (m_bufferram == nullptr) {
        return;
    }

    m_culled     = 0;
    m_degenerate = 0;

    geo_parse();
    build_render_list(out);
}

void Geometrizer::screen_scissor(const Polygon* poly, s16* out) const
{
    // Same transform MAME's model2_3d_render applies before handing the polygon
    // to the scanline rasteriser, and the same one model2_3d_project applies to
    // the vertices: the hardware's y axis points up, the raster's points down, and
    // both are shifted by the CRTC sync registers.
    const s32 raster_height = static_cast<s32>(kRasterHeight);
    const s32 left   = poly->viewport[0] + m_crtc_xoffset;
    const s32 right  = poly->viewport[2] + m_crtc_xoffset;
    const s32 top    = (raster_height - poly->viewport[3]) + m_crtc_yoffset;
    const s32 bottom = (raster_height - poly->viewport[1]) + m_crtc_yoffset;

    const auto clamp_x = [](s32 value) {
        return static_cast<s16>(std::clamp<s32>(value, 0, static_cast<s32>(kRasterWidth)));
    };
    const auto clamp_y = [raster_height](s32 value) {
        return static_cast<s16>(std::clamp<s32>(value, 0, raster_height));
    };

    out[0] = clamp_x(left);
    out[1] = clamp_y(top);
    out[2] = clamp_x(right);
    out[3] = clamp_y(bottom);

    // A viewport the program never set, or one the offsets pushed off the raster,
    // would otherwise become an inside-out rectangle.
    if (out[2] < out[0]) {
        out[2] = out[0];
    }
    if (out[3] < out[1]) {
        out[3] = out[1];
    }
}

void Geometrizer::build_render_list(RenderList* out)
{
    RasterState* raster = m_raster.get();

    out->generated    = raster->poly_list_index;
    out->culled       = m_culled;
    out->clipped_away = m_degenerate;

    if (raster->poly_list_index == 0) {
        return;
    }

    // Windows descend and depth buckets ascend, which together give the hardware's
    // drawing order: a later window always covers an earlier one, and within a
    // window the nearest bucket goes first. Nearest first is what makes the pixel
    // stage's one-bit fill mask work as a depth test: the first polygon to reach a
    // pixel keeps it. The order is established here and simply preserved from here
    // on.
    out->polygons.reserve(raster->poly_list_index);

    for (int window = raster->cur_window; window >= 0; --window) {
        for (u32 depth = raster->min_z; depth <= raster->max_z; ++depth) {
            Polygon* poly = raster->poly_sorted_list[depth];
            while (poly != nullptr) {
                if (poly->window == window) {
                    model2_3d_project(poly);

                    RenderPolygon& target = out->polygons.emplace_back();
                    target.num_vertices   = poly->num_vertices;
                    for (u32 index = 0; index < poly->num_vertices; ++index) {
                        target.v[index] = poly->v[index];
                    }
                    for (u32 index = 0; index < 4; ++index) {
                        target.texheader[index] = poly->texheader[index];
                    }
                    target.luma   = poly->luma;
                    target.texlod = poly->texlod;
                    target.window = poly->window;
                    screen_scissor(poly, target.scissor);
                }
                poly = static_cast<Polygon*>(poly->next);
            }
        }
    }

    m_render_done = true;
}

static inline void transform_point(PolyVertex *point, float *matrix)
{
	float tx = (point->x * matrix[0]) + (point->y * matrix[3]) + (point->pz * matrix[6]) + (matrix[9]);
	float ty = (point->x * matrix[1]) + (point->y * matrix[4]) + (point->pz * matrix[7]) + (matrix[10]);
	float tz = (point->x * matrix[2]) + (point->y * matrix[5]) + (point->pz * matrix[8]) + (matrix[11]);

	point->x = tx;
	point->y = ty;
	point->pz = tz;
}

static inline void transform_vector(PolyVertex *vector, float *matrix)
{
	float tx = (vector->x * matrix[0]) + (vector->y * matrix[3]) + (vector->pz * matrix[6]);
	float ty = (vector->x * matrix[1]) + (vector->y * matrix[4]) + (vector->pz * matrix[7]);
	float tz = (vector->x * matrix[2]) + (vector->y * matrix[5]) + (vector->pz * matrix[8]);

	vector->x = tx;
	vector->y = ty;
	vector->pz = tz;
}

static inline void normalize_vector(PolyVertex *vector)
{
	const float n = sqrt((vector->x * vector->x) + (vector->y * vector->y) + (vector->pz * vector->pz));

	if (n)
	{
		float oon = 1.0f / n;
		vector->x *= oon;
		vector->y *= oon;
		vector->pz *= oon;
	}
}

static inline float dot_product(const PolyVertex &v1, const PolyVertex &v2)
{
	return (v1.x * v2.x) + (v1.y * v2.y) + (v1.pz * v2.pz);
}

static inline void vector_cross3(PolyVertex *dst, const PolyVertex *v0, const PolyVertex *v1, const PolyVertex *v2)
{
	PolyVertex p1, p2;

	p1.x = v1->x - v0->x;   p1.y = v1->y - v0->y;   p1.pz = v1->pz - v0->pz;
	p2.x = v2->x - v0->x;   p2.y = v2->y - v0->y;   p2.pz = v2->pz - v0->pz;

	dst->x = (p1.y * p2.pz) - (p1.pz * p2.y);
	dst->y = (p1.pz * p2.x) - (p1.x * p2.pz);
	dst->pz = (p1.x * p2.y) - (p1.y * p2.x);
}

inline void Geometrizer::apply_focus(GeoState *geo, PolyVertex *p0)
{
	p0->x *= geo->focus.x;
	p0->y *= geo->focus.y;
}

/* 1.8.23 float to 4.12 float converter, courtesy of Aaron Giles */
inline u16 Geometrizer::float_to_zval(float floatval, s32 z_adjust)
{
	s32 fpint = f2u(floatval);
	s32 exponent = ((fpint >> 23) & 0xff) - ((z_adjust >> 23) & 0xff);
	u32 mantissa = fpint & 0x7fffff;

	/* round the low bits and reduce to 12 */
	mantissa += 0x400;
	if (mantissa > 0x7fffff)
	{
		exponent++;
		mantissa = (mantissa & 0x7fffff) >> 1;
	}
	mantissa >>= 11;

	// if negative, clamp to 0
	if (fpint < 0)
		return 0x0000;

	// the rest depends on the exponent
	if (exponent < -12)
		return 0x0000; // less than -12 is too small, return 0
	else if (exponent < 0)
		return (mantissa | 0x1000) >> -exponent; // between -12 and 0 create a denormal with exponent of 0
	else if (exponent < 15)
		return ((exponent + 1) << 12) | mantissa; // between 0 and 14 create a FP value with exponent + 1
	else
		return 0xffff; // above 14 is too large
}

static s32 clip_polygon(PolyVertex *v, s32 num_vertices, PolyVertex *vout, ClipPlane clip_plane)
{
	s32 outcount = 0;

	const PolyVertex *cur = v;
	PolyVertex *out = vout;

	float curdot = dot_product(*cur, clip_plane.normal);
	s32 curin = (curdot >= clip_plane.distance) ? 1 : 0;

	for (s32 i = 0; i < num_vertices; i++)
	{
		const s32 nextvert = (i + 1) % num_vertices;

		/* if the current point is inside the plane, add it */
		if (curin)
			out[outcount++] = *cur;

		const float nextdot = dot_product(v[nextvert], clip_plane.normal);
		const s32 nextin = (nextdot >= clip_plane.distance) ? 1 : 0;

		/* Add a clipped vertex if one end of the current edge is inside the plane and the other is outside */
		if ((curin != nextin) && !std::isnan(curdot) && !std::isnan(nextdot))
		{
			const float scale = (clip_plane.distance - curdot) / (nextdot - curdot);

			out[outcount].x = cur->x + ((v[nextvert].x - cur->x) * scale);
			out[outcount].y = cur->y + ((v[nextvert].y - cur->y) * scale);
			out[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
			out[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
			out[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
			outcount++;
		}

		curdot = nextdot;
		curin = nextin;
		cur++;
	}

	return outcount;
}

inline bool Geometrizer::check_culling(RasterState *raster, u32 attr, float min_z, float max_z)
{
	/* if doubleside is disabled */
	if (((attr >> 17) & 1) == 0)
	{
		/* if it's the backface, cull it */
		if (raster->command_buffer[9] & 0x00800000)
			return true;
	}

	/* if the linktype is 0, then we can also cull it */
	if (((attr >> 8) & 3) == 0)
		return true;

	/* if the minimum z value is bigger than the master z clip value, don't render */
	if (raster->master_z_clip != 0xff && (s32)(1.0 / min_z) > raster->master_z_clip)
		return true;

	/* if the maximum z value is < 0 then we can safely clip the entire Polygon */
	if (max_z < 0)
		return true;

	return false;
}

void Geometrizer::model2_3d_zclip_w(u32 data)
{
	// setting this register to 0xff disables z-clip
	m_raster->master_z_clip = data;
}


template <unsigned NumVerts>
void Geometrizer::model2_3d_process_polygon(RasterState *raster, u32 attr)
{
	QuadM2 object;
	u16 *th, *tp;
	s32 tho;
	u32 i;
	bool cull;
	float zvalue;
	float min_z, max_z;

	static_assert(NumVerts == 3 || NumVerts == 4, "Polygon must have 3 or 4 vertices");

	/* extract P0(n-1) */
	object.v[1].x = u2f(raster->command_buffer[2] << 8);
	object.v[1].y = u2f(raster->command_buffer[3] << 8);
	object.v[1].pz = u2f(raster->command_buffer[4] << 8);

	/* extract P1(n-1) */
	object.v[0].x = u2f(raster->command_buffer[5] << 8);
	object.v[0].y = u2f(raster->command_buffer[6] << 8);
	object.v[0].pz = u2f(raster->command_buffer[7] << 8);

	/* extract P0(n) */
	object.v[2].x = u2f(raster->command_buffer[11] << 8);
	object.v[2].y = u2f(raster->command_buffer[12] << 8);
	object.v[2].pz = u2f(raster->command_buffer[13] << 8);

	if (NumVerts == 4)
	{
		/* extract P1(n) */
		object.v[3].x = u2f(raster->command_buffer[14] << 8);
		object.v[3].y = u2f(raster->command_buffer[15] << 8);
		object.v[3].pz = u2f(raster->command_buffer[16] << 8);
	}
	else
	{
		/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
		raster->command_buffer[14] = raster->command_buffer[11];
		raster->command_buffer[15] = raster->command_buffer[12];
		raster->command_buffer[16] = raster->command_buffer[13];
	}

	/* always calculate the min z and max z value */
	min_z = object.v[0].pz;
	if (object.v[1].pz < min_z) min_z = object.v[1].pz;
	if (object.v[2].pz < min_z) min_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz < min_z) min_z = object.v[3].pz;

	max_z = object.v[0].pz;
	if (object.v[1].pz > max_z) max_z = object.v[1].pz;
	if (object.v[2].pz > max_z) max_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz > max_z) max_z = object.v[3].pz;

	/* read in the texture information */

	/* texture point data */
	if (raster->command_buffer[0] & 0x800000)
		tp = &raster->texture_ram[raster->command_buffer[0] & 0xffff];
	else
		tp = &raster->texture_rom[raster->command_buffer[0] & raster->texture_rom_mask];

	object.v[0].pv = *tp++;
	object.v[0].pu = *tp++;
	object.v[1].pv = *tp++;
	object.v[1].pu = *tp++;
	object.v[2].pv = *tp++;
	object.v[2].pu = *tp++;
	if (NumVerts == 4)
	{
		object.v[3].pv = *tp++;
		object.v[3].pu = *tp++;
	}

	/* update the address */
	raster->command_buffer[0] += NumVerts * 2;

	/* texture header data */
	if (raster->command_buffer[1] & 0x800000)
		th = &raster->texture_ram[raster->command_buffer[1] & 0xffff];
	else
		th = &raster->texture_rom[raster->command_buffer[1] & raster->texture_rom_mask];

	object.texheader[0] = *th++;
	object.texheader[1] = *th++;
	object.texheader[2] = *th++;
	object.texheader[3] = *th++;

	/* extract the texture header offset */
	tho = (attr >> 12) & 0x1f;

	/* adjust for sign */
	if (tho & 0x10)
		tho |= -16;

	/* update the address */
	raster->command_buffer[1] += tho * 4;

	/* set the luma value of this Polygon */
	object.luma = (raster->command_buffer[9] >> 15) & 0xff;

	/* set the texture LOD of this Polygon */
	object.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
	object.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];

	/* determine whether we can cull this Polygon */
	cull = check_culling(raster,attr,min_z,max_z);
	// Counted for diagnostics. Not part of the original.
	if (cull)
		m_culled++;

	/* set the object's z value */
	switch ((attr >> 10) & 3)
	{
		case 0: // old value
			zvalue = raster->polygon_z;
			break;
		case 1: // min z
			zvalue = min_z;
			break;
		case 2: // max z
			zvalue = max_z;
			break;
		case 3: // error
		default:
			zvalue = 1e10;
			break;
	}

	raster->polygon_z = zvalue;

	if (cull == false)
	{
		s32 clipped_verts;
		PolyVertex verts_in[8], verts_out[8];

		for (int i = 0; i < NumVerts; i++)
			verts_in[i] = object.v[i];

		clipped_verts = NumVerts;

		/* do clipping */
		for (int i = 0; i < 4; i++)
		{
			clipped_verts = clip_polygon(verts_in, clipped_verts, verts_out, raster->clip_plane[raster->center_sel][i]);
			for (int j = 0; j < clipped_verts; j++)
				verts_in[j] = verts_out[j];
		}

		if (clipped_verts <= 2)
			m_degenerate++;

		// MAME aborts the whole session when the pool runs out. Dropping the polygon
		// costs part of one frame instead, and the linking further down still has to
		// run or every polygon after this one would be built from stale vertices.
		if (clipped_verts > 2 && raster->poly_list_index >= kMaxPolygons && !m_pool_exhausted)
		{
			m_pool_exhausted = true;
			SM2_WARN("geo: the pool of %u polygons is exhausted; the rest of this frame "
					 "is dropped", kMaxPolygons);
		}

		if (clipped_verts > 2 && raster->poly_list_index < kMaxPolygons)
		{
			Polygon *zpoly;

			/* adjust and set the object z-sort value */
			object.z = float_to_zval(zvalue, raster->z_adjust);

			/* get our list read to add the polygons */
			zpoly = raster->poly_sorted_list[object.z];

			/* go through the clipped vertex list, adding polygons */
			Polygon *poly = &raster->poly_list[raster->poly_list_index++];

			/* copy the object information */
			poly->z = object.z;
			poly->texheader[0] = object.texheader[0];
			poly->texheader[1] = object.texheader[1];
			poly->texheader[2] = object.texheader[2];
			poly->texheader[3] = object.texheader[3];
			poly->luma = object.luma;
			poly->texlod = object.texlod;

			/* set the viewport */
			poly->viewport[0] = raster->viewport[0];
			poly->viewport[1] = raster->viewport[1];
			poly->viewport[2] = raster->viewport[2];
			poly->viewport[3] = raster->viewport[3];

			/* set the center */
			poly->center[0] = raster->center[raster->center_sel][0];
			poly->center[1] = raster->center[raster->center_sel][1];

			/* set the window */
			poly->window = raster->cur_window;

			poly->num_vertices = clipped_verts;

			for (int i = 0; i < clipped_verts; i++)
				poly->v[i] = verts_out[i];

			/* add to our sorted list */
			raster->poly_sorted_list[object.z] = poly;
			poly->next = zpoly;
			zpoly = poly;

			/* keep around the min and max z values for this frame */
			if (object.z < raster->min_z) raster->min_z = object.z;
			if (object.z > raster->max_z) raster->max_z = object.z;
		}
	}

	/* update linking */
	switch (((attr >> 8) & 3))
	{
		case 0:
		case 2:
		{
			/* reuse P0(n) and P1(n) */
			for (i = 0; i < 6; i++)                                        /* P0(n) -> P0(n-1) */
				raster->command_buffer[2+i] = raster->command_buffer[11+i]; /* P1(n) -> P1(n-1) */
		}
		break;

		case 1:
		{
			/* reuse P0(n-1) and P0(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[5+i] = raster->command_buffer[11+i]; /* P0(n) -> P1(n-1) */
		}
		break;

		case 3:
		{
			/* reuse P1(n-1) and P1(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[2+i] = raster->command_buffer[14+i]; /* P1(n) -> P1(n-1) */
		}
		break;
	}
}

inline void Geometrizer::model2_3d_project(Polygon *poly)
{
	for (int i = 0; i < poly->num_vertices; i++)
	{
		/* project the vertices */
		poly->v[i].x = m_crtc_xoffset + poly->center[0] + (poly->v[i].x / (poly->v[i].pz + std::numeric_limits<float>::min()));
		poly->v[i].y = ((384 - poly->center[1])+m_crtc_yoffset) - (poly->v[i].y / (poly->v[i].pz + std::numeric_limits<float>::min()));
	}
}

/* 3D Rasterizer frame start: Resets frame variables */
void Geometrizer::render_frame_start()
{
	RasterState *raster = m_raster.get();

	/* reset the polygon list index */
	raster->poly_list_index = 0;

	/* reset the sorted z list */
	std::fill(std::begin(raster->poly_sorted_list), std::end(raster->poly_sorted_list), nullptr);

	/* reset the min-max sortable Z values */
	raster->min_z = 0xffff;
	raster->max_z = 0;

	/* reset the polygon z value */
	// Zero Gunner sets backgrounds with "previous z value" mode at the start of the display list,
	// needs this to be this big in order to work properly
	raster->polygon_z = 1e10;

	raster->cur_window = 0;

	m_render_done = false;
}

void Geometrizer::model2_3d_push(RasterState *raster, u32 input)
{
	/* see if we have a command in progress */
	if (raster->cur_command != 0)
	{
		raster->command_buffer[raster->command_index++] = input;

		switch (raster->cur_command)
		{
			case 0x00:  /* NOP */
			break;

			case 0x01:  /* Polygon Data */
			{
				u32  attr;

				/* start by looking if we have the basic input data */
				if (raster->command_index < 9)
					return;

				/* get the attributes */
				attr = raster->command_buffer[8];

				/* see if we're done */
				if ((attr & 3) == 0)
				{
					raster->cur_command = 0;
					return;
				}

				/* see if it's a quad or a triangle */
				if (attr & 1)
				{
					/* it's a quad, wait for the rest of the points */
					if (raster->command_index < 17)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<4>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
				else
				{
					/* it's a triangle, wait for the rest of the point */
					if (raster->command_index < 14)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<3>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
			}
			break;

			case 0x03:  /* Window Data */
			{
				u32  i;

				/* make sure we have all the data */
				if (raster->command_index < 6)
					return;

				/* coordinates are 12 bit signed */

				/* extract the viewport start x */
				raster->viewport[0] = (raster->command_buffer[0] >> 12) & 0xfff;

				if (raster->viewport[0] & 0x800)
					raster->viewport[0] = -(0x800 - (raster->viewport[0] & 0x7ff));

				/* extract the viewport start y */
				raster->viewport[1] = raster->command_buffer[0] & 0xfff;

				if (raster->viewport[1] & 0x800)
					raster->viewport[1] = -(0x800 - (raster->viewport[1] & 0x7ff));

				/* extract the viewport end x */
				raster->viewport[2] = (raster->command_buffer[1] >> 12) & 0xfff;

				if (raster->viewport[2] & 0x800)
					raster->viewport[2] = -(0x800 - (raster->viewport[2] & 0x7ff));

				/* extract the viewport end y */
				raster->viewport[3] = raster->command_buffer[1] & 0xfff;

				if (raster->viewport[3] & 0x800)
					raster->viewport[3] = -(0x800 - (raster->viewport[3] & 0x7ff));

				/* extract the centers */
				for (i = 0; i < 4; i++)
				{
					/* center x */
					raster->center[i][0] = (raster->command_buffer[2+i] >> 12) & 0xfff;

					if (raster->center[i][0] & 0x800)
						raster->center[i][0] = -(0x800 - (raster->center[i][0] & 0x7ff));

					/* center y */
					raster->center[i][1] = raster->command_buffer[2+i] & 0xfff;

					if (raster->center[i][1] & 0x800)
						raster->center[i][1] = -(0x800 - (raster->center[i][1] & 0x7ff));

					// calculate clipping planes
					float left_plane = float(raster->center[i][0] - raster->viewport[0]);
					float right_plane = float(raster->viewport[2] - raster->center[i][0]);
					float top_plane = float(raster->viewport[3] - raster->center[i][1]);
					float bottom_plane = float(raster->center[i][1] - raster->viewport[1]);

					raster->clip_plane[i][0].normal.x = 1.0f / std::hypot(1.0f, left_plane);
					raster->clip_plane[i][0].normal.y = 0.0f;
					raster->clip_plane[i][0].normal.pz = left_plane / std::hypot(1.0f, left_plane);

					raster->clip_plane[i][1].normal.x = -1.0f / std::hypot(-1.0f, right_plane);
					raster->clip_plane[i][1].normal.y = 0.0f;
					raster->clip_plane[i][1].normal.pz = right_plane / std::hypot(-1.0f, right_plane);

					raster->clip_plane[i][2].normal.x = 0.0f;
					raster->clip_plane[i][2].normal.y = -1.0f / std::hypot(-1.0f, top_plane);
					raster->clip_plane[i][2].normal.pz = top_plane / std::hypot(-1.0f, top_plane);

					raster->clip_plane[i][3].normal.x = 0.0f;
					raster->clip_plane[i][3].normal.y = 1.0f / std::hypot(1.0f, bottom_plane);
					raster->clip_plane[i][3].normal.pz = bottom_plane / std::hypot(1.0f, bottom_plane);
				}

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			case 0x04:  /* Texture/Log Data write */
			{
				/* make sure we have enough data */
				if (raster->command_index < 2)
					return;

				/* see if the count is non-zero */
				if (raster->command_buffer[1] > 0)
				{
					/* see if we have data available */
					if (raster->command_index >= 3)
					{
						/* get the address */
						u32  address = raster->command_buffer[0];

						/* do the write */
						if (address & 0x800000)
							raster->texture_ram[address & 0xffff] = raster->command_buffer[2];
						else
							raster->log_ram[address & 0x7fff] = raster->command_buffer[2];

						/* increment the address and decrease the count */
						raster->command_buffer[0]++;
						raster->command_buffer[1]--;

						/* decrease the index, so we keep placing data in the same slot */
						raster->command_index--;
					}
				}

				/* see if we're done with this command */
				if (raster->command_buffer[1] == 0)
					raster->cur_command = 0;
			}
			break;

			case 0x08:  /* ZSort mode */
			{
				/* save the zsort mode value */
				raster->z_adjust = raster->command_buffer[0] << 8;

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			default:
			{
				// MAME aborts here. An unknown command means the display list is not
				// what we think it is, which is worth knowing about but not worth
				// losing the session over.
				if (!m_unknown_command_warned)
				{
					m_unknown_command_warned = true;
					SM2_WARN("geo: unknown rasterizer command %08x", raster->cur_command);
				}
				raster->cur_command = 0;
			}
		}
	}
	else
	{
		/* new command */
		raster->cur_command = input & 0x0f;
		raster->command_index = 0;

		/* see if it's object data */
		if (raster->cur_command == 1)
		{
			/* extract reverse bit */
			raster->reverse = (input >> 4) & 1;

			/* extract center select */
			raster->center_sel = (input >> 6) & 3;
		}
	}
}

void Geometrizer::geo_parse_np_ns(GeoState *geo, u32 *input, u32 count)
{
	RasterState *raster = geo->raster;
	PolyVertex point, normal;
	u32  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		/* transform with the current matrix */
		transform_vector(&normal, geo->matrix);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			s32               luma;
			TextureParameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the Polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (s32)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: Normals Present, Specular case */
void Geometrizer::geo_parse_np_s(GeoState *geo, u32 *input, u32 count)
{
	RasterState *raster = geo->raster;
	PolyVertex point, normal;
	u32  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		/* transform with the current matrix */
		transform_vector(&normal, geo->matrix);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			s32             luma;
			TextureParameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the Polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (s32)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, No Specular case */
void Geometrizer::geo_parse_nn_ns(GeoState *geo, u32 *input, u32 count)
{
	RasterState *raster = geo->raster;
	PolyVertex point, normal, p0, p1, p2, p3;
	u32  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			s32             luma;
			TextureParameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the Polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (s32)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, Specular case */
void Geometrizer::geo_parse_nn_s(GeoState *geo, u32 *input, u32 count)
{
	RasterState *raster = geo->raster;
	PolyVertex point, normal, p0, p1, p2, p3;
	u32  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			s32             luma;
			TextureParameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the Polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (s32)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/*******************************************
 *
 *  Geometry Engine Commands
 *
 *******************************************/

/* Command 00: NOP */
u32 *Geometrizer::geo_nop(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;

	/* push the opcode to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);

	return input;
}

/* Command 01: Object Data */
u32 *Geometrizer::geo_object_data(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;
	u32  tpa = *input++;     /* Texture Point Address */
	u32  tha = *input++;     /* Texture Header Address */
	u32  oba = *input++;     /* Object Address */
	u32  obc = *input++;     /* Object Count */

	u32 *obp;                /* Object Pointer */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* select where we're reading polygon information from */
	if (oba & 0x01000000)
	{
		/* Fast polygon RAM */
		obp = &geo->polygon_ram1[oba & 0x7fff];
	}
	else if (oba & 0x00800000)
	{
		/* Polygon ROM */
		obp = &geo->polygon_rom[oba & geo->polygon_rom_mask];
	}
	else
	{
		/* Slow Polygon RAM */
		obp = &geo->polygon_ram0[oba & 0x7fff];
	}

	// if count == 0 then rolls over to max size
	// Virtual On & Gunblade NY
	if (obc == 0)
		obc = 0xfffff;

	switch (geo->mode & 3)
	{
		/* Normals present, No Specular */
		case 0: geo_parse_np_ns(geo, obp, obc); break;

		/* Normals present, Specular */
		case 1: geo_parse_np_s(geo, obp, obc); break;

		/* No Normals present, No Specular */
		case 2: geo_parse_nn_ns(geo, obp, obc); break;

		/* No Normals present, Specular */
		case 3: geo_parse_nn_s(geo, obp, obc); break;
	}

	/* move by 4 parameters */
	return input;
}

/* Command 02: Direct Data */
u32 *Geometrizer::geo_direct_data(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;
	u32  tpa = *input++;     /* Texture Point Address */
	u32  tha = *input++;     /* Texture Header Address */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, (opcode >> 23) - 1);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* push the initial points */
	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	/* read in the attributes */
	u32  attr;
	while (((attr = *input++) & 3) != 0)
	{
		/* push attributes */
		model2_3d_push(raster, attr & 0x00ffffff);

		/* push luma */
		model2_3d_push(raster, (*input++) >> 8);

		/* push distance */
		model2_3d_push(raster, (*input++) >> 8);

		/* push the next point */
		model2_3d_push(raster, (*input++) >> 8); /* x */
		model2_3d_push(raster, (*input++) >> 8); /* y */
		model2_3d_push(raster, (*input++) >> 8); /* z */

		/* if it's a quad, output another point */
		if (attr & 1)
		{
			model2_3d_push(raster, (*input++) >> 8); /* x */
			model2_3d_push(raster, (*input++) >> 8); /* y */
			model2_3d_push(raster, (*input++) >> 8); /* z */
		}
	}

	/* we're done */
	model2_3d_push(raster, 0);

	return input;
}

/* Command 03: Window Data */
u32 *Geometrizer::geo_window_data(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	raster->cur_window++;

	/*
	    we're going to move 6 coordinates to the 3d rasterizer:
	    - starting coordinate
	    - completion coordinate
	    - vanishing point 0 (eye mode 0)
	    - vanishing point 1 (eye mode 1)
	    - vanishing point 2 (eye mode 2)
	    - vanishing point 3 (eye mode 3)
	*/

	for (u32 i = 0; i < 6; i++)
	{
		/* read in the coordinate */
		u32 y = *input++;

		/* convert to the 3d rasterizer format (00XXXYYY) */
		u32 x = (y & 0x0fff0000) >> 4 ;
		y &= 0xfff;

		/* push it */
		model2_3d_push(raster, x | y);
	}

	return input;
}

/* Command 04: Texture Data Write */
u32 *Geometrizer::geo_texture_data(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	u32 count = *input++;

	/* push the count */
	model2_3d_push(raster, count);

	/* loop and send the data */
	for (u32 i = 0; i < count; i++)
		model2_3d_push(raster, *input++);

	return input;
}

/* Command 05: Polygon Data */
u32 *Geometrizer::geo_polygon_data(GeoState *geo, u32 opcode, u32 *input)
{
	u32  address, count, i;
	u32 *p;

	(void)opcode;

	/* read in the address */
	address = *input++;

	/* prepare the pointer */
	if (address & 0x01000000)
	{
		/* Fast polygon RAM */
		p = &geo->polygon_ram1[address & 0x7fff];
	}
	else
	{
		/* Slow Polygon RAM */
		p = &geo->polygon_ram0[address & 0x7fff];
	}

	/* read the count */
	count = *input++;

	/* move the data */
	for (i = 0; i < count; i++)
		*p++ = *input++;

	return input;
}

/* Command 06: Texture Parameters */
u32 *Geometrizer::geo_texture_parameters(GeoState *geo, u32 opcode, u32 *input)
{
	u32  index, count, i, param;

	(void)opcode;

	/* read in the index */
	index = (*input++) >> 2;

	/* read in the conut */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		/* read in the texture parameters */
		param = *input++;

		geo->texture_parameters[index].diffuse = float(param & 0xff);
		geo->texture_parameters[index].ambient = float((param >> 8) & 0xff);
		geo->texture_parameters[index].specular_control = (param >> 24) & 0xff;
		geo->texture_parameters[index].specular_scale = float((param >> 16) & 0xff);

		/* read in the distance coefficient */
		geo->coef_table[index] = u2f(*input++);

		index = (index + 1) & 0x1f;
	}

	return input;
}

/* Command 07: Geo Mode */
u32 *Geometrizer::geo_mode(GeoState *geo, u32 opcode, u32 *input)
{
	(void)opcode;

	/* read in the mode */
	geo->mode = *input++;

	return input;
}

/* Command 08: ZSort Mode */
u32 *Geometrizer::geo_zsort_mode(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;

	/* push the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the mode */
	model2_3d_push(raster, (*input++) >> 8);

	return input;
}

/* Command 09: Focal Distance */
u32 *Geometrizer::geo_focal_distance(GeoState *geo, u32 opcode, u32 *input)
{
	(void)opcode;

	/* read the x focus value */
	geo->focus.x = u2f(*input++);

	/* read the y focus value */
	geo->focus.y = u2f(*input++);

	return input;
}

/* Command 0A: Light Source Vector Write */
u32 *Geometrizer::geo_light_source(GeoState *geo, u32 opcode, u32 *input)
{
	(void)opcode;

	/* read the x light value */
	geo->light.x = u2f(*input++);

	/* read the y light value */
	geo->light.y = u2f(*input++);

	/* read the z light value */
	geo->light.pz = u2f(*input++);

	return input;
}

/* Command 0B: Transformation Matrix Write */
u32 *Geometrizer::geo_matrix_write(GeoState *geo, u32 opcode, u32 *input)
{
	u32  i;

	(void)opcode;

	/* read in the transformation matrix */
	for (i = 0; i < 12; i++)
		geo->matrix[i] = u2f(*input++);

	return input;
}

/* Command 0C: Parallel Transfer Vector Write */
u32 *Geometrizer::geo_translate_write(GeoState *geo, u32 opcode, u32 *input)
{
	u32  i;

	(void)opcode;

	/* read in the translation vector */
	for (i = 0; i < 3; i++)
		geo->matrix[i+9] = u2f(*input++);

	return input;
}

/* Command 0D: Geo Data Memory Push (undocumented, unsupported) */
u32 *Geometrizer::geo_data_mem_push(GeoState *geo, u32 opcode, u32 *input)
{
	u32  address, count, i;

	/*
	    This command pushes data stored in the Geometry DSP's RAM
	    to the hardware 3D rasterizer. Since we don't emulate the
	    DSP, we don't know what the RAM contents are.

	    Eventually, we could check for the address, and if it
	    happens to point to a polygon ROM, we could potentially
	    emulate it partially.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the address */
	address = *input++;

	/* read in the count */
	count = *input++;

	SM2_WARN("SEGA GEO: Executing unsupported geo_data_mem_push (address = %08x, count = %08x)\n", address, count);

	(void)i;
/*
    for (i = 0; i < count; i++)
        model2_3d_push(0);
*/

	return input;
}

/* Command 0E: Geo Test */
u32 *Geometrizer::geo_test(GeoState *geo, u32 opcode, u32 *input)
{
	u32      data, blocks, address, count, checksum, i;

	(void)opcode;

	/* fifo test */
	data = 1;

	for (i = 0; i < 32; i++)
	{
		if (*input++ != data)
		{
			/* TODO: Set Red LED on */
			SM2_WARN("SEGA GEO: FIFO Test failed\n");
		}

		data <<= 1;
	}

	/* get the number of checksums we have to run */
	blocks = *input++;

	for (i = 0; i < blocks; i++)
	{
		u32  sum_even, sum_odd, j;

		/* read in the address */
		address = (*input++) & 0x7fffff;

		/* read in the count */
		count = *input++;

		/* read in the checksum */
		checksum = *input++;

		/* reset the checksum counters */
		sum_even = 0;
		sum_odd = 0;

		for (j = 0; j < count; j++)
		{
			data = geo->polygon_rom[address++];

			address &= geo->polygon_rom_mask;

			sum_even += data >> 16;
			sum_even &= 0xffff;

			sum_odd += data & 0xffff;
			sum_odd &= 0xffff;
		}

		sum_even += checksum >> 16;
		sum_even &= 0xffff;

		sum_odd += checksum & 0xffff;
		sum_odd &= 0xffff;

		if (sum_even != 0 || sum_odd != 0)
		{
			/* TODO: Set Green LED on */
			SM2_WARN("SEGA GEO: Polygon ROM Test failed\n");
		}
	}

	return input;
}

/* Command 0F: End */
u32 *Geometrizer::geo_end(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;

	(void)opcode;

	/* signal the end of this data block the rasterizer */
	model2_3d_push(raster, 0xff000000);

	/* signal end by returning nullptr */
	return nullptr;
}

/* Command 10: Dummy */
u32 *Geometrizer::geo_dummy(GeoState *geo, u32 opcode, u32 *input)
{
//  u32  data;
	(void)opcode;

	/* do the dummy read cycle */
//  data = *input++;
	input++;

	return input;
}

/* Command 14: Log Data Write */
u32 *Geometrizer::geo_log_data(GeoState *geo, u32 opcode, u32 *input)
{
	RasterState *raster = geo->raster;
	u32  i, count;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	count = *input++;

	/* push the count */
	model2_3d_push(raster, count << 2);

	/* loop and send the data */
	for (i = 0; i < count; i++)
	{
		u32  data = *input++;

		model2_3d_push(raster, data & 0xff);
		model2_3d_push(raster, (data >> 8) & 0xff);
		model2_3d_push(raster, (data >> 16) & 0xff);
		model2_3d_push(raster, (data >> 24) & 0xff);
	}

	return input;
}

/* Command 16: LOD */
u32 *Geometrizer::geo_lod(GeoState *geo, u32 opcode, u32 *input)
{
	(void)opcode;

	/* read in the LOD */
	geo->lod = u2f(*input++);

	return input;
}

/* Command 1D: Code Upload  (undocumented, unsupported) */
u32 *Geometrizer::geo_code_upload(GeoState *geo, u32 opcode, u32 *input)
{
	u32  count, i;

	/*
	    This command uploads code to program memory and
	    optionally runs it. Probably used for debugging.

	    No games are known to use this command yet.
	*/

	SM2_WARN("SEGA GEO: Uploading debug code (unimplemented)\n");

	(void)opcode;

	/* read in the flags */
//  flags = *input++;
	input++;

	/* read in the count */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		[[maybe_unused]] u64  code;

		/* read the top part of the opcode */
		code = *input++;

		code <<= 32;

		/* the bottom part comes in two pieces */
		code |= *input++;
		code |= (*input++) << 16;
	}

	/*
	    Bit 10 of flags indicate whether to run iummediately after upload
	*/

/*
    if (flags & 0x400)
        code_jump();
*/

	return input;
}

/* Command 1E: Code Jump (undocumented, unsupported) */
u32 *Geometrizer::geo_code_jump(GeoState *geo, u32 opcode, u32 *input)
{
//  u32  address;

	/*
	    This command jumps to a specified address in program
	    memory. Code can be uploaded with function 1D.
	    Probably used for debugging.

	    No games are known to use this command yet.
	*/

	SM2_WARN("SEGA GEO: Jumping to debug code (unimplemented)\n");

	(void)opcode;

//  address = *input++ & 0x3ff;
	input++;

/*
    code_jump(address)
*/
	return input;
}

u32 *Geometrizer::geo_process_command(GeoState *geo, u32 opcode, u32 *input, bool *end_code)
{
	switch ((opcode >> 23) & 0x1f)
	{
		case 0x00: input = geo_nop(geo, opcode, input);                   break;
		case 0x01: input = geo_object_data(geo, opcode, input);           break;
		case 0x02: input = geo_direct_data(geo, opcode, input);           break;
		case 0x03: input = geo_window_data(geo, opcode, input);           break;
		case 0x04: input = geo_texture_data(geo, opcode, input);          break;
		case 0x05: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x06: input = geo_texture_parameters(geo, opcode, input);    break;
		case 0x07: input = geo_mode(geo, opcode, input);                  break;
		case 0x08: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x09: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x0a: input = geo_light_source(geo, opcode, input);          break;
		case 0x0b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x0c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x0d: input = geo_data_mem_push(geo, opcode, input);         break;
		case 0x0e: input = geo_test(geo, opcode, input);                  break;
		case 0x0f: input = geo_end(geo, opcode, input); *end_code = true; break;
		case 0x10: input = geo_dummy(geo, opcode, input);                 break;
		case 0x11: input = geo_object_data(geo, opcode, input);           break;
		case 0x12: input = geo_direct_data(geo, opcode, input);           break;
		case 0x13: input = geo_window_data(geo, opcode, input);           break;
		case 0x14: input = geo_log_data(geo, opcode, input);              break;
		case 0x15: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x16: input = geo_lod(geo, opcode, input);                   break;
		case 0x17: input = geo_mode(geo, opcode, input);                  break;
		case 0x18: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x19: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x1a: input = geo_light_source(geo, opcode, input);          break;
		case 0x1b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x1c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x1d: input = geo_code_upload(geo, opcode, input);           break;
		case 0x1e: input = geo_code_jump(geo, opcode, input);             break;
		case 0x1f: input = geo_end(geo, opcode, input); *end_code = true; break;
	}

	return input;
}

void Geometrizer::geo_parse()
{
	u32  address = (m_geo_read_start_address & 0x1ffff)/4;
	u32 *input = &m_bufferram[address];
	u32  opcode;
	u32  op_count = 0;
	bool end_code = false;

	// reset raster frame variables
	render_frame_start();

	while (end_code == false && (input - m_bufferram) < 0x20000/4 && op_count++ < 0x8000)
	{
		/* read in the opcode */
		opcode = *input++;

		/* if it's a jump opcode, do the jump */
		if (opcode & 0x80000000)
		{
			/* get the address */
			address = (opcode & 0x1ffff) / 4;

			/* update our pointer */
			input = &m_bufferram[address];

			/* go again */
			continue;
		}

		/* process it */
		input = geo_process_command(m_geo.get(), opcode, input, &end_code);
	}
}


#undef pz
#undef pu
#undef pv

}  // namespace sm2::hw
