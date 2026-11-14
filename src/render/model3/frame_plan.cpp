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
// Portions derived from Supermodel's Src/Graphics/New3D/New3D.cpp and
// R3DShader.cpp, Copyright (c) 2003-2026 The Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.

#include "render/model3/frame_plan.h"

#include "render/model3/textures.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace sm2::render::model3 {

namespace {

PixelRect intersect(const PixelRect& a, const PixelRect& b)
{
    const s32 x0 = std::max(a.x, b.x);
    const s32 y0 = std::max(a.y, b.y);
    const s32 x1 = std::min(a.x + a.width, b.x + b.width);
    const s32 y1 = std::min(a.y + a.height, b.y + b.height);
    return PixelRect{x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)};
}

/// GL lower-left rectangle to top-left.
PixelRect from_gl(int x, int y, int width, int height, u32 target_height)
{
    return PixelRect{x, static_cast<s32>(target_height) - (y + height), width, height};
}

class PlanBuilder {
public:
    PlanBuilder(Scene& scene, bool encode_depth, std::array<bool, 2> blank_pages, FramePlan& plan)
        : m_scene(scene), m_plan(plan), m_encode_depth(encode_depth), m_blank_pages(blank_pages)
    {
    }

    void build();

private:
    void fill_viewport_uniforms(const Viewport& vp);
    void fill_mesh_uniforms(const Mesh& mesh, const Model& model);
    u32  push_uniforms();
    [[nodiscard]] bool fully_discarded() const;
    void set_texel_decode(int format);
    u32  model_index(const Model& model, bool translator_map);
    u32  viewport_index(const Node& node);
    bool collect(int priority, bool overlay, Layer layer, DrawRange& range);
    void scroll_fog();
    void ambient_fog();
    void add_fog(const Node& node, const float rgba[4], float attenuation, float ambient,
                 const float spotFogColor[3], const float spotEllipse[4]);
    void line_of_sight(int priority, LayerPass& pass);

    Scene&     m_scene;
    FramePlan& m_plan;
    bool       m_encode_depth;
    std::array<bool, 2> m_blank_pages;

    R3DUniforms m_uniforms{};
    bool        m_last_uniform_valid = false;

    // Mesh state machine, reset at the start of each (priority, overlay)
    // iteration.
    bool m_mesh_state_dirty   = true;
    bool m_shadow_layered     = false;
    bool m_shadow_no_los      = false;
    bool m_effective_layered  = false;

    std::unordered_map<const Model*, u32> m_model_index[2];
    std::unordered_map<const Node*, u32>  m_viewport_index;
};

void PlanBuilder::fill_viewport_uniforms(const Viewport& vp)
{
    std::memcpy(m_uniforms.projMat, vp.projectionMatrix.data(), sizeof(m_uniforms.projMat));

    m_uniforms.fogColour[0] = vp.fogColour[0];
    m_uniforms.fogColour[1] = vp.fogColour[1];
    m_uniforms.fogColour[2] = vp.fogColour[2];
    m_uniforms.fogColour[3] = 0.0f;

    m_uniforms.fogDensity     = vp.fogDensity;
    m_uniforms.fogStart       = vp.fogStart;
    m_uniforms.fogAttenuation = vp.fogAttenuation;
    m_uniforms.fogAmbient     = vp.fogAmbient;

    // lightingParams as vec3[2], widened to vec4[2].
    for (int i = 0; i < 2; i++) {
        m_uniforms.lighting[i][0] = vp.lightingParams[i * 3 + 0];
        m_uniforms.lighting[i][1] = vp.lightingParams[i * 3 + 1];
        m_uniforms.lighting[i][2] = vp.lightingParams[i * 3 + 2];
        m_uniforms.lighting[i][3] = 0.0f;
    }
    m_uniforms.sunClamp       = vp.sunClamp ? 1u : 0u;
    m_uniforms.intensityClamp = vp.intensityClamp ? 1u : 0u;

    for (int i = 0; i < 4; i++) {
        m_uniforms.spotEllipse[i] = vp.spotEllipse[i];
    }
    m_uniforms.spotRange[0] = vp.spotRange[0];
    m_uniforms.spotRange[1] = vp.spotRange[1];
    for (int i = 0; i < 3; i++) {
        m_uniforms.spotColor[i]    = vp.spotColor[i];
        m_uniforms.spotFogColor[i] = vp.spotFogColor[i];
    }
    m_uniforms.spotColor[3]    = 0.0f;
    m_uniforms.spotFogColor[3] = 0.0f;

    m_uniforms.hardwareStep = vp.hardwareStep;
    m_uniforms.cota         = vp.cota;
    m_uniforms.encodeDepth  = m_encode_depth ? 1u : 0u;

    // spotEllipse is in GL screen coordinates; the shader flips the top-left
    // gl_FragCoord back with this.
    m_uniforms.fragCoordYTransform[0] = -1.0f;
    m_uniforms.fragCoordYTransform[1] = static_cast<float>(m_plan.target_height);
}

