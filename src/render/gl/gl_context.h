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
// OpenGL 4.3 core (4.1 on macOS) context creation: the GL analogue of
// render::vk::Context.
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
    bool es_mode = false;  ///< Request a GLES 3.1 context instead of GL 4.3 (or 4.1) core.
};

/// Owns the GL context, the driver's extension flags and the device name.
/// Unlike render::vk::Context there is no allocator or frames-in-flight ring:
/// the driver manages lifetimes, and persistent mapping (gl_common.h) replaces
/// Vulkan's buffer rotation.
class Context {
public:
    Context() = default;
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    /// Create a 4.3 core context (else 4.1 core) on `window` (which must have been created
    /// with WindowConfig::graphics_api == osd::GraphicsApi::OpenGl) and
    /// resolve every GL function pointer this renderer calls.
    [[nodiscard]] bool init(osd::Window& window, const ContextConfig& config);
    void shutdown();

    [[nodiscard]] SDL_GLContext handle() const { return m_context; }

    /// GL_ARB_buffer_storage (or EXT on GLES), checked once at init.
    [[nodiscard]] bool has_buffer_storage() const { return m_has_buffer_storage; }

    /// True when the context was created in GLES 3.1 mode rather than
    /// desktop GL 4.3 core.
    [[nodiscard]] bool is_es() const { return m_is_es; }

    /// Compute shaders, storage buffers and image store: GLES 3.1 and GL 4.3.
    /// False on a GL 4.1 context (macOS).
    [[nodiscard]] bool has_compute() const { return m_has_compute; }

    [[nodiscard]] const std::string& device_name() const { return m_device_name; }

    /// Present the frame drawn since the last call, honouring the vsync
    /// setting init() was given.
    void swap();

private:
    SDL_Window*    m_window  = nullptr;
    SDL_GLContext  m_context = nullptr;
    bool           m_has_buffer_storage = false;
    bool           m_is_es             = false;
    bool           m_has_compute       = true;
    std::string    m_device_name;
};

}  // namespace sm2::render::gl
