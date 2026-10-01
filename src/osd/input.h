//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#pragma once

#include "core/config.h"
#include "core/types.h"
#include "osd/daytona_board.h"
#include "osd/drive_command.h"
#include "osd/indy_board.h"
#include "rom/game.h"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {
struct Inputs;
}

namespace sm2::osd {

class LightGuns;
class WheelForce;

/// The cabinet's controls, driven by gamepads and the keyboard.
///
/// Model 2 inputs are levels, not events: what matters is whether a button is held
/// at the moment the program polls the I/O controller, not when it changed. So this
/// samples device state once per frame rather than accumulating key presses, and
/// the sample has to happen before the frame runs.
///
/// Gamepads and the keyboard are merged rather than exchanged. Both are always
/// live, so a second player can join on the keyboard, and the operator controls
/// stay reachable without a pad. A control is pressed if any device presses it.
class Input {
public:
    /// Cabinet player positions. Model 2A wires two.
    static constexpr u32 kPlayers = 2;

    /// Bit assignments of the operator port, from MAME's Model 2 input ports. All
    /// active low: a pressed control pulls its bit to zero.
    static constexpr u8 kCoin1   = 0x01;
    static constexpr u8 kCoin2   = 0x02;
    static constexpr u8 kTest    = 0x04;
    static constexpr u8 kService = 0x08;
    static constexpr u8 kStart1  = 0x10;
    static constexpr u8 kStart2  = 0x20;

    /// Bit assignments of a player port, also active low.
    static constexpr u8 kButton1 = 0x01;
    static constexpr u8 kButton2 = 0x02;
    static constexpr u8 kButton3 = 0x04;
    static constexpr u8 kButton4 = 0x08;
    static constexpr u8 kDown    = 0x10;
    static constexpr u8 kUp      = 0x20;
    static constexpr u8 kRight   = 0x40;
    static constexpr u8 kLeft    = 0x80;

    // -- axis conversion ---------------------------------------------------
    // Separated out because none of it can be checked with a gamepad in hand: the
    // interesting cases are the extremes and the rest position, which a person
    // cannot hold precisely.

    /// Direction bits an analogue stick at (x, y) presses.
    ///
    /// The stick stands in for an eight-way switch gate, so the threshold is
    /// deliberately generous: a shallow lean must mean nothing at all, and a
    /// diagonal must close both switches at once.
    [[nodiscard]] static u8 stick_bits(s16 x, s16 y);

    /// An axis that rests in the middle, such as a steering wheel, mapped onto the
    /// eight-bit value the I/O controller's converter reports.
    [[nodiscard]] static u8 axis_to_centred(s16 value);

    /// An axis that rests at one end, such as a pedal.
    [[nodiscard]] static u8 axis_to_pedal(s16 value);

    Input();
    ~Input();

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    /// Feel settings for a force-feedback wheel. `ffb` off leaves the wheel
    /// steering but limp; `strength` is 0..100 percent of the wheel's torque.
    struct WheelSettings {
        bool ffb           = true;
        u32  strength      = 50;
        /// The light centring spring of the Sega Rally and Daytona USA panel,
        /// 0..100 percent of `strength`; no other game has one.
        u32  panel_spring  = 20;
        /// Reverses the force, for a driver that pushes right on the positive
        /// force with which DirectInput, SDL and Linux push left.
        bool ffb_invert    = false;
        u32  steer_degrees = 270;
        u32  lock_degrees  = 240;  ///< physical rotation for full game lock.

        /// Synthetic engine/road rumble (Daytona streams no continuous buzz, so
        /// this is derived from the throttle) and its 0..100 strength.
        bool rumble          = true;
        u32  rumble_strength = 40;

        /// Wheel button index per cabinet role, indexed by Config::WheelRole;
        /// -1 unbinds. Set by the GUI, since numbering differs between wheels.
        std::array<s32, Config::kWheelRoleCount> buttons =
            Config{}.wheel_buttons;

        /// An H shifter's lever in none of the gears is in neutral; and how
        /// long, in ms, a sequential shift between two gears shows neutral.
        bool shifter_neutral  = true;
        u32  shift_neutral_ms = 100;