void PlanBuilder::fill_mesh_uniforms(const Mesh& mesh, const Model& model)
{
    // The model's texture offsets are folded in.
    const int transX    = model.textureOffsetX;
    const int transY    = model.textureOffsetY;
    const int transPage = model.page;

    m_uniforms.textureEnabled     = mesh.textured ? 1u : 0u;
    m_uniforms.microTexture       = mesh.microTexture ? 1u : 0u;
    m_uniforms.texturePage        = mesh.page ^ transPage;
    m_uniforms.microTextureMinLOD = mesh.microTextureMinLOD;
    m_uniforms.microTextureID     = mesh.microTextureID;

    m_uniforms.baseTexInfo[0] = mesh.x + transX;
    m_uniforms.baseTexInfo[1] = mesh.y + transY;
    m_uniforms.baseTexInfo[2] = mesh.width;
    m_uniforms.baseTexInfo[3] = mesh.height;
    m_uniforms.baseTexType    = mesh.format;
    set_texel_decode(mesh.format);

    m_uniforms.textureInverted = mesh.inverted ? 1u : 0u;
    m_uniforms.alphaTest       = mesh.alphaTest ? 1u : 0u;
    m_uniforms.textureAlpha    = mesh.textureAlpha ? 1u : 0u;
    m_uniforms.fogIntensity    = mesh.fogIntensity;

    m_uniforms.lightEnabled    = mesh.lighting ? 1u : 0u;
    m_uniforms.shininess       = mesh.shininess;
    m_uniforms.specularEnabled = mesh.specular ? 1u : 0u;
    m_uniforms.specularValue   = mesh.specularValue;
    m_uniforms.fixedShading    = mesh.fixedShading ? 1u : 0u;
    m_uniforms.smoothShading   = mesh.smoothShading ? 1u : 0u;
    m_uniforms.polyAlpha       = mesh.polyAlpha ? 1u : 0u;

    m_uniforms.textureWrapMode[0] = static_cast<s32>(mesh.wrapModeU);
    m_uniforms.textureWrapMode[1] = static_cast<s32>(mesh.wrapModeV);

    m_uniforms.noLosReturn = mesh.noLosReturn ? 1u : 0u;

    // A noLosReturn change resets layered to off.
    if (m_mesh_state_dirty || mesh.noLosReturn != m_shadow_no_los) {
        m_shadow_no_los     = mesh.noLosReturn;
        m_effective_layered = false;
    }
    if (m_mesh_state_dirty || mesh.layered != m_shadow_layered) {
        m_shadow_layered    = mesh.layered;
        m_effective_layered = mesh.layered;
    }
    m_mesh_state_dirty = false;
}

