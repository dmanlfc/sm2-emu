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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DShaderTriangles.h,
// R3DShaderCommon.h and R3DFrameBuffers.cpp, Copyright (c) 2003-2026 The
// Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The shader functions follow shaders/model3_r3d.vert, model3_r3d.frag,
// model3_scroll_fog.frag, model3_layer.frag and model3_composite_*.frag line
// for line, in single precision.

#include "render/model3/soft_render.h"

#include "render/model3/r3d_math.h"
#include "render/model3/textures.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sm2::render::model3 {

namespace {

// Varyings, in model3_r3d.vert's output order.
enum : int {
    kViewX = 0, kViewY, kViewZ,
    kNormX, kNormY, kNormZ,
    kTexU, kTexV,
    kColR, kColG, kColB, kColA,
    kFixedShade,
    kDiscard,
    kLodBase,
    kVary
};

constexpr s32 kSubBits  = 8;
constexpr s32 kSub      = 1 << kSubBits;
constexpr u32 kChunkTris = 256;
constexpr s32 kBandRows  = 8;
constexpr float kGuard   = 1024.0f;  // guard band, in pixels from the viewport centre

struct ClipVertex {
    float p[4];
    float v[kVary];
};

inline float fract(float x)
{
    return x - std::floor(x);
}

inline float mix(float a, float b, float t)
{
    return a * (1.0f - t) + b * t;
}

inline float clampf(float x, float lo, float hi)
{
    return std::fmin(std::fmax(x, lo), hi);
}

inline u32 to_unorm(float c)
{
    c = std::fmin(std::fmax(c, 0.0f), 1.0f);  // NaN becomes 0
    return static_cast<u32>(c * 255.0f + 0.5f);
}

inline u32 pack(const float c[4])
{
    return to_unorm(c[0]) | (to_unorm(c[1]) << 8) | (to_unorm(c[2]) << 16) | (to_unorm(c[3]) << 24);
}

inline void unpack(u32 p, float c[4])
{
    constexpr float k = 1.0f / 255.0f;
    c[0] = static_cast<float>(p & 0xFF) * k;
    c[1] = static_cast<float>((p >> 8) & 0xFF) * k;
    c[2] = static_cast<float>((p >> 16) & 0xFF) * k;
    c[3] = static_cast<float>(p >> 24) * k;
}

// SRC_ALPHA, ONE_MINUS_SRC_ALPHA on colour and alpha, into an RGBA8 target.
inline u32 blend(u32 dst, const float s[4])
{
    if (!(s[3] > 0.0f)) {
        return dst;
    }
    float d[4];
    unpack(dst, d);
    const float ia = 1.0f - s[3];
    const float o[4] = {s[0] * s[3] + d[0] * ia, s[1] * s[3] + d[1] * ia, s[2] * s[3] + d[2] * ia,
                        s[3] * s[3] + d[3] * ia};
    return pack(o);
}

}  // namespace

struct SoftRenderer::Tri {
    s32   fx[3], fy[3];  // window position, kSubBits of fraction
    float z[3];          // depth
    float iw[3];         // 1 / clip w
    float v[3][kVary];
    s16   minx, maxx, miny, maxy;  // pixels touched, inclusive
    bool  cull_per_pixel;
};

struct SoftRenderer::Chunk {
    u32              draw  = 0;
    u32              first = 0;  // triangle within the draw
    u32              count = 0;
    s32              miny  = 0;
    s32              maxy  = -1;
    std::vector<Tri> tris;
    std::vector<u32> rows;  // miny | maxy << 16 of each triangle
};

struct SoftRenderer::Draw {
    const R3DUniforms*      u = nullptr;
    const R3DModelUniforms* m = nullptr;
    const FVertex*          verts = nullptr;  // the draw's first vertex
    u32                     tris  = 0;
    float                   pm[16]{};  // projMat * modelMat
    PixelRect               viewport;
    PixelRect               scissor;
    bool                    layered = false;
    bool                    no_los  = false;
};

namespace {

using Draw = SoftRenderer::Draw;

// ---------------------------------------------------------------------------
// Fragment shader (model3_r3d.frag)
// ---------------------------------------------------------------------------

struct Shader {
    const R3DUniforms&   u;
    std::span<const u16> texels;

    void extract_colour(u32 value, float c[4]) const
    {
        u32 bits[4];
        for (int i = 0; i < 4; i++) {
            bits[i] = (value >> u.texShift[i]) & u.texMask[i];
            const float f = static_cast<float>(bits[i]);
            c[i] = u.texDivide != 0 ? f / u.texScale[i] : f * u.texScale[i];
        }
        if (u.texAlphaInvert != 0) {
            c[3] = bits[3] == u.texMask[3] ? 0.0f : 1.0f;
        }
    }

    [[nodiscard]] u16 fetch(int page, s32 x, s32 y, int level) const
    {
        if (level < 0 || level >= static_cast<int>(TextureSheets::kStoredLevels)) {
            return 0;
        }
        const usize row = static_cast<usize>(page & 1) * TextureSheets::kPageHeight
                          + TextureSheets::kMipYBase[static_cast<usize>(level)] + static_cast<u32>(y);
        const usize col = TextureSheets::kMipXBase[static_cast<usize>(level)] + static_cast<u32>(x);
        if (row >= TextureSheets::kSize || col >= TextureSheets::kSize) {
            return 0;
        }
        return texels[row * TextureSheets::kSize + col];
    }

