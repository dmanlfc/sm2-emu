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
#include "osd/window.h"

#include "core/log.h"
#include "osd/app_icon.h"

#include <cstdio>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>

namespace sm2::osd {

Window::~Window()
{
    destroy();
}

bool Window::create(const WindowConfig& config)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SM2_ERROR("SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError());
        return false;
    }
    m_owns_sdl     = true;
    m_graphics_api = config.graphics_api;

    // No SDL_WINDOW_HIGH_PIXEL_DENSITY: on fractional-scale Wayland it makes SDL
    // report a size that does not match the true surface. The game is
    // letterbox-scaled to the window regardless, so it buys nothing here.
    SDL_WindowFlags flags = 0;

    if (m_graphics_api == GraphicsApi::Vulkan) {
        // Loading the Vulkan library up front means a missing or broken loader
        // is reported here, with SDL's diagnostics, rather than as a confusing
        // instance-creation failure later.
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            SM2_ERROR("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
            SM2_ERROR("A Vulkan loader is required. On Linux install "
                      "libvulkan1/vulkan-loader; on macOS install the Vulkan SDK "
                      "and source its setup-env.sh so MoltenVK is discoverable.");
            return false;
        }
        flags |= SDL_WINDOW_VULKAN;
    } else {
        // Same reasoning as the Vulkan branch, for the same failure mode: a
        // missing GL library is reported here rather than as an opaque
        // SDL_CreateWindow failure.
        if (!SDL_GL_LoadLibrary(nullptr)) {
            SM2_ERROR("SDL_GL_LoadLibrary failed: %s", SDL_GetError());
            SM2_ERROR("An OpenGL loader is required. On Linux this is normally "
                      "provided by the GPU driver package (mesa or the vendor's "
                      "own driver).");
            return false;
        }
        flags |= SDL_WINDOW_OPENGL;
    }

    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    m_window = SDL_CreateWindow(config.title.c_str(),
                                static_cast<int>(config.width),
                                static_cast<int>(config.height),
                                flags);
    if (m_window == nullptr) {
        SM2_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    m_fullscreen            = config.fullscreen;
    m_fullscreen_resolution = config.fullscreen_resolution;
    if (m_fullscreen) {
        apply_fullscreen_resolution();
    }

    // Application icon. The pixels are a static constexpr array, so the surface
    // may reference them without copying; SDL_SetWindowIcon takes its own copy,
    // so the surface is freed immediately afterwards. A failure here is not
    // fatal -- the window just keeps the default icon.
    if (SDL_Surface* icon = SDL_CreateSurfaceFrom(
            kAppIconWidth, kAppIconHeight, SDL_PIXELFORMAT_RGBA32,
            const_cast<unsigned char*>(kAppIconRgba), kAppIconWidth * 4)) {
        if (!SDL_SetWindowIcon(m_window, icon)) {
            SM2_WARN("SDL_SetWindowIcon failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(icon);
    } else {
        SM2_WARN("could not create icon surface: %s", SDL_GetError());
    }

    u32 pixel_width = 0;
    u32 pixel_height = 0;
    drawable_size(&pixel_width, &pixel_height);
    SM2_INFO("window %ux%u logical, %ux%u pixels (%s)",
             config.width, config.height, pixel_width, pixel_height,
             SDL_GetCurrentVideoDriver());

    return true;
}

void Window::destroy()
{
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_owns_sdl) {
        if (m_graphics_api == GraphicsApi::Vulkan) {
            SDL_Vulkan_UnloadLibrary();
        } else {
            SDL_GL_UnloadLibrary();
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        m_owns_sdl = false;
    }
}

void Window::drawable_size(u32* out_width, u32* out_height) const
{
    int width = 0;
    int height = 0;
    if (m_window != nullptr) {
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
    }
    *out_width  = static_cast<u32>(std::max(width, 0));
    *out_height = static_cast<u32>(std::max(height, 0));
}

void Window::settle_drawable_size(u32* out_width, u32* out_height)
{
    constexpr int kMaxAttempts = 32;

    u32 width = 0;
    u32 height = 0;
    drawable_size(&width, &height);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        SDL_PumpEvents();

        u32 next_width = 0;
        u32 next_height = 0;
        drawable_size(&next_width, &next_height);

        if (next_width == width && next_height == height) {
            break;
        }
        width  = next_width;
        height = next_height;
        SDL_Delay(1);
    }

    *out_width  = width;
    *out_height = height;
}

bool Window::minimised() const
{
    if (m_window == nullptr) {
        return true;
    }
    if ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0) {
        return true;
    }

    // A zero-sized drawable is equivalent for our purposes: there is nothing
    // to present to and a swapchain cannot be created.
    u32 width = 0;
    u32 height = 0;
    drawable_size(&width, &height);
    return width == 0 || height == 0;
}

void Window::set_title(const std::string& title)
{
    if (m_window != nullptr) {
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

void Window::apply_fullscreen_resolution()
{
    int w = 0;
    int h = 0;
    if (m_fullscreen_resolution.empty()
        || std::sscanf(m_fullscreen_resolution.c_str(), "%dx%d", &w, &h) != 2) {
        return;
    }
    int               count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(m_window), &count);
    bool              found = false;
    for (int i = 0; i < count && !found; i++) {
        if (modes[i]->w == w && modes[i]->h == h) {
            found = SDL_SetWindowFullscreenMode(m_window, modes[i]);
        }
    }
    SDL_free(modes);
    if (found) {
        SM2_INFO("fullscreen at %dx%d", w, h);
    } else {
        SM2_WARN("fullscreen_resolution %s is not a mode this display offers; using its own", m_fullscreen_resolution.c_str());
    }
}

void Window::set_fullscreen(bool enable)
{
    if (m_window == nullptr || enable == m_fullscreen) {
        return;
    }
    if (enable) {
        apply_fullscreen_resolution();
    }
    if (!SDL_SetWindowFullscreen(m_window, enable)) {
        SM2_WARN("SDL_SetWindowFullscreen failed: %s", SDL_GetError());
        return;
    }
    m_fullscreen = enable;
}

}  // namespace sm2::osd
