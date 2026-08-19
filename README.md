```
  ____  __  __  ____         _____ __  __ _   _
 / ___||  \/  ||___ \       | ____|  \/  | | | |
 \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
  ___) | |  | | / __/|_____|| |___| |  | | |_| |
 |____/|_|  |_||_____|      |_____|_|  |_|\___/

 A   S E G A   M O D E L   2   E M U L A T O R
```

Linux is the primary target. macOS is supported just because that's partly what I used for development and runs Vulkan through MoltenVK. I don't care for Windows... there I said it.

Background: I started this emulation journey back in February 2025 to look to improve upon Model 2 emulations for Linux since my favourite OS lacked a native emulator and at the time MAME had incompatibility issues and was just slow for small Arm based SBC's. The mission therefore was to look into what MAME did well, understand more from reseach and analysis of Supermodel also (a Model 3 emulator) as inspiration. Supermodel actually led me to wire up OpenGL ES and Vulkan for that particular emulator as I could get quicker results as to the possibility of running Model 2 emulation on a Raspberry Pi 5 and bringing it to the emulation community.

The journey included a lot of discussions with GenAI, I'm not going to lie but the capabilities for GenAI to really lean in and help tackle were somewhat limited. Providing it bite sized tasks sped up my part-time development from November 2025 until now, especially around how all the components hang together. 

Here I am in August 2026 and can actually release something which works (currently only Vulkan) and I'm confident I will finish in the next few months since I can take some of my previous learnings and port it to SM2-Emu.

**Status: playable.** Virtua Fighter 2 plays, with picture and sound.
The i960 CPU, the Model 2A machine, the System 24 tilemap hardware, the MB86234
geometry coprocessor, the geometry engine, a textured Vulkan renderer and the
sound board all work. The game boots, runs its self-test, plays its attract mode,
accepts a coin and reaches a match, with every polygon the hardware would draw — around 1500 a frame, clipped,
depth-sorted and composited between the tilemap layers that belong behind them and
the status display in front.

The colour chain is the hardware's own, evaluated per texel in the fragment
shader: a four-bit texture intensity, bilinear within a mipmap level and linear between two, then the polygon's tone curve out of luminance RAM scaled by the geometry engine's lighting term, then the master colour translation table and the monitor gamma ramp. Translucency is the hardware's too — an alpha test on one texel value, or a screen-locked stipple — which is what puts back the shadows, the foliage and the smoke.

Sound is the real sound board: a 68000 running the game's own sound program, and a Yamaha SCSP with its 32 PCM voices, three timers and effects DSP, ported from MAME. The host and the sound board talk over the serial link they do on hardware, an 8251 feeding the SCSP's MIDI port, so what comes out is what the game asked for.

Input comes from SDL gamepads, with the keyboard live alongside them, and the
machine is paced to its own 57.5245 Hz rather than to the display. The frame is
composited at the hardware's own 496x384 and magnified to the window once, at the
end, so the three-way composite runs on the hardware's pixels rather than on
colours a filter has already blurred. The game database holds Virtua Fighter 2 and
its three earlier revisions.

## What Model 2 is, and why the renderer looks unusual

Model 2 is an i960KB paired with a geometry coprocessor (a Fujitsu MB86234 "TGP"
on Model 2 and 2A, an ADSP-21062 SHARC on 2B, an MB86235 on 2C), a custom
Sega/Lockheed-Martin rasterizer, and Sega System 24 tilemap hardware for the 2D
layers. Output is 496x384 at roughly 57.5 Hz.

Four properties of that rasterizer shape the whole design, because none has a
direct modern equivalent:

- **No depth buffer.** Polygons are bucket-sorted by depth on the CPU and drawn
  front to back against a one-bit fill mask: first writer wins. Games rely on
  this, with per-polygon sort overrides and priority "windows" that ignore depth
  entirely. sm2-emu reproduces it with a stencil attachment rather than a depth
  test.
- **No RGB textures.** A texel is a 4-bit *intensity*. Colour arrives through a
  per-polygon tone curve, a per-polygon 15-bit base colour, a global colour
  translation table and a gamma ramp. The tone curve is applied *after*
  filtering, so it cannot be baked into the texture; sm2-emu keeps texture RAM in
  its packed form and runs the whole chain per texel in the fragment shader.
