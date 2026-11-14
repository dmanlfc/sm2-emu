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

# ---------------------------------------------------------------------------
# sm2_core — types, logging, small utilities
# ---------------------------------------------------------------------------

add_library(sm2_core STATIC
    core/archive.cpp
    core/config.cpp
    core/cpu_topology.cpp
    core/log.cpp
    core/net.cpp
)
target_include_directories(sm2_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sm2_core PRIVATE sm2_warnings)
# Winsock for core/net.cpp. net.cpp also names these via #pragma comment(lib),
# but that is MSVC-only; naming them here covers clang-cl and MinGW too.
if(WIN32)
    target_link_libraries(sm2_core PUBLIC ws2_32 iphlpapi)
endif()

# ---------------------------------------------------------------------------
# sm2_render — backend-neutral render support
# ---------------------------------------------------------------------------
# Every backend needs render/geometry.cpp, so it lives here rather than in a
# GPU backend target.

add_library(sm2_render STATIC
    render/geometry.cpp
    render/image_write.cpp
    render/model3/frame_plan.cpp
    render/model3/model3_frame.cpp
    render/model3/r3d_math.cpp
    render/model3/r3d_model.cpp
    render/model3/scene.cpp
    render/model3/soft_backend.cpp
    render/model3/soft_render.cpp
    render/model3/texture_dump.cpp
    render/model3/texture_key.cpp
    render/model3/textures.cpp
    render/model3/tilegen.cpp
    render/panorama.cpp
    render/texture_dump.cpp
    render/texture_key.cpp
    render/texture_replace.cpp
)
target_link_libraries(sm2_render
    PUBLIC
        sm2_core
    PRIVATE
        sm2_warnings
        sm2_hw
        sm2_miniz
        sm2_stb
)

# ---------------------------------------------------------------------------
# sm2_render_vk — Vulkan backend (opt-in: SM2_BUILD_VULKAN)
# ---------------------------------------------------------------------------

if(SM2_BUILD_VULKAN)
    add_library(sm2_render_vk STATIC
        render/vk/context.cpp
        render/vk/frame_capture.cpp
        render/vk/model3_pass.cpp
        render/vk/poly3d_pass.cpp
        render/vk/present_pass.cpp
        render/vk/tilemap_pass.cpp
        render/vk/vk_common.cpp
        render/vk/vma_impl.cpp
        render/vk/vulkan_backend.cpp
    )
    # sm2_hw is needed only by the 3D pass, and no header here exposes its
    # types by value, so it is private.
    target_link_libraries(sm2_render_vk
        PUBLIC
            sm2_core
            sm2_render
            sm2_shaders
            Vulkan::Vulkan
            GPUOpen::VulkanMemoryAllocator
        PRIVATE
            sm2_warnings
            sm2_hw
            sm2_imgui_vk
            SDL3::SDL3
    )
    # GCC 12-14 emit a spurious -Wfree-nonheap-object inside std::vector when it
    # over-inlines allocate/deallocate at -O2+.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(sm2_render_vk PRIVATE -Wno-free-nonheap-object)
    endif()
endif()

