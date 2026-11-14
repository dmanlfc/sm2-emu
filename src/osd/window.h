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
#pragma once

#include "core/types.h"

#include <string>

struct SDL_Window;

namespace sm2::osd {

/// Native display resolution of Model 2 hardware. The visible area is
/// 496x384 out of a 656x424 total, per MAME's screen timing.
inline constexpr u32 kModel2Width  = 496;
inline constexpr u32 kModel2Height = 384;

/// Which graphics API the window is created for, selecting the SDL window flag
/// and loader call.
enum class GraphicsApi {
    Vulkan,
    OpenGl,
};

struct WindowConfig {
    std::string title       = "Keisho";
    u32         width       = kModel2Width * 2;
    u32         height      = kModel2Height * 2;
    bool        fullscreen  = false;
    std::string fullscreen_resolution;  ///< "WxH" emulated mode, empty for the display's own
    bool        resizable   = true;
    GraphicsApi graphics_api = GraphicsApi::Vulkan;
};

/// SDL3 window. Owns no graphics-API handle: surface/context creation belongs
/// to the active render::Backend, which knows when it is safe to recreate a
/// swapchain or GL context.
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

    /// Poll until the drawable size stops changing, up to a short timeout. The
    /// compositor may still be settling when the resize event arrives.
    void settle_drawable_size(u32* out_width, u32* out_height);

    [[nodiscard]] bool minimised() const;

    void set_title(const std::string& title);
    void set_fullscreen(bool enable);
    [[nodiscard]] bool fullscreen() const { return m_fullscreen; }

    [[nodiscard]] SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window      = nullptr;
    bool        m_fullscreen  = false;
    std::string m_fullscreen_resolution;
    void        apply_fullscreen_resolution();
    bool        m_owns_sdl    = false;
    GraphicsApi m_graphics_api = GraphicsApi::Vulkan;
};

}  // namespace sm2::osd