- **No alpha blending.** Translucency is an alpha test on one texel value, or a
  screen-locked checkerboard stipple. Both are reproduced by discarding fragments,
  which also leaves the fill mask unclaimed so what is behind still gets the pixel.
- **No per-vertex shading.** One 8-bit luminance scalar per polygon, with
  diffuse, ambient and specular already collapsed into it.

Consequently the geometry pipeline runs on the CPU, exactly as the hardware's
did, and Vulkan is handed pre-projected screen-space triangles. There is no
vertex transformation on the GPU, and no need for geometry shaders.

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

No ROM data is distributed with this software. Games are identified by the CRC32
of their contents rather than by filename, so merged archives holding several
revisions resolve correctly; `--game <set>` picks a specific one.

The four Virtua Fighter 2 revisions all live in one archive and differ only in
their program EPROMs, so the four are `vf2`, `vf2b`, `vf2a` and `vf2o`, and any of
them can be run from the same file:

```sh
./build/bin/sm2-emu --game vf2o vf2.zip
```

ROM layouts live in `data/games.xml`, so adding a game is a data edit. A region
is a flat byte array and each chip contributes `chunk` bytes every `stride`
bytes, which expresses every interleaving the hardware uses. The schema is
documented in comments at the top of that file.

`--dump-roms <dir>` writes the assembled regions out as raw binaries.
`tools/verify_rom_layout.py` then checks them against an independent
transcription of MAME's load macros:

```sh
./build/bin/sm2-emu --dump-roms /tmp/dump vf2.zip
tools/verify_rom_layout.py vf2.zip /tmp/dump
```

This matters because an interleaving mistake produces data of exactly the right
size, so only a byte comparison against a separate implementation is conclusive.

### Checking how far a game gets

`--boot-test <frames>` runs the machine with no window and no Vulkan, then reports
where the program reached. It is the quickest way to see whether a change moved
the boot forward:

```sh
./build/bin/sm2-emu --boot-test 600 vf2.zip
```

It prints a summary of the tilemap layers and an ASCII rendering of each, which is
enough to tell whether the program has drawn anything without opening a window.

`--dump-tilemap <dir>` writes more: the four decoded layers and the whole
character set as greyscale images, the composed frame through the real colour
chain, and each layer and priority category on its own. That last set is what
attributes a wrong picture to a specific layer rather than to the compositor.

A boot test also reports what the geometry coprocessor did — instructions retired,
commands taken, results returned, display list words written — and checks its
mathematical lookup units against the standard library using the real table ROMs.
That last check is the only one that can say those answers are correct rather than
merely self-consistent, so a boot test fails if any unit drifts out of tolerance.

### Checking the geometry before anything shades it

A boot test also summarises the frame the geometry engine produced: polygons kept
against polygons culled and clipped away, the spread of vertex counts, how many
priority windows the game used, and the screen and depth extents of the result.
The extents are the useful part. Geometry that clips correctly lands exactly on the
raster's bounds, so a projection or a clipping plane that is subtly wrong shows up
as an extent that overshoots or falls short rather than as a picture that merely
looks odd.

It also breaks the frame down by the pixel path each polygon asks for, which is
what says how much of it the current renderer can actually draw.

`--dump-tilemap` additionally writes `wireframe.ppm`, every polygon's outline in
draw order. It deliberately shows polygons that a shaded renderer would hide,
which is what makes it useful for judging geometry on its own:

```sh
./build/bin/sm2-emu --boot-test 1700 --coin-at 1200 --dump-tilemap /tmp/wf vf2.zip
tools/ppm_to_png.py /tmp/wf/wireframe.ppm
```

Add `--log-unmapped` to see every access that lands outside a mapped region.
Operator settings persist to `nvram/<set>.nv` and `nvram/<set>.eeprom`; change the
directory with `--nvram`.

### Checking what actually reached the screen

`--screenshot <file>` copies the finished frame back to host memory and writes it as
a PPM, and `--run-frames <n>` quits after a fixed number of frames. Together they
make the rendered result something the build can check on its own rather than
something a person has to look at:

