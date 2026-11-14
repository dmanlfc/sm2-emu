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
#include "render/gl/gl_common.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sm2::render::gl {

// One X-macro list drives the declarations in gl_common.h and the definitions
// and loader here.
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
    F(TexSubImage3D, PFNGLTEXSUBIMAGE3DPROC) \
    F(GenerateMipmap, PFNGLGENERATEMIPMAPPROC) \
    F(TexSubImage2D, SM2_PFNGLTEXSUBIMAGE2DPROC) \
    F(TexParameteri, SM2_PFNGLTEXPARAMETERIPROC) \
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
    F(DrawArrays, SM2_PFNGLDRAWARRAYSPROC) \
    F(Enable, SM2_PFNGLENABLEPROC) \
    F(Disable, SM2_PFNGLDISABLEPROC) \
    F(StencilFunc, SM2_PFNGLSTENCILFUNCPROC) \
    F(DepthFunc, SM2_PFNGLDEPTHFUNCPROC) \
    F(DepthMask, SM2_PFNGLDEPTHMASKPROC) \
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
    F(Finish, SM2_PFNGLFINISHPROC) \
    F(BindBufferRange, PFNGLBINDBUFFERRANGEPROC) \
    F(BlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC) \
    F(ClearBufferfv, PFNGLCLEARBUFFERFVPROC) \
    F(ClearBufferuiv, PFNGLCLEARBUFFERUIVPROC) \
    F(ClearBufferfi, PFNGLCLEARBUFFERFIPROC) \
    F(PixelStorei, SM2_PFNGLPIXELSTOREIPROC) \
    F(TexImage2D, SM2_PFNGLTEXIMAGE2DPROC) \
    F(TexImage3D, PFNGLTEXIMAGE3DPROC) \
    F(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
    F(Uniform1i, PFNGLUNIFORM1IPROC)
// clang-format on

#define SM2_GL_DEFINE(name, type) type name = nullptr;
SM2_GL_FUNCTION_LIST(SM2_GL_DEFINE)
#undef SM2_GL_DEFINE

PFNGLBUFFERSTORAGEPROC BufferStorage = nullptr;
PFNGLTEXBUFFERPROC     TexBuffer     = nullptr;
PFNGLTEXSTORAGE2DPROC     TexStorage2D     = nullptr;
PFNGLTEXSTORAGE3DPROC     TexStorage3D     = nullptr;
PFNGLBINDIMAGETEXTUREPROC BindImageTexture = nullptr;
PFNGLDISPATCHCOMPUTEPROC  DispatchCompute  = nullptr;
PFNGLMEMORYBARRIERPROC    MemoryBarrier    = nullptr;

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
    // GLES has only glBufferStorageEXT. Null is fine: create_persistent_buffer()
    // falls back to BufferSubData.
    BufferStorage = reinterpret_cast<PFNGLBUFFERSTORAGEPROC>(
        get_proc(is_es ? "glBufferStorageEXT" : "glBufferStorage"));
}

namespace {

void storage_format(GLenum internal_format, GLenum* format, GLenum* type)
{
    switch (internal_format) {
        case GL_RGBA8UI: *format = GL_RGBA_INTEGER; *type = GL_UNSIGNED_BYTE; return;
        case GL_R16UI:   *format = GL_RED_INTEGER;  *type = GL_UNSIGNED_SHORT; return;
        case GL_R32UI:   *format = GL_RED_INTEGER;  *type = GL_UNSIGNED_INT; return;
        default:         *format = GL_RGBA;         *type = GL_UNSIGNED_BYTE; return;
    }
}

// TexStorage emulated with mutable TexImage levels, capped by MAX_LEVEL so the
// texture is complete exactly as an immutable one would be.
void APIENTRY tex_storage_2d_fallback(GLenum target, GLsizei levels, GLenum internal_format,
                                      GLsizei width, GLsizei height)
{
    GLenum format = 0;
    GLenum type   = 0;
    storage_format(internal_format, &format, &type);
    for (GLsizei level = 0; level < levels; ++level) {
        TexImage2D(target, level, static_cast<GLint>(internal_format), width, height, 0, format,
                   type, nullptr);
        width  = std::max(1, width / 2);
        height = std::max(1, height / 2);
    }
    TexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
}

void APIENTRY tex_storage_3d_fallback(GLenum target, GLsizei levels, GLenum internal_format,
                                      GLsizei width, GLsizei height, GLsizei depth)
{
    GLenum format = 0;
    GLenum type   = 0;
    storage_format(internal_format, &format, &type);
    for (GLsizei level = 0; level < levels; ++level) {
        TexImage3D(target, level, static_cast<GLint>(internal_format), width, height, depth, 0,
                   format, type, nullptr);
        width  = std::max(1, width / 2);
        height = std::max(1, height / 2);
        if (target == GL_TEXTURE_3D) {
            depth = std::max(1, depth / 2);
        }
    }
    TexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
}

}  // namespace

