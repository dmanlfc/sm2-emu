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
// Decodes the byte a game writes to its force-feedback drive board into a
// device-neutral effect, so the wheel and gamepad paths share one reading of it.
// Each title family speaks its own protocol (rom::DriveProtocol).
#pragma once

#include "core/types.h"
#include "rom/game.h"

namespace sm2::osd {

/// Full strength of a decoded command.
inline constexpr int kDriveFull = 1024;

struct DriveCommand {
    enum class Effect : u8 {
        None,       ///< No force.
        Other,      ///< Not a force command (handshake, parameter); keeps the current one.
        Spring,     ///< Centring spring.
        Friction,   ///< Resistance to turning, proportional to wheel speed.
        Vibrate,    ///< Shaking with no direction.
        PushLeft,   ///< Constant force turning the wheel left.
        PushRight,  ///< Constant force turning the wheel right.
        Chop,       ///< Sets the board's torque chopping (`chop`); keeps the current force.
    };

    Effect effect   = Effect::None;
    int    strength = 0;  ///< 0..kDriveFull.
    /// A push the game streams continuously (a torque), rather than a jolt.
    bool   held     = false;
    /// For Chop: 0 ends the chopping, 1..7 chops the torque, flipping it off
    /// and on every 2^(chop+1) interrupts of the drive board.
    u8     chop     = 0;

    [[nodiscard]] bool is_push() const
    {
        return effect == Effect::PushLeft || effect == Effect::PushRight;
    }
};

[[nodiscard]] DriveCommand decode_drive_command(rom::DriveProtocol protocol, u8 value);

}  // namespace sm2::osd
