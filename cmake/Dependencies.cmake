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

# External dependency resolution.
#
# Policy: prefer whatever the system provides (which is what distro packaging
# wants on Linux, the primary target). When a dependency is absent, fall back
# to the copy vendored under 3rdparty/ (git submodules, plus the LZMA SDK whose
# C sources are committed since it has no git repository). Nothing is fetched
# from the network at configure time, so a recursive checkout builds unattended
# and a cross build needs only its sysroot.

set(SM2_3RDPARTY "${CMAKE_SOURCE_DIR}/3rdparty")

# Guard against a non-recursive clone: the vendored fallbacks are submodules.
if(NOT EXISTS "${SM2_3RDPARTY}/musashi/m68kmake.c")
    message(FATAL_ERROR
        "3rdparty submodules are missing. Clone with --recurse-submodules, or run:\n"
        "  git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# Vulkan — headers + loader library
# ---------------------------------------------------------------------------
# find_package(Vulkan) locates both the Vulkan headers (vulkan/vulkan.h) and
# the loader library (libvulkan.so / libvulkan.dylib / vulkan-1.lib). It
# provides the Vulkan::Vulkan imported target that carries both the include
# path and the link library. We link the loader rather than using volk: there
# is one device and one thread issuing commands, so the indirection volk saves
# is not worth the extra moving part.
#
# The Vulkan headers and loader are only required when the Vulkan backend is
# actually being built (SM2_BUILD_VULKAN). A software / OpenGL-only build --
# the default, and what a lower-end ARM board with no Vulkan driver wants --
# needs neither. glslc (below) is still required regardless, because it also
# lints the GL shader dialect.
if(SM2_BUILD_VULKAN)
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR
            "SM2_BUILD_VULKAN is ON but the Vulkan headers and loader were not found.\n"
            "\n"
            "The Vulkan backend requires:\n"
            "  - Vulkan 1.3 headers  (vulkan/vulkan.h)\n"
            "  - Vulkan loader       (libvulkan)\n"
            "\n"
            "Install with:\n"
            "  Debian/Ubuntu : sudo apt install libvulkan-dev vulkan-validationlayers\n"
            "  Fedora        : sudo dnf install vulkan-devel vulkan-validation-layers\n"
            "  Arch          : sudo pacman -S vulkan-devel vulkan-validation-layers\n"
            "  macOS         : brew install vulkan-headers vulkan-loader molten-vk\n"
            "                  (or install the LunarG Vulkan SDK and source its setup-env.sh)\n"
            "\n"
            "For cross-compilation (buildroot, Yocto), ensure the sysroot contains\n"
            "the Vulkan headers and loader for the target, and set CMAKE_FIND_ROOT_PATH\n"
            "or Vulkan_INCLUDE_DIR / Vulkan_LIBRARY explicitly.\n"
            "\n"
            "Or build without it: -DSM2_BUILD_VULKAN=OFF (software + OpenGL only).")
    endif()
endif()

# glslc compiles GLSL to SPIR-V at build time. Prefer the one shipped beside
# the loader, then anything on PATH.
find_program(SM2_GLSLC
    NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/bin"
        "${Vulkan_INCLUDE_DIR}/../bin"
    DOC "glslc GLSL-to-SPIR-V compiler"
)

if(NOT SM2_GLSLC)
    message(FATAL_ERROR
        "glslc was not found.\n"
        "It ships with the Vulkan SDK and with shaderc.\n"
        "  Debian/Ubuntu : apt install glslc            (or install the Vulkan SDK)\n"
        "  Fedora        : dnf install glslc\n"
        "  Arch          : pacman -S shaderc\n"
        "  macOS         : install the LunarG Vulkan SDK, then source its setup-env.sh\n"
        "If the SDK is installed, set VULKAN_SDK or pass -DSM2_GLSLC=/path/to/glslc.")
endif()

# ---------------------------------------------------------------------------
# SDL3 — window, input, audio
# ---------------------------------------------------------------------------

find_package(SDL3 3.2 CONFIG QUIET)

