```
██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
█████╔╝ █████╗  ██║███████╗███████║██║   ██║
██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
           継 承  ( k e i s h ō )

       A SEGA MODEL 1, 2 & 3 EMULATOR
```

I started this project in February 2025 to improve Model 2 emulation on Linux.
There was no native emulator, and MAME had compatibility problems and was too
slow on small ARM boards. The plan was to learn from what MAME does well. My
earlier OpenGL ES and Vulkan work for Batocera showed that Model 2 could run
on a Raspberry Pi 5.

Keisho was formerly named sm2-emu.

In November 2026, I added Sega Model 1 support, and then Sega Model 3.

Linux is the main target, with Batocera first, and there is an AppImage for
other Linux distributions. macOS is supported because I partly develop on it,
and it runs Vulkan through MoltenVK. I don't care for Windows... there, I said
it, but there is a port for the unfortunate :-)

I used some GenAI along the way, but over 90% of the code is hand-written or
reused. Giving GenAI small, focused tasks sped up my part-time work from
November 2025 onwards, first with how the components fit together and later
with the graphical quirks. GenAI used: Google Gemini for the initial GUI
translations, and Claude for the Top Skater graphics problems.

**Status: playable.** All four Model 2 boards run (original, 2A, 2B and 2C), as
does Model 1: Virtua Racing, Virtua Formula, Virtua Fighter, Star Wars Arcade
and Wing War. NetMerc runs but is nearly silent, because one of its sample ROM
dumps is blank. The database has 163 sets (100 Model 1 and 2, 63 Model 3), and
most of them play with full 3D and sound on all three GPU renderers (Vulkan,
OpenGL and OpenGL ES) and on the software renderer.

**Model 3:** of the 54 sets tried so far, 52 reach gameplay. The two Sega Rally
2 prototypes can't run on any existing dump. The drive boards, the billboard,
the net board and the DSB1/DSB2 music boards all work.
The PowerPC runs through a JIT on arm64 and x86-64. None of the sets need
operator setup in the test menu. Star Wars Trilogy's flight-stick firmware
isn't dumped, so the stick is simulated for the handshake and has no force
feedback.

Emulated hardware:

- **CPUs:** the i960 (Model 2), V60 (Model 1) and PowerPC 603e/603ev (Model
  3) main CPUs.
- **Geometry coprocessors:** the MB86233/MB86234 TGP, the ADSP-21062 SHARC and
  the MB86235 TGPx4.
- **Video:** the System 24 tilemaps, and a textured renderer that applies the
  hardware's colour chain per texel; on Model 3, the Real3D Pro-1000 and the
  tile generator.
- **Sound:** the 68000/SCSP board (68000 + 2x SCSP on Model 3), the Model 1
  sound board (68000, YM3438 and two MultiPCMs), and the MPEG music boards
  (DSB1 and DSB2 on Model 3).
- **Other boards:** drive boards, light guns, the link board and the protection
  chips (315-5881); on Model 3, the 53C810 SCSI DMA and the MPC10x bridge.

Input comes from SDL gamepads, with the keyboard always active alongside them.
The machine runs at its own 57.5245 Hz rather than the display's rate. Each
frame is built at the hardware's 496x384 and scaled once at the end.

## System requirements

**Speed depends on one fast CPU core.** On Model 1 and 2 the main CPU and the
geometry coprocessor both run as interpreters on a single thread, so how fast
one core runs decides whether a game holds full speed; Model 3's PowerPC runs
through a JIT. The GPU, the number of cores and
the clock speed on its own matter much less. The Model 2B (SHARC) and 2C
(MB86235) sets are the heaviest; the original Model 2, 2A and Model 1 sets are
noticeably lighter.

### Processor

I tried moving the coprocessor onto its own thread, but it made things slower.
What matters is a fast single core with:

- **Out-of-order execution.** This matters more than clock speed. In-order cores
  (ARM "little" cores such as the Cortex-A53/A55, older Atoms) take several
  times longer for the same work.
