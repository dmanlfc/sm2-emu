//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#pragma once

#include "core/log.h"
#include "core/types.h"

#include <array>
#include <string>
#include <vector>

namespace sm2 {

/// Settings worth keeping between runs.
///
/// Deliberately only the persistent ones. Anything that describes a single run,
/// such as how many frames to capture, stays on the command line: writing it to a
/// file would mean the emulator behaved differently tomorrow for no visible reason.
///
/// Every field's default is the value here, so a missing or empty file behaves
/// exactly as no file at all.
struct Config {
    // -- presentation ------------------------------------------------------

    /// Wait for the display's vertical blank before presenting. Independent of
    /// pacing: this decides whether a frame can tear, not how fast the machine
    /// runs.
    ///
    /// Off by default: the software pacer is the rate authority, and a blocking
    /// FIFO present quantises frame times to the display refresh (a frame a hair
    /// over one vblank waits a whole extra one), which measured ~9% off Daytona
    /// on a Pi 5 for nothing. Turn back on if a bare/uncomposited display tears.
    bool vsync = false;

    /// Hold the machine to its own 57.5245 Hz. Turning this off runs as fast as the
    /// host manages, which is what a capture wants and nothing else does.
    bool throttle = true;

    bool fullscreen = false;

    /// Show the FPS counter overlay in the top-right corner.
    bool show_fps = true;

    /// Light-gun mode: draw the aiming crosshair(s) and hide the OS mouse cursor
    /// over the window, for the gun titles. Off leaves the crosshair hidden and
    /// the cursor visible (aiming still works, it is just not shown).
    bool lightgun = false;

    /// Draw sm2-emu's own aiming crosshair in light-gun mode. On by default;
    /// turn it off when the gun provides its own aiming (e.g. a Sinden with a
    /// physical sight) so the screen is not cluttered. Independent of the mode
    /// itself, which still hides the cursor and enables the gun paths.
    bool lightgun_crosshair = true;

    /// Fire the gun's recoil / rumble motor on each shot, for guns that have one
    /// (Sinden and similar, exposed as a force-feedback device). No effect on a
    /// gun without a motor. Strength is 0..100 percent.
    bool lightgun_recoil          = true;
    u32  lightgun_recoil_strength = 60;

    /// Sinden light-gun border: a solid bright frame around the game image that
    /// the gun's camera tracks. Colour is 0xRRGGBB; thickness is in game-image
    /// pixels. Only drawn in light-gun mode.
    bool sinden_border           = false;
    u32  sinden_border_colour    = 0xffffff;  ///< white by default.
    u32  sinden_border_thickness = 12;

    u32  window_width  = 992;
    u32  window_height = 768;

    /// Exact device name to prefer, as `--list-gpus` prints it. Empty picks the
    /// best-scoring device.
    std::string gpu;

    // -- steering wheel ----------------------------------------------------

    /// Synthesised centring resistance on a wheel that supports it: a spring
    /// whose strength grows with how far the wheel is turned. The drive board is
    /// not emulated, so there is no authentic motor force to replay; this is a
    /// feel, not a reproduction. Off means the wheel still steers, just limp.
    bool wheel_ffb = true;

    /// Centring resistance, 0..100 percent of the wheel's maximum torque.
    u32 wheel_ffb_strength = 30;

    /// A synthesised road/engine rumble: a vibration that rises with the
    /// throttle and with hard steering. Independent of the centring resistance.
    bool wheel_rumble = true;

    /// Rumble strength, 0..100 percent.
    u32 wheel_rumble_strength = 40;

    /// The wheel's own physical rotation range (a G-series PC wheel is ~900).
    u32 wheel_steer_degrees = 270;

    /// Physical rotation at which the game reaches full lock: turning this many
    /// degrees (total, so half each side of centre) drives the steering to its
    /// stop. Lower is more sensitive. Clamped to 180..270.
    u32 wheel_lock_degrees = 240;

    /// Cabinet controls a wheel button can be bound to. Buttons 1..4 are the
    /// arcade buttons, which is also where a driving cabinet's view-change / VR
    /// buttons land (e.g. Daytona's VR1..VR4). Test/Service are the operator
    /// coin-door buttons; Menu is the emulator overlay (F1), not a machine
    /// input. Keep kCount last.
    enum class WheelRole : u32 {
        Start, Coin, Button1, Button2, Button3, Button4, GearUp, GearDown,
        Test, Service, Menu, kCount
    };
    static constexpr u32 kWheelRoleCount = static_cast<u32>(WheelRole::kCount);

    /// Which wheel button (index) drives each role, or -1 for unbound. Button
    /// numbering differs between wheels, so these are set by the user in the GUI
    /// ("press the button for X"); the defaults suit a Logitech G-series.
    /// Test/Service/Menu default unbound.
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
    };

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

    // -- paths -------------------------------------------------------------

    std::string nvram_dir = "nvram";

    /// ROM database to use instead of searching the usual places.
    std::string games_xml;

    // -- diagnostics -------------------------------------------------------

    bool validation = false;

    /// One of trace, debug, info, warning, error.
    std::string log_level = "info";
};

/// Where the configuration is read from and written to when nothing says otherwise.
///
/// A file named sm2-emu.ini in the working directory wins, which is what a build
/// tree wants; otherwise the platform's preferences directory, which is what an
/// installed copy wants. Returns the working-directory path only when that file
/// exists, so a fresh install writes to the proper place.
[[nodiscard]] std::string default_config_path();

/// Read `path` into `out`, leaving fields the file does not mention alone.
///
/// A line the parser does not understand is appended to `problems` and skipped
/// rather than failing the load: a file written by a later version must not stop an
/// earlier one from starting. Returns false only when the file exists but cannot be
/// read; a missing file is success with nothing changed.
[[nodiscard]] bool load_config(const std::string&        path,
                               Config*                   out,
                               std::vector<std::string>* problems);

/// Write `config` to `path`, creating the directory if need be.
///
/// Every field is written with a comment, so the file doubles as the documentation
/// for what can be set.
[[nodiscard]] bool save_config(const std::string& path, const Config& config);

/// Parse a log level name. Returns false on an unrecognised name, leaving `out`
/// alone.
[[nodiscard]] bool parse_log_level(const std::string& name, log::Level* out_level);

}  // namespace sm2