// The texel decode of ExtractColour() as shifts, masks and a scale, so the
// shader needs no per-format branches.
void PlanBuilder::set_texel_decode(int format)
{
    struct Decode {
        u32   shift[4];
        u32   mask[4];
        float scale[4];
        bool  divide;
        bool  invert;
    };
    constexpr float k15 = 1.0F / 15.0F;
    constexpr float k31 = 1.0F / 31.0F;
    static const Decode kTable[12] = {
        {{10, 5, 0, 15}, {0x1F, 0x1F, 0x1F, 1}, {k31, k31, k31, 1.0F}, false, true},  // T1RGB5
        {{0, 0, 0, 4}, {0xF, 0xF, 0xF, 0xF}, {k15, k15, k15, k15}, false, false},             // A4L4 low
        {{4, 4, 4, 0}, {0xF, 0xF, 0xF, 0xF}, {k15, k15, k15, k15}, false, false},             // L4A4 low
        {{8, 8, 8, 12}, {0xF, 0xF, 0xF, 0xF}, {k15, k15, k15, k15}, false, false},            // A4L4 high
        {{12, 12, 12, 8}, {0xF, 0xF, 0xF, 0xF}, {k15, k15, k15, k15}, false, false},          // L4A4 high
        {{0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0xFF}, {255.0F, 255.0F, 255.0F, 255.0F}, true, true},        // L8 low
        {{8, 8, 8, 8}, {0xFF, 0xFF, 0xFF, 0xFF}, {255.0F, 255.0F, 255.0F, 255.0F}, true, true},        // L8 high
        {{12, 8, 4, 0}, {0xF, 0xF, 0xF, 0xF}, {k15, k15, k15, k15}, false, false},            // RGBA4
        {{0, 0, 0, 0}, {0xF, 0xF, 0xF, 0xF}, {15.0F, 15.0F, 15.0F, 15.0F}, true, true},             // L4 nibbles
        {{4, 4, 4, 4}, {0xF, 0xF, 0xF, 0xF}, {15.0F, 15.0F, 15.0F, 15.0F}, true, true},
        {{8, 8, 8, 8}, {0xF, 0xF, 0xF, 0xF}, {15.0F, 15.0F, 15.0F, 15.0F}, true, true},
        {{12, 12, 12, 12}, {0xF, 0xF, 0xF, 0xF}, {15.0F, 15.0F, 15.0F, 15.0F}, true, true},
    };
    const Decode& d = kTable[static_cast<usize>(std::clamp(format, 0, 11))];
    for (int i = 0; i < 4; i++) {
        m_uniforms.texShift[i] = d.shift[i];
        m_uniforms.texMask[i]  = d.mask[i];
        m_uniforms.texScale[i] = d.scale[i];
    }
    m_uniforms.texDivide      = d.divide ? 1u : 0u;
    m_uniforms.texAlphaInvert = d.invert ? 1u : 0u;
}

// Every fragment of the mesh is discarded: its texture page is still all
// zero and a zero texel of its format has alpha 0 (VON2's boot draws many
// such triangles).
bool PlanBuilder::fully_discarded() const
{
    const R3DUniforms& u = m_uniforms;
    return u.textureEnabled != 0 && u.microTexture == 0 && (u.textureAlpha != 0 || u.alphaTest != 0)
           && m_blank_pages[static_cast<usize>(u.texturePage & 1)] && decode_texel(u.baseTexType, 0)[3] == 0.0f;
}

u32 PlanBuilder::push_uniforms()
{
    if (m_last_uniform_valid
        && std::memcmp(&m_plan.uniforms.back(), &m_uniforms, sizeof(R3DUniforms)) == 0) {
        return static_cast<u32>(m_plan.uniforms.size() - 1);
    }
    m_plan.uniforms.push_back(m_uniforms);
    m_last_uniform_valid = true;
    return static_cast<u32>(m_plan.uniforms.size() - 1);
}

u32 PlanBuilder::model_index(const Model& model, bool translator_map)
{
    auto& map = m_model_index[translator_map ? 1 : 0];
    const auto found = map.find(&model);
    if (found != map.end()) {
        return found->second;
    }
    R3DModelUniforms block{};
    std::memcpy(block.modelMat, model.modelMat, sizeof(block.modelMat));
    block.modelScale    = model.scale;
    block.nodeAlpha     = model.alpha;
    block.translatorMap = translator_map ? 1u : 0u;
    m_plan.models.push_back(block);
    const u32 index = static_cast<u32>(m_plan.models.size() - 1);
    map.emplace(&model, index);
    return index;
}

u32 PlanBuilder::viewport_index(const Node& node)
{
    const auto found = m_viewport_index.find(&node);
    if (found != m_viewport_index.end()) {
        return found->second;
    }
    const Viewport& vp = node.viewport;
    ViewportRect rect;
    rect.viewport = from_gl(vp.x, vp.y, vp.width, vp.height, m_plan.target_height);
    const PixelRect target{0, 0, static_cast<s32>(m_plan.target_width), static_cast<s32>(m_plan.target_height)};
    rect.scissor = intersect(intersect(rect.viewport, target), m_plan.display);
    m_plan.viewports.push_back(rect);
    const u32 index = static_cast<u32>(m_plan.viewports.size() - 1);
    m_viewport_index.emplace(&node, index);
    m_plan.widened = m_plan.widened || m_scene.widens(vp);
    return index;
}

