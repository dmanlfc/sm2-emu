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
// Real3D scene traversal, culling, LOD and vertex generation.

#include "render/model3/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sm2::render::model3 {

namespace {

inline float byte_to_float(s8 b)
{
    return (2.0f * static_cast<float>(b) + 1.0f) * static_cast<float>(1.0 / 255.0);
}

}  // namespace

Scene::Scene()
{
    SetStepping(0x10);
    set_geometry(SceneGeometry{});
}

void Scene::set_geometry(const SceneGeometry& geometry)
{
    m_geometry = geometry;

    m_xRes       = geometry.x_res();
    m_yRes       = geometry.y_res();
    m_totalXRes  = geometry.total_x();
    m_totalYRes  = geometry.total_y();
    m_xOffs      = geometry.x_offset();
    m_yOffs      = 0;
    m_wideScreen = geometry.wide_screen();
    m_xRatio     = static_cast<float>(m_xRes) * static_cast<float>(1.0 / 496.0);
    m_yRatio     = static_cast<float>(m_yRes) * static_cast<float>(1.0 / 384.0);
}

void Scene::reset()
{
    m_polyBufferRom.clear();
    m_romMap.clear();
    ++m_romGeneration;
    m_cachedVrom      = nullptr;
    m_cachedVromWords = 0;
}

void Scene::SetStepping(int stepping)
{
    m_step = stepping;

    if ((m_step != 0x10) && (m_step != 0x15) && (m_step != 0x20) && (m_step != 0x21)) {
        m_step = 0x10;
    }

    if (m_step > 0x10) {
        m_offset          = 0;                  // culling nodes are 10 words
        m_vertexFactor    = (1.0f / 2048.0f);   // vertices are in 13.11 format
        m_textureNPFactor = (1.0f / 16384.0f);  // texture NP values are in 10.14 format
    } else {
        m_offset          = 2;                  // 8 words
        m_vertexFactor    = (1.0f / 128.0f);    // 17.7
        m_textureNPFactor = (1.0f / 4096.0f);   // 12.12
    }
}

// Per-frame reset of the scene structures; must run before the scene is rebuilt.
void Scene::ResetPerFrameState()
{
    // release the last frame's resources
    m_polyBufferRam.clear();

    // Retire nodes to the pool so their model storage is reused.
    for (auto& n : m_nodes) {
        n.models.clear();  // retains the capacity reserve() paid for
        m_nodePool.emplace_back(std::move(n));
    }
    m_nodes.clear();

    m_modelMat.release();  // should be unneeded, but harmless
    m_nodeAttribs.Reset();
}

void Scene::build(const hw::model3::VideoFrame& frame)
{
    m_centreLetterbox = frame.centre_letterbox;
    int step = 0x10;
    switch (frame.step) {
        case rom::Model3Step::Step10: step = 0x10; break;
        case rom::Model3Step::Step15: step = 0x15; break;
        case rom::Model3Step::Step20: step = 0x20; break;
        case rom::Model3Step::Step21: step = 0x21; break;
    }
    SetStepping(step);

    m_cullingRAMLo = frame.culling_lo;
    m_cullingRAMHi = frame.culling_hi;
    m_polyRAM      = frame.polygon_ram;
    m_vrom         = frame.vrom;
    m_sunClamp     = frame.sun_clamp;

    // ROM model cache is keyed by VROM address.
    if (m_vrom.data() != m_cachedVrom || m_vrom.size() != m_cachedVromWords) {
        reset();
        m_cachedVrom      = m_vrom.data();
        m_cachedVromWords = m_vrom.size();
    }

    ResetPerFrameState();

    m_blockCullingLatched = frame.block_culling;
    if (m_blockCullingLatched) {
        return;  // block culling disables 3D rendering, nothing to build
    }

    if (m_cullingRAMLo.empty() || m_cullingRAMHi.empty() || m_polyRAM.empty()) {
        return;
    }

    RenderViewport(0x800000, 0);  // build model structure

    // ROM vertex buffer overflow: drop the cache and the models using it.
    if (m_polyBufferRom.size() >= kMaxRomVerts) {
        m_polyBufferRom.clear();
        m_romMap.clear();
        ++m_romGeneration;
        for (auto& n : m_nodes) {
            std::erase_if(n.models, [](const Model& m) { return !m.dynamic; });
        }
    }
}

bool Scene::skip_layer(int layer) const
{
    for (const auto& n : m_nodes) {
        if (n.viewport.priority == layer) {
            if (!n.models.empty()) {
                return false;
            }
        }
    }

    return true;
}

/******************************************************************************
Real3D Address Translation

Functions that interpret word-granular Real3D addresses and return pointers.
******************************************************************************/

// Returns nullptr unless `words` words are mapped at addr.
const u32* Scene::TranslateCullingAddress(u32 addr, u32 words) const
{
    addr &= 0x00FFFFFF;  // caller should have done this already

    if ((addr >= 0x800000) && (addr < 0x840000)) {
        const usize index = addr & 0x3FFFF;
        if (index + words > m_cullingRAMHi.size()) {
            return nullptr;
        }
        return &m_cullingRAMHi[index];
    }
    if (addr < 0x100000) {
        if (addr + words > m_cullingRAMLo.size()) {
            return nullptr;
        }
        return &m_cullingRAMLo[addr];
    }

    return nullptr;
}

// Translates model references
const u32* Scene::TranslateModelAddress(u32 modelAddr) const
{
    modelAddr &= 0x00FFFFFF;  // caller should have done this already

    if (modelAddr < 0x100000) {
        return modelAddr < m_polyRAM.size() ? &m_polyRAM[modelAddr] : nullptr;
    }
    return modelAddr < m_vrom.size() ? &m_vrom[modelAddr] : nullptr;
}

// One past the last word a model at `modelAddr` may read.
const u32* Scene::ModelEnd(u32 modelAddr) const
{
    modelAddr &= 0x00FFFFFF;
    if (modelAddr < 0x100000) {
        return m_polyRAM.data() + m_polyRAM.size();
    }
    return m_vrom.data() + m_vrom.size();
}

u32 Scene::ColourTableEntry(u32 index) const
{
    return index < m_polyRAM.size() ? m_polyRAM[index] : 0;
}

