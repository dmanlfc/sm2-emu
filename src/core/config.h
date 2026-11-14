// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#pragma once

#include "core/log.h"
#include "core/types.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace sm2 {

/// How the finished native frame is magnified to the window at the present
/// stage. Present-stage state only (sampler/shader), so it switches live.
///
///   Nearest       -- point sampling; sharp but shimmers at non-integer scale.
///   Bilinear      -- smooth; soft on 2D text.
///   SharpBilinear -- nearest to the integer multiple, bilinear for the
///                    fractional remainder; sharp text without the shimmer.
///   Integer       -- snap to a whole native multiple (see AspectMode), nearest.
enum class ScalingMethod : u32 { Nearest, Bilinear, SharpBilinear, Integer };

/// Which rectangle the frame is fit into at the present stage.
///
///   FourThree   -- the arcade monitor's 4:3 (the raster was stretched to it).
///   SquarePixel -- the raw 496x384 (1.29:1); square pixels, slightly narrow.
///   Widescreen  -- 16:9: the 3D drawn wider, the 2D kept at 4:3 in the middle.
///   Stretch     -- fill the whole window, ignore aspect (no bars).
enum class AspectMode : u32 { FourThree, SquarePixel, Widescreen, Stretch };

/// 3D texture filtering. Faithful is the hardware-accurate in-shader filtering;
/// Anisotropic sharpens obliquely-viewed surfaces. GPU-gated; falls back to
/// Faithful where unsupported.
enum class TextureFilter : u32 { Faithful, Anisotropic };

/// 2D layer upscaling. Faithful is the crisp nearest enlargement; Xbr/ScaleFx
/// are edge-directed pixel-art filters over the 2D tilemap layers. GPU-gated;
/// falls back to Faithful where unsupported.
enum class Upscale2D : u32 { Faithful, Xbr, ScaleFx };

/// 3D translucency. Stipple is the hardware's screen-locked checkerboard;
/// Blended draws those polygons as the 50% blend the stipple is trying to achieve.
/// GPU-gated; falls back to Stipple where unsupported.
enum class Translucency : u32 { Stipple, Blended };

/// Settings worth keeping between runs. Per-run options such as capture length
/// stay on the command line. A missing or empty file gives these defaults.
struct Config {
    // -- presentation ------------------------------------------------------

    /// Wait for vertical blank before presenting. Decides tearing, not pacing.
    /// Off by default: the software pacer sets the rate, and a blocking FIFO
    /// present quantises frame times to the refresh.
    bool vsync = false;

    /// Hold the machine to its own 57.5245 Hz; off runs as fast as the host can.
    bool throttle = true;

    bool fullscreen = false;
    /// "WxH": fullscreen at a lower mode the compositor scales up to the
    /// display (cheaper presents on slow GPUs); empty uses the display's own.
    std::string fullscreen_resolution;

    /// Show the FPS counter overlay in the top-right corner.
    bool show_fps = false;

    /// Software renderer: draw on a second thread, one frame behind. Forced
    /// off for captures.
    bool software_async = true;

    /// With software_async on a big.LITTLE host, draw on the slow cores.
    bool software_slow_cores = true;

    /// Model 3: build each frame on a second thread while the next one is
    /// emulated; shown one frame later.
    bool model3_async = true;

    /// Show brief on-screen notifications such as "State saved".
    bool show_notifications = true;

    /// Keep recent states so Backspace can step back through play.
    bool rewind = false;

    /// Memory the rewind history may use, in MiB.
    u32 rewind_buffer_mb = 128;

    /// Overlay language: a catalog name from lang/ ("de", "pt_BR"), "en", or
    /// "auto" to follow the system's preferred languages.
    std::string language = "auto";

    /// Gun mode: the aiming crosshair, and the OS cursor hidden. Auto turns it
    /// on in a gun game, hiding the cursor while a player aims with the mouse
    /// or a light gun; On also hides the cursor in every game; Off never.
    enum class LightgunMode : u32 { Auto, On, Off };
    LightgunMode lightgun = LightgunMode::Auto;

