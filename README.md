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

## System requirements

The honest short version: **this is CPU-bound, on a single fast core.** The
emulated machine is an i960 main CPU plus a geometry coprocessor, and both run
as interpreters on one thread. What decides whether a game holds full speed is
almost entirely how fast *one* core can run that pair — not the GPU, not the
core count, and not the clock rate on its own. The heaviest load is the Model
2B (SHARC) and 2C (MB86235) sets; the original and 2A (MB86234 TGP) sets are
noticeably lighter.

### What the processor needs

Single-thread performance provides the best outcome. I tried multi-threading
the coprocessor work out, but that actually hindered performance.

Concretely, a core wants:

- **Out-of-order execution.** This matters more than raw clock. The
  interpreters are branchy, memory-dependent inner loops, and an out-of-order
  core hides that latency where an in-order one stalls on it. An in-order core
  (older ARM "little" cores such as Cortex-A53/A55, in-order Atom) is not
  enough even at a similar clock — the same work takes several times longer.
- **A high single-core clock and a modern architecture is better.** Roughly,
  sustained full speed on the heavy 2B/2C ROM sets wants the per-core
  throughput of a **Cortex-A76-class ARM core at ~2.4 GHz, or any post-2015
  x86-64 desktop/laptop core (Intel i5 / AMD Ryzen class), or Apple Silicon.**
- **64-bit.** aarch64 (ARMv8-A) or x86-64 with SSE2. There is currently no
  32-bit target.
- **NEON / SSE2** for the vectorised paths (present but not the main lever).

As per above, cores beyond the first buy little improvement: the emulation is
single-threaded by design, and while the software renderer splits its
rasteriser across a few cores, the frame's cost is dominated by the one core
running the two CPUs. A faster single core beats more cores every time here.

### Renderer / GPU

The hardware has no depth buffer — visibility is an order-based fill mask, so a
GPU has to shade every hidden pixel — which means the built-in **software
renderer is provided**, and it is the right default on machines without a
strong GPU. GPU backends (Vulkan / OpenGL) pull ahead only on desktop-class
GPUs. Dropping the window resolution does almost nothing, because the limit is
the CPU rather than fill rate. The software renderer needs no GPU at all; the
GPU backends need OpenGL 4.3 core (desktop), OpenGL ES 3.1 (ARM), or Vulkan 1.3
(opt-in), the floor set by the renderer's compute/SSBO passes.

### By platform

| Platform | Notes |
|----------|-------|
| **x86-64 (Linux)** | The primary target. Any modern desktop or laptop core clears every set with headroom on any backend. |
| **macOS** | Apple Silicon (recommended; runs everything far above full speed) or a 2015+ Intel Mac. The GPU path runs through MoltenVK (install the Vulkan SDK); the software path needs neither. |
| **ARM (aarch64 SBC / handheld)** | Needs an out-of-order core. A **Raspberry Pi 5 (Cortex-A76 @ 2.4 GHz)** is the realistic entry point: it holds full speed on the lighter sets but **only sits around — or just below — full speed on the heavier SHARC/MB86235 games**, so it is not comfortable across the whole library. Anything with weaker or in-order cores is below playable on the demanding sets. |

Measured on a Raspberry Pi 5 (Cortex-A76), software backend, in-game: the light
sets run comfortably above 57.5 Hz (Virtua Fighter 2, Daytona, Sega Rally, Manx
TT), while the heavy coprocessor sets land near or under it (House of the Dead,
Fighting Vipers, Dead or Alive, Last Bronx). So the Pi 5 is the sensible floor
for ARM, with the caveat that the most CPU-heavy titles do not yet hold a locked
57.5 Hz there.

**RAM** is undemanding: a loaded set is ~90–110 MiB of ROM regions plus working
buffers, so a couple of hundred MB of headroom is plenty.

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

See [BUILDING.md](BUILDING.md) for the full guide: dependencies, the CMake
options, the three graphics backends, per-distro package lists, the macOS
(MoltenVK) walkthrough and cross-compilation for ARM/embedded targets.

The short version, for a default Linux build (software + OpenGL, no Vulkan):

```sh
git clone --recurse-submodules https://github.com/dmanlfc/sm2-emu.git
cd sm2-emu
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Running

```sh
./build/bin/sm2-emu --list-games
./build/bin/sm2-emu --list-gpus
./build/bin/sm2-emu [--graphics-backend <software|vulkan|opengl>] \
                    [--render-scale <1-4>] \
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

Every game is playable with either a standard gamepad or keyboard.
Wheels, light guns and a mouse are supported as the preferred devices for the
driving and gun titles, but none is required. Gamepads are read through SDL's
gamepad layer, so anything with a mapping works without configuration. The
first pad to connect is player 1, pads can come and go while the game runs, and
`--list-gamepads` shows what was recognised. Face buttons are read by position
rather than by label.