bool Scene::DrawModel(u32 modelAddr)
{
    bool cached = false;

    const u32* const modelAddress = TranslateModelAddress(modelAddr);
    const u32* const modelEnd     = ModelEnd(modelAddr);

    m_nodes.back().models.emplace_back();

    Model* const m = &m_nodes.back().models.back();

    if (IsVROMModel(modelAddr)) {
        // try to find meshes in the rom cache
        const auto cacheEntry = m_romMap.find(modelAddr);

        if (cacheEntry != m_romMap.end() && cacheEntry->second) {
            m->meshes  = cacheEntry->second;
            m->dynamic = false;
            cached     = true;
        } else if (!IsDynamicModel(modelAddress, modelEnd)) {
            m->meshes           = std::make_shared<std::vector<Mesh>>();
            m_romMap[modelAddr] = m->meshes;  // cache the meshes
            m->dynamic          = false;
        } else {
            m->meshes = std::make_shared<std::vector<Mesh>>();
        }
    } else {
        m->meshes = std::make_shared<std::vector<Mesh>>();
    }

    // copy current model matrix
    for (int i = 0; i < 16; i++) {
        m->modelMat[i] = m_modelMat.current[static_cast<usize>(i)];
    }

    // update texture offsets
    m->textureOffsetX = m_nodeAttribs.currentTexOffsetX;
    m->textureOffsetY = m_nodeAttribs.currentTexOffsetY;
    m->page           = m_nodeAttribs.currentPage;
    m->scale          = m_nodeAttribs.currentModelScale;
    m->alpha          = m_nodeAttribs.currentModelAlpha;

    if (!cached) {
        CacheModel(m, modelAddress, modelEnd);
    }

    return true;
}

/*
    0x00:   x------- -------- -------- --------	Is UF ref
            -x------ -------- -------- --------	Is 3D model
            --x----- -------- -------- --------	Is point
            ---x---- -------- -------- --------	Is point ref
            ----x--- -------- -------- --------	Is animation
            -----x-- -------- -------- --------	Is billboard
            ------x- -------- -------- --------	Child is billboard
            -------x -------- -------- --------	Extra child pointer needed
            -------- xxxxx--- -------- --------	Spare (unknown if used)
            -------- -----xxx xxxxxx-- --------	Node ID
            -------- -------- ------x- --------	Discard 1
            -------- -------- -------x --------	Discard 2

            -------- -------- -------- x-------	Reset matrix
            -------- -------- -------- -x------	Use child pointer
            -------- -------- -------- --x-----	Use sibling pointer
            -------- -------- -------- ---x----	No matrix
            -------- -------- -------- ----x---	Indirect child
            -------- -------- -------- -----x--	Valid color table
            -------- -------- -------- ------xx	Node type(0 = viewport, 1 = root node, 2 = culling node)

    0x01, 0x02 only present on Step 1.5+

    0x01:   xxxxxxxx xxxxxxxx xxxxxxxx xxxxxx--	Model scale (float32) last 2 bits are control words
            -------- -------- -------- ------x- Disable culling
            -------- -------- -------- -------x	Valid model scale

    0x02 :	-------- -------- x------- --------	Texture replace
            -------- -------- -x------ --------	Switch bank
            -------- -------- --xxxxxx x-------	X offset
            -------- -------- -------- -xxxxxxx	Y offset

    0x03 :	xxxxxxxx xxxxx--- -------- --------	Color table address 1
            -------- -----xxx xxxx---- --------	LOD table pointer
            -------- -------- ----xxxx xxxxxxxx	Node matrix

    0x04:   Translation X coordinate
    0x05:   Translation Y coordinate
    0x06:   Translation Z coordinate

    0x07:   xxxx---- -------- -------- -------- Color table address 2
            -----x-- -------- -------- -------- Sibling table
            ------x- -------- -------- -------- Point
            -------x -------- -------- -------- Leaf node
            -------- xxxxxxxx xxxxxxxx xxxxxxxx Child pointer

    0x08:   xxxxxxx- -------- -------- -------- Color table address 3
            -------x -------- -------- -------- Null sibling
            -------- xxxxxxxx xxxxxxxx xxxxxxxx Sibling pointer

    0x09:   xxxxxxxx xxxxxxxx -------- -------- Blend radius
            -------- -------- xxxxxxxx xxxxxxxx Culling radius
*/

