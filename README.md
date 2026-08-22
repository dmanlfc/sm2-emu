```
  ____  __  __  ____         _____ __  __ _   _
 / ___||  \/  ||___ \       | ____|  \/  | | | |
 \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
  ___) | |  | | / __/|_____|| |___| |  | | |_| |
 |____/|_|  |_||_____|      |_____|_|  |_|\___/

 A   S E G A   M O D E L   2   E M U L A T O R
```

Background: I started this emulation journey back in February 2025 to look to improve upon Model 2 emulations for Linux since my favourite OS lacked a native emulator and at the time MAME had incompatibility issues and was just slow for small Arm based SBC's. The mission therefore was to look into what MAME did well, understand more from reseach and analysis of Supermodel also (a Model 3 emulator) as inspiration. Supermodel actually led me to wire up OpenGL ES and Vulkan for that particular emulator as I could get quicker results as to the possibility of running Model 2 emulation on a Raspberry Pi 5 and bringing it to the emulation community.

Linux is the primary target. macOS is supported just because that's partly what I used for development and runs Vulkan through MoltenVK. I don't care for Windows... there I said it.

The journey included a lot of discussions with GenAI, I'm not going to lie but the capabilities for GenAI to really lean in and help tackle were somewhat limited. Providing it bite sized tasks sped up my part-time development from November 2025 until now, especially around how all the components hang together. 

Here I am in August 2026 and can actually release something which works (currently only Vulkan) and I'm confident I will finish in the next few months since I can take some of my previous learnings and port it to SM2-Emu.
Further along into 2026 GenAI has been very helpful with all the graphical quirks and elements and sped up development for Phase 7 considerably.

**Status: playable.** All four boards run — original Model 2, 2A, 2B and 2C — with
picture and sound. Of the 83 sets in the database, 40 draw a full 3D scene and 75
produce audio.

All three geometry coprocessors are there — the MB86234 TGP, the ADSP-21062 SHARC
and the MB86235 TGPx4 — along with the System 24 tilemap hardware, a textured
Vulkan renderer with the hardware's own colour chain evaluated per texel, and both
sound boards: the 68000/SCSP the CRX family uses, and the Model 1 audio board (a
68000 with a YM3438 and two MultiPCMs) that Daytona USA, Desert Tank and Virtua Cop
carry instead. Drive boards, lightguns, the link board and the protection devices
are wired.

Input comes from SDL gamepads with the keyboard live alongside them, and the
machine is paced to its own 57.5245 Hz rather than to the display. The frame is
composited at the hardware's 496x384 and magnified once at the end, so the
three-way composite runs on the hardware's pixels rather than on colours a filter
has already blurred.

