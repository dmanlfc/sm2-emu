#  ____  __  __  ____         _____ __  __ _   _
# / ___||  \/  ||___ \       | ____|  \/  | | | |
# \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
#  ___) | |  | | / __/|_____|| |___| |  | | |_| |
# |____/|_|  |_||_____|      |_____|_|  |_|\___/
#
# sm2-emu — A Sega Model 1 & 2 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This header must not be removed. sm2-emu is licensed under the GNU General
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
# early-fragment-tests variant of polygon.frag; see the SM2_EARLY_FRAGMENT_TESTS
# comment there.
sm2_add_shader(sm2_shaders SOURCE polygon.frag           SYMBOL kPolygonFragEarly
    DEFINES SM2_EARLY_FRAGMENT_TESTS=1 VARIANT early)
sm2_add_shader(sm2_shaders SOURCE texel_decode.comp      SYMBOL kTexelDecodeComp)
sm2_add_shader(sm2_shaders SOURCE tilemap_compose.comp   SYMBOL kTilemapComposeComp)