void Scene::DescendCullingNode(u32 addr)
{
    enum class NodeType { undefined = -1, viewport = 0, rootNode = 1, cullingNode = 2 };

    if (m_nodeAttribs.StackLimit()) {
        return;
    }

    const u32* const node = TranslateCullingAddress(addr, 10);

    if (nullptr == node) {
        return;
    }

    // Extract known fields
    const auto nodeType       = static_cast<NodeType>(node[0x00] & 3);
    const u32  child1Ptr      = node[0x07 - m_offset] & 0x7FFFFFF;  // mask colour table bits
    const u32  sibling2Ptr    = node[0x08 - m_offset] & 0x1FFFFFF;  // mask colour table bits
    const u32  matrixOffset   = node[0x03 - m_offset] & 0xFFF;
    const bool resetMatrix    = (node[0x0] & 0x80) > 0;
    const u8   lodTablePointer = static_cast<u8>((node[0x03 - m_offset] >> 12) & 0x7F);

    // check our node type
    if (nodeType == NodeType::viewport) {
        return;  // viewport nodes aren't rendered
    }

    // node discard
    if ((0x300 & node[0]) == 0x300) {  // node transparency: with both bits set the object is invisible
        return;
    }

    // parse siblings
    if ((node[0x00] & 0x07) != 0x06) {  // colour table seems to indicate no siblings
        if (!(sibling2Ptr & 0x1000000) && sibling2Ptr) {
            DescendCullingNode(sibling2Ptr);  // no need to mask the bit; it is already zero
        }
    }

    if ((node[0x00] & 0x04)) {
        m_colorTableAddr = ((node[0x03 - m_offset] >> 19) << 0) | ((node[0x07 - m_offset] >> 28) << 13)
                           | ((node[0x08 - m_offset] >> 25) << 17);
        m_colorTableAddr &= 0x000FFFFF;  // clamp to 4MB (in words) range
    }

    m_nodeAttribs.Push();  // save current attribs

    if (!m_offset) {  // Step 1.5+

        if (node[0x01] & 1) {
            m_nodeAttribs.currentModelScale = u32_as_float(node[0x01] & ~3u);  // mask out control bits
        }

        if (node[0x01] & 2) {
            m_nodeAttribs.currentDisableCulling = true;
        }

        // apply texture offsets, else retain current ones
        if ((node[0x02] & 0x8000)) {
            const int tx = static_cast<int>(32 * ((node[0x02] >> 7) & 0x3F));
            const int ty = static_cast<int>(32 * (node[0x02] & 0x1F));
            m_nodeAttribs.currentTexOffsetX = tx;
            m_nodeAttribs.currentTexOffsetY = ty;
            m_nodeAttribs.currentPage       = static_cast<int>((node[0x02] & 0x4000) >> 14);
        }
    }

    // Apply matrix and translation
    m_modelMat.push_matrix();

    // apply translation vector
    if (node[0x00] & 0x10) {
        const float centroid_x = u32_as_float(node[0x04 - m_offset]);
        const float centroid_y = u32_as_float(node[0x05 - m_offset]);
        const float centroid_z = u32_as_float(node[0x06 - m_offset]);
        m_modelMat.translate(centroid_x, centroid_y, centroid_z);
    }
    // multiply matrix, if specified
    else if (matrixOffset) {
        MultMatrix(matrixOffset, m_modelMat);
    }

    if (resetMatrix) {
        ResetMatrix(m_modelMat);
    }

    const float x = m_modelMat.current[12];
    const float y = m_modelMat.current[13];
    const float z = m_modelMat.current[14];

    const u16   uCullRadius  = static_cast<u16>(node[9 - m_offset] & 0xFFFF);
    const float fCullRadius  = pro_float16(uCullRadius) * m_nodeAttribs.currentModelScale;
    const u16   uBlendRadius = static_cast<u16>(node[9 - m_offset] >> 16);
    const float fBlendRadius = pro_float16(uBlendRadius) * m_nodeAttribs.currentModelScale;

    bool outsideFrustum = false;
    const float cx = m_planes.correction == 1.0f ? x : x * m_planes.correction + z * m_planes.shift;
    if ((z * m_planes.bnlu - cx * m_planes.bnlv) > fCullRadius
        || (z * m_planes.bntu + y * m_planes.bntw) > fCullRadius
        || (z * m_planes.bnru - cx * m_planes.bnrv) > fCullRadius
        || (z * m_planes.bnbu + y * m_planes.bnbw) > fCullRadius) {
        outsideFrustum = true;
    }

    float LODscale = m_nodeAttribs.currentDisableCulling ? std::numeric_limits<float>::max()
                                                         : (fBlendRadius / std::hypot(x, y, z));
    LODscale = std::clamp(LODscale, 0.0f, std::numeric_limits<float>::max());

    // A missing LOD table reads as all-zero thresholds: nothing is LOD-deleted.
    static constexpr LOD kZeroLod[4] = {};
    const LOD* const lod = (m_LODBlendTable != nullptr) ? m_LODBlendTable->table[lodTablePointer].lod : kZeroLod;

    if (m_nodeAttribs.currentDisableCulling || (!outsideFrustum && LODscale >= lod[3].deleteSize)) {

        // Descend down first link
        if ((node[0x00] & 0x08)) {  // 4-element LOD table
            const u32* const lodPtr = TranslateCullingAddress(child1Ptr, 4);

            if (nullptr != lodPtr) {
                int modelLOD;
                for (modelLOD = 0; modelLOD < 3; modelLOD++) {
                    if (LODscale >= lod[modelLOD].deleteSize && lodPtr[modelLOD] & 0x1000000) {
                        break;
                    }
                }

                const float tempAlpha = m_nodeAttribs.currentModelAlpha;

                float nodeAlpha = lod[modelLOD].blendFactor * (LODscale - lod[modelLOD].deleteSize);
                nodeAlpha       = std::clamp(nodeAlpha, 0.0f, 1.0f);
                if (nodeAlpha > static_cast<float>(31.0 / 32.0)) {  // shader discards pixels below 1/32 alpha
                    nodeAlpha = 1.0f;
                } else if (nodeAlpha < static_cast<float>(1.0 / 32.0)) {
                    nodeAlpha = 0.0f;
                }
                m_nodeAttribs.currentModelAlpha *= nodeAlpha;  // each node's alpha is multiplied by its parent's

                if ((node[0x03 - m_offset] & 0x20000000)) {
                    DescendCullingNode(lodPtr[modelLOD] & 0xFFFFFF);

                    if (nodeAlpha < 1.0f && modelLOD != 3) {
                        m_nodeAttribs.currentModelAlpha = (1.0f - nodeAlpha) * tempAlpha;
                        DescendCullingNode(lodPtr[modelLOD + 1] & 0xFFFFFF);
                    }
                } else {
                    DrawModel(lodPtr[modelLOD] & 0xFFFFFF);

                    if (nodeAlpha < 1.0f && modelLOD != 3) {
                        m_nodeAttribs.currentModelAlpha = (1.0f - nodeAlpha) * tempAlpha;
                        DrawModel(lodPtr[modelLOD + 1] & 0xFFFFFF);
                    }
                }
            }
        } else {
            float nodeAlpha = lod[3].blendFactor * (LODscale - lod[3].deleteSize);
            nodeAlpha       = std::clamp(nodeAlpha, 0.0f, 1.0f);
            m_nodeAttribs.currentModelAlpha *= nodeAlpha;  // each node's alpha is multiplied by its parent's

            DescendNodePtr(child1Ptr);
        }
    }

    m_modelMat.pop_matrix();

    // Restore old texture offsets
    m_nodeAttribs.Pop();
}

void Scene::DescendNodePtr(u32 nodeAddr)
{
    // Ignore null links
    if ((nodeAddr & 0x00FFFFFF) == 0) {
        return;
    }

    switch ((nodeAddr >> 24) & 0x5) {  // pointer type encoded in upper 8 bits
        case 0x00:
            DescendCullingNode(nodeAddr & 0xFFFFFF);
            break;
        case 0x01:
            DrawModel(nodeAddr & 0xFFFFFF);
            break;
        case 0x04:
            DescendPointerList(nodeAddr & 0xFFFFFF);
            break;
        default:
            break;
    }
}

void Scene::DescendPointerList(u32 addr)
{
    const u32* const list = TranslateCullingAddress(addr, 1);

    if (nullptr == list) {
        return;
    }

    const u32* const listEnd = (addr & 0x00FFFFFF) >= 0x800000 ? m_cullingRAMHi.data() + m_cullingRAMHi.size()
                                                               : m_cullingRAMLo.data() + m_cullingRAMLo.size();

    for (const u32* entry = list; entry < listEnd; ++entry) {
        if (*entry & 0x01000000) {
            break;  // empty list
        }

        const u32 nodeAddr = *entry & 0x00FFFFFF;  // clear upper 8 bits to ensure this is processed as a culling node

        DescendCullingNode(nodeAddr);

        if (*entry & 0x02000000) {
            break;  // list end
        }
    }
}

/******************************************************************************
Matrix Stack
******************************************************************************/

// Column-major (OpenGL) index from y,x subscripts
#define CMINDEX(y, x) ((x) * 4 + (y))