- **A modern design.** For full speed on the heavy 2B/2C sets, think a
  Cortex-A76 at about 2.4 GHz, any x86-64 desktop or laptop core from 2015 on
  (Intel i5 / AMD Ryzen class), or Apple Silicon.
- **64-bit:** aarch64 or x86-64. There is no 32-bit build.

Extra cores help little, except with the software renderer. It splits its
drawing into bands, and by default (`software_async = true`) draws each frame
on its own thread, one frame behind the emulation, at the cost of a frame of
latency. On big.LITTLE chips (`software_slow_cores = true`) the drawing runs on
the little cores and leaves the big cores to the emulation. Model 3 likewise
builds each frame (2D layers, scene walk) on a second thread while the next
frame is emulated (`model3_async = true`), also a frame behind.

### GPU

The hardware has no depth buffer, so a GPU has to shade every hidden pixel.
That makes the **software renderer** the right choice on machines without a
strong GPU; the GPU renderers only pull ahead on desktop-class graphics. A
smaller window barely helps, because the CPU is the limit. The GPU renderers
need OpenGL 4.3, OpenGL ES 3.1 or Vulkan. On macOS, which stops at OpenGL 4.1,
the OpenGL renderer falls back to a 4.1 path; Vulkan (MoltenVK) is the better
choice there, and the only one that widens 2D backgrounds in 16:9.

### By platform

| Platform | Notes |
|----------|-------|
| **x86-64 Linux** | The main target. Any modern desktop or laptop runs every set at full speed on any renderer. |
| **x86-64 Windows** | Built with MSVC, with the software, OpenGL and Vulkan renderers. Less tested than Linux. |
| **macOS** | Apple Silicon (recommended) or a 2015+ Intel Mac. The release dmg includes MoltenVK and needs nothing else installed. |
| **ARM (SBCs, handhelds)** | A **Raspberry Pi 5** or better. The Pi 5 runs most Model 1 and 2 games at full speed; the heaviest Model 2B/2C games run just below it. Model 3 games run below full speed on a Pi 5, mostly at 42-56 fps at 1080p. |

On a Raspberry Pi 5 with the software renderer, Virtua Fighter 2, Daytona USA,
Sega Rally and Manx TT run comfortably above 57.5 Hz. House of the Dead,
Fighting Vipers, Dead or Alive and Last Bronx land at or below it.