```sh
./build/bin/sm2-emu --no-throttle --no-vsync \
                    --run-frames 620 --screenshot /tmp/shot.ppm vf2.zip
tools/ppm_to_png.py /tmp/shot.ppm
```

Add `--screenshot-interval <n>` to capture a series rather than a single frame,
which is how a whole boot sequence gets reviewed in one run.

A capture is the native 496x384 frame, taken before the magnification to the
window. That makes it independent of the window size, small enough to keep, and
comparable pixel for pixel against the same frame from another emulator. The cost
is that the final scaling is the one stage a capture does not cover.

`tools/ppm_to_png.py` converts any of these dumps to PNG using nothing but the
Python standard library, so it works on a machine with no image tooling.

### Checking the sound

`--dump-audio <file>` writes everything the sound board produced to a WAV, and it
works with `--boot-test` as well as with a window, so a machine with no audio
device can still be checked. Counters say how many voices started and how loud the
result got; a file is what says whether it is music.

```sh
./build/bin/sm2-emu --boot-test 1750 --coin-at 1200 \
                    --dump-audio /tmp/vf2.wav vf2.zip
```

The `--boot-test` report also covers the sound board in its own right: where the
68000's program counter is, how many bytes crossed the serial link in each
direction, how many SCSP registers were touched, and how many voices are sounding.
A silent game with traffic on the link is a different problem from a silent game
without it.

## Controls

Gamepads are read through SDL's gamepad layer, so anything with a mapping works
without configuration. The first pad to connect is player 1; pads can be plugged
in and out while the game runs. `--list-gamepads` shows what was recognised.

| Gamepad | Function |
|---------|----------|
| D-pad or left stick | Stick |
| A B X Y | Buttons 1 to 4 |
| Left / right shoulder | Buttons 3 and 4 again |
| Start | Start |
| Back | Insert a coin |

The keyboard is live at the same time, so a second player can join on it and the
operator controls stay reachable without a pad.

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

Face buttons are read by position rather than by label, so a pad reports the same
physical button whatever it prints on it.

## Settings

Everything worth keeping between runs lives in a `sm2-emu.ini`. `--write-config`
creates one with every setting at its default and a comment explaining each, which
is the quickest way to see what can be set:

```sh
./build/bin/sm2-emu --write-config
```

The file is looked for in the working directory first, which is what a build tree
wants, and otherwise in the platform's configuration directory
(`$XDG_CONFIG_HOME/sm2-emu` or `~/.config/sm2-emu` on Linux, `~/Library/Application
Support/sm2-emu` on macOS). `--config <path>` overrides both. Whichever file was
used is named in the log, so it is never a guess.

A command-line flag always beats the file. A line the parser cannot make sense of
is reported and skipped rather than refused, so a file written by a later version
cannot stop an earlier binary from starting.

## Frame pacing

The machine runs at 57.5245 Hz — 434600 cycles of a 25 MHz clock — which divides
into no monitor's refresh rate. Presenting one emulated frame per display refresh
would run the game four percent fast at 60 Hz, so it is paced against real time
instead, and every emulated frame is presented exactly once. Nothing is duplicated
and nothing is dropped, so no input is ever lost; on a 60 Hz display a frame is
occasionally held for two refreshes, which is unavoidable at this rate without
inventing frames.

Vsync and pacing compose rather than conflict: whichever wants the longer frame
wins. On a display slower than 57.5 Hz vsync would win and the game would run slow,
which is what `--no-vsync` is for. `--no-throttle`, or holding `Tab`, runs as fast
as the machine manages.

### Getting past the attract mode without anyone at the controls

`--coin-at <frame>` inserts two coins, presses start and confirms a character on a
fixed schedule, so a capture can reach the game itself:

```sh
./build/bin/sm2-emu --no-throttle --no-vsync --run-frames 1750 --coin-at 1200 \
                    --screenshot /tmp/shot.ppm vf2.zip
```

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

First playable target is Virtua Fighter 2, chosen because it needs the least
hardware: no drive board, no lightgun, no protection, digital inputs only. Its
three earlier revisions run too, and they are what the clone mechanism was built
for: each declares only its own four program EPROMs and inherits every other
region from the parent, and the right four are picked out of a merged archive by
CRC rather than by name.

