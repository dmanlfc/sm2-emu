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
// Decodes the byte a game writes to its force-feedback drive board into a
// device-neutral effect, so the wheel and gamepad paths share one reading of it.
// Each title family speaks its own protocol (rom::DriveProtocol).
#pragma once

#include "core/types.h"
#include "rom/game.h"

#include <optional>

namespace sm2::osd {

/// Full strength of a decoded command: the strongest push a board can be asked for.
inline constexpr int kDriveFull = 1024;

struct DriveCommand {
    enum class Effect : u8 {
        None,       ///< No force.
        Other,      ///< Not a force command (handshake, parameter); keeps the current one.
        Spring,     ///< Centring spring.
        Friction,   ///< Resistance to turning, proportional to wheel speed.
        PushLeft,   ///< Constant force turning the wheel left.
        PushRight,  ///< Constant force turning the wheel right.
        /// Constant force away from the centre. The board picks the side from
        /// the wheel's position: right of centre pushes right, anywhere else
        /// pushes left.
        Kick,
    };

    Effect effect   = Effect::None;
    int    strength = 0;  ///< 0..kDriveFull.
    /// A push the game streams continuously (a torque), rather than a jolt.
    bool   held     = false;
    /// The board's spring shape: nothing inside `deadzone`, then a ramp of two
    /// board units a pot step from `ramp_from`, clipped at `strength`, which
    /// applies regardless from `full_at`. Axis units; zero `full_at` is a plain ramp.
    int    deadzone  = 0;
    int    ramp_from = 0;
    int    full_at   = 0;
    /// Sega Rally's chop setting, sent as a parameter byte. 0 turns chopping
    /// off; n chops the streamed torque with a period of 2^(n+2) board ticks.
    /// -1 means this byte does not change it.
    int    pulse    = -1;

    /// A constant force in some direction.
    [[nodiscard]] bool is_push() const
    {
        return effect == Effect::PushLeft || effect == Effect::PushRight
            || effect == Effect::Kick;
    }
};

[[nodiscard]] DriveCommand decode_drive_command(rom::DriveProtocol protocol, u8 value);

/// Whether the protocol is played on DaytonaBoard.
[[nodiscard]] inline bool daytona_board(rom::DriveProtocol protocol)
{
    return protocol == rom::DriveProtocol::Daytona || protocol == rom::DriveProtocol::Vr;
}

/// Virtua Racing's byte as Daytona's board takes it, or nullopt for one that is
/// not for the wheel. Per SailorSat's DriveTranslation.bas, which drives a
/// Daytona board from it: strengths are halved, 0x3x takes the wider
/// deadzone, 0x7x is the seat's air bags and 0x9x the meter. 0x20-0x24 are the
/// handle modes the game's own drive board test names.
[[nodiscard]] std::optional<u8> vr_to_daytona(u8 value);

}  // namespace sm2::osd
