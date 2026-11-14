# ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
# ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
# █████╔╝ █████╗  ██║███████╗███████║██║   ██║
# ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
# ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
# ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
#            継 承  ( k e i s h ō )
#
# A Sega Model 1, 2 & 3 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This header must not be removed. Keisho is licensed under the GNU General
# Public License, version 3 or later, with additional terms under section 7 of
# that licence. See LICENSE and NOTICE.
#

# Shader modules, compiled to SPIR-V at build time and embedded as constexpr
# arrays. Consumers link sm2_shaders and #include "shaders/<stem>.h".

sm2_declare_shader_library(sm2_shaders)

sm2_add_shader(sm2_shaders SOURCE fullscreen_quad.vert    SYMBOL kFullscreenQuadVert)
sm2_add_shader(sm2_shaders SOURCE tilemap_composite.frag SYMBOL kTilemapCompositeFrag)
sm2_add_shader(sm2_shaders SOURCE present.frag           SYMBOL kPresentFrag)
sm2_add_shader(sm2_shaders SOURCE polygon.vert           SYMBOL kPolygonVert)
sm2_add_shader(sm2_shaders SOURCE polygon.frag           SYMBOL kPolygonFrag)
# Early-fragment-tests variant; see SM2_EARLY_FRAGMENT_TESTS in polygon.frag.
sm2_add_shader(sm2_shaders SOURCE polygon.frag           SYMBOL kPolygonFragEarly
    DEFINES SM2_EARLY_FRAGMENT_TESTS=1 VARIANT early)
sm2_add_shader(sm2_shaders SOURCE texel_decode.comp      SYMBOL kTexelDecodeComp)
sm2_add_shader(sm2_shaders SOURCE tilemap_compose.comp   SYMBOL kTilemapComposeComp)

# Sega Model 3 (Real3D and tile generator compositing).
sm2_add_shader(sm2_shaders SOURCE model3_r3d.vert             SYMBOL kModel3R3dVert)
sm2_add_shader(sm2_shaders SOURCE model3_r3d.frag             SYMBOL kModel3R3dFrag)
sm2_add_shader(sm2_shaders SOURCE model3_r3d.frag             SYMBOL kModel3R3dFragEncode VARIANT encode DEFINES SM2_ENCODE_DEPTH)
sm2_add_shader(sm2_shaders SOURCE model3_scroll_fog.frag      SYMBOL kModel3ScrollFogFrag)
sm2_add_shader(sm2_shaders SOURCE model3_composite_base.frag  SYMBOL kModel3CompositeBaseFrag)
sm2_add_shader(sm2_shaders SOURCE model3_composite_trans.frag SYMBOL kModel3CompositeTransFrag)
sm2_add_shader(sm2_shaders SOURCE model3_layer.frag           SYMBOL kModel3LayerFrag)