    static float linear_tex_locations(int wrapMode, float size, float uin, float& u0, float& u1)
    {
        const float texelSize     = 1.0f / size;
        const float halfTexelSize = 0.5f / size;
        float       uu            = uin;

        if (wrapMode == 0) {
            uu = uu * size - 0.5f;
            u0 = (std::floor(uu) + 0.5f) / size;
            u0 = fract(u0);
            u1 = u0 + texelSize;
            u1 = fract(u1);
            return fract(uu);
        }
        if (wrapMode == 1) {
            uu = fract(uu);
            uu = uu * size - 0.5f;
            u0 = (std::floor(uu) + 0.5f) / size;
            u1 = u0 + texelSize;
            if (u0 < 0.0f) u0 = 0.0f;
            if (u1 >= 1.0f) u1 = 1.0f - halfTexelSize;
            return fract(uu);
        }
        const float odd = std::floor(uu - 2.0f * std::floor(uu / 2.0f));
        if (odd > 0.0f) {
            uu = 1.0f - fract(uu);
        } else {
            uu = fract(uu);
        }
        uu = uu * size - 0.5f;
        u0 = (std::floor(uu) + 0.5f) / size;
        u1 = u0 + texelSize;
        if (u0 < 0.0f) u0 = 0.0f;
        if (u1 >= 1.0f) u1 = 1.0f - halfTexelSize;
        return fract(uu);
    }

    void tex_bilinear(int page, int wrapS, int wrapT, float sizeX, float sizeY, s32 posX, s32 posY, float s,
                      float t, int level, float out[4]) const
    {
        float tx[2], ty[2];
        const float a = linear_tex_locations(wrapS, sizeX, s, tx[0], tx[1]);
        const float b = linear_tex_locations(wrapT, sizeY, t, ty[0], ty[1]);

        const s32 mx = 2047 >> level;
        const s32 my = 1023 >> level;
        float     p[4][4];  // p0q0, p1q0, p0q1, p1q1
        for (int i = 0; i < 4; i++) {
            const s32 x = static_cast<s32>(tx[i & 1] * sizeX + static_cast<float>(posX)) & mx;
            const s32 y = static_cast<s32>(ty[i >> 1] * sizeY + static_cast<float>(posY)) & my;
            extract_colour(fetch(page, x, y, level), p[i]);
        }

        if (u.alphaTest != 0) {
            auto copy_rgb = [](float* d, const float* s3) {
                d[0] = s3[0];
                d[1] = s3[1];
                d[2] = s3[2];
            };
            if (p[0][3] > p[1][3]) copy_rgb(p[1], p[0]);
            if (p[0][3] > p[2][3]) copy_rgb(p[2], p[0]);

            if (p[1][3] > p[0][3]) copy_rgb(p[0], p[1]);
            if (p[1][3] > p[3][3]) copy_rgb(p[3], p[1]);

            if (p[2][3] > p[0][3]) copy_rgb(p[0], p[2]);
            if (p[2][3] > p[3][3]) copy_rgb(p[3], p[2]);

            if (p[3][3] > p[2][3]) copy_rgb(p[2], p[3]);
            if (p[3][3] > p[1][3]) copy_rgb(p[1], p[3]);
        }

        for (int c = 0; c < 4; c++) {
            const float q0 = mix(p[0][c], p[1][c], a);
            const float q1 = mix(p[2][c], p[3][c], a);
            out[c]         = mix(q0, q1, b);
        }
    }

    void sample_bank(float s, float t, float lod, float out[4]) const
    {
        const float numLevels =
            std::floor(std::log2(std::fmin(static_cast<float>(u.baseTexInfo[2]), static_cast<float>(u.baseTexInfo[3])))) - 1.0f;
        const float fLevel = clampf(lod, 0.0f, numLevels);
        const int   iLevel = static_cast<int>(fLevel);

        float tex1[4];
        tex_bilinear(u.texturePage, u.textureWrapMode[0], u.textureWrapMode[1],
                     static_cast<float>(u.baseTexInfo[2] >> iLevel), static_cast<float>(u.baseTexInfo[3] >> iLevel),
                     u.baseTexInfo[0] >> iLevel, u.baseTexInfo[1] >> iLevel, s, t, iLevel, tex1);

        float tex2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float blendFactor = 0.0f;
        const float ffL = fract(fLevel);
        if (ffL > 0.0f) {
            const int l2 = iLevel + 1;
            tex_bilinear(u.texturePage, u.textureWrapMode[0], u.textureWrapMode[1],
                         static_cast<float>(u.baseTexInfo[2] >> l2), static_cast<float>(u.baseTexInfo[3] >> l2),
                         u.baseTexInfo[0] >> l2, u.baseTexInfo[1] >> l2, s, t, l2, tex2);
            blendFactor = ffL;
        } else if (u.microTexture != 0 && lod < 0.0f) {
            static constexpr float kScaleIndex[4] = {2.0f, 4.0f, 16.0f, 256.0f};
            static constexpr s32   kMicroX[8]     = {0, 0, 128, 128, 0, 0, 128, 128};
            static constexpr s32   kMicroY[8]     = {0, 128, 0, 128, 256, 384, 256, 384};
            const float scale = kScaleIndex[std::clamp(static_cast<int>(u.microTextureMinLOD), 0, 3)];
            const float sx    = (static_cast<float>(u.baseTexInfo[2]) / 128.0f) * scale;
            const float sy    = (static_cast<float>(u.baseTexInfo[3]) / 128.0f) * scale;
            const int   id    = u.microTextureID & 7;
            tex_bilinear((u.texturePage + 1) & 1, 0, 0, 128.0f, 128.0f, kMicroX[id], kMicroY[id], s * sx, t * sy, 0,
                         tex2);
            blendFactor = -lod * std::exp2(-u.microTextureMinLOD) * 0.5f;
            blendFactor = std::fmin(blendFactor, 0.5f);
        }
        for (int c = 0; c < 4; c++) {
            out[c] = mix(tex1[c], tex2[c], blendFactor);
        }
    }