Anything beyond Virtua Fighter 2 needs hardware that is not written yet. The
Model 2B and 2C boards upload their geometry microcode to a real DSP instead of
holding it in ROM; the driving games need the drive board and the analogue channel
wiring, which is per-game; the gun games need a lightgun interface; and several
need protection devices. Adding a set to `data/games.xml` without its ROMs to test
against would only be a guess, so the database holds what has been run.

## Known gaps

- **Nothing has been compared against MAME's own output.** Every stage was checked
  line-for-line against MAME's source and the results look right, but that is not
  the same as agreeing with it.

  A pixel diff is harder than it sounds, and worth being precise about rather than
  leaving as a to-do. Comparing frame N of two emulators requires them to be at the
  same point in the game, and they will not be: the geometry engine here is a model
  of what a DSP does rather than an emulation of it, the coprocessor is interleaved
  eagerly instead of through a scheduler, and the sound board runs on a scanline
  granularity. Those are all defensible individually and all change timing, so the
  attract mode diverges within seconds and a scripted coin lands on a different
  frame. A meaningful comparison would have to synchronise on something the game
  itself does -- a specific write to a specific register -- and capture both sides
  there, which means patching MAME rather than just running it. `--screenshot` now
  produces the native frame so that the comparison is possible; making it is a
  project of its own.
- **The geometry engine is a high-level model, not an emulation.** On Model 2 and
  2A the engine is a DSP whose microcode has never been dumped. What is emulated
  here, following MAME, is what that microcode does, reconstructed from the
  equivalent program later boards upload. Results should match; timing does not.
- **The tilemap sky repeats visibly.** The name table the game writes really does
  repeat characters where the scenery is distant, and the tile chip's registers
  agree with MAME's on every value the game uses, so this is either a perspective
  stretch working as intended or something upstream of the tile chip. It is much
  more obvious now that the 3D covers everything else. Unresolved.
- **No game exercising split modes 2 and 3 has been tested.** All three modes are
  implemented and all three are covered by unit tests, including the row-scrolled
  paths and the side selection, and those tests were checked by mutation: breaking
  the boundary arithmetic or conflating modes 2 and 3 makes them fail. But Virtua
  Fighter 2 only uses mode 1, so what is confirmed is agreement with a reading of
  MAME's source rather than with a program that depends on it.
- **The sound board's DMA and its host-facing interrupt are untested.** Virtua
  Fighter 2 uses neither, so both are ported but unexercised, and MAME flags parts
  of them as wanting checking too. Anything reaching them says so in the log.
- **The audio clock does not lead the frame clock.** Pacing is against the system
  clock alone, and the sound board produces exactly 44100 Hz worth of samples per
  emulated frame, so the queue stays inside a 40 ms band and neither starves nor
  overflows in practice. It is not *derived* from the audio device's consumption
  though, so a device whose real rate differs from its nominal one would drift, and
  the usual answer — letting the audio clock pull the frame rate — is not
  implemented.
- **The SCSP has not been compared against MAME's samples.** The output is
  recognisably the right music, in stereo, with the right voices starting and
  stopping, and `--dump-audio` exists to make a comparison possible, but no
  waveform diff has been done. MAME marks its own SCSP as having imperfect sound.
- **The serial link is modelled a byte at a time, not a bit at a time.** Both ends
  are fixed at 8-N-1, 31250 baud by construction and nothing between them observes
  an individual bit, so only the byte period is kept. A program that reprogrammed
  the framing, or watched the line idle, would not see what hardware does.

## Future work

Items below are not yet implemented. They are listed roughly in order of
increasing difficulty and decreasing proximity to what already works.

### More Model 2A games

The ROM database now holds Sky Target, Virtua Cop 2, Zero Gunner and Dead or
Alive alongside the four Virtua Fighter 2 revisions. Making them run needs:

- **Analog input wiring.** Sega Rally, Manx TT and Motor Raid need the I/O
  controller's analog mux channels connected through SDL gamepad axes to
  steering, throttle and brake. The `Io315_5649` already exposes `set_analog()`;
  what is missing is the SDL side and a per-game binding table.