/*
 * MultMatrix():
 *
 * Multiplies the matrix stack by the specified Real3D matrix. The matrix
 * index is a 12-bit number specifying a matrix number relative to the base.
 * The base matrix MUST be set up before calling this function.
 */
void Scene::MultMatrix(u32 matrixOffset, Mat4& mat)
{
    if (m_matrixBasePtr == nullptr) {  // LA Machineguns
        return;
    }
    if (matrixOffset * 12 + 12 > m_matrixBaseWords) {
        return;
    }

    float        m[4 * 4];
    const float* src = &m_matrixBasePtr[matrixOffset * 12];

    m[CMINDEX(0, 0)] = src[3];
    m[CMINDEX(0, 1)] = src[4];
    m[CMINDEX(0, 2)] = src[5];
    m[CMINDEX(0, 3)] = src[0];
    m[CMINDEX(1, 0)] = src[6];
    m[CMINDEX(1, 1)] = src[7];
    m[CMINDEX(1, 2)] = src[8];
    m[CMINDEX(1, 3)] = src[1];
    m[CMINDEX(2, 0)] = src[9];
    m[CMINDEX(2, 1)] = src[10];
    m[CMINDEX(2, 2)] = src[11];
    m[CMINDEX(2, 3)] = src[2];
    m[CMINDEX(3, 0)] = 0.0;
    m[CMINDEX(3, 1)] = 0.0;
    m[CMINDEX(3, 2)] = 0.0;
    m[CMINDEX(3, 3)] = 1.0;

    mat.mult_matrix(m);
}

/*
 * InitMatrixStack():
 *
 * Initializes the modelview (model space -> view space) matrix stack and
 * Real3D coordinate system. These are the last transforms to be applied (and
 * the first to be defined on the stack) before projection.
 *
 * Model 3 games tend to define the following unusual base matrix:
 *
 *		0	0	-1	0
 *		1	0	0	0
 *		0	-1	0	0
 *		0	0	0	1
 *
 * When this is multiplied by a column vector, the output is:
 *
 *		-Z
 *		X
 *		-Y
 *		1
 *
 * The Real3D seems to accept vectors in Z,X,Y order; the games store
 * everything as X,Y,Z and translate at the end. The Real3D's Y and Z axes are
 * also opposite to OpenGL's. This function inserts a compensating matrix to
 * undo both.
 */
void Scene::InitMatrixStack(u32 matrixBaseAddr, Mat4& mat)
{
    float m[4 * 4];

    // This matrix converts vectors back from the weird Model 3 Z,X,Y ordering
    // and also into OpenGL viewspace (-Y,-Z)
    m[CMINDEX(0, 0)] = 0.0;  m[CMINDEX(0, 1)] = 1.0;  m[CMINDEX(0, 2)] = 0.0;  m[CMINDEX(0, 3)] = 0.0;
    m[CMINDEX(1, 0)] = 0.0;  m[CMINDEX(1, 1)] = 0.0;  m[CMINDEX(1, 2)] = -1.0; m[CMINDEX(1, 3)] = 0.0;
    m[CMINDEX(2, 0)] = -1.0; m[CMINDEX(2, 1)] = 0.0;  m[CMINDEX(2, 2)] = 0.0;  m[CMINDEX(2, 3)] = 0.0;
    m[CMINDEX(3, 0)] = 0.0;  m[CMINDEX(3, 1)] = 0.0;  m[CMINDEX(3, 2)] = 0.0;  m[CMINDEX(3, 3)] = 1.0;

    mat.load_matrix(m);

    // Set matrix base address and apply matrix #0 (coordinate system matrix).
    const u32* base = TranslateCullingAddress(matrixBaseAddr, 12);
    m_matrixBasePtr   = reinterpret_cast<const float*>(base);
    m_matrixBaseWords = 0;
    if (base != nullptr) {
        const u32* end = ((matrixBaseAddr & 0x00FFFFFF) >= 0x800000)
                             ? m_cullingRAMHi.data() + m_cullingRAMHi.size()
                             : m_cullingRAMLo.data() + m_cullingRAMLo.size();
        m_matrixBaseWords = static_cast<u32>(end - base);
    }
    MultMatrix(0, mat);
}

// Sets the rotation back to zero, keeping the position and scale of the current matrix.
void Scene::ResetMatrix(Mat4& mat) const
{
    float m[16];
    std::memcpy(m, mat.current.data(), 16 * 4);

    // Transposing the top 3x3 inverts the rotation, so multiplying by the result cancels it out.
    std::swap(m[1], m[4]);
    std::swap(m[2], m[8]);
    std::swap(m[6], m[9]);

    m[12] = 0;
    m[13] = 0;
    m[14] = 0;
    m[15] = 1;

    // normalise the columns to remove the scale, which would otherwise apply twice
    const float s1 = std::sqrt((m[0] * m[0]) + (m[1] * m[1]) + (m[2] * m[2]));
    const float s2 = std::sqrt((m[4] * m[4]) + (m[5] * m[5]) + (m[6] * m[6]));
    const float s3 = std::sqrt((m[8] * m[8]) + (m[9] * m[9]) + (m[10] * m[10]));

    m[0] /= s1;  m[4] /= s2;  m[8] /= s3;
    m[1] /= s1;  m[5] /= s2;  m[9] /= s3;
    m[2] /= s1;  m[6] /= s2;  m[10] /= s3;

    mat.mult_matrix(m);
}

