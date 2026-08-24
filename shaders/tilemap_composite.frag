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
// Composites one Model 2 tilemap result over what is already in the target.
//
// The emulator rasterises the System 24 layers on the CPU into two RGBA8
// surfaces: the layers whose priority category is zero, which belong behind the
// 3D output, and the layers whose category is one, which belong in front of it.
// This shader draws either of them.
//
// Colours arrive premultiplied by alpha: a pixel nothing wrote is all zeroes, a
// drawn pixel is its colour with alpha one. That matters because the surface is
// magnified with linear filtering, and filtering a straight-alpha edge pulls the
// colour towards black and leaves a dark fringe around every glyph.

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 fragColour;

layout(set = 0, binding = 0) uniform sampler2D uLayer;

// See polygon.vert's SM2_TARGET_GL comment: GL/GLES have no push-constant
// concept, so the GL compile takes a small uniform block instead, at a
// binding this shader has room for (0 is uLayer); the Vulkan compile keeps
// the push constant it already had, unchanged.
#ifdef SM2_TARGET_GL
layout(binding = 1) uniform Push {
#else
layout(push_constant) uniform Push {
#endif
    // Palette entry zero. The hardware shows this wherever nothing is drawn.
    vec4 background;
    // 0: resolve against the background, replacing the target.
    // 1: blend over the target, which by then holds the 3D output.
    // 2: copy, for putting the finished native frame on screen.
    uint mode;
} pc;

void main()
{
    vec4 layer = texture(uLayer, vTexCoord);

    if (pc.mode == 0u) {
        fragColour = vec4(pc.background.rgb * (1.0 - layer.a) + layer.rgb, 1.0);
    } else if (pc.mode == 1u) {
        fragColour = layer;
    } else {
        // The source is already a finished opaque frame. Forcing alpha to one
        // rather than passing it through means a stray zero in the alpha channel
        // cannot make the presented image transparent on a compositing window
        // system.
        fragColour = vec4(layer.rgb, 1.0);
    }
}
