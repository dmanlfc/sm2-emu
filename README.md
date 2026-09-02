```
  ____  __  __  ____         _____ __  __ _   _
 / ___||  \/  ||___ \       | ____|  \/  | | | |
 \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
  ___) | |  | | / __/|_____|| |___| |  | | |_| |
 |____/|_|  |_||_____|      |_____|_|  |_|\___/

 A   S E G A   M O D E L   2   E M U L A T O R
```

Background: I started this emulation journey back in February 2025 to look to
improve upon Model 2 emulation for Linux, since my favourite OS lacked a native
emulator and at the time MAME had incompatibility issues and was just slow for
small ARM-based SBCs. The mission was to look into what MAME did well and learn
more from research and analysis of Supermodel (a Model 3 emulator) as
inspiration. Supermodel actually led me to wire up OpenGL ES and Vulkan for
that emulator, as I could get quicker results on the possibility of running
Model 2 emulation on a Raspberry Pi 5 and bringing it to the emulation
community.

Linux is the primary target. macOS is supported just because that's partly what
I used for development and runs Vulkan through MoltenVK. I don't care for
Windows... there, I said it.

The journey included a lot of discussions with GenAI. I'm not going to lie, but
its ability to really lean in and help tackle the hard parts was somewhat
limited early on. Providing it bite-sized tasks sped up my part-time
development from November 2025 onwards, especially around how all the
components hang together, and later on it was genuinely helpful with the
graphical quirks.

**Status: playable.** All four boards run — original Model 2, 2A, 2B and 2C —
with picture and sound. Of the 83 sets in the database, the majority draw full
3D scenes and produce audio, across three renderers: Vulkan, OpenGL and OpenGL
ES.

All three geometry coprocessors are there — the MB86234 TGP, the ADSP-21062
SHARC and the MB86235 TGPx4 — along with the System 24 tilemap hardware, a
textured renderer with the hardware's own colour chain evaluated per texel, and
both sound boards: the 68000/SCSP the CRX family uses, and the Model 1 audio
board (a 68000 with a YM3438 and two MultiPCMs) that Daytona USA, Desert Tank
and Virtua Cop carry instead. Drive boards, lightguns, the link board and the
protection devices are wired.

Input comes from SDL gamepads with the keyboard live alongside them, and the
machine is paced to its own 57.5245 Hz rather than to the display. The frame is
composited at the hardware's 496x384 and magnified once at the end, so it runs
on the hardware's pixels rather than on colours a filter has already blurred.

## What Model 2 is, and why the renderer looks unusual

Model 2 is an i960KB paired with a geometry coprocessor (a Fujitsu MB86234
"TGP" on Model 2 and 2A, an ADSP-21062 SHARC on 2B, an MB86235 on 2C), a custom
Sega/Lockheed-Martin rasterizer, and Sega System 24 tilemap hardware for the 2D
layers. Output is 496x384 at roughly 57.5 Hz.

Four properties of that rasterizer shape the whole design, because none has a
direct modern equivalent:

- **No depth buffer.** Polygons are bucket-sorted by depth on the CPU and drawn
  front to back against a one-bit fill mask: first writer wins. Reproduced with
  a stencil attachment rather than a depth test.
- **No RGB textures.** A texel is a 4-bit *intensity*; colour arrives through a
  tone curve, a base colour, a translation table and a gamma ramp. The curve is
  applied *after* filtering, so it cannot be baked in — the whole chain runs
  per texel in the fragment shader.
- **No alpha blending.** Translucency is an alpha test on one texel value, or a
  screen-locked stipple. Both discard fragments, which leaves the fill mask
  unclaimed so what is behind still gets the pixel.
- **No per-vertex shading.** One 8-bit luminance scalar per polygon.

So the geometry pipeline runs on the CPU as the hardware's did, and the GPU
backend is handed pre-projected screen-space triangles. No vertex
transformation on the GPU, no geometry shaders.

## Building

Requirements:

- CMake 3.24 or newer, and Ninja
- A C++20 compiler
- `glslc` (from shaderc or the Vulkan SDK) — used at build time to compile and
  lint the shaders
- SDL3 (window, input, audio)
- pugixml (the games.xml parser)
- For the OpenGL / OpenGL ES backends (built by default): the system GL/GLES
  and EGL libraries (Mesa on Linux). No extra headers are needed — SDL3
  provides GL loading.
