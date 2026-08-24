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

# ---------------------------------------------------------------------------
# sm2_core — types, logging, small utilities
# ---------------------------------------------------------------------------

add_library(sm2_core STATIC
    core/config.cpp
    core/log.cpp
)
target_include_directories(sm2_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sm2_core PRIVATE sm2_warnings)

# ---------------------------------------------------------------------------
# sm2_render_vk — Vulkan backend
# ---------------------------------------------------------------------------
# Renamed from sm2_render (phase 8) now that a second render library exists
# (sm2_render_gl, below): every file this target lists is exactly where it
# was before, this is a rename for symmetry, not a reorganisation.

add_library(sm2_render_vk STATIC
    render/geometry.cpp
    render/vk/context.cpp
    render/vk/frame_capture.cpp
    render/vk/poly3d_pass.cpp
    render/vk/present_pass.cpp
    render/vk/tilemap_pass.cpp
    render/vk/vk_common.cpp
    render/vk/vma_impl.cpp
    render/vk/vulkan_backend.cpp
)
# sm2_hw is a dependency of the 3D pass alone, which reads the geometry engine's
# output and asks the video stage to resolve each polygon's colour. It is private
# because nothing in this library's headers exposes those types by value.
target_link_libraries(sm2_render_vk
    PUBLIC
        sm2_core
        sm2_shaders
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
    PRIVATE
        sm2_warnings
        sm2_hw
        sm2_imgui
        SDL3::SDL3
)

# ---------------------------------------------------------------------------
# sm2_render_gl — OpenGL 4.3 core desktop backend (phase 9)
# ---------------------------------------------------------------------------
# render/geometry.cpp is not listed here: it is sm2_render_vk's, and this
# target links that library to reach it rather than compiling a second copy
# -- see render/geometry.h's own doc comment for why that logic must not be
# duplicated per backend.

if(SM2_BUILD_OPENGL_DESKTOP OR SM2_BUILD_OPENGL_ES)
    if(SM2_BUILD_OPENGL_DESKTOP AND SM2_BUILD_OPENGL_ES)
        message(FATAL_ERROR
            "SM2_BUILD_OPENGL_DESKTOP and SM2_BUILD_OPENGL_ES are mutually "
            "exclusive. Enable one or the other, not both.")
    endif()

    add_library(sm2_render_gl STATIC
        render/gl/gl_backend.cpp
        render/gl/gl_common.cpp
        render/gl/gl_context.cpp
        render/gl/gl_frame_capture.cpp
        render/gl/gl_poly3d_pass.cpp
        render/gl/gl_present_pass.cpp
        render/gl/gl_tilemap_pass.cpp
    )
    # sm2_hw is not linked here even though several files above name
    # hw::Model2MachineBase/Model2Video (headers only -- nothing here links
    # against sm2_hw's compiled symbols directly, the same way
    # render/vk/context.h forward-declares osd::Window without
    # sm2_render_vk linking sm2_osd). sm2-emu's own link line already
    # supplies sm2_hw; adding it again here surfaces libsm2_hw.a twice on
    # that link line (confirmed directly: this produced a real "ignoring
    # duplicate libraries" warning once, fixed by removing the redundant
    # link rather than by accepting the warning as unavoidable).
    #
    # sm2_render_vk *is* linked, PRIVATE: gl_poly3d_pass.cpp calls
    # render::triangulate(), which lives there rather than being duplicated
    # per backend (render/geometry.h's own doc comment explains why). This
    # does reintroduce the benign "ignoring duplicate libraries" linker
    # warning the note above describes fixing once already -- checked
    # directly rather than assumed fixed: for a *static* library, CMake must
    # still place a PRIVATE dependency's own link libraries onto the final
    # executable's link line (a static archive cannot resolve its own
    # undefined symbols, so every archive in the chain has to reach the
    # final linker regardless of PRIVATE/PUBLIC), so libsm2_hw.a legitimately
    # appears via both sm2_render_vk and sm2-emu's own direct link now that
    # sm2_render_gl also depends on sm2_render_vk. The warning is cosmetic
    # (confirmed: ctest and a live run both succeed with it present) and not
    # worth restructuring the link graph further to silence.
    target_link_libraries(sm2_render_gl
        PUBLIC
            sm2_core
            sm2_shaders
        PRIVATE
            sm2_warnings
            sm2_render_vk
            $<$<BOOL:${SM2_BUILD_OPENGL_ES}>:sm2_imgui_gles>
            $<$<BOOL:${SM2_BUILD_OPENGL_DESKTOP}>:sm2_imgui_gl>
            SDL3::SDL3
    )
    if(SM2_BUILD_OPENGL_ES)
        target_compile_definitions(sm2_render_gl PRIVATE SM2_FORCE_GLES=1)
    endif()
endif()

# ---------------------------------------------------------------------------
# sm2_cpu — CPU cores ported from MAME
# ---------------------------------------------------------------------------
# Held to the relaxed warning profile: these files are kept textually close to
# their MAME originals so upstream fixes stay diffable, and reformatting them to
# satisfy our own warning set would destroy that.

add_library(sm2_cpu STATIC
    cpu/i960/i960.cpp
    cpu/mb86233/mb86233.cpp
    cpu/mb86235/mb86235.cpp
    cpu/sharc/sharc.cpp
    cpu/z80/z80.cpp
)
target_link_libraries(sm2_cpu
    PUBLIC
        sm2_core
    PRIVATE
        sm2_warnings_ported
)

# ---------------------------------------------------------------------------
# sm2_cpu_m68000 — the sound board's 68000
# ---------------------------------------------------------------------------
# Separate from sm2_cpu because its provenance is different: the core is Musashi
# (MIT) rather than MAME, and the glue here is ours, so unlike sm2_cpu's contents
# it is held to the strict warning profile.

