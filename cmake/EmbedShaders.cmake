#  ____  __  __  ____         _____ __  __ _   _
# / ___||  \/  ||___ \       | ____|  \/  | | | |
# \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
#  ___) | |  | | / __/|_____|| |___| |  | | |_| |
# |____/|_|  |_||_____|      |_____|_|  |_|\___/
#
# sm2-emu — A Sega Model 2 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: BSD-3-Clause
#
# This header must not be removed. The source files in this project may not be
# used to contribute to commercial projects or for monetary gain without the
# express written permission of the author.
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
                --target-env=vulkan1.3
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

    # A per-shader target rather than appending to the driver's SOURCES: sources
    # on a custom target are advisory (they show up in IDEs) and do not create
    # a build dependency, whereas add_dependencies does.
    add_custom_target(${target}_${stem} DEPENDS "${header_path}")
    add_dependencies(${driver} ${target}_${stem})
endfunction()