    /// Draw Keisho's own aiming crosshair in light-gun mode. Independent of the
    /// mode itself, which still hides the cursor and enables the gun paths.
    bool lightgun_crosshair = true;

    /// Hide the white flash the gun games show on each shot.
    bool lightgun_hide_flash = false;

    /// A gamepad's stick points at the screen, centred at rest, instead of
    /// moving the crosshair like a mouse.
    bool lightgun_pad_centred = false;

    /// Fire the gun's recoil motor on each shot, for guns exposed as a
    /// force-feedback device. Strength is 0..100 percent.
    bool lightgun_recoil          = true;
    u32  lightgun_recoil_strength = 60;

    /// Sinden border: a solid frame the gun's camera tracks, drawn only in
    /// light-gun mode. Colour is 0xRRGGBB; thickness is in game-image pixels.
    bool sinden_border           = false;
    u32  sinden_border_colour    = 0xffffff;
    u32  sinden_border_thickness = 12;

    u32  window_width  = 992;
    u32  window_height = 768;

    /// Internal 3D render scale, 1..render::kMaxRenderScale (1 = native). GPU
    /// backends only; takes effect on the next launch.
    u32 render_scale = 1;

    /// Present-stage magnification and shape; both take effect live.
    ScalingMethod scaling_method = ScalingMethod::Nearest;
    AspectMode    aspect_mode    = AspectMode::FourThree;

    /// CRT filter over the finished frame. Strengths are 0..100 percent;
    /// curvature 0 is flat. Live-switchable.
    bool crt_enabled          = false;
    u32  crt_scanline_strength = 40;
    u32  crt_mask_strength     = 30;
    u32  crt_glow_strength     = 20;
    u32  crt_curvature         = 0;

    /// Picture brightness, 0..100: 50 is the arcade-fitted default, lower
    /// darker. Model 1/2 move their colour ramp; Model 3 scales the output.
    /// Live-switchable.
    u32 brightness = 50;

    /// Bezel artwork in fullscreen.
    bool bezel = false;

    /// Enhancements beyond the original hardware, GPU-gated. anisotropy is the
    /// 2..16 tap ceiling, clamped to the GPU maximum.
    TextureFilter texture_filter = TextureFilter::Faithful;
    u32           anisotropy     = 4;
    Upscale2D     upscale_2d     = Upscale2D::Faithful;
    Translucency  translucency   = Translucency::Stipple;

    /// Draw the scenery to the edge of the course instead of the arcade's
    /// short range, in the games that support it (Daytona USA). Live.
    bool extended_draw_distance = false;

    /// Replacement textures from <saves>/textures/<game>/load, and dumping
    /// every texture the 3D draws to <saves>/textures/<game>/dump.
    bool custom_textures = true;
    bool dump_textures   = false;

    /// Exact device name to prefer, as `--list-gpus` prints it. Empty picks the
    /// best-scoring device.
    std::string gpu;

    /// Preferred renderer: "software", "vulkan" or "opengl" (empty = build
    /// default). --graphics-backend overrides it; takes effect next launch.
    std::string graphics_backend;

    // -- steering wheel ----------------------------------------------------

    /// Read wheels and gamepads while the window is not in front.
    bool input_background = false;

    /// Force feedback on a wheel that supports it. Off, the wheel still steers.
    bool wheel_ffb = true;

    /// The gain on the drive board's forces, 0..200 percent: at 100 the board's
    /// full power (63) gives the wheel's maximum torque, every force in
    /// proportion; 200 doubles them, the strongest saturating.
    u32 wheel_ffb_strength = 30;

    /// The Sega Rally / Daytona panel spring, 0..100 percent of Sega Rally's
    /// strongest torque (32) at the gain. Very light on a cabinet; the other
    /// panels have none.
    u32 wheel_panel_spring = 20;

    /// EPR-18261 boards: the motor power (0..63) below which there is no
    /// torque, and the power taken as the board's full one. Linear in between.
    u32 wheel_indy_deadband   = 0;
    u32 wheel_indy_full_power = 63;