- **Lightgun interface.** Virtua Cop 2 reads gun coordinates through the analog
  channels. The coordinates are screen-relative, so the host needs to transform
  mouse or lightgun position into the hardware's coordinate space.
- **315-5881 encryption chip.** Zero Gunner, Dynamite Cop and Pilot Kids encrypt
  their program ROMs. MAME's decryption logic lives in
  `src/devices/machine/315-5838_317-0229_comp.cpp`.
- **Drive board stub.** Sega Rally and Manx TT poll a Z80-based force-feedback
  board over a link. A stub that acknowledges commands without emulating the
  board would unblock them.

### Model 2B — SHARC coprocessor

Model 2B replaces the MB86234 TGP with an Analog Devices ADSP-21062 SHARC that
runs geometry microcode uploaded at boot. Games: Fighting Vipers, Last Bronx,
Sonic the Fighters, Virtual-On, Virtua Striker, Gunblade NY.

- Port or integrate MAME's SHARC core (`src/devices/cpu/sharc/`).
- Wire it to the same FIFO and display-list interface the TGP uses.
- Handle the SHARC's DMA program upload (different from the TGP's word-at-a-time
  upload).

### Model 2C — MB86235 TGPx4

Model 2C uses the MB86235 (MAME: `src/devices/cpu/mb86235/`). Games: House of
the Dead, Sega Touring Car Championship, Top Skater, Wave Runner, Sega Water
Ski.

### Original Model 2

The original board has a different sound system (YM3438 + two MultiPCM chips
instead of the 68000/SCSP) and a slightly different TGP variant. Games: Daytona
USA, Desert Tank, Virtua Cop.

### Accuracy and tooling

- **Pixel comparison with MAME.** Frame-synchronise by register write rather
  than frame count, then diff the native frames. `--screenshot` already produces
  the right format.
- **Audio clock pull.** Let the audio device's real sample rate govern the frame
  clock so the two cannot drift apart.
- **SCSP waveform comparison.** Diff `--dump-audio` output against MAME's.

### Rendering backends

- **OpenGL 3.3+ desktop backend.** An alternative to Vulkan for systems where
  Vulkan support is absent or immature.
- **OpenGL ES 3.x backend.** For ARM devices (Raspberry Pi, embedded boards,
  phones) that lack Vulkan drivers entirely.

### GPU acceleration and Pi 5 performance

The current pipeline runs the geometry engine and the tilemap chip entirely on
the CPU and hands the GPU pre-projected screen-space triangles at native 496×384.
The fragment shader is the main bottleneck: it manually implements trilinear
filtering from a packed 4-bit SSBO (4–16 random memory reads per fragment) and
runs the full tone-curve colour chain per texel. On a tile-based GPU like
VideoCore VII these random SSBO accesses bypass the texture cache entirely.

Offloading opportunities, roughly in order of impact:

- **Pre-decode texture sheets into a GPU-native format.** Convert the packed 4-bit
  texels into `R8_UNORM` with proper mipmaps on upload, then let the hardware
  texture unit do bilinear/trilinear filtering. This replaces hundreds of SSBO
  reads per fragment with one or two hardware texture samples. The tone curve is
  still applied after filtering (one lookup per channel), but the dominant cost
  disappears.
- **Split textured and untextured pipelines.** Untextured polygons need only the
  colour lookup (three `texelFetch` calls) and never `discard`, so a separate
  pipeline can use early stencil rejection — every fragment behind an
  already-drawn pixel is culled before the shader runs.
- **Stencil pre-pass.** Draw all polygons in a depth/stencil-only pass first (no
  fragment output, just claim fill-mask bits), then draw the textured pass with
  stencil EQUAL. The expensive shader only executes on visible pixels. This does
  not work for stipple-transparent polygons (which conditionally leave stencil
  unclaimed), so those still need the current path.
- **GPU tilemap rasterisation.** Replace the software tile renderer with a compute
  or render pass that decodes tiles directly from tile RAM and character RAM on
  the GPU. Eliminates 1.5 MB/frame of CPU→GPU upload and frees the host for
  geometry work.