bool resolve_optional_functions(SDL_FunctionPointer (*get_proc)(const char*), bool compute,
                                bool texture_storage, std::string* out_error)
{
    TexStorage2D     = nullptr;
    TexStorage3D     = nullptr;
    BindImageTexture = nullptr;
    DispatchCompute  = nullptr;
    MemoryBarrier    = nullptr;
    TexBuffer        = nullptr;
    if (texture_storage) {
        TexStorage2D = reinterpret_cast<PFNGLTEXSTORAGE2DPROC>(get_proc("glTexStorage2D"));
        TexStorage3D = reinterpret_cast<PFNGLTEXSTORAGE3DPROC>(get_proc("glTexStorage3D"));
    }
    if (TexStorage2D == nullptr || TexStorage3D == nullptr) {
        TexStorage2D = tex_storage_2d_fallback;
        TexStorage3D = tex_storage_3d_fallback;
    }
    if (compute) {
        BindImageTexture =
            reinterpret_cast<PFNGLBINDIMAGETEXTUREPROC>(get_proc("glBindImageTexture"));
        DispatchCompute = reinterpret_cast<PFNGLDISPATCHCOMPUTEPROC>(get_proc("glDispatchCompute"));
        MemoryBarrier   = reinterpret_cast<PFNGLMEMORYBARRIERPROC>(get_proc("glMemoryBarrier"));
        if (BindImageTexture == nullptr || DispatchCompute == nullptr || MemoryBarrier == nullptr) {
            if (out_error != nullptr) {
                *out_error = "could not resolve the compute entry points";
            }
            return false;
        }
    } else {
        TexBuffer = reinterpret_cast<PFNGLTEXBUFFERPROC>(get_proc("glTexBuffer"));
        if (TexBuffer == nullptr) {
            if (out_error != nullptr) {
                *out_error = "could not resolve glTexBuffer";
            }
            return false;
        }
    }
    return true;
}

