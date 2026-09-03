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

#include "core/config.h"
#include "core/types.h"
#include "rom/game.h"

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <vector>

namespace sm2::hw {
struct Inputs;
}

namespace sm2::osd {

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

    Input() = default;
    ~Input();

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    /// Feel settings for a force-feedback wheel. `ffb` off leaves the wheel
    /// steering but limp; `strength` is 0..100 percent of the wheel's torque.
    struct WheelSettings {
        bool ffb           = true;
        u32  strength      = 50;
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

        /// Wheel axis per analogue control, or -1 to auto-detect. Invert flags
        /// apply to a pedal that reads high released, low pressed.
        s32  steer_axis   = -1;
        s32  accel_axis   = -1;
        s32  brake_axis   = -1;
        bool accel_invert = false;
        bool brake_invert = false;
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
    /// steering range and FFB strength take effect on the next frame.
    void set_wheel_settings(const WheelSettings& wheel) { m_wheel_settings = wheel; }

    /// True when a wheel is connected, for the settings UI to show its controls.
    [[nodiscard]] bool wheel_connected() const { return m_wheel.handle != nullptr; }

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

    /// Update the wheel's centring force from the current steering position.
    /// Call once per frame after poll(). Does nothing without a wheel, without
    /// force feedback, or for a title that is not a driving game (`drive_board`).
    void update_force_feedback(const rom::GameSpec& game, u8 drive_force);

    /// Names of the gamepads currently open, in player order. An empty string means
    /// that player has no pad.
    [[nodiscard]] std::vector<std::string> gamepad_names() const;

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
    };

    /// A steering wheel, opened through the joystick API because a wheel has no
    /// gamepad mapping and its steering axis wants the full 16 bits a gamepad
    /// stick throws away. Drives player one. Force feedback, when the device and
    /// the settings allow it, is a synthesised centring spring.
    struct Wheel {
        SDL_Joystick*  handle  = nullptr;
        SDL_JoystickID id      = 0;
        SDL_Haptic*    haptic  = nullptr;
        /// A constant-force effect the driver honours (FF_CONSTANT) where it
        /// ignores FF_SPRING. The level is set each frame from the game's own
        /// drive-board command byte, decoded in update_force_feedback.
        int            force_effect = -1;  ///< SDL effect id, or -1 if none.
        int            force_level  = 0;   ///< Last level commanded, to skip no-ops.

        /// A periodic (sine) effect run alongside the constant force to produce a
        /// real vibration the wheel hardware oscillates -- the road/impact rumble,
        /// which a once-per-frame constant force cannot convey. -1 if unsupported.
        int            rumble_effect = -1;
        int            rumble_mag    = -1;  ///< last rumble magnitude, to skip no-ops.

        /// How many consecutive frames a one-directional constant force has been
        /// held, to decay a sustained crash push so a free-spinning PC wheel does
        /// not whip to the stop the way the cabinet's heavy wheel never could.
        int            constant_hold  = 0;
        int            constant_dir   = 0;  ///< sign of the held constant force.

        /// Axis numbers on the device. Steering is the self-centring one;
        /// the pedals rest at one end. -1 means the device lacks it.
        int steer_axis = -1;
        int accel_axis = -1;
        int brake_axis = -1;
    };

    void add_gamepad(SDL_JoystickID id);
    void remove_gamepad(SDL_JoystickID id);

    /// Open `id` as a wheel if it looks like one and no wheel is open yet.
    void add_wheel(SDL_JoystickID id);
    void remove_wheel(SDL_JoystickID id);


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

    /// Sequential-shifter state for a wheel's paddles. The cabinet's gearbox is a
    /// five-position gate (four gears + reverse); paddles shift up and down
    /// through it. Mutable because poll() is const but must remember the gear
    /// between frames and fire once per press, not once per frame held.
    mutable u32  m_wheel_gear      = 0;      ///< 0..4 = gears 1..4, reverse.
    mutable bool m_gear_up_held    = false;
    mutable bool m_gear_down_held  = false;
    bool         m_menu_held       = false;  ///< edge state for the Menu-bound wheel button.

    bool             m_started = false;
};

}  // namespace sm2::osd
