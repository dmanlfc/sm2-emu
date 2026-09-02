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
#include "render/gl/gl_context.h"

#include "core/log.h"
#include "osd/window.h"
#include "render/gl/gl_common.h"

#include <SDL3/SDL.h>

#include <string>

namespace sm2::render::gl {
namespace {

/// GLES's mandatory minimums for these can be lower than the version number
/// implies, so they are checked explicitly rather than assumed from the 3.1
/// floor check alone.
struct GlesLimit {
    GLenum      query;
    s32         required;
    const char* name;
};

/// This renderer's own requirements, not the spec's bare minimum: one colour
/// attachment (every FBO here has exactly one), texture size >= 1024
/// (kDecodedHeight), four vertex attributes (render::Vertex's field count).
constexpr GlesLimit kGlesLimits[] = {
    {GL_MAX_TEXTURE_SIZE, 1024, "GL_MAX_TEXTURE_SIZE"},
    {GL_MAX_VERTEX_ATTRIBS, 4, "GL_MAX_VERTEX_ATTRIBS"},
    {GL_MAX_COLOR_ATTACHMENTS, 1, "GL_MAX_COLOR_ATTACHMENTS"},
    {GL_MAX_DRAW_BUFFERS, 1, "GL_MAX_DRAW_BUFFERS"},
};

[[nodiscard]] bool validate_gles_minimums()
{
    bool ok = true;
    for (const GlesLimit& limit : kGlesLimits) {
        s32 value = 0;
        GetIntegerv(limit.query, &value);
        if (value < limit.required) {
            SM2_ERROR("gles: %s is %d, below this renderer's required minimum of %d",
                      limit.name, value, limit.required);
            ok = false;
        }
    }
    return ok;
}

}  // namespace

Context::~Context()
{
    shutdown();
}

bool Context::init(osd::Window& window, const ContextConfig& config)
{
    m_window = window.handle();
    m_is_es  = config.es_mode;

    // Attributes must be set before context creation; SDL applies them to
    // the context SDL_GL_CreateContext produces next, not retroactively.
    if (config.es_mode) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
    // No depth buffer: this renderer's fill mask is stencil-only, matching
    // the Vulkan path's own choice (Poly3DPass's stencil attachment). Model
    // 2's hardware itself has no depth buffer either -- see backend.h's own
    // documentation of why draw order, not depth, decides pixel ownership.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    m_context = SDL_GL_CreateContext(m_window);
    if (m_context == nullptr) {
        SM2_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_GL_MakeCurrent(m_window, m_context)) {
        SM2_ERROR("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        SDL_GL_DestroyContext(m_context);
        m_context = nullptr;
        return false;
    }

    // Function pointers can only be resolved once a context is current --
    // see load_gl_functions()'s own documentation of why this ordering
    // matters, not just as a general precaution.
    std::string load_error;
    if (!load_gl_functions(SDL_GL_GetProcAddress, &load_error)) {
        SM2_ERROR("gl: %s", load_error.c_str());
        SDL_GL_DestroyContext(m_context);
        m_context = nullptr;
        return false;
    }
    resolve_buffer_storage(SDL_GL_GetProcAddress, config.es_mode);

    // The version actually granted, not the version requested: a driver can
    // silently hand back a higher (never lower) context than asked for, and
    // this is what this backend's declared floor actually depends on having.
    s32 major = 0;
    s32 minor = 0;
    GetIntegerv(GL_MAJOR_VERSION, &major);
    GetIntegerv(GL_MINOR_VERSION, &minor);

    if (config.es_mode) {
        const bool meets_floor = (major > 3) || (major == 3 && minor >= 1);
        if (!meets_floor) {
            SM2_ERROR("gles: context is ES %d.%d, but this backend requires "
                      "ES 3.1 or newer (compute shaders and storage buffers "
                      "are not available below that line)",
                      major, minor);
            SDL_GL_DestroyContext(m_context);
            m_context = nullptr;
            return false;
        }
        if (!validate_gles_minimums()) {
            SDL_GL_DestroyContext(m_context);
            m_context = nullptr;
            return false;
        }
        m_has_buffer_storage = has_gl_extension("GL_EXT_buffer_storage");
        m_device_name = "OpenGL ES " + std::to_string(major) + "." + std::to_string(minor);

        SM2_INFO("gles: context ES %d.%d, GL_EXT_buffer_storage %s", major, minor,
                 m_has_buffer_storage ? "present" : "absent (falling back to BufferSubData)");
    } else {
        s32 profile_mask = 0;
        GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile_mask);

        const bool is_core = (profile_mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
        const bool meets_floor = (major > 4) || (major == 4 && minor >= 3);
        if (!meets_floor || !is_core) {
            SM2_ERROR("gl: context is %d.%d %s, but this backend requires 4.3 core "
                      "or newer (compute shaders and storage buffers are not "
                      "available below that line)",
                      major, minor, is_core ? "core" : "not core");
            SDL_GL_DestroyContext(m_context);
            m_context = nullptr;
            return false;
        }
        m_has_buffer_storage = has_gl_extension("GL_ARB_buffer_storage");
        m_device_name = "OpenGL " + std::to_string(major) + "." + std::to_string(minor)
                      + " core";

        SM2_INFO("gl: context %d.%d core, GL_ARB_buffer_storage %s", major, minor,
                 m_has_buffer_storage ? "present" : "absent (falling back to BufferSubData)");
    }

    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);
    return true;
}

void Context::shutdown()
{
    if (m_context != nullptr) {
        SDL_GL_DestroyContext(m_context);
        m_context = nullptr;
    }
    m_window = nullptr;
}

void Context::swap()
{
    SDL_GL_SwapWindow(m_window);
}

}  // namespace sm2::render::gl