        /// Wheel axis per analogue control, or -1 to auto-detect. Invert flags
        /// apply to a pedal that reads high released, low pressed. Half flags,
        /// to a pedal on half of its axis, released at the centre and pressed
        /// towards the high end, the low one when inverted.
        s32  steer_axis   = -1;
        s32  accel_axis   = -1;
        s32  brake_axis   = -1;
        bool accel_invert = false;
        bool brake_invert = false;
        bool accel_half   = false;
        bool brake_half   = false;
    };

    /// Start the gamepad subsystem and open whatever is already plugged in.
    ///
    /// Returns false only if the subsystem itself will not start. A machine with no
    /// gamepad is not an error: the keyboard covers everything.
    [[nodiscard]] bool init(const WheelSettings& wheel);
    void shutdown();

    /// Offer an SDL event. Consumes connection and disconnection only; everything
    /// else is read as state in poll().
    void handle_event(const SDL_Event& event);

    /// Overwrite the digital and analogue fields of `inputs` from current device
    /// state. Call once per frame, before running the frame.
    ///
    /// The game is needed in full, not just its input flags: which mux channel
    /// each control sits on, its travel limits and its value at rest are all
    /// per-title (`rom::GameSpec::analog`), as is the lightgun calibration.
    void poll(hw::Inputs* inputs, const rom::GameSpec& game) const;

    /// Digital-only overload, for callers with no game metadata. Leaves every
    /// analogue channel at zero.
    void poll(hw::Inputs* inputs) const;

    /// Replace the wheel feel settings live, e.g. from a GUI slider. Cheap; the
    /// steering range and FFB strength take effect on the next frame, a new
    /// axis calibration at once.
    void set_wheel_settings(const WheelSettings& wheel);

    /// True when a wheel is connected, for the settings UI to show its controls.
    [[nodiscard]] bool wheel_connected() const { return m_wheel.handle != nullptr; }

    /// How many gamepads are open, for the settings UI to report.
    [[nodiscard]] usize pad_count() const { return m_pads.size(); }

    /// The lowest-numbered wheel button currently held, or -1 if none. The
    /// button-binding UI polls this to capture "press the button for X".
    [[nodiscard]] s32 pressed_wheel_button() const;

    /// True once per press of the wheel button bound to the Menu role, for the
    /// main loop to toggle the overlay. Edge-triggered, so a held button fires
    /// once. Returns false when no wheel is connected or Menu is unbound.
    [[nodiscard]] bool menu_button_pressed();

    /// Number of axes on the connected wheel, or 0 if none.
    [[nodiscard]] int wheel_axis_count() const;

    /// Snapshot the current value of every wheel axis into `out` (up to `count`),
    /// for the calibration UI to record a resting baseline before the user
    /// operates a control.
    void wheel_axis_baseline(s16* out, int count) const;

    /// The axis that has moved furthest from `baseline`, once past a threshold,
    /// or -1 if none has moved enough yet. `positive` is set to whether it moved
    /// up from its baseline (a pedal that reads low when pressed reports false).
    [[nodiscard]] s32 captured_axis(const s16* baseline, int count, bool* positive) const;

    /// Decode the drive-board bytes written during the last frame. The last
    /// force command stays in effect until the game sends another.
    void update_drive_board(const rom::GameSpec& game, std::span<const u8> writes);

    /// Update the wheel's force from the drive board and the steering position.
    /// Call once per frame after update_drive_board(). Does nothing without a
    /// wheel, without force feedback, or for a title with no steering.
    void update_force_feedback(const rom::GameSpec& game);

    /// True while a wheel delivers force feedback to a game with a drive board;
    /// then nothing rumbles, the wheel's own rumble and gamepads included.
    [[nodiscard]] bool wheel_ffb_active(const rom::GameSpec& game) const;

    /// Drive gamepad rumble from the drive board; call once per frame.
    void update_pad_rumble(const rom::GameSpec& game);

    /// Names of the gamepads currently open, in player order. An empty string means
    /// that player has no pad.
    [[nodiscard]] std::vector<std::string> gamepad_names() const;

