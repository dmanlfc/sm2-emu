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
// The desktop OpenGL 4.3 core function pointers this renderer calls, and small
// helpers shared by every gl_*_pass file.
//
// Function pointers rather than a generator-based loader (glad, GLEW, epoxy):
// SDL3 already vendors every constant and PFNGL*PROC typedef this renderer
// needs (SDL_opengl_glext.h), and SDL_GL_GetProcAddress() resolves them --
// see .kiro/specs/model2-gl-backends/design.md sec 1 for why a generator was
// considered and rejected.
//
// Every pointer below is named without its "gl" prefix (BindBuffer, not
// glBindBuffer) and lives inside sm2::render::gl. That is not a style choice:
// SDL_opengl.h declares GL 1.0/1.1 core functions (glClearColor, glViewport,
// glEnable, glGetError, ...) as ordinary extern "C" symbols at global scope
// unconditionally, because it is written for static linking against a real
// libGL. A *global* variable literally named glClearColor collides with that
// declaration -- confirmed directly, "redefinition ... as different kind of
// symbol" -- and no macro suppresses just those declarations while keeping
// the header's typedefs and constants. Living inside this namespace under
// the un-prefixed name sidesteps the collision entirely: sm2::render::gl::
// ClearColor and global ::glClearColor are different symbols by construction,
// so nothing here ever redeclares anything SDL_opengl.h already declared.
// Call sites write `gl::BindBuffer(...)` (or `using namespace
// sm2::render::gl;` once per .cpp and drop the qualifier).
#pragma once

#include "core/types.h"

#include <SDL3/SDL_stdinc.h>

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#include <string>