A number of Model 2A sets are pixel-identical to MAME through the software
renderer. What is left is listed under [Known gaps](#known-gaps) and
[Future work](#future-work), and it is per-title now rather than per board.

## What Model 2 is, and why the renderer looks unusual

Model 2 is an i960KB paired with a geometry coprocessor (a Fujitsu MB86234 "TGP"
on Model 2 and 2A, an ADSP-21062 SHARC on 2B, an MB86235 on 2C), a custom
Sega/Lockheed-Martin rasterizer, and Sega System 24 tilemap hardware for the 2D
layers. Output is 496x384 at roughly 57.5 Hz.

Four properties of that rasterizer shape the whole design, because none has a
direct modern equivalent:

- **No depth buffer.** Polygons are bucket-sorted by depth on the CPU and drawn
  front to back against a one-bit fill mask: first writer wins. Reproduced with a
  stencil attachment rather than a depth test.
- **No RGB textures.** A texel is a 4-bit *intensity*; colour arrives through a
  tone curve, a base colour, a translation table and a gamma ramp. The curve is
  applied *after* filtering, so it cannot be baked in — the whole chain runs per
  texel in the fragment shader.
- **No alpha blending.** Translucency is an alpha test on one texel value, or a
  screen-locked stipple. Both discard fragments, which leaves the fill mask
  unclaimed so what is behind still gets the pixel.
- **No per-vertex shading.** One 8-bit luminance scalar per polygon.

So the geometry pipeline runs on the CPU as the hardware's did, and Vulkan is handed
pre-projected screen-space triangles. No vertex transformation on the GPU, no
geometry shaders.

## Building

Requirements:

- CMake 3.24 or newer, and Ninja
- A C++20 compiler
- Vulkan 1.3 headers, loader and `glslc`
- SDL3 (fetched automatically if not installed)

```sh
. tools/env.sh          # optional: adds ~/.local/bin and the Vulkan SDK to PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Useful options: `-DSM2_ENABLE_VALIDATION=ON` (default in Debug),
`-DSM2_WERROR=ON`, `-DSM2_BUILD_TESTS=OFF`.

### Linux dependencies

```sh
# Debian / Ubuntu
sudo apt install cmake ninja-build build-essential \
                 libvulkan-dev glslc vulkan-validationlayers \
                 libsdl3-dev            # optional; otherwise fetched
```

### macOS

There is no Homebrew requirement, but the [LunarG Vulkan
SDK](https://vulkan.lunarg.com/sdk/home) must be installed, and its
`setup-env.sh` sourced, so that the loader can find MoltenVK:

```sh
. ~/VulkanSDK/setup-env.sh    # or wherever the SDK lives
```

Without this the loader starts but registers no driver, and sm2-emu reports that
no Vulkan devices were found. `tools/env.sh` looks in the usual places and does
this for you.

### Cross-compilation (buildroot, Yocto, embedded)

sm2-emu builds for `x86_64`, `aarch64` and `riscv64`. The only host tool that
runs during the build is `m68kmake` (a small C program that generates the 68000
opcode table). When cross-compiling, build it for the host first and pass its
path:

```sh
# On the host, once:
cc -o m68kmake /path/to/Musashi/m68kmake.c

# Cross-compile:
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
    -DSM2_M68KMAKE=/path/to/host-m68kmake \
    -DSM2_BUILD_TESTS=OFF
cmake --build build
```

The sysroot must provide:
- Vulkan 1.3 headers (`vulkan/vulkan.h`)
- Vulkan loader library (`libvulkan.so`)
- `glslc` on the host PATH (it runs at build time, not on the target)

## Running

```sh
./build/bin/sm2-emu --list-games
./build/bin/sm2-emu --list-gpus
./build/bin/sm2-emu [--validation] [--no-vsync] [--gpu <name>] [--game <set>] vf2.zip
```

No ROM data is distributed with this software. Games are identified by the CRC32 of
their contents rather than by filename, so a merged archive holding several
revisions resolves correctly and `--game <set>` picks one out of it — the four
Virtua Fighter 2 revisions share a single file, as do the Virtua Cop and Sega Rally
families. A clone declares only the chips it respins and inherits the rest from its
parent, including out of the parent's archive if it has none of its own.

```sh
./build/bin/sm2-emu --game vf2o vf2.zip
```

ROM layouts live in `data/games.xml`, so adding a game is a data edit. A region
is a flat byte array and each chip contributes `chunk` bytes every `stride`
bytes, which expresses every interleaving the hardware uses. The schema is
documented at the top of that file. `--dump-roms <dir>` writes the assembled
regions out, and `tools/verify_rom_layout.py` checks them against an independent
transcription of MAME's load macros — worth doing, because an interleaving mistake
produces data of exactly the right size and only a byte comparison catches it.

## Diagnostics

Everything below works headless, so a change can be checked without a window and
without anyone watching.

| Flag | What it gives you |
|------|-------------------|
| `--boot-test <n>` | run `n` frames with no window, then report where the program reached |
| `--dump-tilemap <dir>` | the four decoded layers, the character set, the composed frame, each priority category on its own, and `wireframe.ppm` |
| `--screenshot <f>` | the finished frame as a PPM, at the native 496x384 before magnification |
| `--screenshot-frames <list>` | capture exactly these frame numbers |
| `--dump-audio <f>` | everything the sound board produced, as a WAV |
| `--run-frames <n>` | quit after `n` frames |
| `--coin-at <n>` | insert two coins, press start and confirm a character on a fixed schedule |
| `--log-unmapped` | every access landing outside a mapped region |
| `--nvram <dir>` | where `<set>.nv` and `<set>.eeprom` live |

```sh
./build/bin/sm2-emu --boot-test 1700 --coin-at 1200 --dump-tilemap /tmp/wf vf2.zip
tools/ppm_to_png.py /tmp/wf/wireframe.ppm
```

The boot-test report is the useful part. It covers the coprocessor (instructions
retired, commands taken, results returned) and checks its mathematical lookup
units against the standard library using the real table ROMs, which is the only
check that can call those answers correct rather than merely self-consistent — a
boot test fails if any unit drifts out of tolerance. It summarises the frame the
geometry engine produced: polygons kept against culled and clipped away, and the
screen and depth extents. **The extents are what to read first**, because geometry
that clips correctly lands exactly on the raster's bounds, so a subtly wrong
projection shows as an extent that overshoots or falls short rather than as a
picture that merely looks odd.

It also reports the sound board in its own right — where the 68000 is, how many
bytes crossed the serial link each way, which chip registers were touched, how many
voices are sounding — and says which of the two boards it found. A silent game with
traffic on the link is a different problem from a silent game without it.

`wireframe.ppm` deliberately shows polygons a shaded renderer would hide, which is
what makes it useful for judging geometry alone. `tools/ppm_to_png.py` converts any
of these dumps using only the Python standard library.

Two sweeps run a whole fleet and say which sets are worth a closer look:

```sh
python3 tools/graphics_sweep.py          # flags geometry faults, no MAME needed
python3 tools/audio_sweep.py --board ""  # which sets make a noise, and from when
```

## Controls

Gamepads are read through SDL's gamepad layer, so anything with a mapping works
without configuration. The first pad to connect is player 1, pads can come and go
while the game runs, and `--list-gamepads` shows what was recognised. Face buttons
are read by position rather than by label. Driving games take a wheel and pedals
from the pad's stick and triggers; the gun games take aim from the mouse.

| Gamepad | Function |
|---------|----------|
| D-pad or left stick | Stick |
| A B X Y | Buttons 1 to 4 |
| Left / right shoulder | Buttons 3 and 4 again |
| Start | Start |
| Back | Insert a coin |

The keyboard is live at the same time, so a second player can join on it and the
operator controls stay reachable without a pad:

| Keys | Function |
|------|----------|
| `5` `6` | Coin 1, coin 2 |
| `1` `2` | Start 1, start 2 |
| `9` `0` | Service, test |
| Arrows, `Z` `X` `C` `V` | Player 1 stick and buttons |
| `W` `A` `S` `D`, `G` `H` `J` `K` | Player 2 stick and buttons |
| `Escape` | Quit |
| `P` | Pause |
| `Tab` (held) | Fast-forward |

## Settings

`--write-config` creates a `sm2-emu.ini` with every setting at its default and a
comment explaining each, which is the quickest way to see what can be set. It is
looked for in the working directory first and otherwise in the platform's config
directory (`$XDG_CONFIG_HOME/sm2-emu` on Linux, `~/Library/Application
Support/sm2-emu` on macOS); `--config <path>` overrides both, and whichever file
was used is named in the log. A command-line flag always beats the file, and an
unparseable line is reported and skipped rather than refused, so a file from a
later version cannot stop an earlier binary from starting.

## Frame pacing

The machine runs at 57.5245 Hz — 434600 cycles of a 25 MHz clock — which divides
into no monitor's refresh rate. Presenting one emulated frame per display refresh
would run the game four percent fast at 60 Hz, so it is paced against real time
instead and every emulated frame is presented exactly once: nothing duplicated,
nothing dropped, no input lost. On a 60 Hz display a frame is occasionally held for
two refreshes, which is unavoidable at this rate without inventing frames.

Vsync and pacing compose rather than conflict — whichever wants the longer frame
wins. On a display slower than 57.5 Hz vsync would win and the game would run slow,
which is what `--no-vsync` is for. `--no-throttle`, or holding `Tab`, runs as fast
as the machine manages.

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
| 8 | Accelerate performance with Vulkan first to offload to GPU as much as possible |
| 9 | Add OpenGL Desktop for MacOS & x86_64 Linux & OpenGL ES support for Arm on Linux |
| 10 | Tidy everything up for a release with associated GUI with options |


## Known gaps

- **Pixel comparison against MAME works, but not on every set.** The two emulators
  drift apart within seconds — the geometry engine here is a model of a DSP rather
  than an emulation of one, the coprocessor is interleaved eagerly, the sound boards
  run on a scanline granularity — so a frame-N-against-frame-N diff is meaningless.
  `tools/framecmp.py` locks the offset on whichever sample has the sharpest
  correlation peak and reports MAME↔software, MAME↔Vulkan and software↔Vulkan side
  by side; where the first two agree, a fault is in emulated hardware, where only
  the Vulkan column differs it is the renderer. A good number of Model 2A sets reach
  1.0000 that way.

  Two caveats that have each produced a wrong conclusion here. The lock fails when
  the sampled frames are all static or all animated — Virtua Cop scores every
  candidate within 0.002 of every other — and an unlocked figure means nothing. And
  a 2D splash screen reads 1.0000 while saying nothing about the 3D pipeline, so
  read any score with the polygon count beside it.
- **The geometry engine is a high-level model, not an emulation.** Its microcode has
  never been dumped; what is emulated, following MAME, is what that microcode does,
  reconstructed from the equivalent program later boards upload. Results should
  match, timing does not.
- **The tilemap sky repeats visibly.** The name table really does repeat characters
  where the scenery is distant and the tile chip's registers agree with MAME's on
  every value used, so this is either a perspective stretch working as intended or
  something upstream of the tile chip. Unresolved.
- **Tilemap split modes 2 and 3 are untested by a real program.** All three modes
  are implemented and unit-tested, and the tests were checked by mutation, but no
  set exercised so far uses anything but mode 1.
- **The SCSP's DMA and host-facing interrupt are untested.** Ported but unexercised;
  MAME flags parts of them as wanting checking too. Anything reaching them logs it.
- **The audio clock does not lead the frame clock.** Each board produces its own
  rate's worth of samples per frame, so the queue stays inside a 40 ms band, but it
  is not *derived* from the device's real consumption — a device whose true rate
  differs from its nominal one would drift.
- **Neither sound board has been diffed sample for sample.** The Model 1 board
  agrees with MAME's `-wavwrite` output on onset second and within ~10% on
  per-second RMS, but that is an envelope, not a waveform. Nothing has been done for
  the SCSP, which MAME marks imperfect anyway.
- **The YM3438's FM has only ever been observed silent** — zero through boot and
  attract on all three original Model 2 sets, and so is MAME's, confirmed by probing
  its stream. What *is* verified is that its Timer B clocks the sound driver's
  sequencer, and that without it no PCM voice is keyed on at all.
- **The serial link is modelled per byte, not per bit.** Both ends are fixed at
  8-N-1, 31250 baud and nothing observes an individual bit. A program that
  reprogrammed the framing, or watched the line idle, would not see hardware.

## Future work

Roughly in order of increasing difficulty.

### Games that still do not run

All four boards work, so what is left is per-title. **Dynamite Baseball**,
**Dynamite Baseball 97** and **Sonic Championship** are the three worth chasing:
each produces little or no geometry where MAME produces a full scene.

Manx TT (both DX sets), Motor Raid DX, Virtual-On Relay, Sega Ski Super G and Royal
Ascot II produce nothing here *and nothing in MAME* — all are
`MACHINE_NOT_WORKING` upstream, Sega Ski Super G additionally
`MACHINE_UNEMULATED_PROTECTION` — so there is no reference to work against. Top
Skater draws about as much here as in MAME and MAME aborts partway through a
headless run of it, so there is no stable reference either way.

Separately, the Manx TT Deluxe cabinet carries a Model 1 audio board *on top of*
the 68000/SCSP board every Model 2A has, forked off the same host UART. `hw::M1Audio`
works but `hw::Model2` has no slot for a second board, so those ROMs load unread.
Neither set draws any geometry yet, so there is nothing to listen to either.

### Accuracy and tooling

- **Audio clock pull**, so the device's real rate governs the frame clock.
- **Sample-for-sample waveform comparison** against MAME on both sound boards; only
  a per-second RMS envelope has been checked so far.
- **A static anchor frame for Virtua Cop**, without which `framecmp.py` cannot lock
  an offset for it and no pixel figure can be trusted.

### Rendering backends

- **OpenGL 3.3+ desktop backend.** An alternative to Vulkan for systems where
  Vulkan support is absent or immature.
- **OpenGL ES 3.x backend.** For ARM devices (Raspberry Pi, embedded boards,
  phones) that lack Vulkan drivers entirely.

### GPU acceleration and Pi 5 performance

The geometry engine and the tilemap chip still run on the CPU, and the GPU receives
pre-projected screen-space triangles at native 496×384. Texture decode has already
moved to a compute pass (`shaders/texel_decode.comp`), which unpacks each sheet's
4-bit texels once per upload instead of the fragment shader shifting out a nibble on
every one of up to sixteen taps; verified bit-exact against the previous renderer.
Everything below is still to do:

- **Split textured and untextured pipelines.** Untextured polygons never `discard`,
  so a separate pipeline can use early stencil rejection.
- **Stencil pre-pass.** Claim fill-mask bits in a stencil-only pass, then draw
  textured with stencil EQUAL so the expensive shader runs only on visible pixels.
  Stipple-transparent polygons conditionally leave stencil unclaimed and still need
  the current path.
- **GPU tilemap rasterisation.** Decode tiles from tile and character RAM on the
  GPU, eliminating 1.5 MB/frame of upload.
- **Profile per-frame buffer traffic.** The vertex and parameter buffers are
  host-coherent, which is already efficient on unified-memory ARM SoCs, but stray
  flushes and barriers are worth checking for.

None of this has been measured on Pi 5 hardware — there is no device here to test
on. Frame-time numbers from anyone with a Pi 5 or similar tile-based ARM GPU would
make this section a great deal more useful.

### Internal resolution upscaling

Feasible for the 3D, because the geometry engine outputs floating-point
screen-space vertices that can be scaled before rasterisation. Polygons scale
cleanly and texture coordinates are unaffected, so filter quality improves without
touching the colour chain. Three things complicate it: the source textures are 4-bit
at up to 1024×1024 so the detail ceiling is low; the tilemap would need to move to
the GPU or be post-filtered; and **stipple transparency is raster-locked** to
`(x ^ y) & 1` in screen pixels, so it must be evaluated on the upscaled grid or
replaced with real alpha blending.

An `--internal-resolution <n>` multiplier would scale the offscreen polygon
framebuffer and its stencil attachment and adjust the vertex shader's `invRaster`
uniform, leaving the tilemap and present passes alone.

### Input and peripherals

Gamepad axes already drive the analog channels the driving games read, and the mouse
already drives the lightgun channels. What is missing:

- **Dedicated wheel and pedal devices**, with their own axis layout rather than
  being mapped from a pad's stick and triggers.
- **USB lightguns**, as opposed to the mouse.
- **Force feedback and rumble.** The drive board's commands are already on the
  serial link; translating them to SDL haptic events is not done.

### GUI and usability

- **Expand the settings GUI** with input binding, per-game overrides, a ROM path
  browser and volume control.
- **Game launcher.** A ROM directory scanner, so the command line is optional.
- **Save states**, with multiple slots. Arcade games have no native save.

### Additional features

- **Netplay.** The fixed-rate frame clock and deterministic emulation make rollback
  feasible.
- **Shader post-processing.** User-loadable GLSL/SPIR-V for CRT simulation,
  scanlines and colour grading, after the native frame is composed.
- **Run-ahead**, **rewind** and **input recording** — all natural extensions once
  save states exist.

## Licence and credits

BSD 3-Clause. See `LICENSE`.


I'm standing on the shoulders of giants and SM2-Emu exists because of the MAME project's reverse engineering of this hardware. The emulation is derived from MAME's Sega Model 2 driver and its device cores, which their authors released under the same licence. See `NOTICE` for per-component attribution.