    /// Where each player is aiming, for the crosshair overlay: 0..1 across the
    /// game image (the letterboxed area, not the window). Updated every poll of a
    /// lightgun title.
    struct GunAim {
        bool  active = false;  ///< This player is aiming a gun this frame.
        bool  mouse  = false;  ///< Aimed by the system mouse pointer.
        float x      = 0.5f;
        float y      = 0.5f;
    };
    [[nodiscard]] const std::array<GunAim, kPlayers>& gun_aims() const { return m_gun_aims; }

    /// Number of dedicated evdev light guns opened (0 when built without evdev,
    /// none are plugged in, or the single-mouse fallback is in use).
    [[nodiscard]] usize gun_count() const;

    /// The name of gun `index`, for the settings UI, or empty if out of range.
    [[nodiscard]] std::string gun_name(usize index) const;

    /// Most recent button press on gun `index` (0 if none), consumed on read,
    /// for the settings UI's bind capture. Always 0 without evdev or a gun.
    [[nodiscard]] u16 gun_take_last_pressed(usize index) const;

    /// Recoil settings, pushed from the GUI/config each frame. When on, a gun
    /// with a motor pulses on each trigger pull.
    void set_recoil(bool enabled, u32 strength)
    {
        m_recoil_enabled  = enabled;
        m_recoil_strength = strength;
    }

    /// Gamepad rumble settings, pushed from the GUI/config each frame.
    void set_pad_rumble(bool enabled, u32 strength)
    {
        m_pad_rumble_enabled  = enabled;
        m_pad_rumble_strength = strength;
    }

    /// Per-player gun button bindings: the evdev key code for each GunRole,
    /// pushed from the config each frame. Sized [2 players][kGunRoles].
    static constexpr usize kGunRoles = 8;
    void set_gun_buttons(const std::array<std::array<u32, kGunRoles>, 2>& b)
    {
        m_gun_buttons = b;
    }

    /// Present-stage placement, pushed from the config each frame, so the gun /
    /// mouse pointer maps onto the same letterbox rectangle the backend draws
    /// the image into. A mismatch would offset every shot from the cursor.
    void set_present_placement(AspectMode aspect, ScalingMethod method)
    {
        m_present_aspect = aspect;
        m_present_method = method;
    }

    /// Whether the Sinden border is drawn. A gun aiming against that border
    /// reports positions on the game image; any other gun reports positions on
    /// the whole screen.
    void set_sinden_border(bool on) { m_sinden_border = on; }

    /// Bits to pull low on each port at a given frame, for unattended testing.
    struct ScriptedPress {
        u8 in0 = 0;  ///< Coins, start, service, test.
        u8 in1 = 0;  ///< Player 1.
    };

    /// Play a fixed sequence: two coins, start, then confirm a character.
    ///
    /// Exists so a capture can reach the game itself without anyone at the
    /// controls. Enough to get past the attract mode and the selection screens,
    /// which is what a rendering check needs; it is not a replay system.
    ///
    /// `start1_bit` is the game's own IN0 bit for IPT_START1, since several
    /// titles move it off MAME's default 0x10 (`rom::GameSpec::start1_bit`);
    /// passing 0x10 reproduces the previous fixed behaviour.
    [[nodiscard]] static ScriptedPress scripted_press(u32 frame, u32 coin_frame,
                                                       u8 start1_bit = kStart1);

    /// Describe the bindings, for the help text.
    static void print_bindings();

private:
    /// One open gamepad and the player it drives.
    struct Pad {
        SDL_Gamepad*   handle = nullptr;
        SDL_JoystickID id     = 0;
        u32            player = 0;
        /// Last magnitudes sent, and frames since, so a lapsing effect is re-armed.
        u16            rumble_low  = 0;
        u16            rumble_high = 0;
        int            rumble_age  = 0;
    };

    /// A steering wheel, opened through the joystick API because a wheel has no
    /// gamepad mapping and its steering axis wants the full 16 bits a gamepad
    /// stick throws away. Drives player one. Force feedback, when the device and
    /// the settings allow it, is a synthesised centring spring.
    struct Wheel {
        SDL_Joystick*  handle  = nullptr;
        SDL_JoystickID id      = 0;
        /// Force feedback, or null if the wheel has none. The constant force is
        /// set each frame from the game's drive-board command (drivers ignore
        /// FF_SPRING); the sine effect, where supported, carries the rumble.
        std::unique_ptr<WheelForce> ffb;
        int            force_level  = 0;   ///< Last level commanded, to skip no-ops.
        int            rumble_mag   = -1;  ///< last rumble magnitude, to skip no-ops.
        u16            rumble_period = 0;  ///< ms, of the last rumble sent; 0 before any.