    /// The drive board's strength DIP switches 1-3, 0..7 in binary; 0 (all off)
    /// is the factory setting. Sega Rally adds 0..7 to every torque, Daytona 4
    /// times that, the Indy 500 family 0..7 to every strength (Le Mans 24 ships
    /// at 3).
    u32 wheel_drive_dip = 0;

    /// Reverses the wheel's force, for a driver that pushes right on the
    /// positive force with which DirectInput, SDL and Linux push left.
    bool wheel_ffb_invert = false;

    /// A synthesised road/engine rumble: a vibration that rises with the
    /// throttle and with hard steering. Used on games without force feedback;
    /// a wheel delivering force feedback does not also rumble.
    bool wheel_rumble = true;

    /// Rumble strength, 0..100 percent.
    u32 wheel_rumble_strength = 40;

    /// An added vibration that grows with the throttle, not from the game.
    /// Off leaves only the game's impacts.
    bool wheel_rumble_engine = true;


    /// The wheel's own physical rotation range (a G-series PC wheel is ~900).
    u32 wheel_steer_degrees = 270;

    /// Total physical rotation that reaches full lock, half each side of centre.
    /// Lower is more sensitive. Clamped to 180..270.
    u32 wheel_lock_degrees = 240;

    /// Wheel travel about centre that reads as centre, in percent of its
    /// range. Most wheels want none.
    u32 wheel_steer_deadzone = 0;

    /// Cabinet controls a wheel button can be bound to. Buttons 1..4 include a
    /// driving cabinet's VR view buttons; Menu is the emulator overlay; Gear1..4
    /// and GearNeutral are H-shifter positions. Keep kCount last.
    enum class WheelRole : u32 {
        Start, Coin, Button1, Button2, Button3, Button4, GearUp, GearDown,
        Test, Service, Menu, Gear1, Gear2, Gear3, Gear4, GearNeutral, kCount
    };
    static constexpr u32 kWheelRoleCount = static_cast<u32>(WheelRole::kCount);

    /// Wheel button index per role, or -1 for unbound. The defaults suit a
    /// Logitech G-series.
    std::array<s32, kWheelRoleCount> wheel_buttons = {
        6,   // Start
        7,   // Coin
        0,   // Button1
        1,   // Button2
        2,   // Button3
        3,   // Button4
        4,   // GearUp   (right paddle)
        5,   // GearDown (left paddle)
        -1,  // Test
        -1,  // Service
        -1,  // Menu
        -1,  // Gear1
        -1,  // Gear2
        -1,  // Gear3
        -1,  // Gear4
        -1,  // GearNeutral
    };

    /// With an H shifter, holding no gear means neutral. When off, the last
    /// gear stays engaged.
    bool wheel_shifter_neutral = true;

    /// A paddle or RB/LB shift between two gears passes through neutral for
    /// this many milliseconds (0..500). 0 shifts straight through.
    u32 wheel_shift_neutral_ms = 0;

    /// Which wheel axis drives each analogue control, or -1 to auto-detect
    /// (steering is axis 0; pedals are found by which axes rest at an extreme).
    /// The GUI calibration sets these when a wheel's layout differs.
    s32 wheel_steer_axis = -1;
    s32 wheel_accel_axis = -1;
    s32 wheel_brake_axis = -1;

    /// A pedal whose axis reads high when released and low when pressed. Captured
    /// during calibration; only meaningful when the matching axis is set.
    bool wheel_accel_invert = false;
    bool wheel_brake_invert = false;

    /// A pedal that uses half of its axis: released at the centre and pressed
    /// towards the high end (the low end when inverted). Two pedals can then
    /// share one axis.
    bool wheel_accel_half = false;
    bool wheel_brake_half = false;

    // -- gamepad -----------------------------------------------------------

    /// Gamepad rumble: the game's jolts plus a buzz rising with steering. Driving games only.
    bool pad_rumble = true;

    /// Rumble strength, 0..100 percent of the pad's motor range.
    u32 pad_rumble_strength = 60;