- **Reduce per-frame allocations.** The vertex and parameter buffers are
  host-coherent (written by CPU, read by GPU); on unified-memory ARM SoCs this
  is already efficient, but ensuring no unnecessary flushes or barriers fire is
  worth profiling.

With the first two items alone, the fragment shader cost drops to roughly one
hardware-filtered texture sample plus three table lookups per visible pixel —
well within what VideoCore VII can sustain at 496×384 and 57.5 Hz.

### Internal resolution upscaling

Rendering above the hardware's native 496×384 is feasible for the 3D polygons:
the geometry engine outputs floating-point screen-space vertices, so they can be
scaled to any target resolution before rasterisation. What changes:

- **Polygons** scale cleanly. Vertex positions multiply by the scale factor, and
  texture coordinates are unaffected (they address texels, not pixels), so the
  filter quality improves at higher resolution without changing the colour chain.
- **Texture upscaling** is possible by bilinear or xBRZ/HQ4x filtering the 4-bit
  intensity before the tone curve, or by sampling at higher LOD than the hardware
  would. The limiting factor is the source data: textures are 4-bit at sizes from
  32×32 to 1024×1024, so the detail ceiling is low.
- **Tilemaps** can be upscaled by rendering the tile chip at the target resolution
  if it moves to the GPU, or by a post-process filter (Lanczos, FSR) on the
  composed native frame.
- **Stipple transparency is raster-locked.** The checkerboard pattern is defined
  in screen pixels (`(x ^ y) & 1`), so at higher internal resolution it would
  need to be evaluated at the upscaled pixel grid to stay correct — or replaced
  with real alpha blending, which changes the look but is an acceptable
  enhancement option.
- **The stencil fill-mask scales trivially** since it is just a different render
  target size.

A practical implementation would add an `--internal-resolution <n>` multiplier
(1×, 2×, 4×) that scales the offscreen polygon framebuffer and its stencil
attachment, adjusts the vertex shader's `invRaster` uniform, and leaves the
tilemap and present passes unchanged (they composite at the output resolution
anyway).

### Input and peripherals

- **Steering wheel support.** Map SDL wheel/pedal axes to the analog input
  channels the driving games expect (Daytona, Sega Rally, Manx TT, STCC).
- **Lightgun support.** Map mouse or USB lightgun coordinates to the analog
  channels that Virtua Cop 2, House of the Dead and Gunblade NY read.
- **Force feedback / rumble.** Model 2's drive board sends force-feedback
  commands to the cabinet's motor via a Z80 on a separate PCB. The data is
  already on the serial link; what is missing is translating those commands to
  SDL haptic events on gamepads that support rumble.

### GUI and usability

- **Expand the settings GUI.** Add tabs for input binding, per-game overrides,
  ROM path browser, and audio volume control.
- **Game launcher.** A ROM directory scanner that presents available games in the
  GUI without needing command-line arguments.
- **Save states.** Snapshot and restore the entire machine state (CPU registers,
  RAM, coprocessor, sound board) with a user-selectable save path and multiple
  slots. Essential for arcade games that have no native save mechanism.

### Additional features

- **Netplay.** Peer-to-peer or rollback-based network play for two-player games
  like Virtua Fighter 2 and Fighting Vipers. The fixed-rate frame clock and
  deterministic emulation make rollback feasible.
- **Shader post-processing.** User-loadable GLSL/SPIR-V shaders for CRT
  simulation, scanlines, and colour grading, applied after the native frame is
  composed but before presentation.
- **Run-ahead latency reduction.** Run the machine one or more frames ahead
  internally and discard all but the last, so input reaches the game with zero
  display latency at the cost of CPU time.
- **Rewind.** Ring buffer of recent states, letting the player scrub backwards
  through gameplay — a natural extension of save states.
- **Recording and playback.** Capture input streams for TAS-style replays and
  tool-assisted research into game mechanics.

## Licence and credits

BSD 3-Clause. See `LICENSE`.


I'm standing on the shoulders of giants and SM2-Emu exists because of the MAME project's reverse engineering of this hardware. The emulation is derived from MAME's Sega Model 2 driver and its device cores, which their authors released under the same licence. See `NOTICE` for per-component attribution.
