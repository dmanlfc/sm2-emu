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

#include "core/types.h"

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

    /// Start the gamepad subsystem and open whatever is already plugged in.
    ///
    /// Returns false only if the subsystem itself will not start. A machine with no
    /// gamepad is not an error: the keyboard covers everything.
    [[nodiscard]] bool init();
    void shutdown();

    /// Offer an SDL event. Consumes connection and disconnection only; everything
    /// else is read as state in poll().
    void handle_event(const SDL_Event& event);

    /// Overwrite the digital and analogue fields of `inputs` from current device
    /// state. Call once per frame, before running the frame.
    void poll(hw::Inputs* inputs) const;

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
    [[nodiscard]] static ScriptedPress scripted_press(u32 frame, u32 coin_frame);

    /// Describe the bindings, for the help text.
    static void print_bindings();

private:
    /// One open gamepad and the player it drives.
    struct Pad {
        SDL_Gamepad*   handle = nullptr;
        SDL_JoystickID id     = 0;
        u32            player = 0;
    };

    void add_gamepad(SDL_JoystickID id);
    void remove_gamepad(SDL_JoystickID id);

    /// Lowest player position with no pad, or kPlayers when both are taken.
    [[nodiscard]] u32 first_free_player() const;

    std::vector<Pad> m_pads;
    bool             m_started = false;
};

}  // namespace sm2::osd
