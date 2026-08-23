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
#pragma once

#include "core/types.h"

#include <string>

struct SDL_Window;

namespace sm2::osd {

/// Native display resolution of Model 2 hardware. The visible area is
/// 496x384 out of a 656x424 total, per MAME's screen timing.
inline constexpr u32 kModel2Width  = 496;
inline constexpr u32 kModel2Height = 384;

struct WindowConfig {
    std::string title       = "sm2-emu";
    u32         width       = kModel2Width * 2;
    u32         height      = kModel2Height * 2;
    bool        fullscreen  = false;
    bool        resizable   = true;
};

/// SDL3 window. Owns no Vulkan (or other graphics API) handle itself --
/// surface creation belongs to whichever render::Backend is active (see
/// render/backend.h), which is the only thing that knows when it is safe to
/// recreate a swapchain.
///
/// Not yet backend-neutral in one respect: create() still requests
/// SDL_WINDOW_VULKAN and loads the Vulkan loader library directly, both of
/// which a GL/GLES backend would need different calls for. That is real
/// remaining work for whichever phase adds a second backend, not something
/// the current public interface hides.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete;
    Window& operator=(Window&&)      = delete;

    [[nodiscard]] bool create(const WindowConfig& config);
    void destroy();

    /// Size of the drawable in pixels, which is not the window size under
    /// HiDPI. This is the value the swapchain extent must match.
    void drawable_size(u32* out_width, u32* out_height) const;

    /// Poll until the drawable size stops changing, up to a short timeout.
    ///
    /// A resize is asynchronous: the compositor may still be settling when the
    /// resize event arrives, so building a swapchain immediately can produce
    /// one that is instantly out of date.
    void settle_drawable_size(u32* out_width, u32* out_height);

    [[nodiscard]] bool minimised() const;

    void set_title(const std::string& title);
    void set_fullscreen(bool enable);
    [[nodiscard]] bool fullscreen() const { return m_fullscreen; }

    [[nodiscard]] SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window     = nullptr;
    bool        m_fullscreen = false;
    bool        m_owns_sdl   = false;
};

}  // namespace sm2::osd