// Records one layer's draws for a priority. Returns hasOverlay: whether the
// priority has any high-priority mesh at all.
bool PlanBuilder::collect(int priority, bool overlay, Layer layer, DrawRange& range)
{
    bool hasOverlay = false;
    range.begin     = static_cast<u32>(m_plan.draws.size());

    for (const auto& n : m_scene.nodes()) {
        if (n.viewport.priority != priority || n.models.empty()) {
            continue;
        }

        fill_viewport_uniforms(n.viewport);
        const u32 viewport = viewport_index(n);

        for (const auto& m : n.models) {
            if (!m.meshes || m.meshes->empty()) {
                continue;
            }

            for (const auto& mesh : *m.meshes) {
                if (mesh.highPriority) {
                    hasOverlay = true;
                }

                if (!mesh.Render(layer, m.alpha)) {
                    continue;
                }
                if (mesh.highPriority != overlay) {
                    continue;
                }
                // Before the empty check: an empty mesh still steps the stencil state.
                fill_mesh_uniforms(mesh, m);
                if (mesh.vertexCount <= 0 || fully_discarded()) {
                    continue;
                }

                R3DDraw draw;
                draw.uniforms      = push_uniforms();
                draw.variant       = r3d_variant(m_uniforms);
                draw.model         = model_index(m, mesh.translatorMap);
                draw.viewport      = viewport;
                draw.dynamic       = mesh.vboOffset >= static_cast<int>(kMaxRomVerts);
                draw.first_vertex  = static_cast<u32>(draw.dynamic ? mesh.vboOffset - static_cast<int>(kMaxRomVerts)
                                                                   : mesh.vboOffset);
                draw.vertex_count  = static_cast<u32>(mesh.vertexCount);
                draw.layered       = m_effective_layered;
                draw.no_los_return = mesh.noLosReturn;
                m_plan.draws.push_back(draw);
            }
        }
    }

    range.end = static_cast<u32>(m_plan.draws.size());
    return hasOverlay;
}

void PlanBuilder::add_fog(const Node& node, const float rgba[4], float attenuation, float ambient,
                          const float spotFogColor[3], const float spotEllipse[4])
{
    FogDraw fog;
    fog.viewport = viewport_index(node);
    for (int i = 0; i < 4; i++) {
        fog.uniforms.fogColour[i]   = rgba[i];
        fog.uniforms.spotEllipse[i] = spotEllipse[i];
    }
    for (int i = 0; i < 3; i++) {
        fog.uniforms.spotFogColor[i] = spotFogColor[i];
    }
    fog.uniforms.spotFogColor[3]          = 0.0f;
    fog.uniforms.fragCoordYTransform[0]   = -1.0f;
    fog.uniforms.fragCoordYTransform[1]   = static_cast<float>(m_plan.target_height);
    fog.uniforms.fogAttenuation           = attenuation;
    fog.uniforms.fogAmbient               = ambient;
    m_plan.fogs.push_back(fog);
}

