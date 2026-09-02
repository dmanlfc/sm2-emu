#version 450
//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Vertexless fullscreen triangle.
//
// Three vertices covering the whole clip volume, generated from gl_VertexIndex
// so there is no vertex buffer to bind. Used by every fullscreen pass: the 2D
// tilemap layers, the 3D composite and post-processing.
//
// Draw with vkCmdDraw(cmd, 3, 1, 0, 0) and topology TRIANGLE_LIST.

layout(location = 0) out vec2 vTexCoord;

// GL-only: +1 when glClipControl aligned GL's clip space with Vulkan's
// (no flip needed), -1 otherwise (GLES, or desktop without
// GL_ARB_clip_control). Vulkan never sees this uniform.
// gl_VertexIndex (Vulkan GLSL) and gl_VertexID (GL/GLES core) name the same
// vertex counter but are not interchangeable identifiers -- neither compiles
// under the other dialect. SM2_TARGET_GL is defined only for the GL/GLES
// compile of this shared source (see cmake/EmbedShaders.cmake's lint/embed
// steps), so this is the one line in this shader set that genuinely needs a
// dialect branch.
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
    //
    // The oversized triangle is clipped to the viewport by fixed function,
    // which is cheaper than the four vertices and two triangles of a quad.
    vTexCoord = vec2((SM2_VERTEX_INDEX << 1) & 2, SM2_VERTEX_INDEX & 2);
    vec2 pos = vTexCoord * 2.0 - 1.0;
    // OpenGL's clip space has +Y up and its framebuffer origin is bottom-left,
    // the opposite of Vulkan. The sampled textures are top-left origin, so
    // leaving vTexCoord alone keeps sampling correct while flipping only the
    // emitted clip-space Y makes a pass write into its target with the same
    // orientation Vulkan's positive-height viewport produces.
#ifdef SM2_TARGET_GL
    pos.y = -pos.y;
#endif
    gl_Position = vec4(pos, 0.0, 1.0);
}