    /// An added vibration that grows with steering angle, not from the game.
    /// Off leaves only the game's impacts.
    bool pad_rumble_cornering = true;

    /// Response of a stick's travel past its deadzone, in percent. Full
    /// deflection is always full lock; under 100 the middle of the travel
    /// does less, over 100 it does more.
    u32 pad_stick_sensitivity = 100;

    /// Stick travel about centre that reads as centre, in percent of full
    /// deflection. Raise it for a stick that no longer settles at rest.
    u32 pad_stick_deadzone = 5;

    /// The Guide button shortcuts (menu, save, load, rewind...). Off for a host
    /// that maps the hotkeys itself, such as Batocera.
    bool pad_shortcuts = true;

    /// The keyboard shortcuts. Keep kCount last.
    enum class Hotkey : u32 {
        Exit, Quit, Overlay, Fullscreen, Screenshot, Pause, QuickSave, QuickLoad,
        Fps, FastForward, Rewind, kCount
    };
    static constexpr u32 kHotkeyCount = static_cast<u32>(Hotkey::kCount);

    /// The default key for each shortcut, by SDL key name.
    static constexpr std::array<const char*, kHotkeyCount> kDefaultHotkeys = {
        "Escape", "F9", "F10", "F11", "F12", "P", "F6", "F7", "F8", "Tab", "Backspace",
    };

    /// The key bound to each shortcut, by SDL key name, or empty for none.
    std::array<std::string, kHotkeyCount> hotkeys = [] {
        std::array<std::string, kHotkeyCount> keys;
        for (u32 i = 0; i < kHotkeyCount; ++i) {
            keys[i] = kDefaultHotkeys[i];
        }
        return keys;
    }();

    [[nodiscard]] const std::string& hotkey(Hotkey h) const
    {
        return hotkeys[static_cast<usize>(h)];
    }

    /// The device path of the pad each player wants, as SDL reports it
    /// (/dev/input/eventN on Linux). Empty leaves the slot to whichever pad
    /// comes first. A front end with its own player order sets these.
    std::array<std::string, 2> pad_device;

    /// The cabinet controls a gamepad button can be bound to. Test and Service
    /// stay on the Guide chord, so they are not here. Keep kCount last.
    enum class PadRole : u32 {
        Button1, Button2, Button3, Button4, Up, Down, Left, Right,
        Start, Coin, GearUp, GearDown, kCount
    };
    static constexpr u32 kPadRoleCount = static_cast<u32>(PadRole::kCount);

    /// A binding of kPadAxisPlus + axis is that axis pushed past half travel in
    /// its positive direction (a trigger pulled, a stick right or down);
    /// kPadAxisMinus + axis is the negative direction.
    static constexpr s32 kPadAxisPlus  = 100;
    static constexpr s32 kPadAxisMinus = 200;

    /// The pad's analogue controls: the aim stick (steering, bank, flight
    /// stick), the two pedals and Virtual On's right lever, in X/Y pairs for
    /// the sticks. Which game channel each feeds is the ROM's business. Keep
    /// kCount last.
    enum class PadAxisRole : u32 { AimX, AimY, Accel, Brake, LeverX, LeverY, kCount };
    static constexpr u32 kPadAxisCount = static_cast<u32>(PadAxisRole::kCount);

    /// A gamepad layout: the bindings for both players.
    struct PadLayout {
        /// The SDL_GamepadButton bound to each role, per player, or -1: South 0,
        /// East 1, West 2, North 3, Back 4, Start 6, LeftShoulder 9,
        /// RightShoulder 10, DpadUp 11, DpadDown 12, DpadLeft 13, DpadRight 14.
        std::array<std::array<s32, kPadRoleCount>, 2> buttons = {{
            // B1  B2  B3  B4  Up  Dn  Lf  Rt  Start Coin GearUp GearDown
            {  0,  1,  2,  3, 11, 12, 13, 14,  6,   4,   10,    9 },
            {  0,  1,  2,  3, 11, 12, 13, 14,  6,   4,   10,    9 },
        }};

