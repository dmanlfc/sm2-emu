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
# wants on Linux, the primary target), and fall back to fetching a pinned
# revision so a clean checkout builds unattended on a machine with nothing
# installed but a compiler.

include(FetchContent)

# ---------------------------------------------------------------------------
# Vulkan
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
# At runtime the loader finds an ICD (installable client driver) — on Linux
# that is the GPU vendor's driver, on macOS it is MoltenVK. The ICD is not a
# build-time dependency.

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan headers and loader were not found.\n"
        "\n"
        "sm2-emu requires:\n"
        "  - Vulkan 1.3 headers  (vulkan/vulkan.h)\n"
        "  - Vulkan loader       (libvulkan)\n"
        "  - glslc               (GLSL to SPIR-V compiler, from shaderc)\n"
        "\n"
        "Install with:\n"
        "  Debian/Ubuntu : sudo apt install libvulkan-dev vulkan-validationlayers glslc\n"
        "  Fedora        : sudo dnf install vulkan-devel vulkan-validation-layers glslc\n"
        "  Arch          : sudo pacman -S vulkan-devel vulkan-validation-layers shaderc\n"
        "  macOS         : brew install vulkan-headers vulkan-loader shaderc molten-vk\n"
        "                  (or install the LunarG Vulkan SDK and source its setup-env.sh)\n"
        "\n"
        "For cross-compilation (buildroot, Yocto), ensure the sysroot contains\n"
        "the Vulkan headers and loader for the target, and set CMAKE_FIND_ROOT_PATH\n"
        "or Vulkan_INCLUDE_DIR / Vulkan_LIBRARY explicitly.\n"
        "\n"
        "If the SDK is installed in a non-standard location, set VULKAN_SDK or\n"
        "CMAKE_PREFIX_PATH to point at it.")
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
    set(SM2_SDL3_ORIGIN "fetched (release-3.4.14)")

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

    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.14
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator
# ---------------------------------------------------------------------------
# Hand-rolled one-allocation-per-resource works for a fixed resource set, but
# the texture sheets and per-frame rings want suballocation, and VMA is the
# boring correct answer.

FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# ---------------------------------------------------------------------------
# miniz — zip reading and CRC32 for the ROM loader
# ---------------------------------------------------------------------------
# The amalgamated release archive rather than the git repository: it is two
# files with no CMakeLists.txt, so FetchContent populates it without trying to
# configure it. miniz's own build declares compatibility with CMake < 3.5, which
# CMake 4 refuses, and wrapping two source files ourselves is less trouble than
# working around that.

FetchContent_Declare(miniz
    URL      https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
)
FetchContent_MakeAvailable(miniz)

add_library(sm2_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(sm2_miniz SYSTEM PUBLIC "${miniz_SOURCE_DIR}")
set_target_properties(sm2_miniz PROPERTIES C_STANDARD 11)

# We only read archives that are already in memory or on disk, and the ROM
# loader does its own file handling, so the stdio and writing halves are dead
# weight. Leaving them out also avoids miniz's deprecated large-file paths.
target_compile_definitions(sm2_miniz PUBLIC
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES  # do not shadow zlib's symbol names
)

# ---------------------------------------------------------------------------
# LZMA SDK — 7z reading for the ROM loader
# ---------------------------------------------------------------------------

FetchContent_Declare(lzmasdk
    URL      https://www.7-zip.org/a/lzma2301.7z
    URL_HASH SHA256=317dd834d6bbfd95433488b832e823cd3d4d420101436422c03af88507dd1370
)
FetchContent_MakeAvailable(lzmasdk)

add_library(sm2_7z STATIC
    "${lzmasdk_SOURCE_DIR}/C/7zAlloc.c"
    "${lzmasdk_SOURCE_DIR}/C/7zArcIn.c"
    "${lzmasdk_SOURCE_DIR}/C/7zBuf.c"
    "${lzmasdk_SOURCE_DIR}/C/7zCrc.c"
    "${lzmasdk_SOURCE_DIR}/C/7zCrcOpt.c"
    "${lzmasdk_SOURCE_DIR}/C/7zDec.c"
    "${lzmasdk_SOURCE_DIR}/C/7zFile.c"
    "${lzmasdk_SOURCE_DIR}/C/7zStream.c"
    "${lzmasdk_SOURCE_DIR}/C/Bcj2.c"
    "${lzmasdk_SOURCE_DIR}/C/CpuArch.c"
    "${lzmasdk_SOURCE_DIR}/C/Lzma2Dec.c"
    "${lzmasdk_SOURCE_DIR}/C/LzmaDec.c"
)
target_include_directories(sm2_7z SYSTEM PUBLIC "${lzmasdk_SOURCE_DIR}/C")
set_target_properties(sm2_7z PROPERTIES C_STANDARD 11)
target_compile_definitions(sm2_7z PUBLIC
    Z7_NO_METHODS_FILTERS  # no Delta/BCJ/branch-conversion filters
)

# ---------------------------------------------------------------------------
# pugixml — the games.xml database
# ---------------------------------------------------------------------------

FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(pugixml)

# ---------------------------------------------------------------------------
# Dear ImGui — immediate-mode GUI for the settings overlay
# ---------------------------------------------------------------------------
# ImGui has no CMakeLists.txt; we fetch the source and build it ourselves with
# the SDL3 + Vulkan backends, which is all the emulator needs.

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
)
FetchContent_MakeAvailable(imgui)

