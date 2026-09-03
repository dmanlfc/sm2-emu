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
#include "render/gl/gl_common.h"

#include "core/log.h"

#include <cstring>
#include <vector>

namespace sm2::render::gl {

// One X-macro list drives both the declarations in gl_common.h and the
// definitions + loader here, so the two cannot drift out of sync with each
// other. See gl_common.h's own top comment for why every entry is named
// without its "gl" prefix.
// clang-format off
#define SM2_GL_FUNCTION_LIST(F) \
    F(GenBuffers, PFNGLGENBUFFERSPROC) \
    F(DeleteBuffers, PFNGLDELETEBUFFERSPROC) \
    F(BindBuffer, PFNGLBINDBUFFERPROC) \
    F(BindBufferBase, PFNGLBINDBUFFERBASEPROC) \
    F(BufferData, PFNGLBUFFERDATAPROC) \
    F(BufferSubData, PFNGLBUFFERSUBDATAPROC) \
    F(MapBufferRange, PFNGLMAPBUFFERRANGEPROC) \
    F(UnmapBuffer, PFNGLUNMAPBUFFERPROC) \
    F(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
    F(DeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC) \
    F(BindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
    F(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \
    F(VertexAttribIPointer, PFNGLVERTEXATTRIBIPOINTERPROC) \
    F(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    F(GenTextures, SM2_PFNGLGENTEXTURESPROC) \
    F(DeleteTextures, SM2_PFNGLDELETETEXTURESPROC) \
    F(BindTexture, SM2_PFNGLBINDTEXTUREPROC) \
    F(ActiveTexture, PFNGLACTIVETEXTUREPROC) \
    F(TexStorage2D, PFNGLTEXSTORAGE2DPROC) \
    F(TexStorage3D, PFNGLTEXSTORAGE3DPROC) \
    F(TexSubImage2D, SM2_PFNGLTEXSUBIMAGE2DPROC) \
    F(TexParameteri, SM2_PFNGLTEXPARAMETERIPROC) \
    F(BindImageTexture, PFNGLBINDIMAGETEXTUREPROC) \
    F(GenSamplers, PFNGLGENSAMPLERSPROC) \
    F(DeleteSamplers, PFNGLDELETESAMPLERSPROC) \
    F(BindSampler, PFNGLBINDSAMPLERPROC) \
    F(SamplerParameteri, PFNGLSAMPLERPARAMETERIPROC) \
    F(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
    F(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
    F(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
    F(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
    F(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
    F(DrawBuffers, PFNGLDRAWBUFFERSPROC) \
    F(GenRenderbuffers, PFNGLGENRENDERBUFFERSPROC) \
    F(DeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC) \
    F(BindRenderbuffer, PFNGLBINDRENDERBUFFERPROC) \
    F(RenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC) \
    F(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC) \
    F(CreateShader, PFNGLCREATESHADERPROC) \
    F(DeleteShader, PFNGLDELETESHADERPROC) \
    F(ShaderSource, PFNGLSHADERSOURCEPROC) \
    F(CompileShader, PFNGLCOMPILESHADERPROC) \
    F(GetShaderiv, PFNGLGETSHADERIVPROC) \
    F(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC) \
    F(CreateProgram, PFNGLCREATEPROGRAMPROC) \
    F(DeleteProgram, PFNGLDELETEPROGRAMPROC) \
    F(AttachShader, PFNGLATTACHSHADERPROC) \
    F(LinkProgram, PFNGLLINKPROGRAMPROC) \
    F(GetProgramiv, PFNGLGETPROGRAMIVPROC) \
    F(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC) \
    F(UseProgram, PFNGLUSEPROGRAMPROC) \
    F(UniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC) \
    F(GetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC) \
    F(DispatchCompute, PFNGLDISPATCHCOMPUTEPROC) \
    F(MemoryBarrier, PFNGLMEMORYBARRIERPROC) \
    F(DrawArrays, SM2_PFNGLDRAWARRAYSPROC) \
    F(Enable, SM2_PFNGLENABLEPROC) \
    F(Disable, SM2_PFNGLDISABLEPROC) \
    F(StencilFunc, SM2_PFNGLSTENCILFUNCPROC) \
    F(StencilOp, SM2_PFNGLSTENCILOPPROC) \
    F(StencilMask, SM2_PFNGLSTENCILMASKPROC) \
    F(Clear, SM2_PFNGLCLEARPROC) \
    F(ClearColor, SM2_PFNGLCLEARCOLORPROC) \
    F(ClearStencil, SM2_PFNGLCLEARSTENCILPROC) \
    F(Viewport, SM2_PFNGLVIEWPORTPROC) \
    F(Scissor, SM2_PFNGLSCISSORPROC) \
    F(ColorMask, SM2_PFNGLCOLORMASKPROC) \
    F(BlendFunc, SM2_PFNGLBLENDFUNCPROC) \
    F(ReadPixels, SM2_PFNGLREADPIXELSPROC) \
    F(ReadBuffer, SM2_PFNGLREADBUFFERPROC) \
    F(GetIntegerv, SM2_PFNGLGETINTEGERVPROC) \
    F(GetStringi, PFNGLGETSTRINGIPROC) \
    F(GetError, SM2_PFNGLGETERRORPROC) \
    F(Finish, SM2_PFNGLFINISHPROC)
// clang-format on

#define SM2_GL_DEFINE(name, type) type name = nullptr;
SM2_GL_FUNCTION_LIST(SM2_GL_DEFINE)
#undef SM2_GL_DEFINE

PFNGLBUFFERSTORAGEPROC BufferStorage = nullptr;

namespace {
bool g_loaded = false;
const char* g_version_directive = sm2::render::gl::kDesktopVersionDirective;
}  // namespace

bool load_gl_functions(SDL_FunctionPointer (*get_proc)(const char*), std::string* out_error)
{
#define SM2_GL_RESOLVE(name, type)                                       \
    name = reinterpret_cast<type>(get_proc("gl" #name));                 \
    if (name == nullptr) {                                               \
        if (out_error != nullptr) {                                     \
            *out_error = "could not resolve GL function 'gl" #name "'"; \
        }                                                                \
        return false;                                                    \
    }
    SM2_GL_FUNCTION_LIST(SM2_GL_RESOLVE)
#undef SM2_GL_RESOLVE

    g_loaded = true;
    return true;
}

bool gl_functions_loaded()
{
    return g_loaded;
}

void resolve_buffer_storage(SDL_FunctionPointer (*get_proc)(const char*), bool is_es)
{
    // GLES exposes this only as glBufferStorageEXT, never under the
    // unsuffixed desktop name. A null result is not an error here --
    // create_persistent_buffer() falls back to BufferSubData.
    BufferStorage = reinterpret_cast<PFNGLBUFFERSTORAGEPROC>(
        get_proc(is_es ? "glBufferStorageEXT" : "glBufferStorage"));
}

const char* active_version_directive()
{
    return g_version_directive;
}

void set_version_directive(const char* directive)
{
    g_version_directive = directive;
}

std::string prepare_gl_source(const char* embedded_source, const char* version_directive,
                              const char* extra_define)
{
    std::string source(embedded_source);
    const usize version_pos = source.find("#version");
    if (version_pos != std::string::npos) {
        const usize line_end = source.find('\n', version_pos);
        source.erase(version_pos,
                     line_end == std::string::npos ? std::string::npos
                                                    : line_end - version_pos + 1);
    }

    // Strip Vulkan-only "set = N, " from layout qualifiers. OpenGL has no
    // descriptor sets -- it uses a flat binding-point namespace -- so the
    // set qualifier is illegal in desktop GLSL and must be removed. The
    // shaders are authored for Vulkan (where set is required) and shared
    // with the GL backend via this mechanical rewrite.
    //
    // The match requires that "set" is not preceded by an alphanumeric or
    // underscore character, to avoid false positives like "offset = ...".
    {
        const std::string pattern = "set = ";
        usize pos = 0;
        while ((pos = source.find(pattern, pos)) != std::string::npos) {
            // Only match if "set" is at the start or preceded by a non-identifier
            // character (comma, open-paren, space, etc.).
            if (pos > 0) {
                const char prev = source[pos - 1];
                if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z')
                    || (prev >= '0' && prev <= '9') || prev == '_') {
                    pos += pattern.size();
                    continue;
                }
            }

            // Find the end of "set = N, " -- skip digits after "set = ", then
            // consume the trailing ", " (comma + optional space).
            usize end = pos + pattern.size();
            while (end < source.size() && source[end] >= '0' && source[end] <= '9') {
                ++end;
            }
            // Consume ", " that follows the digit(s).
            if (end < source.size() && source[end] == ',') {
                ++end;
                if (end < source.size() && source[end] == ' ') {
                    ++end;
                }
            }
            source.erase(pos, end - pos);
        }
    }

    std::string header = std::string(version_directive) + "\n#define SM2_TARGET_GL 1\n";
    if (extra_define != nullptr) {
        header += "#define " + std::string(extra_define) + " 1\n";
    }
    // GLES requires explicit precision qualifiers. Inject default precision
    // declarations immediately after the version/define lines so every shader
    // compiles without adding per-variable qualifiers throughout. highp float
    // matches desktop GL's implicit precision and avoids visible artefacts in
    // the tone-curve and filtering arithmetic.
    if (std::string(version_directive).find(" es") != std::string::npos) {
        header += "precision highp float;\n"
                  "precision highp int;\n"
                  "precision highp sampler2D;\n"
                  "precision highp sampler2DArray;\n"
                  "precision highp usampler2DArray;\n"
                  "precision highp image2DArray;\n"
                  "precision highp uimage2DArray;\n";

        // ESSL treats `const` on a local variable as "compile-time constant
        // expression required" rather than the desktop-GL meaning of "immutable
        // after initialisation". Strip `const ` from lines inside function
        // bodies (identified by leading whitespace) so runtime-initialised
        // locals compile. Global-scope const (no leading whitespace) is left
        // alone -- those are literal constants the ESSL compiler accepts.
        {
            const std::string pat = "const ";
            usize pos = 0;
            while ((pos = source.find(pat, pos)) != std::string::npos) {
                // Check if this is inside a function body: preceded by a
                // newline then at least one space/tab (indented line).
                bool indented = false;
                if (pos > 0) {
                    usize line_start = source.rfind('\n', pos - 1);
                    if (line_start == std::string::npos) {
                        line_start = 0;
                    } else {
                        ++line_start;
                    }
                    if (line_start < pos
                        && (source[line_start] == ' ' || source[line_start] == '\t')) {
                        indented = true;
                    }
                }
                if (indented) {
                    source.erase(pos, pat.size());
                } else {
                    pos += pat.size();
                }
            }
        }
    }
    return header + source;
}

bool has_gl_extension(const char* name)
{
    int count = 0;
    GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (int i = 0; i < count; ++i) {
        const auto* extension =
            reinterpret_cast<const char*>(GetStringi(GL_EXTENSIONS, static_cast<u32>(i)));
        if (extension != nullptr && std::strcmp(extension, name) == 0) {
            return true;
        }
    }
    return false;
}

namespace {

/// Compile one shader stage, logging its info log on failure. Returns 0 on
/// failure, matching compile_program()'s own convention.
[[nodiscard]] u32 compile_stage(u32 stage_type, const char* source)
{
    const u32 shader = CreateShader(stage_type);
    ShaderSource(shader, 1, &source, nullptr);
    CompileShader(shader);

    s32 ok = 0;
    GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        s32 log_length = 0;
        GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> log(static_cast<usize>(log_length) + 1, '\0');
        GetShaderInfoLog(shader, log_length, nullptr, log.data());
        SM2_ERROR("gl: shader compile failed: %s", log.data());
        DeleteShader(shader);
        return 0;
    }
    return shader;
}

[[nodiscard]] u32 link_program(std::initializer_list<u32> shaders)
{
    const u32 program = CreateProgram();
    for (u32 shader : shaders) {
        AttachShader(program, shader);
    }
    LinkProgram(program);

    s32 ok = 0;
    GetProgramiv(program, GL_LINK_STATUS, &ok);
    for (u32 shader : shaders) {
        DeleteShader(shader);
    }
    if (ok == 0) {
        s32 log_length = 0;
        GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> log(static_cast<usize>(log_length) + 1, '\0');
        GetProgramInfoLog(program, log_length, nullptr, log.data());
        SM2_ERROR("gl: program link failed: %s", log.data());
        DeleteProgram(program);
        return 0;
    }
    return program;
}

}  // namespace

u32 compile_program(const char* vertex_source, const char* fragment_source)
{
    const u32 vertex = compile_stage(GL_VERTEX_SHADER, vertex_source);
    if (vertex == 0) {
        return 0;
    }
    const u32 fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source);
    if (fragment == 0) {
        DeleteShader(vertex);
        return 0;
    }
    return link_program({vertex, fragment});
}

u32 compile_compute_program(const char* compute_source)
{
    const u32 compute = compile_stage(GL_COMPUTE_SHADER, compute_source);
    if (compute == 0) {
        return 0;
    }
    return link_program({compute});
}

void PersistentBuffer::write(const void* data, usize bytes) const
{
    if (mapped != nullptr) {
        std::memcpy(mapped, data, bytes);
    } else {
        BindBuffer(target, handle);
        BufferSubData(target, 0, static_cast<GLsizeiptr>(bytes), data);
    }
}

PersistentBuffer create_persistent_buffer(usize bytes, u32 target)
{
    PersistentBuffer buffer;
    buffer.size   = bytes;
    buffer.target = target;

    GenBuffers(1, &buffer.handle);
    BindBuffer(target, buffer.handle);

    // GL_ARB_buffer_storage (core since 4.4) gives a persistently-mapped
    // buffer, matching the Vulkan path's own host-coherent, always-mapped
    // resources. Below that -- a 4.3-only driver -- BufferData plus
    // BufferSubData per write is the correct fallback, not a gap: this
    // project's floor is 4.3, and 4.3-only hardware without this extension
    // is expected to exist even if none was available to test against.
    if (has_gl_extension("GL_ARB_buffer_storage")) {
        constexpr GLbitfield kStorageFlags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        BufferStorage(target, static_cast<GLsizeiptr>(bytes), nullptr, kStorageFlags);
        buffer.mapped =
            MapBufferRange(target, 0, static_cast<GLsizeiptr>(bytes), kStorageFlags);
        if (buffer.mapped == nullptr) {
            SM2_WARN("gl: GL_ARB_buffer_storage present but MapBufferRange "
                     "returned null; falling back to BufferSubData");
        }
    }
    if (buffer.mapped == nullptr) {
        BufferData(target, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_DRAW);
    }
    return buffer;
}

void destroy_persistent_buffer(PersistentBuffer* buffer)
{
    if (buffer->handle != 0) {
        if (buffer->mapped != nullptr) {
            BindBuffer(buffer->target, buffer->handle);
            UnmapBuffer(buffer->target);
        }
        DeleteBuffers(1, &buffer->handle);
        *buffer = PersistentBuffer{};
    }
}

u32 create_decoded_sheets_texture(u32 width, u32 height, u32 layers)
{
    u32 texture = 0;
    GenTextures(1, &texture);
    BindTexture(GL_TEXTURE_2D_ARRAY, texture);
    TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, static_cast<GLsizei>(width),
                static_cast<GLsizei>(height), static_cast<GLsizei>(layers));
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

u32 create_tone_texture(u32 width, u32 height)
{
    u32 texture = 0;
    GenTextures(1, &texture);
    BindTexture(GL_TEXTURE_2D, texture);
    TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width),
                static_cast<GLsizei>(height));
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

}  // namespace sm2::render::gl