// Walks a viewport and the viewports linked after it.
void Scene::RenderViewport(u32 addr, int depth)
{
    static const float color[8][3] = {  // RGB1 colour translation
        {0.0f, 0.0f, 0.0f},  // off
        {0.0f, 0.0f, 1.0f},  // blue
        {0.0f, 1.0f, 0.0f},  // green
        {0.0f, 1.0f, 1.0f},  // cyan
        {1.0f, 0.0f, 0.0f},  // red
        {1.0f, 0.0f, 1.0f},  // purple
        {1.0f, 1.0f, 0.0f},  // yellow
        {1.0f, 1.0f, 1.0f}   // white
    };

    if ((addr & 0x00FFFFFF) == 0) {
        return;
    }

    // Guard against looping viewport lists.
    if (depth > 4096) {
        return;
    }

    // Translate address and obtain pointer
    const u32* const vpnode = TranslateCullingAddress(addr, 0x26);

    if (nullptr == vpnode) {
        return;
    }

    const bool vpDisabled = (vpnode[0] & 0x20) != 0;  // only if viewport enabled

    {
        // create node object, reusing one retired by ResetPerFrameState when possible
        if (m_nodePool.empty()) {
            m_nodes.emplace_back(Node());
            m_nodes.back().models.reserve(2048);
        } else {
            m_nodes.emplace_back(std::move(m_nodePool.back()));
            m_nodePool.pop_back();
            m_nodes.back().viewport = Viewport();  // as if freshly constructed
        }

        // get pointer to its viewport
        Viewport* vp = &m_nodes.back().viewport;

        vp->priority = static_cast<int>((vpnode[0] >> 3) & 0x3);
        vp->select   = static_cast<int>((vpnode[0] >> 8) & 0x3);
        vp->number   = static_cast<int>(vpnode[0] >> 10);

        // Fetch viewport parameters
        vp->vpX      = (static_cast<float>(vpnode[0x1A] & 0xFFFF) * static_cast<float>(1.0 / 16.0));  // viewport X (12.4 fixed point)
        vp->vpY      = (static_cast<float>(vpnode[0x1A] >> 16) * static_cast<float>(1.0 / 16.0));     // viewport Y (12.4)
        vp->vpWidth  = (static_cast<float>(vpnode[0x14] & 0xFFFF) * static_cast<float>(1.0 / 4.0));  // width (14.2)
        vp->vpHeight = (static_cast<float>(vpnode[0x14] >> 16) * static_cast<float>(1.0 / 4.0));     // height (14.2)

        // A full-width letterbox with nothing drawn around it: centre it.
        if (m_centreLetterbox && m_layersEmpty && vp->vpX == 0 && vp->vpWidth >= 495
            && vp->vpHeight >= 96 && vp->vpHeight <= 368) {
            vp->vpY = std::floor((384.0f - vp->vpHeight) * 0.5f);
        }

        const u32 matrixBase = vpnode[0x16] & 0xFFFFFF;  // matrix base address

        m_LODBlendTable = reinterpret_cast<const LODBlendTable*>(
            TranslateCullingAddress(vpnode[0x17] & 0xFFFFFF, sizeof(LODBlendTable) / 4));

        const float cv = u32_as_float(vpnode[0x8]);  // 1/(left-right)
        const float cw = u32_as_float(vpnode[0x9]);  // 1/(top-bottom)
        const float io = u32_as_float(vpnode[0xa]);  // top / bottom (ratio) - ish
        const float jo = u32_as_float(vpnode[0xb]);  // left / right (ratio)

        // clipping plane normals
        m_planes.bnlu       = u32_as_float(vpnode[0xc]);
        m_planes.bnlv       = u32_as_float(vpnode[0xd]);
        m_planes.bntu       = u32_as_float(vpnode[0xe]);
        m_planes.bntw       = u32_as_float(vpnode[0xf]);
        m_planes.bnru       = u32_as_float(vpnode[0x10]);
        m_planes.bnrv       = u32_as_float(vpnode[0x11]);
        m_planes.bnbu       = u32_as_float(vpnode[0x12]);
        m_planes.bnbw       = u32_as_float(vpnode[0x13]);
        m_planes.correction = 1.0f;  // calc_viewport() may change it
        m_planes.shift      = 0.0f;

        vp->angle_left   = (0.0f - jo) / cv;
        vp->angle_right  = (1.0f - jo) / cv;
        vp->angle_bottom = -(1.0f - io) / cw;
        vp->angle_top    = -(0.0f - io) / cw;

        vp->cota = u32_as_float(vpnode[0x3]);

        calc_viewport(vp);

        // Lighting (the sun vector points towards the sun, away from the vertex)
        vp->lightingParams[0] = u32_as_float(vpnode[0x05]);                                           // sun X
        vp->lightingParams[1] = -u32_as_float(vpnode[0x06]);                                          // sun Y (negated for GL coordinates)
        vp->lightingParams[2] = -u32_as_float(vpnode[0x04]);                                          // sun Z (negated for GL coordinates)
        vp->lightingParams[3] = std::max(0.f, std::min(u32_as_float(vpnode[0x07]), 1.0f));           // sun intensity (clamp to 0-1)
        vp->lightingParams[4] = static_cast<float>((vpnode[0x24] >> 8) & 0xFF) * static_cast<float>(1.0 / 255.0);  // ambient intensity
        vp->lightingParams[5] = 0.0f;  // reserved

        vp->sunClamp       = m_sunClamp;
        vp->intensityClamp = (m_step == 0x10);  // just step 1.0 ?
        vp->hardwareStep   = m_step;

        // Spotlight
        const int spotColorIdx    = static_cast<int>((vpnode[0x20] >> 11) & 7);  // spotlight colour index
        const int spotFogColorIdx = static_cast<int>((vpnode[0x20] >> 8) & 7);   // spotlight on fog colour index
        vp->spotEllipse[0] = static_cast<float>(static_cast<s16>(vpnode[0x1E] & 0xFFFF)) * static_cast<float>(1.0 / 8.0);  // spotlight X position (13.3 fixed point)
        vp->spotEllipse[1] = static_cast<float>(static_cast<s16>(vpnode[0x1D] & 0xFFFF)) * static_cast<float>(1.0 / 8.0);  // spotlight Y
        vp->spotEllipse[2] = static_cast<float>((vpnode[0x1E] >> 16) & 0xFFFF);  // spotlight X size (16-bit)
        vp->spotEllipse[3] = static_cast<float>((vpnode[0x1D] >> 16) & 0xFFFF);  // spotlight Y size

        vp->spotRange[0] = 1.0f / u32_as_float(vpnode[0x21]);  // spotlight start
        vp->spotRange[1] = u32_as_float(vpnode[0x1F]);         // spotlight extent

        vp->spotColor[0] = color[spotColorIdx][0];  // spotlight colour
        vp->spotColor[1] = color[spotColorIdx][1];
        vp->spotColor[2] = color[spotColorIdx][2];

        vp->spotFogColor[0] = color[spotFogColorIdx][0];  // spotlight colour on fog
        vp->spotFogColor[1] = color[spotFogColorIdx][1];
        vp->spotFogColor[2] = color[spotFogColorIdx][2];

        // spotlight is specified in terms of physical resolution
        vp->spotEllipse[1] = 384.0f - vp->spotEllipse[1];  // flip Y position

        // Avoid division by zero
        vp->spotEllipse[2] = std::max(1.0f, vp->spotEllipse[2]);
        vp->spotEllipse[3] = std::max(1.0f, vp->spotEllipse[3]);

        vp->spotEllipse[2] = std::roundf(2047.0f / vp->spotEllipse[2]);
        vp->spotEllipse[3] = std::roundf(2047.0f / vp->spotEllipse[3]);

        // Scale the spotlight to the OpenGL viewport
        vp->spotEllipse[0] = vp->spotEllipse[0] * m_xRatio + static_cast<float>(m_xOffs);
        vp->spotEllipse[1] = vp->spotEllipse[1] * m_yRatio + static_cast<float>(m_yOffs);
        vp->spotEllipse[2] *= m_xRatio;
        vp->spotEllipse[3] *= m_yRatio;

        // Line of sight position
        vp->losPosX = static_cast<int>((static_cast<float>(vpnode[0x1c] & 0xFFFF) / 16.0f) + 0.5f);  // x position
        vp->losPosY = static_cast<int>((static_cast<float>(vpnode[0x1c] >> 16) / 16.0f) + 0.5f);     // y position 0 starts from the top

        // Fog
        vp->fogColour[0]   = static_cast<float>((vpnode[0x22] >> 16) & 0xFF) * static_cast<float>(1.0 / 255.0);  // fog colour R
        vp->fogColour[1]   = static_cast<float>((vpnode[0x22] >> 8) & 0xFF) * static_cast<float>(1.0 / 255.0);   // fog colour G
        vp->fogColour[2]   = static_cast<float>((vpnode[0x22] >> 0) & 0xFF) * static_cast<float>(1.0 / 255.0);   // fog colour B
        vp->fogDensity     = std::abs(u32_as_float(vpnode[0x23]));  // Ocean Hunter uses negative densities, which look the same
        vp->fogStart       = static_cast<float>(static_cast<s16>(vpnode[0x25] & 0xFFFF)) * static_cast<float>(1.0 / 255.0);  // fog start
        vp->fogAttenuation = static_cast<float>((vpnode[0x24] >> 16) & 0xFF) * static_cast<float>(1.0 / 255.0);  // fog attenuation
        vp->fogAmbient     = static_cast<float>((vpnode[0x25] >> 16) & 0xFF) * static_cast<float>(1.0 / 255.0);  // fog ambient

        // Avoid Infinite and NaN values for Star Wars Trilogy
        if (std::isinf(vp->fogDensity) || std::isnan(vp->fogDensity)) {
            vp->fogDensity = 0;
        }

        vp->scrollFog = static_cast<float>(vpnode[0x20] & 0xFF) * static_cast<float>(1.0 / 255.0);  // scroll fog
        vp->scrollAtt = static_cast<float>(vpnode[0x24] & 0xFF) * static_cast<float>(1.0 / 255.0);  // scroll attenuation

        // Clear texture offsets before proceeding
        m_nodeAttribs.Reset();

        // Set up coordinate system and base matrix
        InitMatrixStack(matrixBase, m_modelMat);

        // Descend down the node link. Need to start with a culling node because that defines our culling radius.
        if (!vpDisabled) {
            const u32 childptr = vpnode[0x02];
            if (((childptr >> 24) & 0x5) == 0) {
                DescendNodePtr(vpnode[0x02]);
            }
        }
    }

    // render next viewport
    if (vpnode[0x01] != 0x01000000) {
        RenderViewport(vpnode[0x01], depth + 1);
    }
}