add_library(sm2_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(sm2_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(sm2_imgui PUBLIC
    Vulkan::Vulkan
    SDL3::SDL3
)
# ImGui uses VK_NO_PROTOTYPES when loading functions itself, but we link the
# loader directly and want the prototypes available. Ensure the no-prototypes
# flag is NOT defined.
target_compile_definitions(sm2_imgui PRIVATE
    IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
)
if(NOT MSVC)
    target_compile_options(sm2_imgui PRIVATE -w)
endif()

# sm2_imgui_gl -- imgui's OpenGL3 renderer backend, for the phase 9 GL
# backend. A separate target from sm2_imgui rather than adding this source to
# it: sm2_imgui already exists and works for Vulkan, and imgui_impl_opengl3.cpp
# is a different renderer backend translation unit entirely (no shared state
# between the two backend files, per imgui's own design -- each is a self-
# contained implementation of the same abstract "render this draw data"
# contract). imgui_impl_opengl3.cpp/.h already exist on disk from the same
# FetchContent_MakeAvailable(imgui) call above (that clones the whole imgui
# repository, backends included), so no second fetch is needed.
if(SM2_BUILD_OPENGL_DESKTOP)
    add_library(sm2_imgui_gl STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(sm2_imgui_gl SYSTEM PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(sm2_imgui_gl PUBLIC SDL3::SDL3)
    # No IMGUI_IMPL_OPENGL_LOADER_CUSTOM here, deliberately: imgui_impl_opengl3.cpp
    # is compiled as its own translation unit, so its bundled gl3w-derived
    # loader (imgui_impl_opengl3_loader.h) cannot collide with this
    # project's own sm2::render::gl loader (gl_common.h) regardless of what
    # either names -- imgui's own header comment on that loader says exactly
    # this ("cannot happen unless you build both in the same compilation
    # unit"), which was checked directly against the actual file rather than
    # assumed. An earlier version of this target defined
    # IMGUI_IMPL_OPENGL_LOADER_CUSTOM on the premise that two loaders would
    # conflict; that premise was wrong, and the fix was simpler than the
    # problem it was solving -- let imgui use its own loader, which is
    # exactly what it recommends for this situation.
    if(NOT MSVC)
        target_compile_options(sm2_imgui_gl PRIVATE -w)
    endif()
endif()

# sm2_imgui_gles -- same as sm2_imgui_gl but for the GLES 3.1 backend.
# imgui_impl_opengl3.cpp supports ES out of the box (it detects the ES
# context at runtime via GL_VERSION string parsing and uses its own bundled
# loader regardless of platform).
if(SM2_BUILD_OPENGL_ES)
    add_library(sm2_imgui_gles STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(sm2_imgui_gles SYSTEM PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(sm2_imgui_gles PUBLIC SDL3::SDL3)
    if(NOT MSVC)
        target_compile_options(sm2_imgui_gles PRIVATE -w)
    endif()
endif()

# ---------------------------------------------------------------------------
# Musashi — the 68000 in the sound board
# ---------------------------------------------------------------------------
# The one piece of hardware emulation not taken from MAME, and deliberately so.
#
# MAME's 68000 is excellent but it is either generated from a table by a Python
# script into two hundred thousand lines per variant, or a fork of Musashi; both
# are woven into MAME's device and address-space framework. Musashi upstream is
# the same lineage, is a self-contained C library designed to be embedded behind
# memory callbacks, and is MIT licensed. It is also a fully documented commodity
# part, so unlike the geometry engine or the SCSP there is no reverse-engineering
# insight in one implementation over another and nothing is lost by not tracking
# MAME's.
#
# Pinned by commit because Musashi has no releases.

FetchContent_Declare(musashi
    GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
    GIT_TAG        313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
    GIT_PROGRESS   TRUE
    SYSTEM
)
# Musashi has no CMakeLists.txt, so this populates the tree and stops there.
FetchContent_MakeAvailable(musashi)

# The sources are copied into the build tree rather than compiled where they
# were fetched, because m68kmake writes its generated output next to its input
# and we do not want to dirty the fetch cache.
#
# The directory layout has to be preserved: m68kcpu.c #includes m68kfpu.c and
# m68kmmu.h textually, m68kcpu.h #includes softfloat/softfloat.h, and
# softfloat.c #includes ../m68kcpu.h. Hence m68kfpu.c and m68kmmu.h are copied
# but are NOT library sources.
set(SM2_MUSASHI_DIR "${CMAKE_BINARY_DIR}/musashi")
file(MAKE_DIRECTORY "${SM2_MUSASHI_DIR}")
file(COPY
        "${musashi_SOURCE_DIR}/m68k.h"
        "${musashi_SOURCE_DIR}/m68kconf.h"
        "${musashi_SOURCE_DIR}/m68kcpu.h"
        "${musashi_SOURCE_DIR}/m68kcpu.c"
        "${musashi_SOURCE_DIR}/m68kmmu.h"
        "${musashi_SOURCE_DIR}/m68kfpu.c"
        "${musashi_SOURCE_DIR}/m68k_in.c"
        "${musashi_SOURCE_DIR}/m68kmake.c"
        "${musashi_SOURCE_DIR}/softfloat"
    DESTINATION "${SM2_MUSASHI_DIR}")

# m68kmake reads m68k_in.c and writes the opcode jump table and handlers.
#
# When cross-compiling (e.g. buildroot for riscv64/aarch64), m68kmake must run
# on the host, not the target. The caller can supply a pre-built host binary
# via -DSM2_M68KMAKE=/path/to/m68kmake. When native-compiling, it is built here.
if(CMAKE_CROSSCOMPILING)
    if(NOT SM2_M68KMAKE)
        message(FATAL_ERROR
            "Cross-compiling requires a host-built m68kmake.\n"
            "Build m68kmake for the host first (a simple: cc -o m68kmake m68kmake.c),\n"
            "then pass -DSM2_M68KMAKE=/path/to/m68kmake to this cmake invocation.")
    endif()
    if(NOT EXISTS "${SM2_M68KMAKE}")
        message(FATAL_ERROR "SM2_M68KMAKE points to '${SM2_M68KMAKE}' which does not exist.")
    endif()
    set(SM2_M68KMAKE_CMD "${SM2_M68KMAKE}")
else()
    add_executable(m68kmake "${SM2_MUSASHI_DIR}/m68kmake.c")
    set_target_properties(m68kmake PROPERTIES C_STANDARD 99)
    if(MSVC)
        target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(m68kmake PRIVATE -w)
    endif()
    set(SM2_M68KMAKE_CMD "$<TARGET_FILE:m68kmake>")
endif()

add_custom_command(
    OUTPUT  "${SM2_MUSASHI_DIR}/m68kops.h" "${SM2_MUSASHI_DIR}/m68kops.c"
    COMMAND ${SM2_M68KMAKE_CMD} "${SM2_MUSASHI_DIR}" "${SM2_MUSASHI_DIR}/m68k_in.c"
    DEPENDS $<$<NOT:$<BOOL:${CMAKE_CROSSCOMPILING}>>:m68kmake> "${SM2_MUSASHI_DIR}/m68k_in.c"
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
# disagree with are overridden here. That keeps the delta from upstream visible
# in one place instead of buried in a forked header.
#
# Only the plain 68000 is wanted, and turning the later variants off drops their
# addressing modes, the FPU and the PMMU test from every memory access.
# M68K_OPT_OFF is 0.
target_compile_definitions(sm2_musashi PUBLIC
    M68K_EMULATE_010=0
    M68K_EMULATE_EC020=0
    M68K_EMULATE_020=0
    M68K_EMULATE_030=0
    M68K_EMULATE_040=0
    M68K_EMULATE_PMMU=0
)

# Third-party C we do not intend to modify, so its warnings are not ours to fix.
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
# Aaron Giles' Yamaha FM library, which is what MAME itself uses: MAME carries it
# in 3rdparty/ymfm and its ym3438_device is a thin wrapper around ymfm::ym3438.
# Taking the same library rather than porting it keeps the FM behaviour identical
# to the reference by construction, and unlike the rest of MAME's sound devices
# ymfm is deliberately standalone -- no emu.h, no device_t, no address spaces --
# so it drops in behind a small interface object.
#
# MAME's vendored copy is not byte-identical to upstream. The differences are in
# ADPCM-B and in ymfm.h's WAV writer, and ymfm_fm.h, ymfm_fm.ipp, ymfm_opn.h and
# ymfm_ssg.* -- everything a YM3438 executes -- are identical, so this and MAME
# run the same FM code. ADPCM-B belongs to the YM2608/YM2610 and is never reached
# from ym2612's register map.
#
# Pinned by commit; ymfm has no releases.
FetchContent_Declare(ymfm
    GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
    GIT_TAG        81aec25ccbb98f4873a255f7551ac4dadac59b4a
    GIT_PROGRESS   TRUE
    SYSTEM
)
# ymfm has no CMakeLists.txt, so as with Musashi this populates the tree and
# stops there, and the translation units a YM3438 needs are compiled below.
FetchContent_MakeAvailable(ymfm)

# ymfm_opn.h includes ymfm_adpcm.h and ymfm_ssg.h unconditionally, so both are
# compiled even though a YM3438 has neither an ADPCM engine nor an SSG.
add_library(sm2_ymfm STATIC
    "${ymfm_SOURCE_DIR}/src/ymfm_opn.cpp"
    "${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp"
    "${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp"
)
target_include_directories(sm2_ymfm SYSTEM PUBLIC "${ymfm_SOURCE_DIR}/src")
target_compile_features(sm2_ymfm PRIVATE cxx_std_20)

# Third-party C++ we do not intend to modify, so its warnings are not ours to fix.
if(MSVC)
    target_compile_options(sm2_ymfm PRIVATE /w)
else()
    target_compile_options(sm2_ymfm PRIVATE -w)
endif()
