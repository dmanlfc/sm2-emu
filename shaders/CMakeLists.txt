#  ____  __  __  ____         _____ __  __ _   _
# / ___||  \/  ||___ \       | ____|  \/  | | | |
# \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
#  ___) | |  | | / __/|_____|| |___| |  | | |_| |
# |____/|_|  |_||_____|      |_____|_|  |_|\___/
#
# sm2-emu — A Sega Model 2 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: BSD-3-Clause
#
# This header must not be removed. The source files in this project may not be
# used to contribute to commercial projects or for monetary gain without the
# express written permission of the author.
#

# Shader modules, compiled to SPIR-V at build time and embedded as constexpr
# arrays. Consumers link sm2_shaders and #include "shaders/<stem>.h".

sm2_declare_shader_library(sm2_shaders)

sm2_add_shader(sm2_shaders SOURCE fullscreen_quad.vert    SYMBOL kFullscreenQuadVert)
sm2_add_shader(sm2_shaders SOURCE tilemap_composite.frag SYMBOL kTilemapCompositeFrag)
sm2_add_shader(sm2_shaders SOURCE polygon.vert           SYMBOL kPolygonVert)
sm2_add_shader(sm2_shaders SOURCE polygon.frag           SYMBOL kPolygonFrag)
sm2_add_shader(sm2_shaders SOURCE texel_decode.comp      SYMBOL kTexelDecodeComp)