void Scene::CopyVertexData(const R3DPoly& r3dPoly, std::vector<FVertex>& vertexArray)
{
    // Le Mans 24 and Dirt Devils draw fully transparent polygons as the first object in each viewport. In Dirt
    // Devils it is parallel to the camera, so invisible, but breaks the depth calculation; in Le Mans 24 it is a
    // diamond that never leaves a hole in the translucent geometry, so the hardware must skip it.
    if (r3dPoly.faceColour[3] == 0) {
        return;
    }

    if (m_numPolyVerts == 4) {
        if (r3dPoly.number == 4) {
            vertexArray.emplace_back(r3dPoly, 0);
            vertexArray.emplace_back(r3dPoly, 1);
            vertexArray.emplace_back(r3dPoly, 2);
            vertexArray.emplace_back(r3dPoly, 3);

            // Replace identical points (a forced triangle) with an average point,
            // or the quad code falls apart.
            FVertex* v = (&vertexArray.back()) - 3;

            for (int i = 0; i < 4; i++) {
                const int next1 = (i + 1) % 4;
                const int next2 = (i + 2) % 4;

                if (FVertex::Equal(v[i], v[next1])) {
                    FVertex::Average(v[next1], v[next2], v[next1]);
                    break;
                }
            }
        } else {
            vertexArray.emplace_back(r3dPoly, 0);
            vertexArray.emplace_back(r3dPoly, 1);
            vertexArray.emplace_back(r3dPoly, 2);
            vertexArray.emplace_back(r3dPoly, 0, 2);  // last point is an average of 0 and 2
        }
    } else {
        vertexArray.emplace_back(r3dPoly, 0);
        vertexArray.emplace_back(r3dPoly, 1);
        vertexArray.emplace_back(r3dPoly, 2);

        if (r3dPoly.number == 4) {
            vertexArray.emplace_back(r3dPoly, 0);
            vertexArray.emplace_back(r3dPoly, 2);
            vertexArray.emplace_back(r3dPoly, 3);
        }
    }
}

void Scene::GetCoordinates(int width, int height, u16 uIn, u16 vIn, float uvScale, float& uOut, float& vOut)
{
    uOut = (static_cast<float>(uIn) * uvScale) / static_cast<float>(width);
    vOut = (static_cast<float>(vIn) * uvScale) / static_cast<float>(height);
}

int Scene::GetTexFormat(int originalFormat, bool contour) const
{
    if (!contour) {
        return originalFormat;  // the same
    }

    switch (originalFormat) {
        case 1:
        case 2:
        case 3:
        case 4:
            return originalFormat + 7;  // these formats are identical to 1-4, except they lose the 4 bit alpha part when contour is enabled
        default:
            return originalFormat;
    }
}

