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
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp, Copyright (c)
// 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The Real3D scene walk. Reads are bounds-checked; stepping, sun clamp and
// block culling come from the VideoFrame; the ROM model cache is dropped when
// VROM changes.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"
#include "render/model3/r3d_model.h"

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace sm2::render::model3 {

// Vertex buffer regions: cached ROM models first, then this frame's RAM ones.
inline constexpr u32 kMaxRamVerts = 300000;
inline constexpr u32 kMaxRomVerts = 1500000;

inline constexpr float kNearPlane = 1e-3f;

// Target size: N * (496 + 2 * columns) by N * 384, for render scale N.
struct SceneGeometry {
    u32  render_scale = 1;
    u32  columns      = 0;  ///< Native columns added on each side (widescreen).
    bool widen        = true;   ///< Full-width viewports reach into the columns.
    bool wide_2d      = false;  ///< Scrolling layers behind the 3D do too.

    [[nodiscard]] u32  x_res() const { return 496 * render_scale; }
    [[nodiscard]] u32  y_res() const { return 384 * render_scale; }
    [[nodiscard]] u32  total_x() const { return (496 + 2 * columns) * render_scale; }
    [[nodiscard]] u32  total_y() const { return y_res(); }
    [[nodiscard]] u32  x_offset() const { return columns * render_scale; }
    [[nodiscard]] bool wide_screen() const { return columns > 0 && widen; }

    bool operator==(const SceneGeometry&) const = default;
};

class Scene {
public:
    Scene();

    void set_geometry(const SceneGeometry& geometry);
    [[nodiscard]] const SceneGeometry& geometry() const { return m_geometry; }

    void reset();

    void build(const hw::model3::VideoFrame& frame);

    /// Before build(): true when the frame's tile layers are all transparent.
    void set_layers_empty(bool empty) { m_layersEmpty = empty; }

    // -- results of the last build() ---------------------------------------------

    [[nodiscard]] const std::vector<Node>& nodes() const { return m_nodes; }

    [[nodiscard]] bool block_culling() const { return m_blockCullingLatched; }

    [[nodiscard]] bool skip_layer(int layer) const;

    [[nodiscard]] std::span<const FVertex> rom_vertices() const { return m_polyBufferRom; }
    [[nodiscard]] u64                      rom_generation() const { return m_romGeneration; }

    // Meshes with vboOffset >= kMaxRomVerts are here, at vboOffset - kMaxRomVerts.
    [[nodiscard]] std::span<const FVertex> ram_vertices() const { return m_polyBufferRam; }

    [[nodiscard]] int stepping() const { return m_step; }

    void translate_los_position(int inX, int inY, int& outX, int& outY) const;

    void calc_viewport(Viewport* vp);

    /// Widescreen: whether this viewport grows into the side columns.
    [[nodiscard]] bool widens(const Viewport& vp) const;

private:
    const u32* TranslateCullingAddress(u32 addr, u32 words) const;
    const u32* TranslateModelAddress(u32 addr) const;
    [[nodiscard]] const u32* ModelEnd(u32 addr) const;

    void MultMatrix(u32 matrixOffset, Mat4& mat);
    void InitMatrixStack(u32 matrixBaseAddr, Mat4& mat);
    void ResetMatrix(Mat4& mat) const;

    bool DrawModel(u32 modelAddr);
    void DescendCullingNode(u32 addr);
    void DescendPointerList(u32 addr);
    void DescendNodePtr(u32 nodeAddr);
    void RenderViewport(u32 addr, int depth);

    [[nodiscard]] int GetTexFormat(int originalFormat, bool contour) const;
    void SetMeshValues(SortingMesh* currentMesh, const PolyHeader& ph);
    void CacheModel(Model* m, const u32* data, const u32* end);
    void CopyVertexData(const R3DPoly& r3dPoly, std::vector<FVertex>& vertexArray);
    static void GetCoordinates(int width, int height, u16 uIn, u16 vIn, float uvScale, float& uOut,
                               float& vOut);
    [[nodiscard]] bool IsDynamicModel(const u32* data, const u32* end) const;
    [[nodiscard]] static bool IsVROMModel(u32 modelAddr) { return modelAddr >= 0x100000; }
    [[nodiscard]] u32 ColourTableEntry(u32 index) const;

    void SetStepping(int stepping);
    void ResetPerFrameState();

    SceneGeometry m_geometry;

    // Misc
    int m_numPolyVerts = 3;

    // Stepping
    int   m_step = 0x10;
    int   m_offset = 2;                         // offset to subtract for words 3 and higher of culling nodes
    float m_vertexFactor = 1.0f / 128.0f;       // fixed-point conversion factor for vertices
    float m_textureNPFactor = 1.0f / 4096.0f;   // fixed-point conversion factor for texture NP values

    bool m_sunClamp = true;
    bool m_blockCullingLatched = false;

    // Memory (the frame being built)
    std::span<const u32> m_cullingRAMLo;
    std::span<const u32> m_cullingRAMHi;
    std::span<const u32> m_polyRAM;
    std::span<const u32> m_vrom;
    const u32*           m_cachedVrom = nullptr;  // VROM the ROM cache was built from
    usize                m_cachedVromWords = 0;

    // Resolution and scaling factors and offsets
    float    m_xRatio = 1, m_yRatio = 1;
    unsigned m_xOffs = 0, m_yOffs = 0;
    unsigned m_xRes = 496, m_yRes = 384;
    unsigned m_totalXRes = 496, m_totalYRes = 384;
    bool     m_wideScreen = false;
    bool     m_layersEmpty = false;
    bool     m_centreLetterbox = false;

    // Real3D Base Matrix Pointer
    const float* m_matrixBasePtr = nullptr;
    u32          m_matrixBaseWords = 0;  // words available from m_matrixBasePtr
    u32          m_colorTableAddr = 0x400;  // address of the colour table in polygon RAM
    const LODBlendTable* m_LODBlendTable = nullptr;

    NodeAttributes m_nodeAttribs;
    Mat4           m_modelMat;  // current modelview matrix

    Vertex m_prev[4] = {};          // members because Sega Bass Fishing starts meshes with vertices shared from the previous one
    u16    m_prevTexCoords[4][2] = {};  // relies on undefined behaviour

    std::vector<Node> m_nodes;     // the entire render frame
    std::vector<Node> m_nodePool;  // retired nodes, reused so their model vectors keep their capacity

    std::vector<FVertex> m_polyBufferRam;  // dynamic polys
    std::vector<FVertex> m_polyBufferRom;  // rom polys
    std::unordered_map<u32, std::shared_ptr<std::vector<Mesh>>> m_romMap;  // all the ROM models
    u64 m_romGeneration = 1;

    struct {
        float bnlu;
        float bnlv;
        float bntu;
        float bntw;
        float bnru;
        float bnrv;
        float bnbu;
        float bnbw;
        float correction;
        float shift;
    } m_planes{};
};

}  // namespace sm2::render::model3
