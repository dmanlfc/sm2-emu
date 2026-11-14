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
# Only needed when SM2_BUILD_VULKAN is on. The loader is linked directly rather
# than through volk: one device and one submitting thread make volk's saving
# not worth it. glslc (below) is required regardless, as it also lints the GL
# shaders.
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

# Windows has no system SDL3 and wants a self-contained exe, so build the
# vendored copy static there; elsewhere prefer the system's.
if(WIN32)
    set(SDL3_FOUND FALSE)
else()
    find_package(SDL3 3.2 CONFIG QUIET)
endif()

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
# Header-only, so the fallback is just an include directory exposing the
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
# The rest of the tree links sm2_miniz and includes <miniz.h>.

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
    # miniz's own CMakeLists is bypassed (it declares CMake < 3.5, which CMake 4
    # refuses), so write the miniz_export.h it would generate; for a static
    # build its macros are empty.
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

    target_compile_definitions(sm2_miniz PUBLIC
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES  # do not shadow zlib's symbol names
    )
endif()

# ---------------------------------------------------------------------------
# stb_image — JPEG/PNG decode for the game picker's box art
# ---------------------------------------------------------------------------
# A committed single header; a generated .c defines the implementation macro.
# Restricted to JPEG and PNG, which is what ArcadeDB serves.
set(SM2_STB_DIR "${SM2_3RDPARTY}/stb")
set(SM2_STB_GEN "${CMAKE_BINARY_DIR}/stb-gen")
file(WRITE "${SM2_STB_GEN}/stb_image_impl.c"
    "#define STB_IMAGE_IMPLEMENTATION\n"
    "#define STBI_ONLY_JPEG\n"
    "#define STBI_ONLY_PNG\n"
    "#include \"stb_image.h\"\n")

# The overlay's shaped text rasterises glyphs by index with stb_truetype. ImGui
# compiles its own copy static (private to imgui_draw.cpp), so build an
# exported one here from the same header.
file(WRITE "${SM2_STB_GEN}/stb_truetype_impl.c"
    "#define STB_TRUETYPE_IMPLEMENTATION\n"
    "#include \"imstb_truetype.h\"\n")

add_library(sm2_stb STATIC
    "${SM2_STB_GEN}/stb_image_impl.c"
    "${SM2_STB_GEN}/stb_truetype_impl.c")
target_include_directories(sm2_stb SYSTEM PUBLIC "${SM2_STB_DIR}")
set_source_files_properties("${SM2_STB_GEN}/stb_truetype_impl.c" PROPERTIES
    INCLUDE_DIRECTORIES "${SM2_3RDPARTY}/imgui")
set_target_properties(sm2_stb PROPERTIES C_STANDARD 11)
if(MSVC)
    target_compile_options(sm2_stb PRIVATE /w)