# ---------------------------------------------------------------------------
# sm2_render_gl — OpenGL 4.3 core desktop backend
# ---------------------------------------------------------------------------

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
        render/gl/model3_pass.cpp
    )
    # sm2_hw is used through forward declarations only; keisho's own link line
    # supplies it.
    target_link_libraries(sm2_render_gl
        PUBLIC
            sm2_core
            sm2_shaders
        PRIVATE
            sm2_warnings
            sm2_render
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
# Held to the relaxed warning profile: these files stay textually close to
# their MAME originals so upstream fixes remain diffable.

add_library(sm2_cpu STATIC
    cpu/i960/i960.cpp
    cpu/mb86233/mb86233.cpp
    cpu/mb86235/mb86235.cpp
    cpu/sharc/sharc.cpp
    cpu/v60/v60.cpp
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
# Separate from sm2_cpu because the core is Musashi (MIT) rather than MAME and
# the glue is ours, so it is held to the strict warning profile.

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
# sm2_cpu_jit — JIT infrastructure shared by the dynamically recompiled cores
# ---------------------------------------------------------------------------
# Both emitters are built on every host, whichever one it runs.

add_library(sm2_cpu_jit STATIC
    cpu/jit/arm64_emitter.cpp
    cpu/jit/block_cache.cpp
    cpu/jit/code_buffer.cpp
    cpu/jit/dispatcher.cpp
    cpu/jit/guest_memory.cpp
    cpu/jit/x64_emitter.cpp
)
target_link_libraries(sm2_cpu_jit
    PUBLIC
        sm2_core
    PRIVATE
        sm2_warnings
)

# ---------------------------------------------------------------------------
# sm2_cpu_ppc — the Model 3 PowerPC 603e / 603ev
# ---------------------------------------------------------------------------

add_library(sm2_cpu_ppc STATIC
    cpu/ppc/ppc.cpp
    cpu/ppc/ppc_decode.cpp
    cpu/ppc/ppc_helper.cpp
    cpu/ppc/ppc_interp.cpp
    cpu/ppc/ppc_jit_arm64.cpp
    cpu/ppc/ppc_jit_x64.cpp
)
target_link_libraries(sm2_cpu_ppc
    PUBLIC
        sm2_core
    PRIVATE
        sm2_cpu_jit
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
# Relaxed warning profile, for the same reason as sm2_cpu.

add_library(sm2_hw_ported STATIC
    hw/geometrizer.cpp
    hw/mpeg_audio.cpp
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
    hw/comm_udp.cpp
    hw/copro_fifo.cpp
    hw/copro_tgp.cpp
    hw/dsb2.cpp
    hw/dsbz80.cpp
    hw/eeprom_93c46.cpp
    hw/i8251.cpp
    hw/io_315_5649.cpp
    hw/m1audio.cpp
    hw/m1comm.cpp
    hw/m2comm.cpp
    hw/mb8421.cpp
    hw/model1io.cpp
    hw/model1io2.cpp
    hw/model2.cpp
    hw/model2b.cpp
    hw/model2c.cpp
    hw/model2_debug.cpp
    hw/model1.cpp
    hw/model1_geometry.cpp
    hw/model1_tgp.cpp
    hw/model2_original.cpp
    hw/model2_sound.cpp
    hw/model2_softrender.cpp
    hw/model2_softrender_async.cpp
    hw/model2_video.cpp
    hw/msm6253.cpp
    hw/rewind.cpp
    hw/save_state_io.cpp
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
        sm2_miniz
)

# ---------------------------------------------------------------------------
# sm2_hw_model3 — the Sega Model 3 machine, its chips and its sound board
# ---------------------------------------------------------------------------
# Builds on sm2_hw (EEPROM, 315-5881, link board). create_machine lives in
# sm2_machines below so the two do not depend on each other.

add_library(sm2_hw_model3 STATIC
    hw/model3/drive_board.cpp
    hw/model3/jtag.cpp
    hw/model3/model3.cpp
    hw/model3/model3_cpu_ppc.cpp
    hw/model3/model3_sound.cpp
    hw/model3/mpc10x.cpp
    hw/model3/net_board.cpp
    hw/model3/real3d.cpp
    hw/model3/rtc72421.cpp
    hw/model3/scheduler.cpp
    hw/model3/scsi_53c810.cpp
    hw/model3/tilegen.cpp
)
target_link_libraries(sm2_hw_model3
    PUBLIC
        sm2_core
        sm2_hw
        sm2_rom
    PRIVATE
        sm2_warnings
        sm2_miniz
)

# sm2_machines — create_machine(), which constructs every board
add_library(sm2_machines STATIC hw/machine_factory.cpp)
target_link_libraries(sm2_machines
    PUBLIC
        sm2_hw
        sm2_hw_model3
    PRIVATE
        sm2_warnings
)

# Deferred so it does not depend on where sm2_cpu_ppc is defined in this file.
function(sm2_model3_link_ppc)
    if(TARGET sm2_cpu_ppc)
        target_compile_definitions(sm2_hw_model3 PRIVATE SM2_MODEL3_HAVE_PPC=1)
        target_link_libraries(sm2_hw_model3 PUBLIC sm2_cpu_ppc)
    endif()
endfunction()
cmake_language(DEFER CALL sm2_model3_link_ppc)

# Without the SoundPort adapter the machine runs silent.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hw/model3/model3_sound_port.cpp")
    target_sources(sm2_hw_model3 PRIVATE hw/model3/model3_sound_port.cpp)
    target_compile_definitions(sm2_hw_model3 PRIVATE SM2_MODEL3_HAVE_SOUND=1)
endif()

# ---------------------------------------------------------------------------
# sm2_osd — window, input, audio, config
# ---------------------------------------------------------------------------

add_library(sm2_osd STATIC
    osd/audio.cpp
    osd/outputs.cpp
    osd/daytona_board.cpp
    osd/drive_command.cpp
    osd/frame_pacer.cpp
    osd/gui.cpp
    osd/i18n.cpp
    osd/indy_board.cpp
    osd/input.cpp
    osd/scraper.cpp
    osd/ui_text.cpp
    osd/ui/kit.cpp
    osd/ui/theme.cpp
    osd/wheel_ffb.cpp
    osd/window.cpp
)
# The host's application menu: a "Settings..." item on macOS, nothing elsewhere.
if(SM2_PLATFORM_MACOS)
    enable_language(OBJCXX)
    target_sources(sm2_osd PRIVATE osd/app_menu_macos.mm)
    target_link_libraries(sm2_osd PRIVATE "-framework Cocoa")
else()
    target_sources(sm2_osd PRIVATE osd/app_menu_none.cpp)
endif()
# System font lookup for the overlay's translations, per platform.
if(SM2_HAVE_FONTCONFIG)
    target_sources(sm2_osd PRIVATE osd/sysfont_fontconfig.cpp)
    target_link_libraries(sm2_osd PRIVATE PkgConfig::FONTCONFIG)
elseif(SM2_PLATFORM_MACOS)
    target_sources(sm2_osd PRIVATE osd/sysfont_coretext.cpp)
    target_link_libraries(sm2_osd PRIVATE "-framework CoreText" "-framework CoreFoundation")
elseif(SM2_PLATFORM_WINDOWS)
    target_sources(sm2_osd PRIVATE osd/sysfont_dwrite.cpp)
    target_link_libraries(sm2_osd PRIVATE dwrite)
else()
    target_sources(sm2_osd PRIVATE osd/sysfont_none.cpp)
endif()
# The overlay's assets, embedded as byte arrays: the Gamepad page's pad
# picture and the menu's font (Noto Sans, Latin/Greek/Cyrillic subset).
set(SM2_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(asset pad_xbox_outline.png notosans_ui.ttf)
    get_filename_component(symbol "${asset}" NAME_WE)
    set(asset_in  "${CMAKE_CURRENT_SOURCE_DIR}/osd/assets/${asset}")
    set(asset_out "${SM2_GENERATED_DIR}/assets/${symbol}.h")
    add_custom_command(
        OUTPUT  "${asset_out}"
        COMMAND ${CMAKE_COMMAND} -DSM2_IN=${asset_in} -DSM2_OUT=${asset_out}
                -DSM2_SYMBOL=${symbol} -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
        DEPENDS "${asset_in}" "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
        COMMENT "Embedding ${asset}")
    target_sources(sm2_osd PRIVATE "${asset_out}")
endforeach()
target_include_directories(sm2_osd PRIVATE "${SM2_GENERATED_DIR}")
if(SM2_HAVE_EVDEV)
    target_sources(sm2_osd PRIVATE osd/evdev_gun.cpp)
    target_compile_definitions(sm2_osd PUBLIC SM2_HAVE_EVDEV=1 SM2_HAVE_LIGHTGUNS=1)
    target_link_libraries(sm2_osd PRIVATE PkgConfig::UDEV)
elseif(SM2_PLATFORM_WINDOWS)
    target_sources(sm2_osd PRIVATE osd/rawinput_gun.cpp)
    target_compile_definitions(sm2_osd PUBLIC SM2_HAVE_LIGHTGUNS=1)
    target_link_libraries(sm2_osd PRIVATE hid cfgmgr32)
endif()
target_link_libraries(sm2_osd
    PUBLIC
        sm2_core
        sm2_hw
        sm2_imgui
        SDL3::SDL3
    PRIVATE
        sm2_render
        sm2_stb
        sm2_harfbuzz
        sm2_sheenbidi
        sm2_warnings
)
# libcurl for the artwork scraper; absent -> scraper compiles to a no-op.
if(SM2_HAVE_CURL)
    target_link_libraries(sm2_osd PRIVATE CURL::libcurl)
    target_compile_definitions(sm2_osd PRIVATE SM2_HAVE_CURL=1)
endif()

# ---------------------------------------------------------------------------
# Keisho — the executable
# ---------------------------------------------------------------------------

add_executable(keisho
    main.cpp
)

# On macOS build a .app bundle so Finder and the Dock show the icon.
if(SM2_PLATFORM_MACOS)
    set(SM2_MACOS_ICNS "${CMAKE_SOURCE_DIR}/data/icons/keisho.icns")
    set_target_properties(keisho PROPERTIES
        OUTPUT_NAME   "Keisho"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST      "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME     "Keisho"
        MACOSX_BUNDLE_GUI_IDENTIFIER  "io.github.keisho-emu.keisho"
        MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE       "keisho.icns")
    # Package the icns and the ROM database into Contents/Resources so the
    # bundle is self-contained.
    target_sources(keisho PRIVATE
        "${SM2_MACOS_ICNS}"
        "${CMAKE_SOURCE_DIR}/data/games.xml")
    set_source_files_properties(
        "${SM2_MACOS_ICNS}"
        "${CMAKE_SOURCE_DIR}/data/games.xml"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    # The overlay's translations, in Contents/Resources/lang.
    file(GLOB SM2_LANG_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/lang/*.po")
    target_sources(keisho PRIVATE ${SM2_LANG_FILES})
    set_source_files_properties(${SM2_LANG_FILES}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/lang")
    # Fonts for scripts a system may lack, in Contents/Resources/lang/fonts.
    file(GLOB SM2_LANG_FONTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/lang/fonts/*")
    target_sources(keisho PRIVATE ${SM2_LANG_FONTS})
    set_source_files_properties(${SM2_LANG_FONTS}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/lang/fonts")
elseif(SM2_PLATFORM_WINDOWS)
    # Compile the .ico into the .exe so Explorer and the taskbar show it. The
    # running window's icon is set in osd/window.cpp.
    set(SM2_WIN_ICON "${CMAKE_SOURCE_DIR}/data/icons/keisho.ico")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/keisho.rc.in"
        "${CMAKE_BINARY_DIR}/keisho.rc"
        @ONLY)
    target_sources(keisho PRIVATE "${CMAKE_BINARY_DIR}/keisho.rc")
    # Delay-load the Vulkan loader: an old or missing vulkan-1.dll lacks the
    # 1.3 exports and would stop the exe starting, even for OpenGL/software.
    if(MSVC AND SM2_BUILD_VULKAN)
        target_link_options(keisho PRIVATE "/DELAYLOAD:vulkan-1.dll")
        target_link_libraries(keisho PRIVATE delayimp)
    endif()
endif()
# sm2_core, sm2_cpu and sm2_rom come in PUBLIC through sm2_osd and sm2_hw;
# naming them again makes the linker warn about duplicate archives.
target_link_libraries(keisho
    PRIVATE
        sm2_osd
        sm2_render
        sm2_hw
        sm2_hw_model3
        sm2_machines
        sm2_warnings
)
if(SM2_BUILD_VULKAN)
    target_link_libraries(keisho PRIVATE sm2_render_vk)
endif()
if(SM2_BUILD_OPENGL_DESKTOP OR SM2_BUILD_OPENGL_ES)
    target_link_libraries(keisho PRIVATE sm2_render_gl)
endif()

target_compile_definitions(keisho PRIVATE
    $<$<BOOL:${SM2_BUILD_VULKAN}>:SM2_HAVE_VULKAN=1>
    $<$<BOOL:${SM2_BUILD_OPENGL_DESKTOP}>:SM2_HAVE_OPENGL_DESKTOP=1>
    $<$<BOOL:${SM2_BUILD_OPENGL_ES}>:SM2_HAVE_OPENGL_ES=1>
)

# Keep games.xml beside the binary so a freshly built tree runs as is. The
# macOS bundle carries it in Contents/Resources instead.
if(NOT SM2_PLATFORM_MACOS)
    add_custom_command(TARGET keisho POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/data/games.xml"
                "$<TARGET_FILE_DIR:keisho>/games.xml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/data/lang"
                "$<TARGET_FILE_DIR:keisho>/lang"
        COMMENT "Staging games.xml and translations"
        VERBATIM
    )
endif()

target_compile_definitions(sm2_osd PRIVATE SM2_VERSION="${PROJECT_VERSION}")
target_compile_definitions(keisho PRIVATE
    SM2_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${SM2_ENABLE_VALIDATION}>:SM2_ENABLE_VALIDATION=1>
)

# On macOS the Vulkan loader is not in a default search path, so record where
# it was found. Without this the binary only runs from a shell that has sourced
# the SDK's setup-env.sh.
if(SM2_PLATFORM_MACOS AND SM2_BUILD_VULKAN)
    get_target_property(sm2_vulkan_library Vulkan::Vulkan IMPORTED_LOCATION)
    if(sm2_vulkan_library)
        get_filename_component(sm2_vulkan_dir "${sm2_vulkan_library}" DIRECTORY)
        set_target_properties(keisho PROPERTIES
            BUILD_RPATH   "${sm2_vulkan_dir}"
            INSTALL_RPATH "${sm2_vulkan_dir}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
# `cmake --install` lays the emulator out in a standard prefix:
#   bin/keisho                          the executable
#   share/keisho/games.xml              the ROM database (GameDatabase::locate
#                                        searches ../share/keisho for it)
#   share/keisho/lang/*.po              the overlay's translations
#   share/keisho/lang/fonts/*.ttf       fonts for scripts the system may lack
# plus the desktop-integration icons per platform (below). The in-window icon
# is compiled in (src/osd/app_icon.h).

include(GNUInstallDirs)

set(SM2_ICON_DIR "${CMAKE_SOURCE_DIR}/data/icons")

if(SM2_PLATFORM_MACOS)
    # The .app is self-contained and needs nothing alongside it.
    install(TARGETS keisho BUNDLE DESTINATION ".")
elseif(SM2_PLATFORM_WINDOWS)
    # Keep games.xml beside the .exe (GameDatabase::locate checks the exe dir)
    # so the install prefix is one self-contained folder to zip and run.
    install(TARGETS keisho RUNTIME DESTINATION ".")
    install(FILES "${CMAKE_SOURCE_DIR}/data/games.xml" DESTINATION ".")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/lang" DESTINATION "."
            FILES_MATCHING PATTERN "*.po" PATTERN "*.ttf" PATTERN "OFL.txt")
    install(FILES "${SM2_ICON_DIR}/keisho.ico" DESTINATION ".")
else()
    install(TARGETS keisho RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${CMAKE_SOURCE_DIR}/data/games.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/keisho")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/lang"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/keisho"
            FILES_MATCHING PATTERN "*.po" PATTERN "*.ttf" PATTERN "OFL.txt")
endif()

if(SM2_PLATFORM_LINUX)
    # Freedesktop hicolor icon theme: one PNG per size, named after the
    # .desktop file's Icon key.
    foreach(sz 48 128 256 512)
        install(FILES "${SM2_ICON_DIR}/keisho-${sz}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps"
                RENAME "keisho.png")
    endforeach()
    # Scalable slot: our SVG carries the exact artwork, so hand it to themes
    # that prefer scalable icons.
    install(FILES "${SM2_ICON_DIR}/keisho.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

    # A .desktop launcher so the icon and app show up in the applications menu.
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/keisho.desktop.in"
        "${CMAKE_BINARY_DIR}/keisho.desktop"
        @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/keisho.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()

# macOS needs nothing extra here: the icon (keisho.icns) and games.xml are
# packaged inside the .app bundle above.
