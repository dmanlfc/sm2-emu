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
// Owns the native-resolution frame and puts it on the screen: the GL
// counterpart of render::vk::PresentPass.
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

    /// `render_scale` (1..kMaxRenderScale) sizes the composite target, its
    /// stencil and begin_frame()'s viewport to N*native. Host uploads and
    /// screenshots stay native.
    [[nodiscard]] bool init(u32 render_scale, u32 width = kWidth);
    void shutdown();

    /// Size of the composite target, N*native.
    [[nodiscard]] u32 width() const { return m_native_width * m_render_scale; }
    [[nodiscard]] u32 height() const { return scaled_height(m_render_scale); }

    /// Claim this frame's native texture as the framebuffer other passes
    /// draw into. Binds m_fbo; the caller's own draw calls follow.
    void begin_frame();

    /// The texture begin_frame() bound, for reading back a capture.
    [[nodiscard]] u32 native_texture() const { return m_native_texture; }

    /// The framebuffer begin_frame() binds, for passes that bind it themselves.
    [[nodiscard]] u32 fbo() const { return m_fbo; }

    [[nodiscard]] u32 native_width() const { return m_native_width; }
    static constexpr u32 native_height() { return kHeight; }

    /// Replace the native frame with pixels rendered on the CPU, in place of
    /// the tilemap and 3D passes. `pixels` must hold kWidth * kHeight RGBA8
    /// texels. Call after begin_frame().
    void upload_from_host(std::span<const u32> pixels);

    /// Adopt new present-stage options (scaling method, aspect mode, CRT).
    /// Reallocates nothing.
    void set_options(const PresentOptions& options) { m_options = options; }

    /// Scale the finished frame onto the bound framebuffer, into the rectangle
    /// the aspect mode and scaling method select. Clears the whole target first
    /// so the letterbox bars are defined.
    void present(u32 window_width, u32 window_height);

private:
    [[nodiscard]] bool create_target();
    [[nodiscard]] bool create_program();

    u32 m_native_texture = 0;
    u32 m_fbo            = 0;

    /// Fill-mask stencil for the 3D draw, which happens directly into this
    /// framebuffer between the tilemap layers.
    u32 m_stencil_renderbuffer = 0;

    u32 m_program  = 0;
    u32 m_push_ubo = 0;
    u32 m_vao      = 0;

    /// Internal 3D render scale; the composite target and stencil are N*native.
    u32 m_render_scale = 1;
    u32 m_native_width = kWidth;

    /// Live present-stage options; present() reads these each frame.
    PresentOptions m_options;
};

}  // namespace sm2::render::gl