- For the Vulkan backend (off by default): Vulkan 1.3 headers and loader, plus
  VulkanMemoryAllocator.

Dependencies are taken from the system. If a required one is missing the build
stops with a message naming the package to install — it does not silently
download anything. Pass `-DSM2_ALLOW_FETCH=ON` to let the build fetch and build
the packaged libraries (SDL3, pugixml, VulkanMemoryAllocator) itself, which is
convenient on a dev box or on macOS. A few dependencies with no packaged form
(the Musashi 68000 core, the ymfm FM library, miniz, the LZMA SDK and Dear
ImGui) are always vendored and built from pinned sources regardless.

```sh
. tools/env.sh          # optional: adds ~/.local/bin and the Vulkan SDK to PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The default build produces a binary with the software renderer and the OpenGL
4.3 core desktop backend — no Vulkan driver or SDK required, which is what
lower-end ARM boards want. Add Vulkan with `-DSM2_BUILD_VULKAN=ON`.

`Release` (`-O3`) is the default and what you want for running games. For
debugging, use `-DCMAKE_BUILD_TYPE=Debug` (unoptimised, with symbols; also
turns Vulkan validation on by default when the Vulkan backend is built).

Useful options: `-DSM2_ENABLE_VALIDATION=ON` (default in Debug),
`-DSM2_WERROR=ON`, `-DSM2_BUILD_TESTS=OFF`.

### Graphics backends

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

### Linux dependencies

```sh
# Debian / Ubuntu — default build (software + OpenGL)
sudo apt install cmake ninja-build build-essential \
                 glslc \
                 libgl-dev libgles-dev libegl-dev \
                 libsdl3-dev libpugixml-dev

# Add these only if building the Vulkan backend (-DSM2_BUILD_VULKAN=ON)
sudo apt install libvulkan-dev vulkan-validationlayers \
                 libvulkan-memory-allocator-dev
```

```sh
# Arch / Manjaro — default build (software + OpenGL)
sudo pacman -S --needed base-devel cmake ninja shaderc mesa sdl3 pugixml

# Add these only if building the Vulkan backend (-DSM2_BUILD_VULKAN=ON)
sudo pacman -S --needed vulkan-headers vulkan-icd-loader \
                        vulkan-validation-layers vulkan-memory-allocator
```

`mesa` provides the GL, GLES and EGL libraries and `shaderc` provides `glslc`.

### macOS

macOS uses the desktop OpenGL backend by default and needs nothing special. To
build the Vulkan backend as well (`-DSM2_BUILD_VULKAN=ON`), install the [LunarG
Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and source its `setup-env.sh`
so the loader can find MoltenVK:

```sh
. ~/VulkanSDK/setup-env.sh    # or wherever the SDK lives
```

Without this the loader starts but registers no driver, and sm2-emu reports
that no Vulkan devices were found. `tools/env.sh` looks in the usual places and
does this for you.

### Cross-compilation (Buildroot, Yocto, Batocera, embedded)

sm2-emu builds for `x86_64`, `aarch64` and `riscv64`. The only host tool that
runs during the build is `m68kmake` (a small C program that generates the 68000
opcode table). When cross-compiling, build it for the host first and pass its
path:

```sh
# On the host, once:
cc -o m68kmake /path/to/Musashi/m68kmake.c

# Cross-compile (GLES backend shown, typical for an ARM target):
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DSM2_M68KMAKE=/path/to/host-m68kmake \
    -DSM2_BUILD_OPENGL_DESKTOP=OFF -DSM2_BUILD_OPENGL_ES=ON \
    -DSM2_BUILD_TESTS=OFF
