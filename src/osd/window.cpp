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
#include "osd/window.h"

#include "core/log.h"

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
    m_owns_sdl = true;

    // Loading the Vulkan library up front means a missing or broken loader is
    // reported here, with SDL's diagnostics, rather than as a confusing
    // instance-creation failure later.
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        SM2_ERROR("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        SM2_ERROR("A Vulkan loader is required. On Linux install "
                  "libvulkan1/vulkan-loader; on macOS install the Vulkan SDK "
                  "and source its setup-env.sh so MoltenVK is discoverable.");
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
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

    m_fullscreen = config.fullscreen;

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
        SDL_Vulkan_UnloadLibrary();
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

void Window::set_fullscreen(bool enable)
{
    if (m_window == nullptr || enable == m_fullscreen) {
        return;
    }
    if (!SDL_SetWindowFullscreen(m_window, enable)) {
        SM2_WARN("SDL_SetWindowFullscreen failed: %s", SDL_GetError());
        return;
    }
    m_fullscreen = enable;
}

}  // namespace sm2::osd