| Gamepad | Function |
|---------|----------|
| D-pad or left stick | Stick, steering and other centred analog axes |
| A B X Y | Buttons 1 to 4 (also VR 1-4 on titles with view buttons) |
| Left / right shoulder | Buttons 3/4; also gear/shift down/up on racers |
| Left / right trigger | Brake / accelerate on driving titles |
| Right stick | Aim on gun titles (no mouse needed) |
| Start / Back | Start / insert a coin |
| Guide + Start / Back | Service / Test (operator menus) |

The keyboard is live at the same time, so a second player can join on it and
the operator controls stay reachable without a pad:

| Keys | Function |
|------|----------|
| `5` `6` | Coin 1, coin 2 |
| `1` `2` | Start 1, start 2 |
| `9` `0` | Service, test |
| Arrows, `Z` `X` `C` `V` | Player 1 stick and buttons |
| `W` `A` `S` `D`, `G` `H` `J` `K` | Player 2 stick and buttons |
| Arrows | Steering and other centred analog axes |
| `Left Ctrl` / `Left Alt` | Accelerate / brake (driving titles) |
| `F1` / `F2` | Shift up / down (Indy 500, Manx TT family) |
| `F1`–`F4`, `F5` | Gears 1 to 4, neutral (on gate-gearbox titles) |
| `B` `N` `M` `,` | VR / view buttons 1 to 4 (on titles that have them) |
| `Space` | Desert Tank forward/reverse shift |
| Arrows + `Left Ctrl` | Aim + fire on gun titles (no mouse needed) |
| `Escape` | Return to the game picker (quits if launched with no picker) |
| `P` | Pause |
| `F9` | Quit |
| `F10` | Toggle the settings menu |
| `F11` | Toggle fullscreen (`Cmd+F` on macOS, where the OS reserves F11) |
| `F12` | Save a screenshot |
| `Tab` (held) | Fast-forward |

**Virtual On** (a twin-stick cabinet) on the keyboard: `W`/`A`/`S`/`D` are the
left lever, the arrow keys the right lever, `Q`/`E` the left shot/dash and
`Right Shift`/`Right Ctrl` the right shot/dash. On a pad the two analog sticks
are the levers, the triggers the shots and the bumpers the dashes.

The renderer (GPU or software) is chosen at launch with `--graphics-backend`
and cannot be switched at runtime.

### Wheels and light guns

Beyond the pad and keyboard, dedicated peripherals are supported and configured
in the settings overlay (`F10`):

- **Wheels and pedals** with their own axis layout, calibrated in the **Wheel**
  tab (steering range, pedal axes, button mapping). Synthesised centring
  resistance and road/engine rumble are provided for wheels with a motor, since
  the drive board's real force is not replayed.
- **Light guns** over evdev on Linux (any `ID_INPUT_GUN` device),
  including on-screen recoil for guns with a motor and an optional Sinden border if necessary;
  the mouse remains the fallback aiming device on every platform.
- **Gamepad rumble** on the driving games, driven from the emulated drive board.

## Settings

Most settings live in the **settings overlay** (`F10`) and in `sm2-emu.ini`,
which is created automatically on first run with every setting at its default
and a comment explaining each. It is searched
for in the working directory first or otherwise in the platform's config
directory (`$XDG_CONFIG_HOME/sm2-emu` on Linux, `~/Library/Application
Support/sm2-emu` on macOS); `--config <dir>` overrides both, and whichever file
was used is named in the log. Changes made in the overlay are saved on exit. A
command-line flag always beats the file, and an unparseable line is reported and
skipped rather than refused.

### Video: scaling, aspect and CRT

The Video tab (and the config file) control how the finished frame is presented,
all applied live:

- **2D scaling** — how the frame is magnified to the window: nearest, bilinear,
  sharp-bilinear (the default; crisp 2D text without the shimmer nearest shows
  at non-integer window sizes), or integer (whole-multiple, pixel-perfect).
- **Aspect** — 4:3 (the arcade monitor), square pixels (the raw 496×384), or
  stretch (fill the window).
- **CRT filter** — an optional cosmetic arcade-monitor look (scanlines, shadow
  mask, glow, curvature), off by default.

### Enhancement (optional, GPU-gated)

Beyond the hardware-faithful default, the Video tab offers opt-in quality
enhancements for capable GPUs — off by default, so a fresh install looks exactly
like the arcade:

- **3D texture filter** — anisotropic filtering sharpens obliquely-viewed
  surfaces (road, track, walls) that the hardware's isotropic filter leaves
  blurry in the distance. The quality is clamped to what the GPU reports.
- **2D upscale** — xBR or ScaleFX edge-smooth the 2D layers (HUD, text, menus)
  so diagonals read as clean slopes rather than stairsteps.