Handhelds with big.LITTLE chips do better than their big cores suggest, because
the little cores take the drawing. On an Anbernic RG 55G1 (2x Cortex-A78 + 6x
Cortex-A55), pinned to the A78s, every set holds full speed: Daytona 106 fps,
Virtua Fighter 2 109, Dead or Alive 84, House of the Dead 83, Fighting Vipers
74 and Last Bronx 68 (software renderer, unthrottled). Left to the scheduler,
it lands on the A55s and drops to 40-60 fps, so pin it to the big cores
(Batocera's CPU cores setting does this).

**RAM:** a loaded set takes about 90-110 MB, so a couple of hundred MB free is
plenty.

## Why the renderer is unusual

Model 2 pairs an i960KB with a geometry coprocessor (an MB86234 TGP on Model 2
and 2A, a SHARC on 2B, an MB86235 on 2C), a custom Sega/Lockheed Martin
rasteriser and System 24 tilemaps for the 2D layers. Output is 496x384 at about
57.5 Hz. The rasteriser has no modern equivalent in four ways:

- **No depth buffer.** Polygons are sorted by depth and drawn front to back
  against a one-bit mask: the first write wins. Keisho uses a stencil buffer
  for this.
- **No RGB textures.** A texel is a 4-bit intensity, coloured through a tone
  curve, a base colour, a lookup table and a gamma ramp. The curve applies after
  filtering, so the whole chain runs per texel in the fragment shader.
- **No alpha blending.** See-through surfaces use an alpha test or a fixed
  checkerboard stipple. Both drop pixels and leave the mask open, so whatever is
  behind still draws.
- **No per-vertex shading.** Each polygon has one 8-bit brightness value.

So the geometry runs on the CPU, as on the hardware, and the GPU only receives
finished screen-space triangles.

## Building

See [BUILDING.md](BUILDING.md) for dependencies, CMake options, the three
graphics backends, per-distro packages, macOS and cross-compiling for ARM.

A default Linux build (software + OpenGL):

```sh
git clone --recurse-submodules https://github.com/Keisho-Emu/Keisho.git
cd Keisho
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Running

```sh
./build/bin/keisho --list-games
./build/bin/keisho --list-gpus
./build/bin/keisho [--graphics-backend <software|vulkan|opengl>] \
                    [--render-scale <1-8>] \
                    [--fullscreen] [--no-vsync] [--game <set>] vf2.zip
```

No ROMs are included. Games are identified by the CRC32 of their contents, not
by file name, so one merged archive can hold several revisions and `--game`
picks one. For example, the four Virtua Fighter 2 revisions share one file:

```sh
./build/bin/keisho --game vf2o vf2.zip
```

A clone lists only the chips it changes and takes the rest from its parent,
even from the parent's archive. Each system has its own folder on the Paths
tab (`--model1-rom-dir`, `--model2-rom-dir`, `--model3-rom-dir`); Model 1 and
Model 3 fall back to the Model 2 folder when theirs is empty.

Dropping a `.zip` or `.7z` on the window starts that game. If a set can't
start, a dialog says why and lists any missing chips.

ROM layouts live in `data/games.xml`, so a game that isn't included can be
added by editing it. The format is described at the top of that file.

## Controls

Every game can be played with a gamepad or the keyboard. Wheels, light guns and
a mouse work for the driving and gun games, but aren't required. Any gamepad
SDL knows works without setup. The first pad connected is player 1, pads can be
plugged in or out while playing, and `--list-gamepads` shows what was found.
Face buttons go by position, not label. The **Gamepad** tab sets each game's
button layout.

| Gamepad | Function |
|---------|----------|
| D-pad or left stick | Stick, steering and other centred controls |
| A B X Y | Buttons 1 to 4 (also VR view buttons 1-4) |
| Left / right shoulder | Buttons 3/4; gear down/up on racers |
| Left / right trigger | Brake / accelerate on driving games |
| Either stick | Aim on gun games; right trigger fires, left trigger reloads |
| Start / Back | Start / insert coin |
| Guide + L3 / R3 | Service / Test (operator menus) |
| Guide + A | Settings menu |
| Guide + X / Y | Quick save / quick load |
| Guide + d-pad left / right (held) | Rewind (when on) / fast-forward |
| Guide + LB | Screenshot |
| Guide + Start | Back to the game picker, or quit |

The Guide shortcuts follow Batocera's hotkey layout. Front ends with their own
hotkeys can turn them off with `pad_shortcuts = false`, as Batocera does.
Service and Test stay on Guide + L3 / R3 either way.

The keyboard works at the same time, so a second player can use it:

| Keys | Function |
|------|----------|
| `5` `6` | Coin 1, coin 2 |
| `1` `2` | Start 1, start 2 |
| `9` `0` | Service, test |
| Arrows, `Z` `X` `C` `V` | Player 1 stick and buttons |
| `W` `A` `S` `D`, `G` `H` `J` `K` | Player 2 stick and buttons |
| `Left Ctrl` / `Left Alt` | Accelerate / brake |
| `F1` / `F2` | Shift up / down (Indy 500, Manx TT) |
| `F1`-`F4`, `F5` | Gears 1 to 4, neutral (H-gate games) |
| `B` `N` `M` `,` | VR view buttons 1 to 4 |
| `Space` | Desert Tank forward/reverse |
| Arrows + `Left Ctrl` | Aim and fire on gun games |
| `Escape` | Back to the game picker (quits if there is none) |
| `P` | Pause |
| `F6` / `F7` | Quick save / quick load |
| `F8` | FPS counter |
| `F9` | Quit |
| `F10` | Settings menu |
| `F11` | Fullscreen (`Cmd+F` on macOS) |
| `F12` | Screenshot |
| `Tab` (held) | Fast-forward |
| `Backspace` (held) | Rewind, when on |

**Virtual On** and **Virtual On 2** on the keyboard: `W` `A` `S` `D` is the
left lever, the arrows the right lever, `Q` / `E` the left shot / dash and
`Right Shift` / `Right Ctrl` the right shot / dash. On one pad, the sticks (or
the d-pad and Y/A/X/B) are the levers, the triggers the shots and the bumpers
the dashes.

**Ski Champ:** the bumpers are the left and right poles (`Q` / `E`), and X / Y /
B the three Select buttons (`Z` `X` `C`).

**Star Wars Trilogy:** the stick flies, the triggers fire, and A / B are the
event buttons. **Sega Bass Fishing:** the left stick is the rod, the right stick
the cabinet's joystick, the right trigger the reel, the left trigger the line
tension (Get Bass), and A casts.

**Flying games:** Star Wars Arcade and Wing War have a throttle lever on the
right stick: push to accelerate, pull back to slow down, and Wing War hovers
with it fully back. Wing War's triggers do the same; on the keyboard it is
`Left Ctrl` / `Left Alt`.

The renderer is chosen at launch with `--graphics-backend` and can't be
switched while running.

### Wheels and light guns

Both are set up in the settings menu (`F10`):

- **Wheels and pedals** are calibrated in the **Wheel** tab. Wheels with force
  feedback get the game's own drive board forces; others get rumble.
- **Light guns:** the first gun found is player 1, the second player 2.
  - **Linux:** any device tagged `ID_INPUT_GUN` works, and guns with a motor
    get recoil.
  - **Aim:** a gun calibrated to the whole screen is mapped onto the picture.
    Shooting a side bar or past the edge counts as off screen, so it can
    reload. A Sinden border is available.
  - **Without a gun**, player 1 aims with the mouse (left button fires, right
    reloads) and player 2 with a gamepad. A gun game shows the crosshair and
    hides the mouse pointer by itself.
  - **Calibration:** Batocera's gun calibration tools draw their targets in
    game.
  - **Crosshairs:** custom PNGs go in the crosshairs folder (Paths tab), named
    `hotd-p1.png` / `hotd-p2.png`, or `hotd.png` for both, with the parent set
    or `default.png` as fallback.
- **Gamepad rumble** follows the drive board in driving games.

#### Light guns on Windows

- **Detection:** a mouse whose name contains "gun" or "aimtrak" counts as a gun
  (GUN4IR, Sinden, AimTrak).
- **Setup:** guns must be in absolute mouse mode and plugged in before Keisho
  starts. Aiming works windowed too.
- **Buttons:** only the five mouse buttons are read.
- **Recoil:** Keisho doesn't drive the motor on Windows. Turn on Windows or
  Network outputs in the System tab and let MAMEHooker or Hook Of The Reaper
  fire it.

### Save states

`F6` quick-saves and `F7` quick-loads, using one slot per game. The **States**
tab has four more slots per game, each with Save, Load and Delete. States are
stored as `<game>.<slot>.kstate` in the `states` folder inside the saves
folder, so they move with `--nvram` / `nvram_dir`. States saved by v0.9.45
or later still load, and sm2-emu's `.sm2state` files are converted to
`.kstate` the first time Keisho finds them; states from this build won't load
in older ones.

### Rewind

With **Rewind** on (System tab, or `rewind = true`), holding `Backspace` steps
back through recent play. History is kept in memory, up to `rewind_buffer_mb`
(128 MB by default, about half a minute). It costs a little speed, so it's off
by default.

## Settings

Most settings are in the settings menu (`F10`) and in `keisho.ini`, which is
created on first run. The file is looked for in the working directory, then in
the config folder (`$XDG_CONFIG_HOME/keisho` on Linux,
`~/Library/Application Support/Keisho` on macOS); `--config <dir>` overrides
both. Menu changes are saved on exit, and command-line options always win.

### Language

The menu follows the system language, or the one set under **Video → Language**
or `language` in the ini (`en`, `de`, `ja`, `pt_BR`, ...). It switches live and
uses the system's fonts for scripts the built-in font doesn't cover, and the
Noto fonts in `data/lang/fonts/` for Thai, Hindi, Bengali, Tamil and Urdu
where the system has none (as on Batocera).

Translations live in `data/lang/` as gettext `.po` files. They started out
machine-translated, so fixes from native speakers are very welcome: edit the
`.po` file (Poedit works) and open a pull request. `data/lang/update-po.sh`
updates every catalogue after the source strings change.

### Video

Set in the **Video** tab and applied live:

- **Scaling:** nearest (default), bilinear, sharp bilinear or integer.
- **Aspect:** 4:3 (the arcade monitor), square pixels, 16:9 or stretch. 16:9
  draws the 3D wider and keeps the 2D in the middle (experimental).
- **Render scale:** draws the 3D at up to 8x native (GPU renderers only). The
  2D stays pixel-sharp.
- **Brightness:** where the Model 2 picture's black sits, and the overall
  brightness on Model 3; 50% is the default.
- **Extended draw distance:** Daytona USA draws scenery to the edge of the
  course instead of the arcade's short range.
- **Fullscreen resolution:** `fullscreen_resolution = 1280x720` in the ini (or
  `--fullscreen-resolution`) runs fullscreen at a lower mode the compositor
  scales up. On a slow GPU (Raspberry Pi 5 with Vulkan) this is much faster, at
  the cost of a slightly softer picture.
- **CRT filter:** optional scanlines, mask, glow and curvature.
- **Bezels:** artwork around the 4:3 picture, fullscreen only. Put PNGs in the
  bezels folder (Paths tab), named after the set (`daytona.png`), its parent,
  or `default.png`. The image needs a transparent window in the middle; a
  1440x1080 window in a 1920x1080 image keeps the game where it is.

### Enhancements

Off by default, so a fresh install looks like the arcade. GPU renderers only:

- **3D texture filter:** anisotropic filtering sharpens roads and walls seen at
  an angle.
- **2D upscale:** xBR or ScaleFX smooth the 2D layers (HUD, text, menus).
- **Translucency:** the hardware draws see-through surfaces as a checkerboard.
  Blended draws them as the 50% see-through look it's aiming for.

Extended draw distance and translucency are Model 1 and 2 settings; the menu
greys them out while a Model 3 game runs. On Model 3, the 2D upscale smooths
the 2D layers without using xBR or ScaleFX.

### Audio

Volume, per-game volume, and **Music volume** for games with a separate music
board (Star Wars Arcade, Sega Touring Car Championship and Top Skater), for
example to mute copyrighted music in recordings.

**Speakers** picks Stereo or Surround for the four-speaker Model 3 cabinets
(Scud Race, Daytona USA 2, Sega Rally 2, The Lost World). Surround (`surround =
true`) opens the output with four channels and plays the rear pair on rear
speakers; Stereo folds them into the front pair. Other games are stereo
either way, and a two-channel output device folds the rear pair itself.

### Custom textures

The 3D textures can be dumped and replaced with sharper ones (GPU renderers):

1. Turn on **Dump textures** in the Video tab and play. On exit, every texture
   the game drew is saved to `<saves>/textures/<game>/dump/`, with an
   `index.html` to browse them.
2. Save your improved version under the **same file name** in
   `<saves>/textures/<game>/load/`, at any whole multiple of the original size
   and in the same orientation.
3. With **Custom textures** on (the default), they load the next time the game
   starts, or straight away with **Reload**.

Raise the render scale to see the extra detail.

On Model 3 the textures are already in colour, so a dump is the texture as
stored (`tex_<hash>.png`) and a replacement is drawn as is, keeping its alpha
for cut-outs and see-through parts. A replacement only applies while the game
holds that exact texture; mip levels are made from it.

## Cabinet link

Games with a link board (Daytona USA, Sega Rally, Indy 500, Super GT 24h,
Virtua Racing and others) can race across a LAN, one Keisho per machine. With
the link off, a game passes its network check and plays as a single cabinet.

Cabinets form a **ring**: each one listens on its own address and sends to the
next. With two machines, point each at the other. Master/slave is chosen in the
game's own test menu.

Set it up in the **Network** tab or in `keisho.ini`:

| Setting | Meaning |
|---------|---------|
| `link_enabled` | Turn the link on. |
| `link_local_ip` | This machine's address. Blank listens on every interface. **Detect** in the GUI fills it in. |
| `link_subnet_mask` | Shown in the GUI only. |
| `link_port` | UDP port to listen on. Default `15112`, as in MAME. |
| `link_next_ip` | The next cabinet's address; with two machines, the other one. |
| `link_next_port` | The next cabinet's UDP port. Default `15112`. |
| `link_cabinet_index` | This cabinet's position in the ring, for your reference. |
| `link_delay` | Model 3 only: frames by which the other cabinets are read late (default 6). Raise it if a race over Wi-Fi stutters; 2 or 3 suits a wired LAN. |

Two machines at `192.168.1.10` and `192.168.1.11`:

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

Open the UDP port between the machines. Lost packets are fine, as the games
resend their state every frame. A machine that can't open its port plays
standalone. The Network tab shows the link state, and changes apply at the next
game launch. This links the emulated boards the way real cabinets were linked;
it isn't rollback netplay.

Model 3 link games (Daytona USA 2, Scud Race, Sega Rally 2, Le Mans 24,
Spikeout, Harley-Davidson, Ski Champ, Virtual On 2, Dirt Devils) use the same
settings; in each game's test menu, set one cabinet as master and the others
as slaves.
Cabinets can start in any order. Every frame, each one waits if it must for
the others' last few frames, so a linked race runs at the slowest machine's
speed. A lost packet is asked for again, so Wi-Fi works. All nine have been
played linked between two machines, and Daytona USA 2, Scud Race and Le Mans
24 four ways over Wi-Fi between a Mac and three handhelds running Batocera
(Daytona USA 2 with one of them as the LIVE monitor).

## Frame pacing

The machine runs at 57.5245 Hz, which no display matches. Showing one emulated
frame per refresh would run 4% fast on a 60 Hz screen, so Keisho paces to
real time and shows every frame exactly once. On a 60 Hz display a frame is
sometimes held for two refreshes; that can't be avoided without making up
frames. On a display slower than 57.5 Hz, vsync would slow the game down, so
use `--no-vsync`. Hold `Tab` to run as fast as possible.

## Known gaps

- **The geometry engine is a high-level model.** Its microcode has never been
  dumped, so it follows MAME in reproducing what the microcode does. Results
  should match, but its timing is tuned separately to match the arcade
  hardware.
- **The tilemap sky repeats visibly** on some sets. The tile data really does
  repeat, so this may be correct. Unresolved.
- **Sound isn't sample-exact.** Both sound boards have been compared against
  arcade recordings for timing and level, not waveform by waveform.
- **Manx TT Deluxe** carries a Model 1 sound board on top of its usual one.
  That board works, but isn't fitted to the 2A machine yet, so its ROMs go
  unused.
- **Royal Ascot II** is marked as not working.
- **Power Sled** may not run properly, as it's a complicated setup that needs
  several linked machines.

## Roadmap

Ideas being explored for the future:

- A libretro core for RetroArch
- Mobile phones

## Licence and credits

Keisho is licensed under the GNU General Public License v3.0 or later, with a
few additional terms. See `LICENSE` and `NOTICE` for details.

Keisho exists because of the MAME project's reverse engineering of this
hardware. Much of the emulation is derived from MAME's Sega Model 1 and Model 2
drivers and their device cores, released by their authors under the BSD
3-Clause licence, which that code keeps (see `LICENSES/BSD-3-Clause.txt`).

See `NOTICE` for per-component credits and the list of ported files.