if(SDL3_FOUND)
    set(SM2_SDL3_ORIGIN "system (${SDL3_VERSION})")
else()
    set(SM2_SDL3_ORIGIN "vendored (3rdparty/SDL)")

    # Trim the build to what the emulator actually uses. SDL's own tests and
    # the shared library are pure overhead here.
    set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN        ON  CACHE BOOL "" FORCE)
    set(SDL_RENDER        OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)

    add_subdirectory("${SM2_3RDPARTY}/SDL" EXCLUDE_FROM_ALL SYSTEM)
endif()

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator (Vulkan backend only)
# ---------------------------------------------------------------------------
# Hand-rolled one-allocation-per-resource works for a fixed resource set, but
# the texture sheets and per-frame rings want suballocation, and VMA is the
# boring correct answer. Only needed when the Vulkan backend is built. VMA is
# header-only, so the fallback is just an include directory exposing the
# GPUOpen::VulkanMemoryAllocator target the source expects.

if(SM2_BUILD_VULKAN)
    find_package(VulkanMemoryAllocator CONFIG QUIET)
    if(NOT VulkanMemoryAllocator_FOUND)
        add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
        set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SM2_3RDPARTY}/VulkanMemoryAllocator/include")
    endif()
endif()

# ---------------------------------------------------------------------------
# miniz — zip reading and CRC32 for the ROM loader
# ---------------------------------------------------------------------------
# Prefer a system miniz (find_package(miniz) exports miniz::miniz); otherwise
# build it from the submodule. The rest of the tree links sm2_miniz and
# includes <miniz.h>.

find_package(miniz CONFIG QUIET)
if(miniz_FOUND)
    # The upstream install puts the header at include/miniz/miniz.h; expose
    # that subdirectory so the loader's #include <miniz.h> still resolves.
    add_library(sm2_miniz INTERFACE)
    target_link_libraries(sm2_miniz INTERFACE miniz::miniz)
    get_target_property(sm2_miniz_incs miniz::miniz INTERFACE_INCLUDE_DIRECTORIES)
    if(sm2_miniz_incs)
        foreach(dir IN LISTS sm2_miniz_incs)
            if(EXISTS "${dir}/miniz/miniz.h")
                target_include_directories(sm2_miniz SYSTEM INTERFACE "${dir}/miniz")
            endif()
        endforeach()
    endif()
else()
    # The git repository ships miniz split across four translation units, unlike
    # the amalgamated release archive. All four are compiled; miniz's own
    # CMakeLists is bypassed (it declares CMake < 3.5, which CMake 4 refuses).
    #
    # miniz.h includes "miniz_export.h", a visibility header miniz's CMake would
    # normally generate. For a static build the macros are empty, so write that
    # header ourselves rather than pulling in miniz's build.
    set(SM2_MINIZ_GEN "${CMAKE_BINARY_DIR}/miniz-gen")
    file(WRITE "${SM2_MINIZ_GEN}/miniz_export.h"
        "#ifndef MINIZ_EXPORT_H\n#define MINIZ_EXPORT_H\n"
        "#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n#endif\n")

    add_library(sm2_miniz STATIC
        "${SM2_3RDPARTY}/miniz/miniz.c"
        "${SM2_3RDPARTY}/miniz/miniz_tdef.c"
        "${SM2_3RDPARTY}/miniz/miniz_tinfl.c"
        "${SM2_3RDPARTY}/miniz/miniz_zip.c"
    )
    target_include_directories(sm2_miniz SYSTEM PUBLIC
        "${SM2_3RDPARTY}/miniz"
        "${SM2_MINIZ_GEN}"
    )
    set_target_properties(sm2_miniz PROPERTIES C_STANDARD 11)

    # We only read archives already in memory or on disk, and the ROM loader
    # does its own file handling, so the stdio and writing halves are dead
    # weight. This also avoids miniz's deprecated large-file paths.
    target_compile_definitions(sm2_miniz PUBLIC
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES  # do not shadow zlib's symbol names
    )