void Scene::SetMeshValues(SortingMesh* currentMesh, const PolyHeader& ph)
{
    // copy attributes
    currentMesh->textured      = ph.TexEnabled();
    currentMesh->alphaTest     = ph.AlphaTest();
    currentMesh->textureAlpha  = ph.TextureAlpha();
    currentMesh->polyAlpha     = ph.PolyAlpha();
    currentMesh->lighting      = ph.LightEnabled();
    currentMesh->fixedShading  = ph.FixedShading() && !ph.SmoothShading();
    currentMesh->highPriority  = ph.HighPriority();
    currentMesh->transLSelect  = ph.TranslucencyPatternSelect();
    currentMesh->layered       = ph.Layered();
    currentMesh->specular      = ph.SpecularEnabled();
    currentMesh->shininess     = ph.Shininess();
    currentMesh->specularValue = ph.SpecularValue();
    currentMesh->fogIntensity  = ph.LightModifier();
    currentMesh->translatorMap = ph.TranslatorMap();
    currentMesh->noLosReturn   = ph.NoLosReturn();
    currentMesh->smoothShading = ph.SmoothShading();

    if (currentMesh->textured) {

        currentMesh->format = GetTexFormat(ph.TexFormat(), ph.AlphaTest());

        if (currentMesh->format == 7) {
            currentMesh->alphaTest = false;  // alpha test is a 1 bit test, this format needs a lower threshold, since it has 16 levels of transparency
        }

        currentMesh->x            = ph.X();
        currentMesh->y            = ph.Y();
        currentMesh->width        = ph.TexWidth();
        currentMesh->height       = ph.TexHeight();
        currentMesh->microTexture = ph.MicroTexture();
        currentMesh->inverted     = ph.TranslatorMapOffset() == 2;
        currentMesh->page         = ph.Page();

        {
            bool smoothU = ph.TexSmoothU();
            bool smoothV = ph.TexSmoothV();

            if (ph.AlphaTest()) {
                smoothU = false;  // smooth wrap makes no sense for alpha tested polys with pixel dilate
                smoothV = false;
            }

            if (ph.TexUMirror()) {
                currentMesh->wrapModeU = smoothU ? Mesh::TexWrapMode::mirror : Mesh::TexWrapMode::mirrorClamp;
            } else {
                currentMesh->wrapModeU = smoothU ? Mesh::TexWrapMode::repeat : Mesh::TexWrapMode::repeatClamp;
            }

            if (ph.TexVMirror()) {
                currentMesh->wrapModeV = smoothV ? Mesh::TexWrapMode::mirror : Mesh::TexWrapMode::mirrorClamp;
            } else {
                currentMesh->wrapModeV = smoothV ? Mesh::TexWrapMode::repeat : Mesh::TexWrapMode::repeatClamp;
            }
        }

        if (currentMesh->microTexture) {
            currentMesh->microTextureID     = ph.MicroTextureID();
            currentMesh->microTextureMinLOD = static_cast<float>(ph.MicroTextureMinLOD());
        }
    }
}

void Scene::CacheModel(Model* m, const u32* data, const u32* end)
{
    if (data == nullptr) {
        return;
    }

    u16         texCoords[4][2] = {};
    PolyHeader  ph(data);
    u64         lastHash    = ~u64{0};
    SortingMesh* currentMesh = nullptr;

    // An unordered_map on purpose: its iteration order is the draw order.
    std::unordered_map<u64, SortingMesh> sMap;

    // A header is 7 words and a polygon adds up to 4 vertices of 4 words.
    auto fits = [end](const u32* p, usize words) { return p + words <= end; };
    if (!fits(ph.header, 7)) {
        return;
    }

    int numTriangles = 0;
    {
        PolyHeader walk(data);
        while (true) {
            numTriangles += (walk.NumVerts() == 4) ? 2 : 1;
            if (!fits(walk.header, 7 + 16) || !walk.NextPoly() || !fits(walk.header, 7)) {
                break;
            }
        }
    }

    // Cache all polygons
    do {
        if (!fits(ph.header, 7 + static_cast<usize>(ph.NumVerts() - ph.NumSharedVerts()) * 4)) {
            break;
        }

        R3DPoly p;  // current polygon
        float   uvScale;

        if (ph.header[6] == 0) {
            break;
        }

        // hash the polygon attributes
        const u64 hash = ph.Hash();

        if (hash != lastHash) {
            const auto entry = sMap.try_emplace(hash);

            currentMesh = &entry.first->second;

            if (entry.second) {
                // make space for our vertices
                currentMesh->verts.reserve(static_cast<usize>(numTriangles) * 3);

                // set mesh values
                SetMeshValues(currentMesh, ph);
            }
        }

        // Obtain basic polygon parameters
        p.number = ph.NumVerts();
        uvScale  = ph.UVScale();

        ph.FaceNormal(p.faceNormal);

        // Fetch reused vertices according to bitfield, then new verts
        int j = 0;
        for (int i = 0; i < 4; i++) {  // up to 4 reused vertices
            if (ph.SharedVertex(i)) {
                p.v[j] = m_prev[i];

                texCoords[j][0] = m_prevTexCoords[i][0];
                texCoords[j][1] = m_prevTexCoords[i][1];

                // recalculate tex coords when sharing vertices across different texture tiles
                if (hash != lastHash) {
                    if (currentMesh->textured) {
                        GetCoordinates(currentMesh->width, currentMesh->height, texCoords[j][0], texCoords[j][1],
                                       uvScale, p.v[j].texcoords[0], p.v[j].texcoords[1]);
                    }
                }

                j++;
            }
        }

        lastHash = hash;

        // copy face attributes

        if (!ph.PolyColor()) {
            const u32 colorIdx = static_cast<u32>(ph.ColorIndex());
            const u32 entry    = ColourTableEntry(m_colorTableAddr + colorIdx);
            p.faceColour[2]    = static_cast<u8>(entry & 0xFF);
            p.faceColour[1]    = static_cast<u8>((entry >> 8) & 0xFF);
            p.faceColour[0]    = static_cast<u8>((entry >> 16) & 0xFF);
        } else {
            p.faceColour[0] = static_cast<u8>(ph.header[4] >> 24);
            p.faceColour[1] = static_cast<u8>((ph.header[4] >> 16) & 0xFF);
            p.faceColour[2] = static_cast<u8>((ph.header[4] >> 8) & 0xFF);
        }

        p.faceColour[3] = ph.Transparency();

        if (ph.Discard1() && !ph.Discard2()) {
            p.faceColour[3] /= 2;
        }

        // With flat shading, normals from shared vertices can't be reused.
        for (int i = 0; i < p.number && !ph.SmoothShading(); i++) {
            p.v[i].normal[0] = p.faceNormal[0];
            p.v[i].normal[1] = p.faceNormal[1];
            p.v[i].normal[2] = p.faceNormal[2];
        }

        p.textureNP = ph.TextureNP() * m_textureNPFactor;

        const u32* vData = ph.StartOfData();  // vertex data starts here

        // remaining vertices are new and defined here
        for (; j < p.number; j++) {
            // Fetch vertices
            const u32 ix = vData[0];
            const u32 iy = vData[1];
            const u32 iz = vData[2];
            const u32 it = vData[3];

            // Decode vertices
            p.v[j].pos[0] = static_cast<float>(static_cast<s32>(ix) >> 8) * m_vertexFactor;
            p.v[j].pos[1] = static_cast<float>(static_cast<s32>(iy) >> 8) * m_vertexFactor;
            p.v[j].pos[2] = static_cast<float>(static_cast<s32>(iz) >> 8) * m_vertexFactor;
            p.v[j].pos[3] = 1.0f;

            // Per vertex normals
            if (ph.SmoothShading()) {
                p.v[j].normal[0] = byte_to_float(static_cast<s8>(ix & 0xFF));
                p.v[j].normal[1] = byte_to_float(static_cast<s8>(iy & 0xFF));
                p.v[j].normal[2] = byte_to_float(static_cast<s8>(iz & 0xFF));
            }

            if (ph.FixedShading() && !ph.SmoothShading()) {  // fixed shading seems disabled when real normals are set
                float shade;

                // with specular enabled, fixed shading values are unsigned
                if (ph.SpecularEnabled()) {
                    shade = static_cast<float>(ix & 0xFF) * static_cast<float>(1.0 / 255.0);
                } else {
                    shade = byte_to_float(static_cast<s8>(ix & 0xFF));
                }

                p.v[j].fixedShade = shade;
            }

            float texU = 0;
            float texV = 0;

            // tex coords
            if (currentMesh->textured) {
                GetCoordinates(currentMesh->width, currentMesh->height, static_cast<u16>(it >> 16),
                               static_cast<u16>(it & 0xFFFF), uvScale, texU, texV);
            }

            p.v[j].texcoords[0] = texU;
            p.v[j].texcoords[1] = texV;

            // cache un-normalised tex coordinates
            texCoords[j][0] = static_cast<u16>(it >> 16);
            texCoords[j][1] = static_cast<u16>(it & 0xFFFF);

            vData += 4;
        }

        // double up vertices for two-sided lighting
        if (ph.DoubleSided() && !ph.Discard()) {

            R3DPoly tempP = p;

            // flip normals
            invert3(tempP.faceNormal);

            for (int i2 = 0; i2 < tempP.number; i2++) {
                invert3(tempP.v[i2].normal);
            }

            CopyVertexData(tempP, currentMesh->verts);
        }

        // Copy this polygon into the model buffer
        if (!ph.Discard()) {
            CopyVertexData(p, currentMesh->verts);
        }

        // Copy current vertices into previous vertex array
        for (int i = 0; i < 4; i++) {
            m_prev[i]             = p.v[i];
            m_prevTexCoords[i][0] = texCoords[i][0];
            m_prevTexCoords[i][1] = texCoords[i][1];
        }

    } while (ph.NextPoly() && fits(ph.header, 7));

    // the data is sorted; copy it to the main structures

    m->meshes->reserve(sMap.size());

    for (auto& it : sMap) {

        if (m->dynamic) {
            // calculate vertex buffer values for current mesh
            it.second.vboOffset   = static_cast<int>(m_polyBufferRam.size() + kMaxRomVerts);
            it.second.vertexCount = static_cast<int>(it.second.verts.size());

            // copy poly data to main buffer
            m_polyBufferRam.insert(m_polyBufferRam.end(), it.second.verts.begin(), it.second.verts.end());
        } else {
            // calculate vertex buffer values for current mesh
            it.second.vboOffset   = static_cast<int>(m_polyBufferRom.size());
            it.second.vertexCount = static_cast<int>(it.second.verts.size());

            // copy poly data to main buffer
            m_polyBufferRom.insert(m_polyBufferRom.end(), it.second.verts.begin(), it.second.verts.end());
        }

        // Copy the temporary mesh into the model; its vertex data is already in
        // the main buffer.
        m->meshes->push_back(static_cast<const Mesh&>(it.second));
    }
}

