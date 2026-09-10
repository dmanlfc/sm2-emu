```
  ____  __  __  ____         _____ __  __ _   _
 / ___||  \/  ||___ \       | ____|  \/  | | | |
 \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
  ___) | |  | | / __/|_____|| |___| |  | | |_| |
 |____/|_|  |_||_____|      |_____|_|  |_|\___/

 A   S E G A   M O D E L   2   E M U L A T O R
```

# Building

Requirements:

- CMake 3.24 or newer, and Ninja
- A C++20 compiler
- `glslc` (from shaderc or the Vulkan SDK) — used at build time to compile and
  lint the shaders
- SDL3, pugixml, miniz, the LZMA SDK, stb_image, Dear ImGui (with its SDL3
  backend) and — for the Vulkan backend — VulkanMemoryAllocator
- libcurl, for the game picker's artwork scraping. Optional: without it the
  picker still lists and launches every game, just with no downloaded art or
  descriptions.
- For the OpenGL / OpenGL ES backends (built by default): the system GL/GLES
  and EGL libraries (Mesa on Linux). No extra headers are needed — SDL3
  provides GL loading.
- For the Vulkan backend (off by default): Vulkan 1.3 headers and loader.

Each dependency is taken from the system when `find_package` locates it, and
otherwise built from the copy bundled under `3rdparty/`. Most of those are git
submodules; the LZMA SDK and stb_image have no git repository of their own, so
their sources are committed directly. A recursive checkout therefore builds
with no network access at configure time — nothing is downloaded on the fly.
Clone accordingly:

```sh
git clone --recurse-submodules https://github.com/dmanlfc/sm2-emu.git
# or, in an existing checkout:
git submodule update --init --recursive
```

The Musashi 68000 core and the ymfm FM library have no system-package form, so
they always come from `3rdparty/`. `glslc` is the one build-time tool that must
be on `PATH` (it runs on the build host, not the target).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The default build produces a binary with the software renderer and the OpenGL
4.3 core desktop backend — no Vulkan driver or SDK required, which is what
lower-end ARM boards want. Add Vulkan with `-DSM2_BUILD_VULKAN=ON`.

