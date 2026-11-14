# ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
# ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
# █████╔╝ █████╗  ██║███████╗███████║██║   ██║
# ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
# ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
# ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
#            継 承  ( k e i s h ō )
#
# A Sega Model 1, 2 & 3 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This header must not be removed. Keisho is licensed under the GNU General
# Public License, version 3 or later, with additional terms under section 7 of
# that licence. See LICENSE and NOTICE.
#

# Build-time GLSL -> SPIR-V -> C++ header pipeline.
#
#   sm2_add_shader(sm2_shaders
#       SOURCE  model2_2d.frag
#       SYMBOL  kModel2Frag2D          # optional, derived from SOURCE if absent
#       DEFINES UPSCALE_MODE=1         # optional
#       VARIANT upscale1               # optional, required if DEFINES is used
#   )
#
# Each invocation adds two build steps to the named target: glslc, then a
# cmake -P pass that rewrites the module as a constexpr array. The generated
# headers land in a directory the target exposes via its INTERFACE include
# path, so consumers just #include "shaders/<name>.h".

set(SM2_EMBED_SPIRV_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake"
    CACHE INTERNAL "Path to the SPIR-V embedding script")
set(SM2_EMBED_GLSL_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedGlsl.cmake"
    CACHE INTERNAL "Path to the GLSL source embedding script")

# Whether any OpenGL-family backend is being built. Both options are cache
# variables known at configure time, so no generator expression is needed.
if(SM2_BUILD_OPENGL_DESKTOP OR SM2_BUILD_OPENGL_ES)
    set(SM2_NEEDS_GL_SHADERS TRUE)
else()
    set(SM2_NEEDS_GL_SHADERS FALSE)
endif()

function(sm2_declare_shader_library target)
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${gen_dir}/shaders")

    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE "${gen_dir}")

    # Real target the generated headers hang off; INTERFACE libraries cannot
    # own build steps. Each shader appends itself as a dependency of this.
    add_custom_target(${target}_compile ALL
        COMMENT "Shaders for ${target} are up to date")

    add_dependencies(${target} ${target}_compile)

    set_target_properties(${target} PROPERTIES
        SM2_SHADER_GEN_DIR "${gen_dir}"
        SM2_SHADER_DRIVER  "${target}_compile")
endfunction()

function(sm2_add_shader target)
    cmake_parse_arguments(ARG "" "SOURCE;SYMBOL;VARIANT" "DEFINES" ${ARGN})

    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "sm2_add_shader: SOURCE is required")
    endif()
    if(ARG_DEFINES AND NOT ARG_VARIANT)
        message(FATAL_ERROR
            "sm2_add_shader: DEFINES requires VARIANT so that variants of "
            "${ARG_SOURCE} do not collide on one output file")
    endif()

    get_target_property(gen_dir ${target} SM2_SHADER_GEN_DIR)
    get_target_property(driver  ${target} SM2_SHADER_DRIVER)

    set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
    if(NOT EXISTS "${source_path}")
        message(FATAL_ERROR "sm2_add_shader: no such file ${source_path}")
    endif()

    # foo.frag -> foo_frag; with a variant, foo_frag_upscale1
    string(REGEX REPLACE "[./]" "_" stem "${ARG_SOURCE}")
    if(ARG_VARIANT)
        set(stem "${stem}_${ARG_VARIANT}")
    endif()

    if(ARG_SYMBOL)
        set(symbol "${ARG_SYMBOL}")
    else()
        set(symbol "k${stem}")
    endif()

    set(spv_path    "${gen_dir}/${stem}.spv")
    set(header_path "${gen_dir}/shaders/${stem}.h")

    set(define_flags "")
    foreach(def IN LISTS ARG_DEFINES)
        list(APPEND define_flags "-D${def}")
    endforeach()

    set(werror_flag "")
    if(SM2_SHADER_WERROR)
        set(werror_flag "-Werror")
    endif()

    # Included .glsl fragments are not compiled on their own but any of them
    # changing must retrigger every shader. Listing the whole glob is
    # conservative and avoids parsing #include directives.
    file(GLOB shader_includes CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.glsl")

    add_custom_command(
        OUTPUT  "${spv_path}"
        COMMAND "${SM2_GLSLC}"
                --target-env=vulkan1.0
                -O
                ${werror_flag}
                ${define_flags}
                -I "${CMAKE_CURRENT_SOURCE_DIR}"
                -MD -MF "${spv_path}.d"
                -o "${spv_path}"
                "${source_path}"
        DEPENDS "${source_path}" ${shader_includes}
        DEPFILE "${spv_path}.d"
        COMMENT "glslc ${ARG_SOURCE}$<$<BOOL:${ARG_VARIANT}>: [${ARG_VARIANT}]>"
        VERBATIM
    )

    add_custom_command(
        OUTPUT  "${header_path}"
        COMMAND "${CMAKE_COMMAND}"
                -DSM2_IN=${spv_path}
                -DSM2_OUT=${header_path}
                -DSM2_SYMBOL=${symbol}
                -DSM2_SOURCE_NAME=${ARG_SOURCE}
                -P "${SM2_EMBED_SPIRV_SCRIPT}"
        DEPENDS "${spv_path}" "${SM2_EMBED_SPIRV_SCRIPT}"
        COMMENT "Embedding ${stem}.spv"
        VERBATIM
    )

    set(gl_outputs "${header_path}")

    if(SM2_NEEDS_GL_SHADERS)
        # Syntax-only lint against the GL dialect, so a shader the GL backend
        # could not compile at runtime fails the build instead. The .spv is
        # discarded. SM2_TARGET_GL matches what the backend prepends at runtime,
        # so the GL branch of any #ifdef is the one checked.
        set(gl_lint_path "${gen_dir}/${stem}.gl.spv")
        add_custom_command(
            OUTPUT  "${gl_lint_path}"
            COMMAND "${SM2_GLSLC}"
                    --target-env=opengl4.5
                    -DSM2_TARGET_GL=1
                    ${werror_flag}
                    ${define_flags}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    -o "${gl_lint_path}"
                    "${source_path}"
            DEPENDS "${source_path}" ${shader_includes}
            COMMENT "glslc ${ARG_SOURCE} [opengl lint]"
            VERBATIM
        )

        set(gl_symbol "${symbol}Glsl")
        set(gl_header_path "${gen_dir}/shaders/${stem}_glsl.h")
        add_custom_command(
            OUTPUT  "${gl_header_path}"
            COMMAND "${CMAKE_COMMAND}"
                    -DSM2_IN=${source_path}
                    -DSM2_OUT=${gl_header_path}
                    -DSM2_SYMBOL=${gl_symbol}
                    -DSM2_SOURCE_NAME=${ARG_SOURCE}
                    -P "${SM2_EMBED_GLSL_SCRIPT}"
            # Depending on the lint makes a GL syntax error fail the build.
            DEPENDS "${source_path}" "${gl_lint_path}" "${SM2_EMBED_GLSL_SCRIPT}"
            COMMENT "Embedding ${stem} as GLSL source"
            VERBATIM
        )
        list(APPEND gl_outputs "${gl_header_path}")
    endif()

    # Sources on a custom target do not create a build dependency, so each
    # shader gets its own target for add_dependencies.
    add_custom_target(${target}_${stem} DEPENDS ${gl_outputs})
    add_dependencies(${driver} ${target}_${stem})
endfunction()
