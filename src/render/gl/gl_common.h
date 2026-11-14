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
// The desktop OpenGL 4.3 (or 4.1) core function pointers this renderer calls, and small
// helpers shared by every gl_*_pass file.
//
// Function pointers rather than a loader such as glad: SDL3 already vendors the
// constants and PFNGL*PROC typedefs, and SDL_GL_GetProcAddress() resolves them.
//
// The pointers drop the "gl" prefix (gl::BindBuffer) because SDL_opengl.h
// declares the GL 1.0/1.1 functions as global extern "C" symbols, which a
// global variable of the same name would collide with.
#pragma once

#include "core/types.h"

#include <SDL3/SDL_stdinc.h>

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#include <string>

namespace sm2::render::gl {

// GL 1.0/1.1 entry points are typed by hand from SDL_opengl.h's signatures:
// SDL_OPENGL_1_FUNCTION_TYPEDEFS would provide the typedefs, but the same guard
// re-exposes the colliding extern declarations.
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
typedef void     (APIENTRYP SM2_PFNGLDEPTHFUNCPROC)     (GLenum func);
typedef void     (APIENTRYP SM2_PFNGLDEPTHMASKPROC)     (GLboolean flag);
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
typedef void     (APIENTRYP SM2_PFNGLPIXELSTOREIPROC)   (GLenum pname, GLint param);
typedef void     (APIENTRYP SM2_PFNGLTEXIMAGE2DPROC)    (GLenum target, GLint level, GLint internalformat,
                                                         GLsizei width, GLsizei height, GLint border,
                                                         GLenum format, GLenum type, const void* pixels);
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
/// Resolved by resolve_optional_functions(); emulated with TexImage when the
/// context has no texture storage (GL 4.1 without GL_ARB_texture_storage).
extern PFNGLTEXSTORAGE2DPROC            TexStorage2D;
extern PFNGLTEXSTORAGE3DPROC            TexStorage3D;
extern PFNGLTEXSUBIMAGE3DPROC           TexSubImage3D;
extern PFNGLGENERATEMIPMAPPROC          GenerateMipmap;
extern SM2_PFNGLTEXSUBIMAGE2DPROC       TexSubImage2D;
extern SM2_PFNGLTEXPARAMETERIPROC       TexParameteri;
/// Resolved by resolve_optional_functions(); null below GL 4.3.
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
extern PFNGLDISPATCHCOMPUTEPROC         DispatchCompute;  ///< optional, as BindImageTexture
extern PFNGLMEMORYBARRIERPROC           MemoryBarrier;    ///< optional, as BindImageTexture
extern SM2_PFNGLDRAWARRAYSPROC          DrawArrays;
extern SM2_PFNGLENABLEPROC              Enable;
extern SM2_PFNGLDISABLEPROC             Disable;
extern SM2_PFNGLSTENCILFUNCPROC         StencilFunc;
extern SM2_PFNGLDEPTHFUNCPROC           DepthFunc;
extern SM2_PFNGLDEPTHMASKPROC           DepthMask;
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
// Used by the Model 3 passes (model3_pass.cpp).
extern PFNGLBINDBUFFERRANGEPROC         BindBufferRange;
extern PFNGLBLITFRAMEBUFFERPROC         BlitFramebuffer;
extern PFNGLCLEARBUFFERFVPROC           ClearBufferfv;
extern PFNGLCLEARBUFFERUIVPROC          ClearBufferuiv;
extern PFNGLCLEARBUFFERFIPROC           ClearBufferfi;
extern SM2_PFNGLPIXELSTOREIPROC         PixelStorei;
// GL 4.1 fallbacks (buffer textures, sampler units set by uniform, TexImage storage).
extern SM2_PFNGLTEXIMAGE2DPROC          TexImage2D;
extern PFNGLTEXIMAGE3DPROC              TexImage3D;
/// Resolved by resolve_optional_functions(); only without compute.
extern PFNGLTEXBUFFERPROC               TexBuffer;
extern PFNGLGETUNIFORMLOCATIONPROC      GetUniformLocation;
extern PFNGLUNIFORM1IPROC               Uniform1i;

/// Resolved by resolve_buffer_storage(), not load_gl_functions(). Null when
/// the extension is absent; create_persistent_buffer() falls back to
/// BufferSubData.
extern PFNGLBUFFERSTORAGEPROC           BufferStorage;
// clang-format on

/// Resolve every GL function pointer this renderer calls, via `get_proc`
/// (ordinarily SDL_GL_GetProcAddress). Call once, with the context current.
/// False if any could not be resolved, logging the first by name.
[[nodiscard]] bool load_gl_functions(SDL_FunctionPointer (*get_proc)(const char*),
                                     std::string* out_error);

/// True once load_gl_functions() has succeeded.
[[nodiscard]] bool gl_functions_loaded();

/// Resolves BufferStorage separately, since GLES only has glBufferStorageEXT.
/// Call once, after load_gl_functions(). A null result is not an error.
void resolve_buffer_storage(SDL_FunctionPointer (*get_proc)(const char*), bool is_es);

/// Resolves the entry points not every supported context has: compute and
/// image store (`compute`: GL 4.3 / GLES 3.1) and immutable texture storage
/// (`texture_storage`), which falls back to a TexImage emulation, or buffer
/// textures without compute. Returns false if the path chosen can't be resolved.
[[nodiscard]] bool resolve_optional_functions(SDL_FunctionPointer (*get_proc)(const char*),
                                              bool compute, bool texture_storage,
                                              std::string* out_error);

/// Extension check for a core profile, via GetStringi(GL_EXTENSIONS, i).
[[nodiscard]] bool has_gl_extension(const char* name);

/// Replace an embedded shader's `#version 450` line with `version_directive`
/// and inject `#define SM2_TARGET_GL 1` after it (`#version` must come first).
/// Every GL shader goes through this before compile_program().
/// `extra_define`, when non-null, is emitted as `#define <name> 1`; glslc's
/// build-time `-D` never reaches the embedded GL source.
[[nodiscard]] std::string prepare_gl_source(const char* embedded_source,
                                            const char* version_directive,
                                            const char* extra_define = nullptr);

/// Desktop GL's version_directive for prepare_gl_source().
inline constexpr const char* kDesktopVersionDirective = "#version 430 core";

/// The macOS fallback: no compute shaders, storage buffers, image store or
/// layout(binding). prepare_gl_source() defines
/// SM2_GL41 for it, and compile_program() turns binding qualifiers into
/// uniform/block bindings after link.
inline constexpr const char* kGl41VersionDirective = "#version 410 core";

/// True when shaders target kGl41VersionDirective.
[[nodiscard]] bool is_gl41();

/// GLES 3.1's version_directive; prepare_gl_source() also injects the
/// precision qualifiers ES requires.
inline constexpr const char* kEsVersionDirective = "#version 310 es";

/// Returns the version directive matching the currently active context.
/// Set by GlBackend::init() once the context is created.
[[nodiscard]] const char* active_version_directive();

/// Set the version directive for subsequent shader compilation. Called once
/// during backend init after the context type (desktop/ES) is known.
void set_version_directive(const char* directive);

/// Compile a vertex+fragment program from source run through
/// prepare_gl_source(). Logs the error text and returns 0 on failure.
[[nodiscard]] u32 compile_program(const char* vertex_source, const char* fragment_source);

/// As compile_program(), for a single compute shader.
[[nodiscard]] u32 compile_compute_program(const char* compute_source);

/// A GL buffer, persistently mapped where GL_ARB_buffer_storage allows, else
/// updated through BufferSubData.
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