        bool           autocenter = false;  ///< device autocentre still holding it.
        bool           can_rumble = false;  ///< has rumble motors of its own.

        /// Fallback when the device has no haptic effects: SDL's plain rumble,
        /// which goes straight to evdev FF_RUMBLE. Re-armed like the pad path,
        /// since a rumble lapses.
        u16            rumble_low  = 0;
        u16            rumble_high = 0;
        int            rumble_age  = 0;

        /// How many consecutive frames a one-directional constant force has been
        /// held, to decay a sustained crash push so a free-spinning PC wheel does
        /// not whip to the stop the way the cabinet's heavy wheel never could.
        int            constant_hold  = 0;
        int            constant_dir   = 0;  ///< sign of the held constant force.
        int            last_deflection = 0; ///< steering position last frame, for friction.

        /// Axis numbers on the device. Steering is the self-centring one;
        /// the pedals rest at one end. -1 means the device lacks it.
        int  steer_axis   = -1;
        int  accel_axis   = -1;
        int  brake_axis   = -1;
        bool accel_invert = false;
        bool brake_invert = false;
        bool accel_half   = false;  ///< pedal on half its axis, see WheelSettings.
        bool brake_half   = false;

        /// SDL reports 0 for an axis that has not sent an event yet, which on a
        /// pedal is half pressed. Until one moves, use its value read at open.
        static constexpr int      kMaxAxes = 8;
        std::array<s16, kMaxAxes> axis_rest{};
        u32                       axes_moved = 0;  ///< bitmask of axes seen moving.
    };

    [[nodiscard]] s16 wheel_axis(int axis) const;

    /// How far a pedal on `axis` is pressed, 0 released to 1 fully: over the
    /// whole axis, or over half of it from the centre when `half`; `invert`
    /// for a pedal pressed towards the low end.
    [[nodiscard]] float pedal_travel(int axis, bool invert, bool half) const;


    void add_gamepad(SDL_JoystickID id);
    void remove_gamepad(SDL_JoystickID id);

    /// Open `id` as a wheel if it looks like one and no wheel is open yet.
    void add_wheel(SDL_JoystickID id);
    void remove_wheel(SDL_JoystickID id);

    /// Give the open wheel's axes their roles: calibrated, or auto-detected.
    void assign_wheel_axes();


    /// Read one driving control straight off the wheel, or a sentinel byte when
    /// the wheel has no axis for it. Steering is centred, pedals rest low.
    [[nodiscard]] bool sample_wheel_channel(const rom::AnalogChannel& channel,
                                            u8* out) const;

    /// The pad driving a given player, or nullptr if that player has none.
    [[nodiscard]] SDL_Gamepad* pad_for(u32 player) const;

    /// Sample one logical control and scale it into an analogue channel's own
    /// calibrated range.
    [[nodiscard]] u8 sample_channel(const rom::AnalogChannel& channel) const;

    void gather_lightguns(hw::Inputs* inputs, const rom::GameSpec& game) const;

    /// Lowest player position with no pad, or kPlayers when both are taken.
    [[nodiscard]] u32 first_free_player() const;

    std::vector<Pad> m_pads;
    Wheel            m_wheel;
    WheelSettings    m_wheel_settings;

    /// Sequential-shifter state for a wheel's paddles, seeded from the game's
    /// start_gear and re-seeded when another game is loaded. Mutable because
    /// poll() is const but must remember the gear between frames and fire once
    /// per press, not once per frame held.
    mutable u32         m_wheel_gear = 1;    ///< 0 = neutral, 1..4 = the gears.
    mutable std::string m_gear_game;         ///< game the gate was seeded for.
    mutable bool m_gear_up_held    = false;
    mutable bool m_gear_down_held  = false;
    /// Frames of neutral left in a sequential shift between two gears.
    mutable u32  m_shift_neutral_frames = 0;