bool Scene::IsDynamicModel(const u32* data, const u32* end) const
{
    if (data == nullptr) {
        return false;
    }

    PolyHeader p(data);

    do {
        if (p.header + 7 > end) {
            return false;
        }
        if ((p.header[1] & 2) == 0) {  // model has rgb colour palette
            return true;
        }

    } while (p.NextPoly());

    return false;
}

void Scene::calc_viewport(Viewport* vp)
{
    const float l = vp->angle_left;  // the projection frustum's shape, also used for culling
    const float r = vp->angle_right;
    const float t = vp->angle_top;
    const float b = vp->angle_bottom;

    vp->projectionMatrix.load_identity();

    if (widens(*vp)) {
        // Grow the frustum to the wide target's edges at the 4:3 viewport's
        // own scale, so the 4:3 picture stays put, and cull against the wider
        // one by mapping x back.
        const int   left   = static_cast<int>(m_xOffs) + static_cast<int>(vp->vpX * m_xRatio);
        const int   width  = static_cast<int>(vp->vpWidth * m_xRatio);
        const float per_px = (r - l) / static_cast<float>(width);
        const float wl     = l - static_cast<float>(left) * per_px;
        const float wr     = r + (static_cast<float>(m_totalXRes) - static_cast<float>(left + width)) * per_px;
        const float ratio  = (r - l) / (wr - wl);
        m_planes.correction = ratio;
        m_planes.shift      = wl * ratio - l;

        vp->x      = 0;
        vp->y      = static_cast<int>(m_yOffs) + static_cast<int>((384.0f - (vp->vpY + vp->vpHeight)) * m_yRatio);
        vp->width  = static_cast<int>(m_totalXRes);
        vp->height = static_cast<int>(vp->vpHeight * m_yRatio);

        vp->projectionMatrix.frustum_rz(wl, wr, b, t, kNearPlane);
    } else {

        vp->x      = static_cast<int>(m_xOffs) + static_cast<int>(vp->vpX * m_xRatio);
        vp->y      = static_cast<int>(m_yOffs) + static_cast<int>((384.0f - (vp->vpY + vp->vpHeight)) * m_yRatio);
        vp->width  = static_cast<int>(vp->vpWidth * m_xRatio);
        vp->height = static_cast<int>(vp->vpHeight * m_yRatio);

        vp->projectionMatrix.frustum_rz(l, r, b, t, kNearPlane);
    }
}

bool Scene::widens(const Viewport& vp) const
{
    return m_wideScreen && vp.vpX == 0 && vp.vpWidth >= 495 && vp.vpY == 0 && vp.vpHeight >= 383;
}

void Scene::translate_los_position(int inX, int inY, int& outX, int& outY) const
{
    // remap the Real3D's 496x384 to the target viewport
    inY = 384 - inY;

    outX = static_cast<int>(m_xOffs) + static_cast<int>(static_cast<float>(inX) * m_xRatio);
    outY = static_cast<int>(m_yOffs) + static_cast<int>(static_cast<float>(inY) * m_yRatio);
    outX = std::clamp(outX, 0, static_cast<int>(m_totalXRes) - 1);
    outY = std::clamp(outY, 0, static_cast<int>(m_totalYRes) - 1);
}

}  // namespace sm2::render::model3