    // GetTextureValue(); false = discard.
    bool texture_value(const float* v, float fw, float out[4]) const
    {
        const float lod = -std::log2(fw * fw * v[kLodBase]);
        sample_bank(v[kTexU], v[kTexV], lod, out);

        if (u.textureInverted != 0) {
            out[0] = 1.0f - out[0];
            out[1] = 1.0f - out[1];
            out[2] = 1.0f - out[2];
        }
        if (u.alphaTest != 0 && out[3] < (32.0f / 255.0f)) {
            return false;
        }
        if (u.textureAlpha != 0) {
            if (u.discardAlpha != 0) {
                if (out[3] < 1.0f) {
                    return false;
                }
            } else if ((out[3] * v[kColA]) >= 1.0f) {
                return false;
            }
        }
        if (u.textureAlpha == 0 && u.alphaTest == 0) {
            out[3] = 1.0f;
        }
        return true;
    }

    // main() after the back-face test and depth; false = discard.
    bool shade(const float* v, float fw, float fragX, float fragY, float out[4]) const
    {
        const float fogAlpha = u.fogIntensity * clampf(u.fogStart + (-v[kViewZ]) * u.fogDensity, 0.0f, 1.0f);
        const float fogRGB[3] = {u.fogColour[0] * u.fogAmbient, u.fogColour[1] * u.fogAmbient,
                                 u.fogColour[2] * u.fogAmbient};

        float tex[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (u.textureEnabled != 0 && !texture_value(v, fw, tex)) {
            return false;
        }

        float col[4] = {v[kColR], v[kColG], v[kColB], v[kColA]};
        if (u.hardwareStep == 0x15 && u.lightEnabled == 0) {
            const float k = u.fixedShading != 0 ? 1.0f + v[kFixedShade] + u.lighting[1][1] : 1.5f;
            col[0] *= k;
            col[1] *= k;
            col[2] *= k;
        }
        float fin[4] = {tex[0] * col[0], tex[1] * col[1], tex[2] * col[2], tex[3] * col[3]};
        if (fin[3] < (1.0f / 32.0f)) {
            return false;
        }

        // ComputeSpotlight()
        const float sx = fragX;
        const float sy = u.fragCoordYTransform[1] + u.fragCoordYTransform[0] * fragY;
        const float ex = (sx - u.spotEllipse[0]) / u.spotEllipse[2];
        const float ey = (sy - u.spotEllipse[1]) / u.spotEllipse[3];
        const float ellipse = std::fmax(0.0f, 1.0f - (ex * ex + ey * ey));
        const float enable  = (-v[kViewZ] < u.spotRange[0]) ? 0.0f : 1.0f;
        float range = 0.0f;
        if (u.spotRange[1] != 0.0f) {
            const float absExtent = std::fabs(u.spotRange[1]);
            const float d         = std::fmin(u.spotRange[0] + absExtent + v[kViewZ], 0.0f);
            const float inv_r     = 1.0f / (1.0f + absExtent);
            const float q         = d * inv_r - 1.0f;
            range = 1.0f / (q * q);
            range *= enable;
        }
        const float lobeEffect    = range * ellipse;
        const float lobeFogEffect = enable * ellipse;

        if (u.lightEnabled != 0) {
            const float* sun = u.lighting[0];
            float sunFactor;
            if (u.fixedShading != 0) {
                sunFactor = v[kFixedShade];
            } else {
                sunFactor = sun[0] * v[kNormX] + sun[1] * v[kNormY] + sun[2] * v[kNormZ];
            }
            sunFactor = clampf(sunFactor, -1.0f, 1.0f);
            if (u.sunClamp != 0 || fin[3] < 0.999f) {
                sunFactor = std::fmax(sunFactor, 0.0f);
            }
            float li[3];
            for (int c = 0; c < 3; c++) {
                li[c] = sunFactor * u.lighting[1][0] + u.lighting[1][1];
                li[c] += u.spotColor[c] * lobeEffect;
                if (u.intensityClamp != 0) {
                    li[c] = std::fmin(li[c], 1.0f);
                }
                fin[c] *= li[c];
            }

            if (u.specularEnabled != 0) {
                float specularFactor;
                if (u.smoothShading != 0) {
                    static constexpr float kExp[4]  = {8.0f, 16.0f, 32.0f, 64.0f};
                    static constexpr float kMult[4] = {1.6f, 1.6f, 2.4f, 3.2f};
                    const int   s     = std::clamp(static_cast<int>(u.shininess), 0, 3);
                    const float NdotL = std::fmax(0.0f, sunFactor);
                    specularFactor = std::pow(NdotL, kExp[s]);
                    specularFactor *= kMult[s];
                } else {
                    // reflect(-sun, N).z
                    const float nd = v[kNormX] * -sun[0] + v[kNormY] * -sun[1] + v[kNormZ] * -sun[2];
                    const float rz = -sun[2] - 2.0f * nd * v[kNormZ];
                    specularFactor = std::fmax(0.0f, rz);
                }
                specularFactor *= u.specularValue;
                specularFactor *= u.lighting[1][0];
                if (col[3] < 1.0f) {
                    fin[3] = std::fmax(fin[3], specularFactor);
                }
                fin[0] += specularFactor;
                fin[1] += specularFactor;
                fin[2] += specularFactor;
            }
        }

        for (int c = 0; c < 3; c++) {
            fin[c] = std::fmin(fin[c], 1.0f);
            const float spotFog = u.spotFogColor[c] * u.fogAttenuation * u.fogColour[c] * lobeFogEffect;
            fin[c] = mix(fin[c], fogRGB[c] + spotFog, fogAlpha);
        }
        std::memcpy(out, fin, sizeof(fin));
        return true;
    }
};

// Sutherland-Hodgman against dist(v) >= 0.
template <typename Dist>
u32 clip_polygon(const ClipVertex* in, u32 count, ClipVertex* out, Dist dist)
{
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        const ClipVertex& a  = in[i];
        const ClipVertex& b  = in[(i + 1) % count];
        const float       da = dist(a);
        const float       db = dist(b);
        if (da >= 0.0f) {
            out[n++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            ClipVertex& c = out[n++];
            for (int k = 0; k < 4; k++) c.p[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
            for (int k = 0; k < kVary; k++) c.v[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
        }
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------

SoftRenderer::SoftRenderer()
{
    const u32 cores   = std::max(1u, std::thread::hardware_concurrency());
    const u32 workers = std::min(cores, 16u) - 1;
    for (u32 i = 0; i < workers; i++) {
        m_threads.emplace_back([this] { worker_loop(); });
    }
}

SoftRenderer::~SoftRenderer()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_work_cv.notify_all();
    for (std::thread& t : m_threads) {
        t.join();
    }
}

void SoftRenderer::worker_loop()
{
    u64 seen = 0;
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_work_cv.wait(lock, [&] { return m_quit || m_generation != seen; });
        if (m_quit) {
            return;
        }
        seen = m_generation;
        const std::function<void(u32)>* job = m_job;
        const u32 count = m_job_count;
        lock.unlock();
        for (u32 i = m_next.fetch_add(1); i < count; i = m_next.fetch_add(1)) {
            (*job)(i);
        }
        lock.lock();
        if (--m_busy == 0) {
            m_done_cv.notify_all();
        }
    }
}

void SoftRenderer::parallel(u32 count, const std::function<void(u32)>& job)
{
    if (count == 0) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_job       = &job;
        m_job_count = count;
        m_next.store(0);
        m_busy = static_cast<u32>(m_threads.size());  // every worker checks in
        ++m_generation;
    }
    m_work_cv.notify_all();
    for (u32 i = m_next.fetch_add(1); i < count; i = m_next.fetch_add(1)) {
        job(i);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done_cv.wait(lock, [this] { return m_busy == 0; });
    m_job = nullptr;
}

// ---------------------------------------------------------------------------
// Vertex stage: model3_r3d.vert, clipping, the viewport transform
// ---------------------------------------------------------------------------

void SoftRenderer::setup(FrameBuilder& frame)
{
    const FramePlan& plan  = *m_plan;
    const Scene&     scene = frame.scene();
    const auto       rom   = scene.rom_vertices();
    const auto       ram   = scene.ram_vertices();
    const usize      rom_count = std::min<usize>(rom.size(), kMaxRomVerts);
    const usize      ram_count = std::min<usize>(ram.size(), kMaxRamVerts);

    m_draws.resize(plan.draws.size());
    m_draw_chunk.assign(plan.draws.size() + 1, 0);
    u32 chunks = 0;
    for (usize i = 0; i < plan.draws.size(); i++) {
        const R3DDraw& d   = plan.draws[i];
        Draw&          out = m_draws[i];
        out.u       = &plan.uniforms[d.uniforms];
        out.m       = &plan.models[d.model];
        out.layered = d.layered;
        out.no_los  = d.no_los_return;
        out.viewport = plan.viewports[d.viewport].viewport;
        out.scissor  = plan.viewports[d.viewport].scissor;
        const usize limit = d.dynamic ? ram_count : rom_count;
        const auto  base  = d.dynamic ? ram.data() : rom.data();
        const usize end   = std::min<usize>(static_cast<usize>(d.first_vertex) + d.vertex_count, limit);
        out.verts = base + d.first_vertex;
        out.tris  = end > d.first_vertex ? static_cast<u32>((end - d.first_vertex) / 3) : 0;
        if (out.scissor.width <= 0 || out.scissor.height <= 0) {
            out.tris = 0;
        }
        // GLSL's projMat * modelMat, column major.
        const float* P = out.u->projMat;
        const float* M = out.m->modelMat;
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                out.pm[c * 4 + r] = P[0 * 4 + r] * M[c * 4 + 0] + P[1 * 4 + r] * M[c * 4 + 1]
                                    + P[2 * 4 + r] * M[c * 4 + 2] + P[3 * 4 + r] * M[c * 4 + 3];
            }
        }
        m_draw_chunk[i] = chunks;
        chunks += (out.tris + kChunkTris - 1) / kChunkTris;
    }
    m_draw_chunk[plan.draws.size()] = chunks;
    if (m_chunks.size() < chunks) {
        m_chunks.resize(chunks);
    }
    for (usize i = 0; i < plan.draws.size(); i++) {
        for (u32 c = m_draw_chunk[i], first = 0; c < m_draw_chunk[i + 1]; c++, first += kChunkTris) {
            Chunk& ch = m_chunks[c];
            ch.draw   = static_cast<u32>(i);
            ch.first  = first;
            ch.count  = std::min(kChunkTris, m_draws[i].tris - first);
        }
    }
    parallel(chunks, [this](u32 c) { setup_chunk(m_chunks[c]); });
}

void SoftRenderer::setup_chunk(Chunk& chunk)
{
    chunk.tris.clear();
    chunk.rows.clear();
    chunk.miny = 0x7FFF;
    chunk.maxy = -1;
    const Draw&             d = m_draws[chunk.draw];
    const R3DUniforms&      u = *d.u;
    const R3DModelUniforms& m = *d.m;
    const float*            M = m.modelMat;

    const float sx0 = static_cast<float>(d.scissor.x);
    const float sy0 = static_cast<float>(d.scissor.y);
    const float sx1 = static_cast<float>(d.scissor.x + d.scissor.width);
    const float sy1 = static_cast<float>(d.scissor.y + d.scissor.height);
    const float vw  = static_cast<float>(d.viewport.width);
    const float vh  = static_cast<float>(d.viewport.height);
    const float ox  = static_cast<float>(d.viewport.x) + vw * 0.5f;
    const float oy  = static_cast<float>(d.viewport.y + d.viewport.height) + (-vh) * 0.5f;
    const float gx  = kGuard / std::max(vw * 0.5f, 1.0f);
    const float gy  = kGuard / std::max(vh * 0.5f, 1.0f);

    for (u32 t = chunk.first; t < chunk.first + chunk.count; t++) {
        ClipVertex poly[3];
        int        culled = 0;
        for (int k = 0; k < 3; k++) {
            const FVertex& fv  = d.verts[t * 3 + static_cast<u32>(k)];
            const float*   pos = fv.base.pos;
            ClipVertex&    cv  = poly[k];
            for (int r = 0; r < 4; r++) {
                cv.p[r] = d.pm[0 * 4 + r] * pos[0] + d.pm[1 * 4 + r] * pos[1] + d.pm[2 * 4 + r] * pos[2]
                          + d.pm[3 * 4 + r] * pos[3];
            }
            float* v = cv.v;
            for (int r = 0; r < 3; r++) {
                v[kViewX + r] = M[0 * 4 + r] * pos[0] + M[1 * 4 + r] * pos[1] + M[2 * 4 + r] * pos[2]
                                + M[3 * 4 + r] * pos[3];
                v[kNormX + r] = (M[0 * 4 + r] / m.modelScale) * fv.base.normal[0]
                                + (M[1 * 4 + r] / m.modelScale) * fv.base.normal[1]
                                + (M[2 * 4 + r] / m.modelScale) * fv.base.normal[2];
            }
            float fn[3];
            for (int r = 0; r < 3; r++) {
                fn[r] = M[0 * 4 + r] * fv.faceNormal[0] + M[1 * 4 + r] * fv.faceNormal[1] + M[2 * 4 + r] * fv.faceNormal[2];
            }
            v[kDiscard] = v[kViewX] * fn[0] + v[kViewY] * fn[1] + v[kViewZ] * fn[2];
            for (int c = 0; c < 4; c++) {
                v[kColR + c] = static_cast<float>(fv.faceColour[c]) / 255.0f;
            }
            if (m.translatorMap != 0) {
                v[kColR] *= 16.0f;
                v[kColG] *= 16.0f;
                v[kColB] *= 16.0f;
            }
            v[kColA] *= m.nodeAlpha;
            v[kTexU]       = fv.base.texcoords[0];
            v[kTexV]       = fv.base.texcoords[1];
            v[kFixedShade] = fv.base.fixedShade;
            v[kLodBase]    = v[kDiscard] * -u.cota * fv.textureNP;
            culled += v[kDiscard] > 0.0f ? 1 : 0;
        }
        if (culled == 3) {
            continue;  // every fragment fails the back-face test
        }

        // Vulkan's clip volume: 0 <= z <= w; x and y only past a guard band.
        ClipVertex a[12], b[12];
        u32 n = 3;
        bool inside = true;
        for (const ClipVertex& cv : poly) {
            inside = inside && cv.p[2] >= 0.0f && cv.p[2] <= cv.p[3];
        }
        const ClipVertex* cur = poly;
        if (!inside) {
            n = clip_polygon(poly, n, a, [](const ClipVertex& c) { return c.p[3] - c.p[2]; });
            n = clip_polygon(a, n, b, [](const ClipVertex& c) { return c.p[2]; });
            cur = b;
        }
        if (n < 3) {
            continue;
        }
        bool guard = false;
        for (u32 i = 0; i < n; i++) {
            guard = guard || std::fabs(cur[i].p[0]) > gx * cur[i].p[3] || std::fabs(cur[i].p[1]) > gy * cur[i].p[3];
        }
        if (guard) {
            ClipVertex* src = cur == b ? b : a;
            if (cur == poly) {
                std::copy(poly, poly + 3, a);
                src = a;
            }
            ClipVertex* dst = src == a ? b : a;
            n = clip_polygon(src, n, dst, [gx](const ClipVertex& c) { return gx * c.p[3] - c.p[0]; });
            n = clip_polygon(dst, n, src, [gx](const ClipVertex& c) { return gx * c.p[3] + c.p[0]; });
            n = clip_polygon(src, n, dst, [gy](const ClipVertex& c) { return gy * c.p[3] - c.p[1]; });
            n = clip_polygon(dst, n, src, [gy](const ClipVertex& c) { return gy * c.p[3] + c.p[1]; });
            cur = src;
            if (n < 3) {
                continue;
            }
        }

        // Window coordinates (a negative-height viewport, as Vulkan's).
        s32   fx[12], fy[12];
        float z[12], iw[12];
        for (u32 i = 0; i < n; i++) {
            const float w = cur[i].p[3];
            iw[i]         = 1.0f / w;
            const float wx = (vw * 0.5f) * (cur[i].p[0] * iw[i]) + ox;
            const float wy = (-vh * 0.5f) * (cur[i].p[1] * iw[i]) + oy;
            z[i]  = cur[i].p[2] * iw[i];
            fx[i] = static_cast<s32>(std::lrintf(wx * static_cast<float>(kSub)));
            fy[i] = static_cast<s32>(std::lrintf(wy * static_cast<float>(kSub)));
        }

        for (u32 i = 1; i + 1 < n; i++) {
            u32 idx[3] = {0, i, i + 1};
            const s64 area = static_cast<s64>(fx[idx[1]] - fx[idx[0]]) * (fy[idx[2]] - fy[idx[0]])
                             - static_cast<s64>(fx[idx[2]] - fx[idx[0]]) * (fy[idx[1]] - fy[idx[0]]);
            if (area == 0) {
                continue;
            }
            if (area < 0) {
                std::swap(idx[1], idx[2]);
            }
            s32 minfx = fx[idx[0]], maxfx = fx[idx[0]], minfy = fy[idx[0]], maxfy = fy[idx[0]];
            for (int k = 1; k < 3; k++) {
                minfx = std::min(minfx, fx[idx[k]]);
                maxfx = std::max(maxfx, fx[idx[k]]);
                minfy = std::min(minfy, fy[idx[k]]);
                maxfy = std::max(maxfy, fy[idx[k]]);
            }
            // Pixels whose centre can be inside, within the scissor.
            const auto first_pixel = [](s32 f) { return static_cast<s32>(std::floor((static_cast<float>(f) - kSub / 2) / kSub)); };
            s32 x0 = first_pixel(minfx), x1 = first_pixel(maxfx) + 1;
            s32 y0 = first_pixel(minfy), y1 = first_pixel(maxfy) + 1;
            x0 = std::max(x0, static_cast<s32>(sx0));
            y0 = std::max(y0, static_cast<s32>(sy0));
            x1 = std::min(x1, static_cast<s32>(sx1) - 1);
            y1 = std::min(y1, static_cast<s32>(sy1) - 1);
            if (x0 > x1 || y0 > y1) {
                continue;
            }
            Tri& tri = chunk.tris.emplace_back();
            for (int k = 0; k < 3; k++) {
                tri.fx[k] = fx[idx[k]];
                tri.fy[k] = fy[idx[k]];
                tri.z[k]  = z[idx[k]];
                tri.iw[k] = iw[idx[k]];
                std::memcpy(tri.v[k], cur[idx[k]].v, sizeof(tri.v[k]));
            }
            tri.minx = static_cast<s16>(x0);
            tri.maxx = static_cast<s16>(x1);
            tri.miny = static_cast<s16>(y0);
            tri.maxy = static_cast<s16>(y1);
            tri.cull_per_pixel = culled != 0;
            chunk.rows.push_back(static_cast<u32>(y0) | (static_cast<u32>(y1) << 16));
            chunk.miny = std::min(chunk.miny, y0);
            chunk.maxy = std::max(chunk.maxy, y1);
        }
    }
}

// ---------------------------------------------------------------------------
// Fragment stage
// ---------------------------------------------------------------------------

void SoftRenderer::draw_range(u32 begin, u32 end, int layer, s32 y0, s32 y1, u32 los_slot)
{
    for (u32 i = begin; i < end; i++) {
        const Draw& d = m_draws[i];
        if (d.scissor.y >= y1 || d.scissor.y + d.scissor.height <= y0) {
            continue;
        }
        for (u32 c = m_draw_chunk[i]; c < m_draw_chunk[i + 1]; c++) {
            const Chunk& ch = m_chunks[c];
            if (ch.miny >= y1 || ch.maxy < y0) {
                continue;
            }
            for (usize t = 0; t < ch.tris.size(); t++) {
                const u32 r = ch.rows[t];
                if (static_cast<s32>(r & 0xFFFF) >= y1 || static_cast<s32>(r >> 16) < y0) {
                    continue;
                }
                raster(ch.tris[t], d, layer, y0, y1, los_slot);
            }
        }
    }
}

void SoftRenderer::raster(const Tri& t, const Draw& d, int layer, s32 y0, s32 y1, u32 los_slot)
{
    const s32 ya = std::max<s32>(t.miny, y0);
    const s32 yb = std::min<s32>(t.maxy, y1 - 1);
    if (ya > yb) {
        return;
    }
    const s32 xa = t.minx;
    const s32 xb = t.maxx;

    // Edge k runs from vertex k to k+1; E >= 0 inside, the top-left rule on E == 0.
    s64 dx[3], dy[3], e_row[3], bias[3];
    const s64 px0 = static_cast<s64>(xa) * kSub + kSub / 2;
    const s64 py0 = static_cast<s64>(ya) * kSub + kSub / 2;
    for (int k = 0; k < 3; k++) {
        const int j  = (k + 1) % 3;
        const s64 ex = static_cast<s64>(t.fx[j]) - t.fx[k];
        const s64 ey = static_cast<s64>(t.fy[j]) - t.fy[k];
        dx[k]        = -ey * kSub;  // per pixel step in x
        dy[k]        = ex * kSub;
        e_row[k]     = ex * (py0 - t.fy[k]) - ey * (px0 - t.fx[k]);
        const bool top_left = (ey == 0 && ex > 0) || ey < 0;
        bias[k]      = top_left ? 0 : -1;
    }
    const s64   area     = static_cast<s64>(t.fx[1] - t.fx[0]) * (t.fy[2] - t.fy[0])
                           - static_cast<s64>(t.fx[2] - t.fx[0]) * (t.fy[1] - t.fy[0]);
    const float inv_area = 1.0f / static_cast<float>(area);

    // Screen-linear l1 = E[2->0], l2 = E[0->1]; attributes from the
    // perspective-correct weights l * iw / fw.
    const float iw0 = t.iw[0], iw1 = t.iw[1], iw2 = t.iw[2];
    const float zd1 = t.z[1] - t.z[0], zd2 = t.z[2] - t.z[0];
    float vb[kVary], vd1[kVary], vd2[kVary];
    for (int k = 0; k < kVary; k++) {
        vb[k]  = t.v[0][k];
        vd1[k] = t.v[1][k] - t.v[0][k];
        vd2[k] = t.v[2][k] - t.v[0][k];
    }

    const R3DUniforms& u = *d.u;
    const Shader       shader{u, m_texels};
    const u32          width = m_width;
    const bool         gequal = layer == 0;
    const LayerPass*   los    = los_slot != ~0u ? &m_plan->passes[los_slot] : nullptr;
    const float        near_val = u.projMat[14];

    for (s32 y = ya; y <= yb; y++) {
        s64 e[3] = {e_row[0], e_row[1], e_row[2]};
        e_row[0] += dy[0];
        e_row[1] += dy[1];
        e_row[2] += dy[2];
        const usize row = static_cast<usize>(y) * width;
        for (s32 x = xa; x <= xb; x++, e[0] += dx[0], e[1] += dx[1], e[2] += dx[2]) {
            if (((e[0] + bias[0]) | (e[1] + bias[1]) | (e[2] + bias[2])) < 0) {
                continue;
            }
            const float l1 = static_cast<float>(e[2]) * inv_area;
            const float l2 = static_cast<float>(e[0]) * inv_area;
            const usize p  = row + static_cast<usize>(x);

            u8& st = m_stencil[p];
            if (d.layered && (st & 0x7F) != 0) {
                continue;  // stencil fail: keep
            }
            const float l0    = 1.0f - l1 - l2;
            const float depth = t.z[0] + zd1 * l1 + zd2 * l2;
            const bool  pass  = gequal ? depth >= m_depth[p] : depth > m_depth[p];
            if (!pass && !d.layered) {
                continue;
            }

            const float fw = l0 * iw0 + l1 * iw1 + l2 * iw2;
            const float w  = 1.0f / fw;
            const float p1 = l1 * iw1 * w;
            const float p2 = l2 * iw2 * w;
            float       v[kVary];
            if (t.cull_per_pixel) {
                v[kDiscard] = vb[kDiscard] + vd1[kDiscard] * p1 + vd2[kDiscard] * p2;
                if (v[kDiscard] > 0.0f) {
                    continue;
                }
            }
            for (int k = 0; k < kVary; k++) {
                v[k] = vb[k] + vd1[k] * p1 + vd2[k] * p2;
            }
            float colour[4];
            if (!shader.shade(v, fw, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, colour)) {
                continue;
            }
            if (d.layered) {
                st = static_cast<u8>((st & 0x80) | ((st + 1) & 0x7F));
                if (!pass) {
                    continue;
                }
            } else {
                st = static_cast<u8>((st & 0x7F) | (d.no_los ? 0x80 : 0x00));
            }
            m_depth[p] = depth;

            if (layer == 0) {
                if (u.colourLayer == kLayerColour) {
                    colour[3] = 1.0f;
                    m_colour[0][p] = pack(colour);
                    m_colour[1][p] = 0;
                    m_colour[2][p] = 0;
                } else {
                    m_colour[0][p] = 0;
                    m_colour[1][p] = u.colourLayer == kLayerTrans0 ? pack(colour) : 0;
                    m_colour[2][p] = u.colourLayer == kLayerTrans1 ? pack(colour) : 0;
                }
                if (los != nullptr && x == los->los_x && y == los->los_y) {
                    LosSample& s = m_los_samples[los_slot];
                    s.depth_bits = float_as_u32(near_val * fw);
                    s.no_return  = u.noLosReturn != 0;
                }
            } else {
                m_colour[static_cast<usize>(layer)][p] = u.colourLayer == layer ? pack(colour) : 0;
            }
        }
    }
}

void SoftRenderer::draw_band(s32 y0, s32 y1, std::span<u32> out)
{
    const FramePlan& plan  = *m_plan;
    const usize      first = static_cast<usize>(y0) * m_width;
    const usize      count = static_cast<usize>(y1 - y0) * m_width;

    if (!plan.white_flash) {
        for (auto& c : m_colour) {
            std::fill_n(c.begin() + static_cast<std::ptrdiff_t>(first), count, 0u);
        }
        // model3_scroll_fog.frag over each fog viewport.
        for (const FogDraw& fog : plan.fogs) {
            const PixelRect& s = plan.viewports[fog.viewport].scissor;
            const ScrollFogUniforms& f = fog.uniforms;
            const s32 ya = std::max(y0, s.y);
            const s32 yb = std::min(y1, s.y + s.height);
            for (s32 y = ya; y < yb; y++) {
                for (s32 x = std::max(s.x, 0); x < std::min<s32>(s.x + s.width, static_cast<s32>(m_width)); x++) {
                    const float sx = static_cast<float>(x) + 0.5f;
                    const float sy = f.fragCoordYTransform[1] + f.fragCoordYTransform[0] * (static_cast<float>(y) + 0.5f);
                    const float ex = (sx - f.spotEllipse[0]) / f.spotEllipse[2];
                    const float ey = (sy - f.spotEllipse[1]) / f.spotEllipse[3];
                    float ellipse  = std::sqrt(ex * ex + ey * ey);
                    ellipse        = std::fmax(0.0f, 1.0f - ellipse * ellipse);
                    float c[4];
                    for (int k = 0; k < 3; k++) {
                        const float spot = mix(0.0f, f.spotFogColor[k] * ellipse * f.fogColour[k], f.fogAttenuation);
                        c[k] = f.fogColour[k] * f.fogAmbient + spot;
                    }
                    c[3] = f.fogColour[3];
                    const usize p = static_cast<usize>(y) * m_width + static_cast<usize>(x);
                    m_colour[0][p] = pack(c);
                    m_colour[1][p] = 0;
                    m_colour[2][p] = 0;
                }
            }
        }

        for (usize i = 0; i < plan.passes.size(); i++) {
            const LayerPass& pass = plan.passes[i];
            std::fill_n(m_depth.begin() + static_cast<std::ptrdiff_t>(first), count, 0.0f);
            std::fill_n(m_stencil.begin() + static_cast<std::ptrdiff_t>(first), count, u8{0});
            draw_range(pass.colour.begin, pass.colour.end, 0, y0, y1, pass.los ? static_cast<u32>(i) : ~0u);
            const bool both = !pass.trans1.empty() && !pass.trans2.empty();
            if (both) {
                std::copy_n(m_depth.begin() + static_cast<std::ptrdiff_t>(first), count,
                            m_depth_copy.begin() + static_cast<std::ptrdiff_t>(first));
                std::copy_n(m_stencil.begin() + static_cast<std::ptrdiff_t>(first), count,
                            m_stencil_copy.begin() + static_cast<std::ptrdiff_t>(first));
            }
            draw_range(pass.trans1.begin, pass.trans1.end, 1, y0, y1, ~0u);
            if (both) {
                std::copy_n(m_depth_copy.begin() + static_cast<std::ptrdiff_t>(first), count,
                            m_depth.begin() + static_cast<std::ptrdiff_t>(first));
                std::copy_n(m_stencil_copy.begin() + static_cast<std::ptrdiff_t>(first), count,
                            m_stencil.begin() + static_cast<std::ptrdiff_t>(first));
            }
            draw_range(pass.trans2.begin, pass.trans2.end, 2, y0, y1, ~0u);
        }
    }
    composite(y0, y1, out);
}

// The 2D and 3D layers composited, as Model3Pass::composite() does.
void SoftRenderer::composite(s32 y0, s32 y1, std::span<u32> out)
{
    const FramePlan& plan  = *m_plan;
    const u32        black = 0xFF000000u;
    const PixelRect& disp  = plan.display;
    const s32 dx0 = std::max(disp.x, 0);
    const s32 dx1 = std::min<s32>(disp.x + disp.width, static_cast<s32>(m_width));
    const s32 lw  = static_cast<s32>(m_layer_width);
    const s32 lh  = static_cast<s32>(TileGen::kHeight);

    auto layer_texel = [&](int l, s32 x, s32 y, float c[4]) -> bool {
        const LayerDraw& d = plan.layers[l];
        const float u = (static_cast<float>(x) + 0.5f - static_cast<float>(d.rect.x)) / static_cast<float>(d.rect.width);
        const float v = (static_cast<float>(y) + 0.5f - static_cast<float>(d.rect.y)) / static_cast<float>(d.rect.height);
        if (u < d.clip_lo || v < 0.0f || u >= d.clip_hi || v >= 1.0f || m_layers[l].empty()) {
            return false;
        }
        const float s = u * d.uv_scale * static_cast<float>(lw);
        const float t = v * static_cast<float>(lh);
        const u32*  tex = m_layers[l].data();
        if (!m_smooth_2d) {
            const s32 tx = std::clamp(static_cast<s32>(std::floor(s)), 0, lw - 1);
            const s32 ty = std::clamp(static_cast<s32>(std::floor(t)), 0, lh - 1);
            unpack(tex[ty * lw + tx], c);
            return true;
        }
        const float fs = s - 0.5f, ft = t - 0.5f;
        const float bs = std::floor(fs), bt = std::floor(ft);
        const float as = fs - bs, at = ft - bt;
        const s32 xs[2] = {std::clamp(static_cast<s32>(bs), 0, lw - 1), std::clamp(static_cast<s32>(bs) + 1, 0, lw - 1)};
        const s32 ys[2] = {std::clamp(static_cast<s32>(bt), 0, lh - 1), std::clamp(static_cast<s32>(bt) + 1, 0, lh - 1)};
        float p[4][4];
        for (int i = 0; i < 4; i++) {
            unpack(tex[ys[i >> 1] * lw + xs[i & 1]], p[i]);
        }
        for (int k = 0; k < 4; k++) {
            c[k] = mix(mix(p[0][k], p[1][k], as), mix(p[2][k], p[3][k], as), at);
        }
        return true;
    };

    for (s32 y = y0; y < y1; y++) {
        u32* row = out.data() + static_cast<usize>(y) * m_width;
        std::fill_n(row, m_width, black);
        if (y < disp.y || y >= disp.y + disp.height) {
            continue;
        }
        for (s32 x = dx0; x < dx1; x++) {
            u32   dst = black;
            float c[4];
            if (layer_texel(0, x, y, c)) {
                dst = blend(dst, c);
            }
            const usize p = static_cast<usize>(y) * m_width + static_cast<usize>(x);
            if (plan.white_flash) {
                dst = 0xFFFFFFFFu;
            } else {
                unpack(m_colour[0][p], c);
                dst = blend(dst, c);
                float t1[4], t2[4];
                unpack(m_colour[1][p], t1);
                unpack(m_colour[2][p], t2);
                if (t1[3] * t2[3] > 0.0f) {
                    const float f = (t2[3] + (1.0f - t1[3])) / 2.0f;
                    c[0] = mix(t1[0], t2[0], f);
                    c[1] = mix(t1[1], t2[1], f);
                    c[2] = mix(t1[2], t2[2], f);
                    c[3] = 1.0f;
                    dst  = blend(dst, c);
                } else if (t1[3] > 0.0f) {
                    dst = blend(dst, t1);
                } else {
                    dst = blend(dst, t2);
                }
            }
            if (layer_texel(1, x, y, c)) {
                dst = blend(dst, c);
            }
            row[x] = dst;
        }
    }
}

void SoftRenderer::render(FrameBuilder& frame, std::span<u32> out)
{
    const FramePlan& plan = frame.plan();
    m_plan   = &plan;
    m_width  = plan.target_width;
    m_height = plan.target_height;
    m_texels = frame.textures().texels();
    for (int l = 0; l < 2; l++) {
        m_layers[l] = frame.layer(l);
    }
    m_layer_width = static_cast<u32>(m_layers[0].size() / TileGen::kHeight);
    if (m_layers[1].size() != m_layers[0].size()) {
        m_layers[1] = {};
    }
    m_los = LineOfSight{};
    if (out.size() < static_cast<usize>(m_width) * m_height || m_width == 0) {
        return;
    }

    const usize pixels = static_cast<usize>(m_width) * m_height;
    for (auto& c : m_colour) {
        c.resize(pixels);
    }
    m_depth.resize(pixels);
    m_stencil.resize(pixels);
    m_depth_copy.resize(pixels);
    m_stencil_copy.resize(pixels);
    m_los_samples.assign(plan.passes.size(), LosSample{});

    if (!plan.white_flash) {
        setup(frame);
    }
    const u32 bands = (m_height + kBandRows - 1) / kBandRows;
    parallel(bands, [this, out](u32 b) {
        const s32 y0 = static_cast<s32>(b) * kBandRows;
        draw_band(y0, std::min<s32>(y0 + kBandRows, static_cast<s32>(m_height)), out);
    });

    m_los.frame_number = frame.frame_number();
    for (usize i = 0; i < plan.passes.size(); i++) {
        const LayerPass& pass = plan.passes[i];
        if (pass.los) {
            m_los.range[static_cast<usize>(pass.priority)] =
                decode_line_of_sight(m_los_samples[i].depth_bits, m_los_samples[i].no_return);
        }
    }
}

}  // namespace sm2::render::model3