## Networking (cabinet link)

Several Model 2 titles support linking cabinets so multiple machines play
together — Sega Rally, Daytona USA, Super GT 24h, Indy 500 and the other titles
that carry the communication board. sm2-emu can link two or more instances over
a LAN, so separate machines race against each other. Left off, a single
instance still boots its network check and settles as a standalone cabinet, so
nothing is required for solo play.

The comms board is wired as a **ring**: each cabinet listens on its own address
and sends to the *next* cabinet in the ring. For the common two-machine case,
you point each machine at the other. The master/slave role within the link is
chosen in the game's own test menu (via the Test and Service inputs), not in
these settings.

Configure it from the **Network** tab in the settings overlay (`F10`), or in
`sm2-emu.ini`. The keys are:

| Setting | Meaning |
|---------|---------|
| `link_enabled` | Turn cabinet linking on. Off keeps the standalone loopback. |
| `link_local_ip` | This machine's own address. Blank binds every interface. The GUI's **Detect** button fills it from the primary network adapter. |
| `link_subnet_mask` | Informational, shown in the GUI. |
| `link_port` | UDP port this cabinet listens on. Default `15112` (MAME's default). |
| `link_next_ip` | The next cabinet in the ring, i.e. where this machine sends. For two machines, the other machine's IP. Blank disables sending. |
| `link_next_port` | UDP port of the next cabinet. Default `15112`. |
| `link_cabinet_index` | This cabinet's 0-based position in the ring, for your own bookkeeping. |

A two-machine example, both running the same title, one at `192.168.1.10` and
the other at `192.168.1.11`:

```ini
# Machine A (192.168.1.10)
link_enabled  = true
link_local_ip = 192.168.1.10
link_next_ip  = 192.168.1.11

# Machine B (192.168.1.11)
link_enabled  = true
link_local_ip = 192.168.1.11
link_next_ip  = 192.168.1.10
```

The link uses UDP, so make sure the chosen port is open between the machines.
The transport is best-effort by design — the ring protocol re-sends its state
every frame and tolerates the odd lost datagram — and a machine whose socket
cannot bind simply stays standalone rather than failing to start. The Network
tab shows the live link state (waiting, establishing, or linked as cabinet *N*
of *M*), and enabling or changing the link takes effect on the next game launch.

Note that these are separate instances of the emulator with independent frame
clocks, not a rollback-netplay implementation; it links the emulated comms
board the way real cabinets were linked on a shared LAN.

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
run slow, which is what `--no-vsync` is for. Holding `Tab` runs as fast as the
machine manages.

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
| 10 | Release polish: settings GUI, game picker, present-stage scaling / aspect / CRT, opt-in 3D and 2D enhancement, wheels and light guns, controller config **(WIP)** |

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

- A handful of sets produce nothing here *and nothing in MAME*, because they
  are marked not-working upstream: Manx TT (both DX sets), Motor Raid DX,
  Virtual-On Relay, Royal Ascot II, and Sega Ski Super G (also unemulated
  protection). There is no reference to work against for these.

Separately, the Manx TT Deluxe cabinet carries a Model 1 audio board *on top
of* the 68000/SCSP board every Model 2A has. The audio board itself works, but
the machine has no slot for a second board yet, so those ROMs load unread.

### Internal resolution scaling

The 3D can be rendered above the native 496×384. `--render-scale <1-4>` (also a
setting, and in the Video tab of the overlay) rasterises the 3D pass at N times
native — 2× is 992×768, 4× is 1984×1536 — for crisper polygon edges and
textures. The 2D tilemap and HUD are fixed ROM bitmaps that cannot gain detail,
so they are upscaled nearest-neighbour and stay pixel-sharp; the whole frame is
composited at the scaled resolution and fitted to the window at the end. GPU
backends only (the software renderer stays native); 1× is the default and is
byte-identical to the pre-feature output. Stipple transparency stays locked to
the native grid, so translucent surfaces keep their hardware look at any scale.
The extra cost is GPU fill-rate only — the emulated machine runs identically at
every scale. This is separate from the present-stage 2D scaling and the optional
xBR/ScaleFX 2D upscaling described under **Settings** above.

### GUI and usability

- **Expand the settings GUI** with input binding, per-game overrides and volume
  control. (Wheel and gun buttons/axes already bind in the GUI.)
- **Save states**, with multiple slots. Arcade games have no native save.

## Licence and credits

BSD 3-Clause. See `LICENSE`.

I'm standing on the shoulders of giants, and SM2-Emu exists because of the MAME
project's reverse engineering of this hardware. The emulation is derived from
MAME's Sega Model 2 driver and its device cores, which their authors released
under the same licence. See `NOTICE` for per-component attribution.
