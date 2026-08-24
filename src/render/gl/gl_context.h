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
// OpenGL 4.3 core context creation: the GL analogue of render::vk::Context,
// covering the same responsibility (own the platform handle, own the
// swap/present-interval knob, know the device's name and capability limits)
// against a GL context instead of a Vulkan device and swapchain.
#pragma once

#include "core/types.h"

#include <string>

struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace sm2::osd {
class Window;
}  // namespace sm2::osd

namespace sm2::render::gl {

struct ContextConfig {
    bool vsync   = true;
    bool es_mode = false;  ///< Request a GLES 3.1 context instead of GL 4.3 core.
};

/// Owns the GL context and the small amount of state that has nowhere else
/// to live: which extensions this driver actually has, and the device name
/// for diagnostics.
///
/// No VMA equivalent and no frames-in-flight ring, unlike
/// render::vk::Context: GL's driver manages buffer and texture lifetime
/// itself, and GL_ARB_buffer_storage's persistent mapping (see gl_common.h)
/// replaces the host-visible-buffer rotation Vulkan's lack of that guarantee
/// requires. Phase 8's own design.md already called the Vulkan backend's x3
/// duplication "waste" under GL before this backend existed to remove it.
class Context {
public:
    Context() = default;
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    /// Create a 4.3 core context on `window` (which must have been created
    /// with WindowConfig::graphics_api == osd::GraphicsApi::OpenGl) and
    /// resolve every GL function pointer this renderer calls.
    [[nodiscard]] bool init(osd::Window& window, const ContextConfig& config);
    void shutdown();

    [[nodiscard]] SDL_GLContext handle() const { return m_context; }

    /// GL_ARB_buffer_storage, checked once at init and cached: every
    /// PersistentBuffer creation call would otherwise re-walk the extension
    /// string list.
    [[nodiscard]] bool has_buffer_storage() const { return m_has_buffer_storage; }

    /// True when the context was created in GLES 3.1 mode rather than
    /// desktop GL 4.3 core.
    [[nodiscard]] bool is_es() const { return m_is_es; }

    [[nodiscard]] const std::string& device_name() const { return m_device_name; }

    /// Present the frame drawn since the last call, honouring the vsync
    /// setting init() was given.
    void swap();

private:
    SDL_Window*    m_window  = nullptr;
    SDL_GLContext  m_context = nullptr;
    bool           m_has_buffer_storage = false;
    bool           m_is_es             = false;
    std::string    m_device_name;
};

}  // namespace sm2::render::gl