endif()

# ---------------------------------------------------------------------------
# LZMA SDK — 7z reading for the ROM loader
# ---------------------------------------------------------------------------
# Prefer a system LZMA SDK (find_package(lzmasdk) exports lzmasdk::lzmasdk);
# otherwise compile the subset the 7z reader needs from the vendored copy.
# The SDK ships as raw C with no git repository, so those sources are committed
# under 3rdparty/lzma-sdk rather than tracked as a submodule.

find_package(lzmasdk CONFIG QUIET)
if(lzmasdk_FOUND)
    add_library(sm2_7z INTERFACE)
    target_link_libraries(sm2_7z INTERFACE lzmasdk::lzmasdk)
else()
    add_library(sm2_7z STATIC
        "${SM2_3RDPARTY}/lzma-sdk/C/7zAlloc.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zArcIn.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zBuf.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zCrc.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zCrcOpt.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zDec.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zFile.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/7zStream.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/Bcj2.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/CpuArch.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/Lzma2Dec.c"
        "${SM2_3RDPARTY}/lzma-sdk/C/LzmaDec.c"
    )
    target_include_directories(sm2_7z SYSTEM PUBLIC "${SM2_3RDPARTY}/lzma-sdk/C")
    set_target_properties(sm2_7z PROPERTIES C_STANDARD 11)
    target_compile_definitions(sm2_7z PUBLIC
        Z7_NO_METHODS_FILTERS  # no Delta/BCJ/branch-conversion filters
    )
endif()

# ---------------------------------------------------------------------------
# pugixml — the games.xml database
# ---------------------------------------------------------------------------
# src/ links pugixml::static. A distro package may export only pugixml::pugixml
# (its shared library), so alias that to pugixml::static when the static target
# is absent -- the loader is read-only and does not care which it links.

find_package(pugixml CONFIG QUIET)
if(pugixml_FOUND)
    if(NOT TARGET pugixml::static AND TARGET pugixml::pugixml)
        add_library(pugixml::static ALIAS pugixml::pugixml)
    endif()
else()
    add_subdirectory("${SM2_3RDPARTY}/pugixml" EXCLUDE_FROM_ALL SYSTEM)
    if(NOT TARGET pugixml::static AND TARGET pugixml::pugixml)
        add_library(pugixml::static ALIAS pugixml::pugixml)
    elseif(NOT TARGET pugixml::static AND TARGET pugixml)
        add_library(pugixml::static ALIAS pugixml)
    endif()
endif()

# ---------------------------------------------------------------------------
# Dear ImGui — immediate-mode GUI for the settings overlay
# ---------------------------------------------------------------------------
# Prefer a system ImGui (find_package(imgui) exports imgui::imgui with the
# SDL3, OpenGL3 and Vulkan backends compiled in). Otherwise build it from the
# submodule. Either way the rest of the tree links the sm2_imgui* targets and
# includes <imgui.h> / <imgui_impl_*.h>.
#
# ImGui's renderer backends are not a normal installed library -- each is a
# self-contained translation unit a consumer usually compiles itself. When a
# system package has already compiled them into imgui::imgui, the four
# sm2_imgui* targets collapse to interface wrappers over it. The per-backend
# split still matters on the vendored path, so an SM2_BUILD_VULKAN=OFF build
# links no Vulkan.

find_package(imgui CONFIG QUIET)
if(imgui_FOUND)
    add_library(sm2_imgui INTERFACE)
    target_link_libraries(sm2_imgui INTERFACE imgui::imgui SDL3::SDL3)

    if(SM2_BUILD_VULKAN)
        add_library(sm2_imgui_vk INTERFACE)
        target_link_libraries(sm2_imgui_vk INTERFACE imgui::imgui Vulkan::Vulkan SDL3::SDL3)
        target_compile_definitions(sm2_imgui_vk INTERFACE
            IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING)
    endif()
    if(SM2_BUILD_OPENGL_DESKTOP)
        add_library(sm2_imgui_gl INTERFACE)
        target_link_libraries(sm2_imgui_gl INTERFACE imgui::imgui SDL3::SDL3)
    endif()
    if(SM2_BUILD_OPENGL_ES)
        add_library(sm2_imgui_gles INTERFACE)
        target_link_libraries(sm2_imgui_gles INTERFACE imgui::imgui SDL3::SDL3)
    endif()
