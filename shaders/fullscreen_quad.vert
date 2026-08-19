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

void main()
{
    // index 0 -> (-1,-1)  uv (0,0)
    // index 1 -> ( 3,-1)  uv (2,0)
    // index 2 -> (-1, 3)  uv (0,2)
    //
    // The oversized triangle is clipped to the viewport by fixed function,
    // which is cheaper than the four vertices and two triangles of a quad.
    vTexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vTexCoord * 2.0 - 1.0, 0.0, 1.0);
}