cmake --build build
```

`CMAKE_BUILD_TYPE` and its optimisation flags (`-O3` for `Release`) are a
CMake-level setting, so they apply to the target compiler the toolchain file
selects — a cross build gets the same optimisation as a native one, for its
own architecture. There is no `-march=native` anywhere, so a build stays
portable across the boards it targets.

The target sysroot must provide (same system libraries as a native build, for
the target architecture):

- SDL3 and pugixml
- the GL/GLES and EGL libraries, if a GL backend is built (the usual case)
- Vulkan 1.3 headers (`vulkan/vulkan.h`), loader (`libvulkan.so`) and
  VulkanMemoryAllocator, only if `-DSM2_BUILD_VULKAN=ON`
- `glslc` on the host PATH (it runs at build time, not on the target)

The Musashi, ymfm, miniz, LZMA-SDK and Dear ImGui sources are fetched at
configure time and built into the target objects, so those need host network
access at configure time but nothing in the sysroot.

## Running

```sh
./build/bin/sm2-emu --list-games
./build/bin/sm2-emu --list-gpus
./build/bin/sm2-emu [--graphics-backend <software|vulkan|opengl>] \
                    [--fullscreen] [--no-vsync] [--game <set>] vf2.zip
```

No ROM data is distributed with this software. Games are identified by the
CRC32 of their contents rather than by filename, so a merged archive holding
several revisions resolves correctly and `--game <set>` picks one out of it —
the four Virtua Fighter 2 revisions share a single file, as do the Virtua Cop
and Sega Rally families. A clone declares only the chips it respins and
inherits the rest from its parent, including out of the parent's archive if it
has none of its own.

```sh
./build/bin/sm2-emu --game vf2o vf2.zip
```

ROM layouts live in `data/games.xml`, so adding a game is a data edit. A region
is a flat byte array and each chip contributes `chunk` bytes every `stride`
bytes, which expresses every interleaving the hardware uses. The schema is
documented at the top of that file.

## Controls

Gamepads are read through SDL's gamepad layer, so anything with a mapping works
without configuration. The first pad to connect is player 1, pads can come and
go while the game runs, and `--list-gamepads` shows what was recognised. Face
buttons are read by position rather than by label. Driving games take a wheel
and pedals from the pad's stick and triggers; the gun games take aim from the
mouse.

| Gamepad | Function |
|---------|----------|
| D-pad or left stick | Stick |
| A B X Y | Buttons 1 to 4 |
| Left / right shoulder | Buttons 3 and 4 again |
| Start | Start |
| Back | Insert a coin |

The keyboard is live at the same time, so a second player can join on it and
the operator controls stay reachable without a pad:

| Keys | Function |
|------|----------|
| `5` `6` | Coin 1, coin 2 |
| `1` `2` | Start 1, start 2 |
| `9` `0` | Service, test |
| Arrows, `Z` `X` `C` `V` | Player 1 stick and buttons |
| `W` `A` `S` `D`, `G` `H` `J` `K` | Player 2 stick and buttons |
| `Escape` | Quit |
| `P` | Pause |
| `F2` | Switch between the GPU renderer and the software renderer |
| `Tab` (held) | Fast-forward |

## Settings

`--write-config` creates a `sm2-emu.ini` with every setting at its default and
a comment explaining each, which is the quickest way to see what can be set. It
is looked for in the working directory first and otherwise in the platform's
config directory (`$XDG_CONFIG_HOME/sm2-emu` on Linux, `~/Library/Application
Support/sm2-emu` on macOS); `--config <path>` overrides both, and whichever
file was used is named in the log. A command-line flag always beats the file,
and an unparseable line is reported and skipped rather than refused, so a file
from a later version cannot stop an earlier binary from starting.

## Frame pacing

The machine runs at 57.5245 Hz — 434600 cycles of a 25 MHz clock — which
divides into no monitor's refresh rate. Presenting one emulated frame per
display refresh would run the game four percent fast at 60 Hz, so it is paced
against real time instead and every emulated frame is presented exactly once:
nothing duplicated, nothing dropped, no input lost. On a 60 Hz display a frame
is occasionally held for two refreshes, which is unavoidable at this rate
without inventing frames.

Vsync and pacing compose rather than conflict — whichever wants the longer
frame wins. On a display slower than 57.5 Hz vsync would win and the game would
run slow, which is what `--no-vsync` is for. `--no-throttle`, or holding `Tab`,
runs as fast as the machine manages.

## Roadmap

| Phase | Milestone |
|:-----:|-----------|
| 0 | Window, Vulkan 1.3 device, swapchain, shader pipeline **(done)** |
| 1 | ROM loader, i960KB core, Model 2A memory map, timers and interrupts **(done)** |
| 2 | System 24 tilemaps — the first real picture **(done)** |
| 3 | TGP coprocessor, geometry engine, flat-shaded 3D **(done)** |
| 4 | Textures, the colour chain, translucency **(done)** |
| 5 | Gamepad input, configuration, frame pacing, 68000 + SCSP sound **(done)** |
| 6 | Presentation **(done)**, tilemap edge cases, accuracy, more games |
| 7 | Expand compatibility to load and run more games; Model 1 audio board **(done)** |
| 8 | Accelerate performance with Vulkan, offloading to the GPU **(done)** |
| 9 | OpenGL 4.3 desktop and OpenGL ES 3.1 backends **(done)** |
| 10 | Tidy everything up for a release with an associated GUI and options |

## Known gaps

- **The geometry engine is a high-level model, not an emulation.** Its
  microcode has never been dumped; what is emulated, following MAME, is what
  that microcode does, reconstructed from the equivalent program later boards
  upload. Results should match, timing does not.
- **The tilemap sky repeats visibly** on some sets. The name table really does
  repeat characters where the scenery is distant, so this is either a
  perspective stretch working as intended or something upstream of the tile
  chip. Unresolved.
- **Neither sound board has been diffed sample for sample.** The Model 1 board
  agrees with MAME's reference output on onset and roughly on level, but that
  is an envelope, not a waveform. The SCSP, which MAME itself marks imperfect,
  has not been checked to that depth.

## Future work

Roughly in order of increasing difficulty.

### Games with known issues

Everything with a local ROM archive runs. The exceptions:

- **Top Skater — shattered 3D geometry (post-1.0.0 fix).** Its 3D screens draw
  badly broken polygons. The fault is in the geometry engine, not the
  renderers — the software, Vulkan and OpenGL backends all draw the same
  corruption from the same geometry output — and MAME's own driver crashes
  before reaching those screens, so there is no reference to fix against yet.
- A handful of sets produce nothing here *and nothing in MAME*, because they
  are marked not-working upstream: Manx TT (both DX sets), Motor Raid DX,
  Virtual-On Relay, Royal Ascot II, and Sega Ski Super G (also unemulated
  protection). There is no reference to work against for these.

Separately, the Manx TT Deluxe cabinet carries a Model 1 audio board *on top
of* the 68000/SCSP board every Model 2A has. The audio board itself works, but
the machine has no slot for a second board yet, so those ROMs load unread.

### Internal resolution upscaling

Feasible for the 3D, because the geometry engine outputs floating-point
screen-space vertices that can be scaled before rasterisation. Polygons scale
cleanly and texture coordinates are unaffected, so filter quality improves
without touching the colour chain. Complications: the source textures are 4-bit
at up to 1024×1024 so the detail ceiling is low; the tilemap would need scaling
too; and stipple transparency is raster-locked to screen pixels, so it must be
evaluated on the upscaled grid or replaced with real alpha blending.

### Input and peripherals

Gamepad axes already drive the analog channels the driving games read, and the
mouse already drives the lightgun channels. What is missing:

- **Dedicated wheel and pedal devices**, with their own axis layout rather than
  being mapped from a pad's stick and triggers.
- **USB lightguns**, as opposed to the mouse.
- **Force feedback and rumble.** The drive board's commands are already on the
  serial link; translating them to SDL haptic events is not done.

### GUI and usability

- **Expand the settings GUI** with input binding, per-game overrides, a ROM
  path browser and volume control.
- **Game launcher.** A ROM directory scanner, so the command line is optional.
- **Save states**, with multiple slots. Arcade games have no native save.

### Additional features

- **Netplay.** The fixed-rate frame clock and deterministic emulation make
  rollback feasible.
- **Shader post-processing.** User-loadable GLSL/SPIR-V for CRT simulation,
  scanlines and colour grading, after the native frame is composed.
- **Run-ahead**, **rewind** and **input recording** — all natural extensions
  once save states exist.

## Licence and credits

BSD 3-Clause. See `LICENSE`.

I'm standing on the shoulders of giants, and SM2-Emu exists because of the MAME
project's reverse engineering of this hardware. The emulation is derived from
MAME's Sega Model 2 driver and its device cores, which their authors released
under the same licence. See `NOTICE` for per-component attribution.