bool is_gl41()
{
    return std::strcmp(g_version_directive, kGl41VersionDirective) == 0;
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

    // Strip Vulkan-only "set = N, " from layout qualifiers; GL has no
    // descriptor sets. "set" must not follow an identifier character, so
    // "offset = ..." is left alone.
    {
        const std::string pattern = "set = ";
        usize pos = 0;
        while ((pos = source.find(pattern, pos)) != std::string::npos) {
            if (pos > 0) {
                const char prev = source[pos - 1];
                if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z')
                    || (prev >= '0' && prev <= '9') || prev == '_') {
                    pos += pattern.size();
                    continue;
                }
            }

            usize end = pos + pattern.size();
            while (end < source.size() && source[end] >= '0' && source[end] <= '9') {
                ++end;
            }
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
    const bool es   = std::string(version_directive).find(" es") != std::string::npos;
    const bool gl41 = std::strcmp(version_directive, kGl41VersionDirective) == 0;
    if (gl41) {
        header += "#define SM2_GL41 1\n";
        // GLSL 4.10 wants the interpolation qualifier before in/out.
        for (const char* qualifier : {"in", "out"}) {
            const std::string from = std::string(qualifier) + " flat ";
            const std::string to   = std::string("flat ") + qualifier + " ";
            for (usize pos = 0; (pos = source.find(from, pos)) != std::string::npos;) {
                source.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
    }
    if (es) {
        // GLES requires explicit precision qualifiers; highp matches desktop GL.
        header += "precision highp float;\n"
                  "precision highp int;\n"
                  "precision highp sampler2D;\n"
                  "precision highp sampler2DArray;\n"
                  "precision highp usampler2D;\n"
                  "precision highp usampler2DArray;\n"
                  "precision highp image2DArray;\n"
                  "precision highp uimage2DArray;\n";
    }
    if (es || gl41) {
        // ESSL and GLSL before 4.20 require a local `const` to be a constant
        // expression. Strip `const ` from indented lines so runtime-initialised
        // locals compile; unindented global constants are left alone.
        {
            const std::string pat = "const ";
            usize pos = 0;
            while ((pos = source.find(pat, pos)) != std::string::npos) {
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

/// A layout(binding = N) qualifier removed for GLSL 4.10, reapplied after link.
struct BindingFixup {
    std::string name;
    s32         binding = 0;
    bool        block   = false;
};

[[nodiscard]] std::string trim(const std::string& text)
{
    const usize first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const usize last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

[[nodiscard]] std::string next_word(const std::string& text, usize* pos)
{
    while (*pos < text.size() && (text[*pos] == ' ' || text[*pos] == '\t')) {
        ++*pos;
    }
    const usize start = *pos;
    while (*pos < text.size()
           && (std::isalnum(static_cast<unsigned char>(text[*pos])) != 0 || text[*pos] == '_')) {
        ++*pos;
    }
    return text.substr(start, *pos - start);
}

/// Strip layout binding qualifiers (GLSL 4.20) from `source`, recording the
/// uniform blocks and samplers they named. Follows #ifdef/#ifndef/#else so a
/// Vulkan-only branch's bindings are ignored.
[[nodiscard]] std::string strip_bindings(const std::string& source,
                                         std::vector<BindingFixup>* fixups)
{
    std::vector<std::string> defined;
    std::vector<std::pair<bool, bool>> stack;  // (parent active, condition)
    bool active = true;
    std::string out;
    out.reserve(source.size());

    usize line_start = 0;
    while (line_start < source.size()) {
        usize line_end = source.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = source.size();
        }
        std::string line = source.substr(line_start, line_end - line_start);
        const std::string text = trim(line);

        if (!text.empty() && text[0] == '#') {
            usize pos = 1;
            const std::string directive = next_word(text, &pos);
            const std::string name      = next_word(text, &pos);
            const bool is_defined =
                std::find(defined.begin(), defined.end(), name) != defined.end();
            if (directive == "define" && active) {
                defined.push_back(name);
            } else if (directive == "ifdef" || directive == "ifndef") {
                const bool condition = (directive == "ifdef") == is_defined;
                stack.emplace_back(active, condition);
                active = active && condition;
            } else if (directive == "if") {
                stack.emplace_back(active, true);  // not evaluated
            } else if ((directive == "else" || directive == "elif") && !stack.empty()) {
                stack.back().second = !stack.back().second;
                active = stack.back().first && stack.back().second;
            } else if (directive == "endif" && !stack.empty()) {
                active = stack.back().first;
                stack.pop_back();
            }
        } else if (active && text.rfind("layout(", 0) == 0) {
            const usize open  = line.find("layout(");
            const usize close = line.find(')', open);
            const usize key   = line.find("binding", open);
            if (close != std::string::npos && key != std::string::npos && key < close) {
                const usize eq = line.find('=', key);
                const s32 binding = std::atoi(line.c_str() + eq + 1);

                // Rebuild the qualifier list without the binding entry.
                std::string kept;
                usize item_start = open + 7;
                while (item_start <= close) {
                    usize item_end = line.find(',', item_start);
                    if (item_end == std::string::npos || item_end > close) {
                        item_end = close;
                    }
                    const std::string item = trim(line.substr(item_start, item_end - item_start));
                    if (!item.empty() && item.rfind("binding", 0) != 0) {
                        kept += (kept.empty() ? "" : ", ") + item;
                    }
                    item_start = item_end + 1;
                }
                const std::string rest = line.substr(close + 1);
                line = line.substr(0, open) + (kept.empty() ? "" : "layout(" + kept + ")") + rest;

                usize pos = 0;
                std::string word;
                while (!(word = next_word(rest, &pos)).empty() && word != "uniform") {
                }
                if (word == "uniform") {
                    BindingFixup fixup;
                    fixup.binding = binding;
                    const std::string type = next_word(rest, &pos);
                    const std::string name = next_word(rest, &pos);
                    fixup.block = name.empty();
                    fixup.name  = fixup.block ? type : name;
                    fixups->push_back(fixup);
                }
            }
        }
        out += line;
        if (line_end < source.size()) {
            out += '\n';
        }
        line_start = line_end + 1;
    }
    return out;
}

void apply_bindings(u32 program, const std::vector<BindingFixup>& fixups)
{
    s32 previous = 0;
    GetIntegerv(GL_CURRENT_PROGRAM, &previous);
    UseProgram(program);
    for (const BindingFixup& fixup : fixups) {
        if (fixup.block) {
            const u32 index = GetUniformBlockIndex(program, fixup.name.c_str());
            if (index != GL_INVALID_INDEX) {
                UniformBlockBinding(program, index, static_cast<u32>(fixup.binding));
            }
        } else {
            const s32 location = GetUniformLocation(program, fixup.name.c_str());
            if (location >= 0) {
                Uniform1i(location, fixup.binding);
            }
        }
    }
    UseProgram(static_cast<u32>(previous));
}

/// Compile one shader stage, logging its info log on failure. 0 on failure.
[[nodiscard]] u32 compile_stage(u32 stage_type, const char* source)
{
    // Non-ASCII only appears in comments (the file banner); not every GLSL
    // compiler accepts it there.
    std::string ascii(source);
    for (char& c : ascii) {
        if (static_cast<unsigned char>(c) >= 0x80) c = ' ';
    }
    const char* text   = ascii.c_str();
    const u32   shader = CreateShader(stage_type);
    ShaderSource(shader, 1, &text, nullptr);
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
    std::vector<BindingFixup> fixups;
    std::string               vertex_text;
    std::string               fragment_text;
    if (is_gl41()) {
        vertex_text     = strip_bindings(vertex_source, &fixups);
        fragment_text   = strip_bindings(fragment_source, &fixups);
        vertex_source   = vertex_text.c_str();
        fragment_source = fragment_text.c_str();
    }

    const u32 vertex = compile_stage(GL_VERTEX_SHADER, vertex_source);
    if (vertex == 0) {
        return 0;
    }
    const u32 fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source);
    if (fragment == 0) {
        DeleteShader(vertex);
        return 0;
    }
    const u32 program = link_program({vertex, fragment});
    if (program != 0 && !fixups.empty()) {
        apply_bindings(program, fixups);
    }
    return program;
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

    // GL_ARB_buffer_storage gives a persistently mapped buffer like the Vulkan
    // path's; without it, BufferSubData per write.
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