void PlanBuilder::scroll_fog()
{
    // A best guess at the logic, from what games do:
    //
    // ocean hunter		- every viewport sets scroll fog. Start from the lowest priority layer, as the higher ones are sometimes garbage.
    // scud race		- the first viewports in the priority layer have no scroll values; the later ones all have valid ones.
    // daytona			- seems not to use scroll fog, but sets values in the first viewports and none in the last. The end credits use scroll fog, constrained to the viewport.
    // vf3				- only the first viewport sets it, with the highest select value; the rest of the priority layer have lower select values.
    // sega bassfishing	- the first viewport in priority 1 sets the scroll value; the rest have the wrong value and a higher select value.
    // spikeout final	- only the second viewport in the priority layer sets scroll values, with the highest select value.
    //
    // So the Real3D seems to pick the highest scroll fog value, starting from the lowest priority layer, and stops at
    // the first layer that has one. Fogging seems constrained to that viewport. Scroll fog needs a density or start
    // value to work, but these can come from another viewport with the same fog colour.

    const Node* nodePtr = nullptr;
    const auto& nodes   = m_scene.nodes();

    for (int i = 0; i < 4 && !nodePtr; i++) {
        for (const auto& n : nodes) {
            if (n.viewport.priority == i) {
                if (n.viewport.scrollFog > 0.f) {
                    // keep the highest scroll fog value
                    if (nodePtr) {
                        if (nodePtr->viewport.scrollFog < n.viewport.scrollFog) {
                            nodePtr = &n;
                        }
                        continue;
                    }
                    nodePtr = &n;
                }
            }
        }
    }

    if (!nodePtr) {
        return;
    }

    // Look for a viewport with that fog colour that sets a fog density, start or spotlight on fog;
    // if all of these are zero, fogging is effectively disabled.
    for (const auto& n : nodes) {
        if (nodePtr->viewport.fogColour[0] == n.viewport.fogColour[0]
            && nodePtr->viewport.fogColour[1] == n.viewport.fogColour[1]
            && nodePtr->viewport.fogColour[2] == n.viewport.fogColour[2]) {

            // spotlight on fog is disabled if its colour is black
            bool spotOnFogEnabled = false;
            for (auto c : n.viewport.spotFogColor) {
                if (c > 0.0f) {
                    spotOnFogEnabled = true;
                    break;
                }
            }

            const bool fog = n.viewport.fogDensity > 0.0f || n.viewport.fogStart > 0.0f || n.viewport.scrollAtt > 0.0f;
            const bool spotlightFog = n.viewport.fogAttenuation > 0.0f && spotOnFogEnabled;

            if (fog || spotlightFog) {
                const auto& vp = nodePtr->viewport;
                const float rgba[4] = {vp.fogColour[0], vp.fogColour[1], vp.fogColour[2], vp.scrollFog};

                // with regular fog enabled, the hardware seems to skip the spotlight on fog (ECA)
                const float fogAttentuation = (fog && spotlightFog) ? 0.0f : n.viewport.fogAttenuation;

                add_fog(*nodePtr, rgba, fogAttentuation, n.viewport.fogAmbient, n.viewport.spotFogColor,
                        n.viewport.spotEllipse);
                break;
            }
        }
    }
}

void PlanBuilder::ambient_fog()
{
    // Not fully understood. Some games set fog ambient, which seems to darken the 2D background layer too when
    // scroll fog is not set: roughly tileGenColour * fogAmbient, so 1.0 is a no-op and lower values darken the
    // image. Scroll fog already includes the fog ambient, as it darkens the fog colour. Like scroll fog, this
    // probably works at the mixer level rather than in the frame buffer.
    //
    // Pick the lowest fog ambient value from the first priority layer only. The effect seems to need a fog
    // density or fog start value (Lost World).

    float       fogAmbient = 1.0f;
    const Node* nodePtr    = nullptr;

    for (int i = 0; i < 4; i++) {
        bool hasPriority = false;

        for (const auto& n : m_scene.nodes()) {
            const auto& vp = n.viewport;

            if (vp.priority == i) {
                hasPriority = true;

                if (vp.scrollFog > 0.0f) {
                    continue;
                }

                // skip viewports with no fog density or fog start
                if (vp.fogDensity <= 0.0f && vp.fogStart <= 0.0f) {
                    continue;
                }

                if (vp.fogAmbient < fogAmbient) {
                    nodePtr    = &n;
                    fogAmbient = vp.fogAmbient;
                }
            }
        }

        if (nodePtr || hasPriority) {
            break;
        }
    }

    if (nodePtr) {
        const auto& vp      = nodePtr->viewport;
        const float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f - fogAmbient};
        add_fog(*nodePtr, rgba, 0.0f, 1.0f, vp.spotFogColor, vp.spotEllipse);  // assume the spotlight is not used
    }
}