else()
    target_compile_options(sm2_stb PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# libcurl — HTTP for the game picker's artwork scraper (soft dependency)
# ---------------------------------------------------------------------------
# System-preferred and optional: without it the scraper's networking compiles
# out (SM2_HAVE_CURL undefined) and the picker runs offline. Hence no vendored
# fallback and no fatal error when absent.
find_package(CURL QUIET)
if(CURL_FOUND)
    set(SM2_HAVE_CURL TRUE)
    set(SM2_CURL_ORIGIN "system (${CURL_VERSION_STRING})")
else()
    set(SM2_HAVE_CURL FALSE)
    set(SM2_CURL_ORIGIN "not found — artwork scraping disabled (picker offline-only)")
endif()

# ---------------------------------------------------------------------------
# LZMA SDK — 7z reading for the ROM loader
# ---------------------------------------------------------------------------
# The vendored fallback compiles only the subset the 7z reader needs. The SDK
# has no git repository, so its sources are committed rather than a submodule.

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
# A system imgui::imgui is expected to have the SDL3, OpenGL3 and Vulkan
# backends compiled in, so the sm2_imgui* targets become thin wrappers over it.
# On the vendored path each renderer backend gets its own target, so a build
# without Vulkan links no Vulkan.

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

    # sm2_imgui: core ImGui plus the SDL3 platform backend, no renderer backend.
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

    # sm2_imgui_vk: ImGui with its Vulkan renderer backend.
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

    # sm2_imgui_gl: ImGui with its OpenGL3 renderer backend. Its bundled loader
    # stays private to imgui_impl_opengl3.cpp and cannot collide with ours, so
    # no IMGUI_IMPL_OPENGL_LOADER_CUSTOM.
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

    # sm2_imgui_gles: as sm2_imgui_gl; the backend detects an ES context at runtime.
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
# Deliberately not taken from MAME: an MIT-licensed, embeddable library for a
# fully documented part. The sources are copied into the build tree because
# m68kmake writes its output beside its input. The layout must be preserved, as
# the sources include each other by relative path; m68kfpu.c and m68kmmu.h are
# included textually and are not library sources.
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

# m68kmake generates the opcode handlers and must run on the build host, so it
# is built as a separate ExternalProject with the host compiler, ignoring any
# cross toolchain file.
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

# m68kconf.h's switches are #ifndef-guarded, so upstream's copy is used as is
# and overridden here. Only the plain 68000 is wanted; disabling the later
# variants keeps their checks out of every memory access. M68K_OPT_OFF is 0.
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
    # -fno-common: as common symbols, the large cycle and jump tables get an
    # alignment from their size that Mach-O's linker warns about on every link.
    target_compile_options(sm2_musashi PRIVATE -w -fno-common)
endif()

# ---------------------------------------------------------------------------
# ymfm — the YM3438 on the Model 1 audio board
# ---------------------------------------------------------------------------
# Aaron Giles' Yamaha FM library, the same one MAME uses, so FM behaviour
# matches the reference by construction.
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

# ---------------------------------------------------------------------------
# HarfBuzz and SheenBidi — text shaping for translated overlay text
# ---------------------------------------------------------------------------
# Arabic, Hebrew and the Indic scripts need shaping and bidi reordering that
# ImGui does not do. HarfBuzz is static on Windows and macOS so the packages
# carry no extra library; SheenBidi is rarely packaged, so always vendored.

if(WIN32 OR APPLE)
    set(harfbuzz_FOUND FALSE)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(HARFBUZZ IMPORTED_TARGET harfbuzz>=4.0)
    endif()
endif()

if(HARFBUZZ_FOUND)
    add_library(sm2_harfbuzz INTERFACE)
    target_link_libraries(sm2_harfbuzz INTERFACE PkgConfig::HARFBUZZ)
    set(SM2_HARFBUZZ_ORIGIN "system (${HARFBUZZ_VERSION})")
else()
    # harfbuzz.cc is upstream's single translation unit build of the library.
    add_library(sm2_harfbuzz STATIC "${SM2_3RDPARTY}/harfbuzz/src/harfbuzz.cc")
    target_include_directories(sm2_harfbuzz SYSTEM PUBLIC "${SM2_3RDPARTY}/harfbuzz/src")
    if(MSVC)
        target_compile_options(sm2_harfbuzz PRIVATE /w /bigobj)
    else()
        target_compile_options(sm2_harfbuzz PRIVATE -w)
    endif()
    set(SM2_HARFBUZZ_ORIGIN "vendored (3rdparty/harfbuzz)")
endif()

add_library(sm2_sheenbidi STATIC "${SM2_3RDPARTY}/SheenBidi/Source/SheenBidi.c")
target_include_directories(sm2_sheenbidi SYSTEM PUBLIC "${SM2_3RDPARTY}/SheenBidi/Headers")
target_include_directories(sm2_sheenbidi PRIVATE "${SM2_3RDPARTY}/SheenBidi/Source")
target_compile_definitions(sm2_sheenbidi PRIVATE SB_CONFIG_UNITY)
set_target_properties(sm2_sheenbidi PROPERTIES C_STANDARD 11)
if(MSVC)
    target_compile_options(sm2_sheenbidi PRIVATE /w /utf-8)
else()
    target_compile_options(sm2_sheenbidi PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# System font lookup — fonts for the overlay's translated text
# ---------------------------------------------------------------------------
# The overlay draws English in ImGui's built-in ProggyClean and takes every
# other script from the fonts the OS already has: fontconfig on Linux,
# CoreText on macOS and DirectWrite on Windows. Linux without fontconfig still
# builds; languages ProggyClean cannot draw then fall back to English.
set(SM2_HAVE_FONTCONFIG FALSE)
if(SM2_PLATFORM_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FONTCONFIG IMPORTED_TARGET fontconfig)
    endif()
    if(FONTCONFIG_FOUND)
        set(SM2_HAVE_FONTCONFIG TRUE)
    else()
        message(WARNING
            "fontconfig was not found, so translations needing fonts beyond "
            "Latin-1 will show in English. Install libfontconfig-dev "
            "(Debian/Ubuntu), fontconfig-devel (Fedora) or fontconfig (Arch).")
    endif()
endif()