else()
    set(SM2_IMGUI_DIR "${SM2_3RDPARTY}/imgui")

    # sm2_imgui -- core ImGui plus the SDL3 platform backend only. No renderer
    # backend and no Vulkan link: osd/gui.cpp uses only ImGui_ImplSDL3_* (the
    # platform half), and each GPU backend brings its own ImGui renderer backend
    # (sm2_imgui_vk / sm2_imgui_gl / sm2_imgui_gles). This is what lets a build
    # with SM2_BUILD_VULKAN=OFF carry no Vulkan dependency at all.
    add_library(sm2_imgui STATIC
        "${SM2_IMGUI_DIR}/imgui.cpp"
        "${SM2_IMGUI_DIR}/imgui_demo.cpp"
        "${SM2_IMGUI_DIR}/imgui_draw.cpp"
        "${SM2_IMGUI_DIR}/imgui_tables.cpp"
        "${SM2_IMGUI_DIR}/imgui_widgets.cpp"
        "${SM2_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    )
    target_include_directories(sm2_imgui SYSTEM PUBLIC
        "${SM2_IMGUI_DIR}"
        "${SM2_IMGUI_DIR}/backends"
    )
    target_link_libraries(sm2_imgui PUBLIC SDL3::SDL3)
    if(NOT MSVC)
        target_compile_options(sm2_imgui PRIVATE -w)
    endif()

    # sm2_imgui_vk -- ImGui's Vulkan renderer backend. Separate target so the
    # Vulkan link and the dynamic-rendering define stay out of the base
    # sm2_imgui. Only built when the Vulkan backend is.
    if(SM2_BUILD_VULKAN)
        add_library(sm2_imgui_vk STATIC
            "${SM2_IMGUI_DIR}/imgui.cpp"
            "${SM2_IMGUI_DIR}/imgui_demo.cpp"
            "${SM2_IMGUI_DIR}/imgui_draw.cpp"
            "${SM2_IMGUI_DIR}/imgui_tables.cpp"
            "${SM2_IMGUI_DIR}/imgui_widgets.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
        )
        target_include_directories(sm2_imgui_vk SYSTEM PUBLIC
            "${SM2_IMGUI_DIR}"
            "${SM2_IMGUI_DIR}/backends"
        )
        target_link_libraries(sm2_imgui_vk PUBLIC
            Vulkan::Vulkan
            SDL3::SDL3
        )
        # We link the loader directly and want the prototypes available, so the
        # dynamic-rendering path is enabled and VK_NO_PROTOTYPES is not set.
        target_compile_definitions(sm2_imgui_vk PRIVATE
            IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
        )
        if(NOT MSVC)
            target_compile_options(sm2_imgui_vk PRIVATE -w)
        endif()
    endif()

    # sm2_imgui_gl -- imgui's OpenGL3 renderer backend. imgui_impl_opengl3.cpp
    # is its own translation unit, so its bundled gl3w-derived loader cannot
    # collide with this project's own sm2::render::gl loader; imgui's own header
    # comment on that loader says exactly this. So no IMGUI_IMPL_OPENGL_LOADER_CUSTOM.
    if(SM2_BUILD_OPENGL_DESKTOP)
        add_library(sm2_imgui_gl STATIC
            "${SM2_IMGUI_DIR}/imgui.cpp"
            "${SM2_IMGUI_DIR}/imgui_demo.cpp"
            "${SM2_IMGUI_DIR}/imgui_draw.cpp"
            "${SM2_IMGUI_DIR}/imgui_tables.cpp"
            "${SM2_IMGUI_DIR}/imgui_widgets.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
        )
        target_include_directories(sm2_imgui_gl SYSTEM PUBLIC
            "${SM2_IMGUI_DIR}"
            "${SM2_IMGUI_DIR}/backends"
        )
        target_link_libraries(sm2_imgui_gl PUBLIC SDL3::SDL3)
        if(NOT MSVC)
            target_compile_options(sm2_imgui_gl PRIVATE -w)
        endif()
    endif()

    # sm2_imgui_gles -- same as sm2_imgui_gl but for the GLES 3.1 backend.
    # imgui_impl_opengl3.cpp supports ES out of the box (it detects the ES
    # context at runtime and uses its own bundled loader regardless of platform).
    if(SM2_BUILD_OPENGL_ES)
        add_library(sm2_imgui_gles STATIC
            "${SM2_IMGUI_DIR}/imgui.cpp"
            "${SM2_IMGUI_DIR}/imgui_demo.cpp"
            "${SM2_IMGUI_DIR}/imgui_draw.cpp"
            "${SM2_IMGUI_DIR}/imgui_tables.cpp"
            "${SM2_IMGUI_DIR}/imgui_widgets.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            "${SM2_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
        )
        target_include_directories(sm2_imgui_gles SYSTEM PUBLIC
            "${SM2_IMGUI_DIR}"
            "${SM2_IMGUI_DIR}/backends"
        )
        target_link_libraries(sm2_imgui_gles PUBLIC SDL3::SDL3)
        if(NOT MSVC)
            target_compile_options(sm2_imgui_gles PRIVATE -w)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Musashi — the 68000 in the sound board
# ---------------------------------------------------------------------------
# The one piece of hardware emulation not taken from MAME, and deliberately so.
# Musashi is a self-contained C library designed to be embedded behind memory
# callbacks, is MIT licensed, and is a fully documented commodity part, so
# unlike the geometry engine or the SCSP there is no reverse-engineering insight
# in one implementation over another. Vendored as a submodule; it has no system
# package form.
#
# The sources are copied into the build tree rather than compiled where they
# live, because m68kmake writes its generated output next to its input and we do
# not want to dirty the submodule.
#
# The directory layout has to be preserved: m68kcpu.c #includes m68kfpu.c and
# m68kmmu.h textually, m68kcpu.h #includes softfloat/softfloat.h, and
# softfloat.c #includes ../m68kcpu.h. Hence m68kfpu.c and m68kmmu.h are copied
# but are NOT library sources.
set(SM2_MUSASHI_SRC "${SM2_3RDPARTY}/musashi")
set(SM2_MUSASHI_DIR "${CMAKE_BINARY_DIR}/musashi")
file(MAKE_DIRECTORY "${SM2_MUSASHI_DIR}")
file(COPY
        "${SM2_MUSASHI_SRC}/m68k.h"
        "${SM2_MUSASHI_SRC}/m68kconf.h"
        "${SM2_MUSASHI_SRC}/m68kcpu.h"
        "${SM2_MUSASHI_SRC}/m68kcpu.c"
        "${SM2_MUSASHI_SRC}/m68kmmu.h"
        "${SM2_MUSASHI_SRC}/m68kfpu.c"
        "${SM2_MUSASHI_SRC}/m68k_in.c"
        "${SM2_MUSASHI_SRC}/m68kmake.c"
        "${SM2_MUSASHI_SRC}/softfloat"
    DESTINATION "${SM2_MUSASHI_DIR}")

# m68kmake reads m68k_in.c and writes the opcode jump table and handlers. It has
# to run on the build host, which for a cross build is a different architecture
# from the target. Rather than ask the caller to pre-build it, configure and
# build it here as a separate host-toolchain project: ExternalProject runs at
# build time with its own (default, host) compiler, ignoring this build's
# CMAKE_TOOLCHAIN_FILE. Native and cross builds are then identical and need no
# manual step.
include(ExternalProject)
ExternalProject_Add(m68kmake_host
    SOURCE_DIR   "${CMAKE_SOURCE_DIR}/cmake/m68kmake-host"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=Release"
        "-DSM2_M68KMAKE_SRC=${SM2_MUSASHI_DIR}/m68kmake.c"
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    BUILD_ALWAYS OFF
    INSTALL_DIR  "${CMAKE_BINARY_DIR}/m68kmake-host-install"
)
set(SM2_M68KMAKE_CMD "${CMAKE_BINARY_DIR}/m68kmake-host-install/bin/m68kmake${CMAKE_HOST_EXECUTABLE_SUFFIX}")

add_custom_command(
    OUTPUT  "${SM2_MUSASHI_DIR}/m68kops.h" "${SM2_MUSASHI_DIR}/m68kops.c"
    COMMAND "${SM2_M68KMAKE_CMD}" "${SM2_MUSASHI_DIR}" "${SM2_MUSASHI_DIR}/m68k_in.c"
    DEPENDS m68kmake_host "${SM2_MUSASHI_DIR}/m68k_in.c"
    WORKING_DIRECTORY "${SM2_MUSASHI_DIR}"
    COMMENT "Generating Musashi opcode handlers"
    VERBATIM
)

add_library(sm2_musashi STATIC
    "${SM2_MUSASHI_DIR}/m68kcpu.c"
    "${SM2_MUSASHI_DIR}/m68kops.c"
    "${SM2_MUSASHI_DIR}/softfloat/softfloat.c"
)
target_include_directories(sm2_musashi SYSTEM PUBLIC "${SM2_MUSASHI_DIR}")
set_target_properties(sm2_musashi PROPERTIES C_STANDARD 99)

# Musashi is configured through m68kconf.h, but every switch in it is
# #ifndef-guarded, so upstream's copy is used unmodified and the handful we
# disagree with are overridden here. Only the plain 68000 is wanted, and turning
# the later variants off drops their addressing modes, the FPU and the PMMU test
# from every memory access. M68K_OPT_OFF is 0.
target_compile_definitions(sm2_musashi PUBLIC
    M68K_EMULATE_010=0
    M68K_EMULATE_EC020=0
    M68K_EMULATE_020=0
    M68K_EMULATE_030=0
    M68K_EMULATE_040=0
    M68K_EMULATE_PMMU=0
)

if(MSVC)
    target_compile_options(sm2_musashi PRIVATE /w)
else()
    # -fno-common turns Musashi's tentative definitions into real ones. Without
    # it the 320 KB cycle table and the 512 KB opcode jump table become common
    # symbols with no stated alignment, and Mach-O's linker picks one from their
    # size that exceeds what a segment can hold, which it then warns about on
    # every link. As definitions they simply ask for eight bytes.
    target_compile_options(sm2_musashi PRIVATE -w -fno-common)
endif()

# ---------------------------------------------------------------------------
# ymfm — the YM3438 on the Model 1 audio board
# ---------------------------------------------------------------------------
# Aaron Giles' Yamaha FM library, which is what MAME itself uses. Taking the
# same library rather than porting it keeps the FM behaviour identical to the
# reference by construction. ymfm is deliberately standalone -- no emu.h, no
# device_t, no address spaces -- so it drops in behind a small interface object.
# Vendored as a submodule; it has no system package form.
set(SM2_YMFM_DIR "${SM2_3RDPARTY}/ymfm")

# ymfm_opn.h includes ymfm_adpcm.h and ymfm_ssg.h unconditionally, so both are
# compiled even though a YM3438 has neither an ADPCM engine nor an SSG.
add_library(sm2_ymfm STATIC
    "${SM2_YMFM_DIR}/src/ymfm_opn.cpp"
    "${SM2_YMFM_DIR}/src/ymfm_adpcm.cpp"
    "${SM2_YMFM_DIR}/src/ymfm_ssg.cpp"
)
target_include_directories(sm2_ymfm SYSTEM PUBLIC "${SM2_YMFM_DIR}/src")
target_compile_features(sm2_ymfm PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(sm2_ymfm PRIVATE /w)
else()
    target_compile_options(sm2_ymfm PRIVATE -w)
endif()