// Decides where to sample line of sight.
void PlanBuilder::line_of_sight(int priority, LayerPass& pass)
{
    for (const auto& n : m_scene.nodes()) {
        if (n.viewport.priority == priority) {
            if (n.viewport.losPosX || n.viewport.losPosY) {
                int losX = 0;
                int losY = 0;
                m_scene.translate_los_position(n.viewport.losPosX, n.viewport.losPosY, losX, losY);
                pass.los   = true;
                pass.los_x = std::clamp(losX, 0, static_cast<int>(m_plan.target_width) - 1);
                pass.los_y = std::clamp(static_cast<int>(m_plan.target_height) - 1 - losY, 0,
                                        static_cast<int>(m_plan.target_height) - 1);
                return;
            }
        }
    }
}

void PlanBuilder::build()
{
    const SceneGeometry& g = m_scene.geometry();
    m_plan.target_width    = g.total_x();
    m_plan.target_height   = g.total_y();

    // The display scissor and the 2D layer viewport, inset or shifted by the
    // 2D layer compensation (2 pixels at 384 lines).
    const s32 correction = static_cast<s32>((static_cast<double>(g.y_res()) / 384.0) * 2.0 + 0.5);
    const s32 x_res      = static_cast<s32>(g.x_res());
    const s32 y_res      = static_cast<s32>(g.y_res());
    const s32 x_off      = static_cast<s32>(g.x_offset());
    if (g.wide_screen()) {
        m_plan.display = PixelRect{0, correction, static_cast<s32>(g.total_x()), y_res - correction * 2};
    } else {
        m_plan.display = PixelRect{x_off + correction, correction, x_res - correction * 2, y_res - correction * 2};
    }
    m_plan.layer_2d = PixelRect{x_off - correction, -correction, x_res, y_res};

    m_plan.white_flash = m_scene.block_culling();
    if (m_plan.white_flash) {
        return;
    }

    ambient_fog();
    scroll_fog();  // the fog layer, if any, must be drawn here

    for (int pri = 0; pri <= 3; pri++) {

        if (m_scene.skip_layer(pri)) {
            continue;
        }

        for (int i = 0; i < 2; i++) {
            LayerPass pass;
            pass.priority = pri;
            pass.overlay  = (i == 1);

            // Reset the mesh state at the top of every iteration.
            m_mesh_state_dirty = true;
            m_shadow_layered   = false;
            m_shadow_no_los    = false;

            m_uniforms.discardAlpha = 1u;
            m_uniforms.colourLayer  = kLayerColour;
            const bool hasOverlay   = collect(pri, pass.overlay, Layer::colour, pass.colour);

            if (!pass.overlay) {
                line_of_sight(pri, pass);
            }

            m_uniforms.discardAlpha = 0u;
            m_uniforms.colourLayer  = kLayerTrans0;
            collect(pri, pass.overlay, Layer::trans1, pass.trans1);

            m_uniforms.colourLayer = kLayerTrans1;
            collect(pri, pass.overlay, Layer::trans2, pass.trans2);

            m_plan.passes.push_back(pass);

            if (!hasOverlay) {
                break;  // no high priority polys
            }
        }
    }
}

}  // namespace

bool FramePlan::los_active() const
{
    return std::any_of(passes.begin(), passes.end(), [](const LayerPass& p) { return p.los; });
}

void build_frame_plan(Scene& scene, bool encode_depth, std::array<bool, 2> blank_pages, FramePlan& plan)
{
    plan.white_flash = false;
    plan.widened     = false;
    plan.fogs.clear();
    plan.passes.clear();
    plan.draws.clear();
    plan.uniforms.clear();
    plan.models.clear();
    plan.viewports.clear();

    PlanBuilder builder(scene, encode_depth, blank_pages, plan);
    builder.build();
}

float decode_line_of_sight(u32 depth_bits, bool no_los_return)
{
    const float depth = u32_as_float(depth_bits);

    u8    stencilVal = no_los_return ? 0x80 : 0x00;
    float zVal       = depth / kNearPlane;

    // mask the stencil, as layered polygon attributes use the lower bits
    stencilVal &= 0x80;

    // A zero stencil means sky or nothing; 1 means geometry. The Real3D sets the float's low bit when the line
    // of sight test passes (hits no geometry).
    u32 bits = float_as_u32(zVal);
    if (stencilVal == 0) {
        bits |= 1;
    } else {
        bits &= ~1u;
    }
    return u32_as_float(bits);
}

}  // namespace sm2::render::model3
