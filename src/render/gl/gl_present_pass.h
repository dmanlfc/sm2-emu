//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Owns the native-resolution frame and puts it on the screen. GL analogue of
// render::vk::PresentPass -- see that class's own doc comment for why
// compositing happens before magnifying rather than after.
#pragma once

#include "render/gl/gl_common.h"
#include "render/geometry.h"

#include <span>

namespace sm2::render::gl {

class PresentPass {
public:
    static constexpr u32 kWidth  = render::kNativeWidth;
    static constexpr u32 kHeight = render::kNativeHeight;

    PresentPass() = default;
    ~PresentPass();

    PresentPass(const PresentPass&)            = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    /// `render_scale` (1..kMaxRenderScale) sizes the composite target and its
    /// fill-mask stencil to N*native, and the viewport begin_frame() sets. The
    /// native frame the software renderer uploads and a screenshot reads back
    /// stay native, so N=1 is unchanged.
    [[nodiscard]] bool init(u32 render_scale);
    void shutdown();

    /// Size of the composite target, N*native.
    [[nodiscard]] u32 width() const { return scaled_width(m_render_scale); }
    [[nodiscard]] u32 height() const { return scaled_height(m_render_scale); }

    /// Claim this frame's native texture as the framebuffer other passes
    /// draw into. Binds m_fbo; the caller's own draw calls follow.
    void begin_frame();

    /// The texture begin_frame() bound, for reading back a capture.
    [[nodiscard]] u32 native_texture() const { return m_native_texture; }

    static constexpr u32 native_width()  { return kWidth; }
    static constexpr u32 native_height() { return kHeight; }

    /// Replace the native frame with pixels rendered on the CPU, in place of
    /// the tilemap and 3D passes. `pixels` must hold kWidth * kHeight RGBA8
    /// texels. Call after begin_frame().
    void upload_from_host(std::span<const u32> pixels);

    /// Adopt new present-stage options (scaling method, aspect mode, CRT).
    /// Pure present state, so this reallocates nothing.
    void set_options(const PresentOptions& options) { m_options = options; }

    /// Scale the finished native frame onto the currently bound framebuffer
    /// (the window, via framebuffer 0), into the rectangle the current aspect
    /// mode and scaling method select within `window_width` by
    /// `window_height`. Clears the whole target first so the letterbox bars
    /// are defined.
    void present(u32 window_width, u32 window_height);

private:
    [[nodiscard]] bool create_target();
    [[nodiscard]] bool create_program();

    u32 m_native_texture = 0;
    u32 m_fbo            = 0;

    /// Fill-mask stencil for the 3D draw, which now happens directly into this
    /// native framebuffer between the below and above tilemap layers.
    u32 m_stencil_renderbuffer = 0;

    u32 m_program  = 0;
    u32 m_push_ubo = 0;
    u32 m_vao      = 0;

    /// Internal 3D render scale; the composite target and stencil are N*native.
    u32 m_render_scale = 1;

    /// Live present-stage options; present() reads these each frame.
    PresentOptions m_options;
};

}  // namespace sm2::render::gl