        /// The SDL_GamepadAxis bound to each analogue role, per player, and
        /// whether it reads inverted. -1 keeps the default: aim on the left
        /// stick, accel on the right trigger, brake on the left trigger, the
        /// lever on the right stick. LeftX 0, LeftY 1, RightX 2, RightY 3,
        /// LeftTrigger 4, RightTrigger 5.
        std::array<std::array<s32, kPadAxisCount>, 2> axes = {{
            { -1, -1, -1, -1, -1, -1 },
            { -1, -1, -1, -1, -1, -1 },
        }};
        std::array<std::array<bool, kPadAxisCount>, 2> axis_invert = {{
            { false, false, false, false, false, false },
            { false, false, false, false, false, false },
        }};
        /// A binding that floors the pedal while held, or -1.
        std::array<std::array<s32, kPadAxisCount>, 2> axis_buttons = {{
            { -1, -1, -1, -1, -1, -1 },
            { -1, -1, -1, -1, -1, -1 },
        }};

        bool operator==(const PadLayout&) const = default;
    };

    /// The default layout, used by every game without its own.
    PadLayout pad;

    /// Per-game layouts, keyed by family (parent set name). Written as
    /// [family] sections at the end of the ini.
    std::map<std::string, PadLayout> pad_layouts;

    /// The layout to apply to `family` now.
    [[nodiscard]] const PadLayout& pad_layout_for(const std::string& family) const
    {
        const auto found = pad_layouts.find(family);
        return found != pad_layouts.end() ? found->second : pad;
    }

    // -- cabinet outputs ---------------------------------------------------

    /// Publish lamps and drive-board bytes over MAME's network output protocol.
    bool outputs_network = false;

    /// TCP port for the network outputs; MAME uses 8000.
    u32 outputs_network_port = 8000;

    /// Publish the same outputs over MAME's Windows-message protocol (Windows only).
    bool outputs_windows = false;

    // -- light-gun buttons -------------------------------------------------

    /// Actions a gun's buttons can drive. Reload doubles as Missile on titles
    /// that have one; the Hat directions are a D-pad some guns carry. kCount last.
    enum class GunRole : u32 {
        Trigger, Reload, Coin, Start, HatUp, HatDown, HatLeft, HatRight, kCount
    };
    static constexpr u32 kGunRoleCount = static_cast<u32>(GunRole::kCount);

    /// Raw evdev key code per gun role, per player, or 0 for unbound. Addressed
    /// by evdev code (not a joystick index) so bindings port across gun rules.
    /// Defaults suit a Batocera Sinden.
    std::array<std::array<u32, kGunRoleCount>, 2> gun_buttons = {{
        // Trigger, Reload,  Coin,    Start,   HatU,    HatD,    HatL,    HatR
        {0x110u,   0x111u,  0x101u,  0x102u,  0x105u,  0x106u,  0x107u,  0x108u},
        {0x110u,   0x111u,  0x101u,  0x102u,  0x105u,  0x106u,  0x107u,  0x108u},
    }};

    /// The light gun that plays player 1 / player 2, or empty for the default
    /// order (sorted by device path). Any part of the device path, ignoring
    /// case: the Raw Input path on Windows (e.g. "VID_16C0&PID_0F38&MI_00"),
    /// the evdev node (/dev/input/eventN) on Linux. On Windows a pinned mouse
    /// is used as a gun even when its product name does not say so.
    std::array<std::string, 2> gun_devices;

    // -- audio -------------------------------------------------------------

    /// Per-game volume; off plays every game at its default level.
    bool game_volume = false;

    /// Percent per game family, keyed by parent set name; absent means 100.
    std::map<std::string, u32> game_volumes;

    /// The music board's level in percent, for the games with one.
    u32 music_volume = 100;

    static constexpr u32 kDefaultGameVolume = 100;
    static constexpr u32 kMaxGameVolume     = 150;

    /// The volume to apply to `family` now.
    [[nodiscard]] u32 volume_for(const std::string& family) const
    {
        if (!game_volume) {
            return kDefaultGameVolume;
        }
        const auto found = game_volumes.find(family);
        return found != game_volumes.end() ? found->second : kDefaultGameVolume;
    }