On **macOS** the default is not enough: Apple's desktop OpenGL tops out at 4.1,
below this renderer's 4.3 floor, so a windowed build needs
`-DSM2_BUILD_VULKAN=ON` (MoltenVK). See the [macOS](#macos) section for a
step-by-step guide.

`Release` (`-O3`) is the default and what you want for running games. For
debugging, use `-DCMAKE_BUILD_TYPE=Debug` (unoptimised, with symbols; also
turns Vulkan validation on by default when the Vulkan backend is built).

Useful options: `-DSM2_ENABLE_VALIDATION=ON` (default in Debug),
`-DSM2_WERROR=ON`, `-DSM2_BUILD_TESTS=OFF`.

## Graphics backends

Up to three renderers are available, chosen at runtime with
`--graphics-backend software|vulkan|opengl`:

- **software** — the CPU rasteriser (also the correctness oracle). Always
  built. It presents through whichever GPU backend was compiled in.
- **vulkan** — the Vulkan backend. Opt-in at build time.
- **opengl** — whichever OpenGL flavour the binary was built with.

Every GPU backend is a build-time choice. Software is always built; the rest
are gated by CMake options so a build only carries what its target needs:

| Option | Default | Backend |
|--------|:-------:|---------|
| `SM2_BUILD_VULKAN`         | OFF | Vulkan 1.3 (needs the Vulkan headers + loader) |
| `SM2_BUILD_OPENGL_DESKTOP` | ON  | OpenGL 4.3 core (desktop x86_64, macOS) |
| `SM2_BUILD_OPENGL_ES`      | OFF | OpenGL ES 3.1 (ARM devices, e.g. Raspberry Pi 5) |

`SM2_BUILD_OPENGL_DESKTOP` and `SM2_BUILD_OPENGL_ES` are mutually exclusive — a
binary carries one GL flavour. Vulkan can be combined with either.

```sh
# Default: software + OpenGL 4.3 core, no Vulkan required
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Desktop with Vulkan as well
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSM2_BUILD_VULKAN=ON

# ARM / GLES (e.g. Raspberry Pi 5): software + OpenGL ES 3.1
cmake -S . -B build-gles -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSM2_BUILD_OPENGL_DESKTOP=OFF -DSM2_BUILD_OPENGL_ES=ON
```

The floor is OpenGL 4.3 core / OpenGL ES 3.1 — the renderer uses compute
shaders and storage buffers, which do not exist below that line. On X11 the
GLES backend forces the EGL path automatically (GLX cannot provide a GLES
context); it also works under Wayland.

## Linux dependencies

```sh
# Debian / Ubuntu — default build (software + OpenGL)
sudo apt install cmake ninja-build build-essential \
                 glslc \
                 libgl-dev libgles-dev libegl-dev \
                 libsdl3-dev libpugixml-dev libcurl4-openssl-dev

# Add these only if building the Vulkan backend (-DSM2_BUILD_VULKAN=ON)
sudo apt install libvulkan-dev vulkan-validationlayers \
                 libvulkan-memory-allocator-dev
```

`libcurl` is optional (drop it to build without artwork scraping).
Debian/Ubuntu has no packages for miniz, the LZMA SDK, stb_image or a Dear ImGui
with the SDL3 backend; those come from `3rdparty/` automatically, so nothing
extra is needed as long as the submodules are checked out.

```sh
# Arch / Manjaro — default build (software + OpenGL)
sudo pacman -S --needed base-devel cmake ninja shaderc mesa sdl3 pugixml miniz curl

# Add these only if building the Vulkan backend (-DSM2_BUILD_VULKAN=ON)
sudo pacman -S --needed vulkan-headers vulkan-icd-loader \
                        vulkan-validation-layers vulkan-memory-allocator
```

`mesa` provides the GL, GLES and EGL libraries and `shaderc` provides `glslc`;
`curl` is optional (artwork scraping). Arch has no package for the LZMA SDK,
stb_image or a Dear ImGui with the SDL3 backend; those come from `3rdparty/`
automatically.

## macOS

On macOS you want the **Vulkan backend**, built on top of MoltenVK. This is not
optional in practice: Apple's desktop OpenGL is capped at 4.1, and this
renderer needs OpenGL 4.3 core (for compute shaders and storage buffers). The
default OpenGL backend therefore cannot create a context on macOS, and because
even the software renderer presents its frame through a GPU backend, a
Vulkan-less build can only run headless (`--boot-test`). Build with
`-DSM2_BUILD_VULKAN=ON` and you get a working window, with either the Vulkan or
the software renderer selectable at runtime.

Step by step, from a clean machine:

1. **Install the Xcode command-line tools** (the C++20 compiler and system
   headers):

   ```sh
   xcode-select --install
   ```

2. **Install [Homebrew](https://brew.sh)** if you don't have it, then the build
   tools and dependencies:

   ```sh
   brew install cmake ninja pkg-config \
                shaderc \
                sdl3 pugixml curl \
                vulkan-headers vulkan-loader molten-vk
   ```

   - `shaderc` provides `glslc`, which compiles and lints the shaders at build
     time and is required for every build.
   - `vulkan-headers`, `vulkan-loader` and `molten-vk` are the Vulkan backend's
     dependencies. `molten-vk` is the driver that runs Vulkan on Metal.
   - `curl` is optional — it powers the game picker's artwork scraping. Drop it
     and everything still lists and launches, just without downloaded art.
   - miniz, the LZMA SDK, stb_image and Dear ImGui (with its SDL3 backend) have
     no Homebrew formula, so they are built from `3rdparty/` automatically.

   Alternatively, install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
   instead of the three `vulkan-*` / `molten-vk` formulae. It is not on a
   default search path, so source its environment once **before configuring**
   so `find_package(Vulkan)` can locate the headers and loader:

   ```sh
   . ~/VulkanSDK/setup-env.sh    # or wherever the SDK lives
   ```

   This is a configure-time step only. The build records where the loader was
   found and bakes it into the binary, so the finished executable runs without
   any environment set up first.

3. **Clone with submodules** (see the top of this document):

   ```sh
   git clone --recurse-submodules https://github.com/dmanlfc/sm2-emu.git
   cd sm2-emu
   ```

4. **Configure and build** with Vulkan enabled:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DSM2_BUILD_VULKAN=ON
   cmake --build build
   ctest --test-dir build
   ```

5. **Run**, selecting a renderer. Both present through MoltenVK:

   ```sh
   ./build/bin/sm2-emu --graphics-backend vulkan   vf2.zip
   ./build/bin/sm2-emu --graphics-backend software vf2.zip
   ```

   Confirm MoltenVK was found with `./build/bin/sm2-emu --list-gpus`; it should
   name your Metal device. Nothing needs to be sourced to run the binary — on
   macOS the build records the loader's path in the executable. If `--list-gpus`
   reports that no Vulkan devices were found, `molten-vk` is missing: install it
   (`brew install molten-vk`) and reconfigure so it is picked up.

## Cross-compilation (Buildroot, Yocto, Batocera, embedded)

sm2-emu builds for `x86_64`, `aarch64` and `riscv64`. Cross-compiling needs no
special dependency handling: pass a toolchain file and build.

```sh
# GLES backend shown, typical for an ARM target:
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DSM2_BUILD_OPENGL_DESKTOP=OFF -DSM2_BUILD_OPENGL_ES=ON \
    -DSM2_BUILD_TESTS=OFF
cmake --build build
```

`m68kmake` (a small C program that generates the 68000 opcode table) has to run
on the build host, not the target. CMake handles this automatically: it is
configured and built as a separate host-toolchain sub-project during the build,
so there is no manual pre-build step and no flag to pass, native or cross.

`CMAKE_BUILD_TYPE` and its optimisation flags (`-O3` for `Release`) are a
CMake-level setting, so they apply to the target compiler the toolchain file
selects — a cross build gets the same optimisation as a native one, for its
own architecture. There is no `-march=native` anywhere, so a build stays
portable across the boards it targets.

The target sysroot may provide any of the dependencies below; each one CMake
does not find there is built from the copy under `3rdparty/` instead, so a
recursive checkout cross-compiles with no network access:

- SDL3, pugixml, miniz, the LZMA SDK, stb_image and Dear ImGui
- libcurl, if artwork scraping is wanted (optional; omit for an offline picker)
- the GL/GLES and EGL libraries, if a GL backend is built (the usual case)
- Vulkan 1.3 headers (`vulkan/vulkan.h`), loader (`libvulkan.so`) and
  VulkanMemoryAllocator, only if `-DSM2_BUILD_VULKAN=ON`
- `glslc` on the host PATH (it runs at build time, not on the target)

The Musashi 68000 core and the ymfm FM library always come from `3rdparty/`,
having no system-package form.