namespace sm2::render::gl {

// GL 1.0/1.1 entry points have no PFNGL*PROC typedef visible here: SDL3 only
// emits them when SDL_OPENGL_1_FUNCTION_TYPEDEFS is defined before including
// SDL_opengl.h, and that same guard also re-exposes the colliding plain
// extern declarations this header exists to avoid (checked directly: the two
// live behind the same #ifdef block, not separate ones). These GL 1.1 entry
// points have been stable for over three decades, so hand-typing them from
// SDL_opengl.h's own extern signatures (matched line for line, not
// reconstructed from memory) is a one-time cost, not an ongoing maintenance
// burden.
// clang-format off
typedef void     (APIENTRYP SM2_PFNGLGENTEXTURESPROC)   (GLsizei n, GLuint* textures);
typedef void     (APIENTRYP SM2_PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void     (APIENTRYP SM2_PFNGLBINDTEXTUREPROC)   (GLenum target, GLuint texture);
typedef void     (APIENTRYP SM2_PFNGLTEXSUBIMAGE2DPROC) (GLenum target, GLint level, GLint xoffset,
                                                         GLint yoffset, GLsizei width, GLsizei height,
                                                         GLenum format, GLenum type, const void* pixels);
typedef void     (APIENTRYP SM2_PFNGLTEXPARAMETERIPROC) (GLenum target, GLenum pname, GLint param);
typedef void     (APIENTRYP SM2_PFNGLDRAWARRAYSPROC)    (GLenum mode, GLint first, GLsizei count);
typedef void     (APIENTRYP SM2_PFNGLENABLEPROC)        (GLenum cap);
typedef void     (APIENTRYP SM2_PFNGLDISABLEPROC)       (GLenum cap);
typedef void     (APIENTRYP SM2_PFNGLSTENCILFUNCPROC)   (GLenum func, GLint ref, GLuint mask);
typedef void     (APIENTRYP SM2_PFNGLSTENCILOPPROC)     (GLenum fail, GLenum zfail, GLenum zpass);
typedef void     (APIENTRYP SM2_PFNGLSTENCILMASKPROC)   (GLuint mask);
typedef void     (APIENTRYP SM2_PFNGLCLEARPROC)         (GLbitfield mask);
typedef void     (APIENTRYP SM2_PFNGLCLEARCOLORPROC)    (GLclampf red, GLclampf green, GLclampf blue,
                                                         GLclampf alpha);
typedef void     (APIENTRYP SM2_PFNGLCLEARSTENCILPROC)  (GLint s);
typedef void     (APIENTRYP SM2_PFNGLVIEWPORTPROC)      (GLint x, GLint y, GLsizei width, GLsizei height);
typedef void     (APIENTRYP SM2_PFNGLSCISSORPROC)       (GLint x, GLint y, GLsizei width, GLsizei height);
typedef void     (APIENTRYP SM2_PFNGLCOLORMASKPROC)     (GLboolean red, GLboolean green, GLboolean blue,
                                                         GLboolean alpha);
typedef void     (APIENTRYP SM2_PFNGLBLENDFUNCPROC)     (GLenum sfactor, GLenum dfactor);
typedef void     (APIENTRYP SM2_PFNGLREADPIXELSPROC)    (GLint x, GLint y, GLsizei width, GLsizei height,
                                                         GLenum format, GLenum type, void* pixels);
typedef void     (APIENTRYP SM2_PFNGLREADBUFFERPROC)    (GLenum mode);
typedef void     (APIENTRYP SM2_PFNGLGETINTEGERVPROC)   (GLenum pname, GLint* params);
typedef GLenum   (APIENTRYP SM2_PFNGLGETERRORPROC)      (void);
typedef void     (APIENTRYP SM2_PFNGLFINISHPROC)        (void);
// clang-format on

// clang-format off
extern PFNGLGENBUFFERSPROC              GenBuffers;
extern PFNGLDELETEBUFFERSPROC           DeleteBuffers;
extern PFNGLBINDBUFFERPROC              BindBuffer;
extern PFNGLBINDBUFFERBASEPROC          BindBufferBase;
extern PFNGLBUFFERDATAPROC              BufferData;
extern PFNGLBUFFERSUBDATAPROC           BufferSubData;
extern PFNGLMAPBUFFERRANGEPROC          MapBufferRange;
extern PFNGLUNMAPBUFFERPROC             UnmapBuffer;
extern PFNGLGENVERTEXARRAYSPROC         GenVertexArrays;
extern PFNGLDELETEVERTEXARRAYSPROC      DeleteVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC         BindVertexArray;
extern PFNGLVERTEXATTRIBPOINTERPROC     VertexAttribPointer;
extern PFNGLVERTEXATTRIBIPOINTERPROC    VertexAttribIPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
extern SM2_PFNGLGENTEXTURESPROC         GenTextures;
extern SM2_PFNGLDELETETEXTURESPROC      DeleteTextures;
extern SM2_PFNGLBINDTEXTUREPROC         BindTexture;
extern PFNGLACTIVETEXTUREPROC           ActiveTexture;
extern PFNGLTEXSTORAGE2DPROC            TexStorage2D;
extern PFNGLTEXSTORAGE3DPROC            TexStorage3D;
extern SM2_PFNGLTEXSUBIMAGE2DPROC       TexSubImage2D;
extern SM2_PFNGLTEXPARAMETERIPROC       TexParameteri;
extern PFNGLBINDIMAGETEXTUREPROC        BindImageTexture;
extern PFNGLGENSAMPLERSPROC             GenSamplers;
extern PFNGLDELETESAMPLERSPROC          DeleteSamplers;
extern PFNGLBINDSAMPLERPROC             BindSampler;
extern PFNGLSAMPLERPARAMETERIPROC       SamplerParameteri;
extern PFNGLGENFRAMEBUFFERSPROC         GenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC      DeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC         BindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC    FramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC  CheckFramebufferStatus;
extern PFNGLDRAWBUFFERSPROC             DrawBuffers;
extern PFNGLGENRENDERBUFFERSPROC        GenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC     DeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC        BindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC     RenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
extern PFNGLCREATESHADERPROC            CreateShader;
extern PFNGLDELETESHADERPROC            DeleteShader;
extern PFNGLSHADERSOURCEPROC            ShaderSource;
extern PFNGLCOMPILESHADERPROC           CompileShader;
extern PFNGLGETSHADERIVPROC             GetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC        GetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC           CreateProgram;
extern PFNGLDELETEPROGRAMPROC           DeleteProgram;
extern PFNGLATTACHSHADERPROC            AttachShader;
extern PFNGLLINKPROGRAMPROC             LinkProgram;
extern PFNGLGETPROGRAMIVPROC            GetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC       GetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC              UseProgram;
extern PFNGLUNIFORMBLOCKBINDINGPROC     UniformBlockBinding;
extern PFNGLGETUNIFORMBLOCKINDEXPROC    GetUniformBlockIndex;
extern PFNGLDISPATCHCOMPUTEPROC         DispatchCompute;
extern PFNGLMEMORYBARRIERPROC           MemoryBarrier;
extern SM2_PFNGLDRAWARRAYSPROC          DrawArrays;
extern SM2_PFNGLENABLEPROC              Enable;
extern SM2_PFNGLDISABLEPROC             Disable;
extern SM2_PFNGLSTENCILFUNCPROC         StencilFunc;
extern SM2_PFNGLSTENCILOPPROC           StencilOp;
extern SM2_PFNGLSTENCILMASKPROC         StencilMask;
extern SM2_PFNGLCLEARPROC               Clear;
extern SM2_PFNGLCLEARCOLORPROC          ClearColor;
extern SM2_PFNGLCLEARSTENCILPROC        ClearStencil;
extern SM2_PFNGLVIEWPORTPROC            Viewport;
extern SM2_PFNGLSCISSORPROC             Scissor;
extern SM2_PFNGLCOLORMASKPROC           ColorMask;
extern SM2_PFNGLBLENDFUNCPROC           BlendFunc;
extern SM2_PFNGLREADPIXELSPROC          ReadPixels;
extern SM2_PFNGLREADBUFFERPROC          ReadBuffer;
extern SM2_PFNGLGETINTEGERVPROC         GetIntegerv;
extern PFNGLGETSTRINGIPROC              GetStringi;
extern SM2_PFNGLGETERRORPROC            GetError;
extern SM2_PFNGLFINISHPROC              Finish;

/// Resolved by resolve_buffer_storage(), not load_gl_functions(). Null when
/// the extension is absent; create_persistent_buffer() falls back to
/// BufferSubData.
extern PFNGLBUFFERSTORAGEPROC           BufferStorage;
// clang-format on

/// Resolve every GL function pointer this renderer calls, via `get_proc`
/// (ordinarily SDL_GL_GetProcAddress). Must be called once, after a GL
/// context is current -- SDL_GL_GetProcAddress's own documentation requires
/// a current context, and on some platforms the returned pointers are only
/// valid for the context that was current when they were resolved.
///
/// Returns false if any function this renderer actually calls could not be
/// resolved, naming the first one that failed -- a context that reports 4.3
/// core but is missing an entry point this renderer needs is a driver bug
/// worth surfacing by name, not a silent null-pointer crash three calls
/// later.
[[nodiscard]] bool load_gl_functions(SDL_FunctionPointer (*get_proc)(const char*),
                                     std::string* out_error);

/// True once load_gl_functions() has succeeded. Guards every other function
/// in this namespace from being called before the pointers exist.
[[nodiscard]] bool gl_functions_loaded();

/// Resolves BufferStorage separately from load_gl_functions(): GLES has no
/// core-promoted equivalent under the desktop name, only glBufferStorageEXT
/// when GL_EXT_buffer_storage is present. Call once, after
/// load_gl_functions() succeeds. A null result is not an error.
void resolve_buffer_storage(SDL_FunctionPointer (*get_proc)(const char*), bool is_es);

/// Extension check that works on a core profile, where glGetString(GL_EXTENSIONS)
/// was removed: iterates GetStringi(GL_EXTENSIONS, i) for i in
/// [0, GL_NUM_EXTENSIONS) rather than parsing one space-separated string.
[[nodiscard]] bool has_gl_extension(const char* name);

/// Rewrite an embedded shader's `#version` line for this target and inject
/// `#define SM2_TARGET_GL 1` immediately after it.
///
/// The embedded source (shaders/*_glsl.h) still carries `#version 450`,
/// which is what the Vulkan SPIR-V compile of the *same* source needs, but
/// `#version` must be the first token in GLSL source -- confirmed directly,
/// glslc rejects a #define placed above #version with "must occur first in
/// shader" -- so SM2_TARGET_GL cannot simply be prepended in front of it the
/// way a naive read of the shader-editing task might suggest. This finds the
/// first `#version` line, replaces it with `version_directive`, and inserts
/// the define on the line after. Every gl_*_pass call site must run its
/// embedded source through this before handing it to compile_program()/
/// compile_compute_program().
[[nodiscard]] std::string prepare_gl_source(const char* embedded_source,
                                            const char* version_directive);

/// "#version 430 core" -- desktop GL's own version_directive for
/// prepare_gl_source(), named so no call site repeats the literal.
inline constexpr const char* kDesktopVersionDirective = "#version 430 core";

/// "#version 310 es" -- GLES 3.1's own version_directive for
/// prepare_gl_source(). ES requires explicit precision qualifiers which are
/// injected by prepare_gl_source() alongside the SM2_TARGET_GL define when
/// this directive is used.
inline constexpr const char* kEsVersionDirective = "#version 310 es";

/// Returns the version directive matching the currently active context.
/// Set by GlBackend::init() once the context is created.
[[nodiscard]] const char* active_version_directive();

/// Set the version directive for subsequent shader compilation. Called once
/// during backend init after the context type (desktop/ES) is known.
void set_version_directive(const char* directive);

/// Compile a vertex+fragment program from GLSL source already run through
/// prepare_gl_source(), logging the shader and link error text through
/// SM2_ERROR on failure rather than aborting -- the same "report and
/// propagate" convention SM2_VK_TRY already uses on the Vulkan side, adapted
/// to GL's own GetShaderiv/GetProgramiv query style. Returns 0 on failure.
[[nodiscard]] u32 compile_program(const char* vertex_source, const char* fragment_source);

/// As compile_program(), for a single compute shader.
[[nodiscard]] u32 compile_compute_program(const char* compute_source);

/// A GL buffer that is either persistently mapped (GL_ARB_buffer_storage) or
/// backed by plain BufferData with updates going through BufferSubData,
/// decided once by has_gl_extension("GL_ARB_buffer_storage") and recorded
/// here so a caller does not have to re-check it per write.
struct PersistentBuffer {
    u32   handle = 0;
    void* mapped = nullptr;  ///< null if this buffer is not persistently mapped.
    usize size   = 0;
    u32   target = 0;  ///< GL_SHADER_STORAGE_BUFFER, GL_ARRAY_BUFFER, ...

    /// Write the whole buffer. Uses the mapped pointer directly when one
    /// exists; otherwise a single BufferSubData covering the whole range.
    void write(const void* data, usize bytes) const;
};

[[nodiscard]] PersistentBuffer create_persistent_buffer(usize bytes, u32 target);
void                           destroy_persistent_buffer(PersistentBuffer* buffer);

/// GL_RGBA8UI, GL_TEXTURE_2D_ARRAY, immutable storage: the decoded texture
/// sheets' format, matching Poly3DPass::kDecodedWidth/kDecodedHeight exactly.
[[nodiscard]] u32 create_decoded_sheets_texture(u32 width, u32 height, u32 layers);

/// GL_RGBA8, GL_TEXTURE_2D, immutable storage: the tone-curve lookup image.
[[nodiscard]] u32 create_tone_texture(u32 width, u32 height);

}  // namespace sm2::render::gl