    // -- cabinet link (networking) -----------------------------------------

    /// Link this cabinet to others over the LAN. Off keeps the in-process
    /// loopback, so a lone cabinet settles as node 1 of 1. The comms board is a
    /// ring: each cabinet listens on its own address and sends to the next.
    bool link_enabled = false;

    /// This cabinet's listen address; a blank IP binds every interface.
    std::string link_local_ip;
    std::string link_subnet_mask;  ///< Informational; prefilled from the NIC.
    u32         link_port = 15112;

    /// The next cabinet in the ring (where frames are sent). Empty disables sending.
    std::string link_next_ip;
    u32         link_next_port = 15112;

    /// This cabinet's 0-based ring position, for display only; the board
    /// negotiates the real link id.
    u32 link_cabinet_index = 0;

    /// Model 3 link: how many frames late the other cabinets' updates are read.
    /// More hides Wi-Fi jitter; each frame delays the other cars by 1/60 s.
    u32 link_delay = 6;

    // -- paths -------------------------------------------------------------

    /// ROM archives; a game named with no path loads <rom_dir>/<name>.{zip,7z}.
    /// No default (ROMs are not app data): empty until the user sets it.
    std::string rom_dir;

    /// Model 1 ROM archives; empty means rom_dir holds them too.
    std::string model1_rom_dir;

    /// Model 3 ROM archives; empty means rom_dir holds them too.
    std::string model3_rom_dir;

    /// Saves: per-game .nv and .eeprom images. Empty here;
    /// resolve_default_paths() fills it under the platform data directory.
    std::string nvram_dir;

    /// F12 screenshots. Empty here; defaulted like nvram_dir.
    std::string screenshot_dir;

    /// Empty means `bezels` beside the ini.
    std::string bezel_dir;

    /// Empty means `crosshairs` beside the ini.
    std::string crosshair_dir;

    /// Game-picker art/metadata cache. Always derived as `<config dir>/artwork` by
    /// resolve_default_paths(). A runtime field only, for the picker to read.
    std::string artwork_dir;

    /// Save states, `<game>.<slot>.kstate`, in `<nvram_dir>/states`; a runtime
    /// field only, not persisted.
    std::string states_dir;

    // -- library -----------------------------------------------------------

    /// Let the game picker fetch art/descriptions from ArcadeDB. Off keeps
    /// Keisho offline; the picker still lists and launches every game.
    bool scrape_artwork = true;

    // -- diagnostics -------------------------------------------------------

    bool validation = false;

    /// One of trace, debug, info, warning, error.
    std::string log_level = "info";
};

/// The settings file in `directory`.
[[nodiscard]] std::string config_file_in(const std::string& directory);

/// The default configuration path: keisho.ini in the working directory if it
/// exists, otherwise the platform's preferences directory.
[[nodiscard]] std::string default_config_path();

/// Platform data directory for saves and screenshots: Linux $XDG_DATA_HOME or
/// ~/.local/share, macOS ~/Library/Application Support, Windows %APPDATA%, each
/// with /keisho (Keisho on macOS and Windows). `config_in_cwd` returns ".".
[[nodiscard]] std::string data_directory(bool config_in_cwd);

/// Fills empty path fields; artwork_dir goes in `config_dir/artwork`.
void resolve_default_paths(Config* config, bool config_in_cwd,
                           const std::string& config_dir);

/// Read `path` into `out`, leaving unmentioned fields alone. Unknown lines go to
/// `problems` and are skipped, so a newer file still loads. False only when the
/// file exists but cannot be read.
[[nodiscard]] bool load_config(const std::string&        path,
                               Config*                   out,
                               std::vector<std::string>* problems);

/// Write `config` to `path`, with a comment per field, creating the directory
/// if need be.
[[nodiscard]] bool save_config(const std::string& path, const Config& config);

/// Parse a log level name. False on an unrecognised name, leaving the output alone.
[[nodiscard]] bool parse_log_level(const std::string& name, log::Level* out_level);

}  // namespace sm2