add_library(sm2_cpu_m68000 STATIC
    cpu/m68000/m68000.cpp
)
target_link_libraries(sm2_cpu_m68000
    PUBLIC
        sm2_core
        sm2_musashi
    PRIVATE
        sm2_warnings
)

# ---------------------------------------------------------------------------
# sm2_rom — game database and ROM loading
# ---------------------------------------------------------------------------

add_library(sm2_rom STATIC
    rom/game.cpp
    rom/game_db.cpp
    rom/rom_loader.cpp
    rom/rom_set.cpp
)
target_link_libraries(sm2_rom
    PUBLIC
        sm2_core
    PRIVATE
        sm2_warnings
        sm2_miniz
        sm2_7z
        pugixml::static
)

# ---------------------------------------------------------------------------
# sm2_hw_ported — hardware emulation kept textually close to its MAME original
# ---------------------------------------------------------------------------
# Same reasoning as sm2_cpu: these files are deliberately not reformatted to
# satisfy our own warning set, because doing so would destroy the ability to diff
# them against upstream when MAME fixes something.

add_library(sm2_hw_ported STATIC
    hw/geometrizer.cpp
    hw/multipcm.cpp
    hw/scsp.cpp
    hw/scsp_dsp.cpp
)
target_link_libraries(sm2_hw_ported
    PUBLIC
        sm2_core
    PRIVATE
        sm2_warnings_ported
)

# ---------------------------------------------------------------------------
# sm2_hw — the machine: memory map, timers, interrupts, I/O
# ---------------------------------------------------------------------------

add_library(sm2_hw STATIC
    hw/copro_fifo.cpp
    hw/copro_tgp.cpp
    hw/eeprom_93c46.cpp
    hw/i8251.cpp
    hw/io_315_5649.cpp
    hw/m1audio.cpp
    hw/m2comm.cpp
    hw/machine_factory.cpp
    hw/mb8421.cpp
    hw/model1io.cpp
    hw/model1io2.cpp
    hw/model2.cpp
    hw/model2b.cpp
    hw/model2c.cpp
    hw/model2_debug.cpp
    hw/model2_original.cpp
    hw/model2_sound.cpp
    hw/model2_softrender.cpp
    hw/model2_video.cpp
    hw/msm6253.cpp
    hw/sega_315_5338a.cpp
    hw/sega_315_5838_comp.cpp
    hw/sega_315_5881_crypt.cpp
    hw/segaic24.cpp
    hw/ym3438.cpp
    hw/z80ctc.cpp
    hw/z80pio.cpp
)
target_link_libraries(sm2_hw
    PUBLIC
        sm2_core
        sm2_cpu
        sm2_cpu_m68000
        sm2_hw_ported
        sm2_rom
        # ymfm is the FM synthesis behind hw/ym3438; PUBLIC because ym3438.h
        # exposes ymfm::ym3438 by value.
        sm2_ymfm
    PRIVATE
        sm2_warnings
)

# ---------------------------------------------------------------------------
# sm2_osd — window, input, audio, config
# ---------------------------------------------------------------------------

add_library(sm2_osd STATIC
    osd/audio.cpp
    osd/frame_pacer.cpp
    osd/gui.cpp
    osd/input.cpp
    osd/window.cpp
)
target_link_libraries(sm2_osd
    PUBLIC
        sm2_core
        sm2_hw
        sm2_imgui
        SDL3::SDL3
    PRIVATE
        sm2_warnings
)

# ---------------------------------------------------------------------------
# sm2-emu — the executable
# ---------------------------------------------------------------------------

add_executable(sm2-emu
    main.cpp
)
# sm2_core is not listed: sm2_osd, sm2_render_vk and sm2_hw all expose it
# PUBLIC, and naming it again makes the linker warn about a duplicate
# archive. sm2_hw brings in sm2_cpu and sm2_rom the same way.
target_link_libraries(sm2-emu
    PRIVATE
        sm2_osd
        sm2_render_vk
        sm2_hw
        sm2_warnings
)
if(SM2_BUILD_OPENGL_DESKTOP OR SM2_BUILD_OPENGL_ES)
    target_link_libraries(sm2-emu PRIVATE sm2_render_gl)
endif()

target_compile_definitions(sm2-emu PRIVATE
    $<$<BOOL:${SM2_BUILD_OPENGL_DESKTOP}>:SM2_HAVE_OPENGL_DESKTOP=1>
    $<$<BOOL:${SM2_BUILD_OPENGL_ES}>:SM2_HAVE_OPENGL_ES=1>
)

# Keep games.xml beside the binary so a freshly built tree runs without any
# environment set up. GameDatabase::locate also looks in ../share/sm2-emu for
# installed builds.
add_custom_command(TARGET sm2-emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/games.xml"
            "$<TARGET_FILE_DIR:sm2-emu>/games.xml"
    COMMENT "Staging games.xml"
    VERBATIM
)

target_compile_definitions(sm2-emu PRIVATE
    SM2_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${SM2_ENABLE_VALIDATION}>:SM2_ENABLE_VALIDATION=1>
)

# On macOS the Vulkan loader is not in a default search path, so record where
# it was found. Without this the binary only runs from a shell that has sourced
# the SDK's setup-env.sh.
if(SM2_PLATFORM_MACOS)
    get_target_property(sm2_vulkan_library Vulkan::Vulkan IMPORTED_LOCATION)
    if(sm2_vulkan_library)
        get_filename_component(sm2_vulkan_dir "${sm2_vulkan_library}" DIRECTORY)
        set_target_properties(sm2-emu PROPERTIES
            BUILD_RPATH   "${sm2_vulkan_dir}"
            INSTALL_RPATH "${sm2_vulkan_dir}")
    endif()
endif()
