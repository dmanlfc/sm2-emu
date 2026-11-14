#version 450
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
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Vertexless fullscreen triangle.
//
// Three vertices covering the whole clip volume, generated from gl_VertexIndex
// so there is no vertex buffer to bind. Used by every fullscreen pass: the 2D
// tilemap layers, the 3D composite and post-processing.
//
// Draw with vkCmdDraw(cmd, 3, 1, 0, 0) and topology TRIANGLE_LIST.

layout(location = 0) out vec2 vTexCoord;

// gl_VertexIndex (Vulkan) and gl_VertexID (GL/GLES) name the same counter, but
// neither compiles under the other dialect. SM2_TARGET_GL is defined only for
// the GL/GLES compile (see cmake/EmbedShaders.cmake).
#ifdef SM2_TARGET_GL
#define SM2_VERTEX_INDEX gl_VertexID
#else
#define SM2_VERTEX_INDEX gl_VertexIndex
#endif

void main()
{
    // index 0 -> (-1,-1)  uv (0,0)
    // index 1 -> ( 3,-1)  uv (2,0)
    // index 2 -> (-1, 3)  uv (0,2)
    // The oversized triangle is clipped to the viewport, which is cheaper than a quad.
    vTexCoord = vec2((SM2_VERTEX_INDEX << 1) & 2, SM2_VERTEX_INDEX & 2);
    vec2 pos = vTexCoord * 2.0 - 1.0;
    // GL's clip space is +Y up with a bottom-left origin, the opposite of Vulkan.
    // Flipping only the clip-space Y keeps sampling correct and makes a pass write
    // its target in the same orientation as Vulkan.
#ifdef SM2_TARGET_GL
    pos.y = -pos.y;
#endif
    gl_Position = vec4(pos, 0.0, 1.0);
}
