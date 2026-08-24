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

    [[nodiscard]] bool init();
    void shutdown();

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

    /// Scale the finished native frame onto the currently bound framebuffer
    /// (the window, via framebuffer 0), letterboxed to 4:3 within
    /// `window_width` by `window_height`. Clears the whole target first so
    /// the letterbox bars are defined.
    void present(u32 window_width, u32 window_height);

private:
    [[nodiscard]] bool create_target();
    [[nodiscard]] bool create_program();

    u32 m_native_texture = 0;
    u32 m_fbo            = 0;

    u32 m_program  = 0;
    u32 m_push_ubo = 0;
    u32 m_vao      = 0;
};

}  // namespace sm2::render::gl