    /// Gamepad shoulder-button shifter edge state, shared with the wheel gate.
    mutable bool m_pad_gear_up_held   = false;
    mutable bool m_pad_gear_down_held = false;
    bool         m_menu_held       = false;  ///< edge state for the Menu-bound wheel button.

    /// Desert Tank's forward/reverse shift.
    mutable bool m_desert_shift      = false;  ///< latched forward(false)/reverse(true).
    mutable bool m_desert_shift_held = false;  ///< edge state of the shift button.

    /// Gun-cursor position per player, 0..1 in game-image space, for the
    /// keyboard/pad aiming fallback on gun titles (mouse/dedicated gun preferred).
    /// Nudged by the pad sticks and keyboard arrows; starts centred.
    mutable std::array<float, kPlayers> m_gun_cursor_x = {0.5f, 0.5f};
    mutable std::array<float, kPlayers> m_gun_cursor_y = {0.5f, 0.5f};

    /// The pad/keyboard cursor holds a player's aim until the mouse moves again.
    mutable std::array<bool, kPlayers> m_gun_cursor_owns = {false, false};
    mutable float                      m_gun_last_ptr_x  = -1.0f;
    mutable float                      m_gun_last_ptr_y  = -1.0f;

    /// Per-device light guns, when built and present. Held by pointer so the
    /// platform detail stays out of this header; null when no guns were
    /// opened, in which case the single-mouse pointer path is used.
    /// Mutable because poll() is const but must drain each gun's event queue.
#ifdef SM2_HAVE_LIGHTGUNS
    mutable std::unique_ptr<LightGuns> m_guns;
#endif

    /// Latest per-player aim in game-image space, for the crosshair overlay.
    /// Written by gather_lightguns, read by the GUI. Mutable for the same reason.
    mutable std::array<GunAim, kPlayers> m_gun_aims{};

    /// Recoil settings and the previous trigger level per gun, so a pulse fires
    /// once on the press edge rather than every frame the trigger is held.
    /// Sized to LightGuns::kMaxGuns (which is forward-declared here) plus slack.
    static constexpr usize kMaxGuns = 8;
    bool                             m_recoil_enabled  = true;
    u32                              m_recoil_strength = 60;

    /// The drive board's current force command.
    DriveCommand                     m_drive_command;

    /// Daytona's drive board, which works out its forces from the wheel: fed
    /// the game's bytes, it drives a force-feedback wheel.
    DaytonaBoard                     m_daytona;

    /// Indy 500's drive board (Touring Car, Over Rev, Super GT), the same way.
    IndyBoard                        m_indy;

    /// Sega Rally's board-side torque chopping: 0 off, 1..7 set by $11..$17.
    /// It outlives the torque commands until a $10 ends it, as on the board.
    u8                               m_drive_chop  = 0;
    /// The Sega Rally board's interrupts counted since start, simulated: its
    /// chopping flips on bits of this count.
    double                           m_drive_ticks = 0.0;

    /// The burst currently playing, shared by every pad: one drive board, one car.
    int                              m_pad_rumble_level    = 0;
    int                              m_pad_rumble_dir      = 0;  ///< last jolt direction
    int                              m_pad_rumble_hold     = 0;  ///< frames left in the burst
    bool                             m_pad_rumble_enabled  = true;
    u32                              m_pad_rumble_strength = 60;
    mutable std::array<bool, kMaxGuns> m_trigger_was_down{};

    /// Gun role -> evdev key code, per player; Sinden defaults until the config
    /// is applied. [player][role].
    std::array<std::array<u32, kGunRoles>, 2> m_gun_buttons = {{
        {0x110u, 0x111u, 0x101u, 0x102u, 0x105u, 0x106u, 0x107u, 0x108u},
        {0x110u, 0x111u, 0x101u, 0x102u, 0x105u, 0x106u, 0x107u, 0x108u},
    }};
    enum GunRoleIdx { GrTrigger, GrReload, GrCoin, GrStart,
                      GrHatUp, GrHatDown, GrHatLeft, GrHatRight };

    /// Present placement in effect, for the gun/pointer letterbox mapping.
    AspectMode    m_present_aspect = AspectMode::FourThree;
    ScalingMethod m_present_method = ScalingMethod::SharpBilinear;
    bool          m_sinden_border  = false;

    bool             m_started = false;
};

}  // namespace sm2::osd
