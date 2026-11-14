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
// The answer of the EPR-18261 drive board (Indy 500, Touring Car, Over Rev,
// Super GT 24h) on the I/O controller's port D.
#pragma once

#include "core/types.h"

namespace sm2::hw {

/// The centre the board works from. A blank or uncalibrated NVRAM sends $FF,
/// which is full lock and never a real centre.
[[nodiscard]] constexpr u8 indy_board_centre(u8 sent)
{
    return sent == 0xff ? u8{0x80} : sent;
}

struct IndyDriveReply {
    u8 command = 0x00;
    u8 centre  = 0x80;  ///< $80 at power-up; $9x sets its high nibble, $Cx its low.

    /// A byte the game wrote to the board.
    void write(u8 value)
    {
        command = value;
        if ((value >> 4) == 0x9) {
            centre = static_cast<u8>((centre & 0x0f) | (value << 4));
        } else if ((value >> 4) == 0xc) {
            centre = static_cast<u8>((centre & 0xf0) | (value & 0x0f));
        }
    }

    /// The board's answer, where `wheel` is the steering pot's value.
    [[nodiscard]] u8 read(u8 wheel) const
    {
        switch (command) {
            case 0x80:
            case 0x83: return wheel;
            case 0x81: return 0xff;
            case 0x82: return 0xff;  // DIP switches 1, all OFF
            case 0x84: return indy_board_centre(centre);
            default:   return command;
        }
    }
};

}  // namespace sm2::hw
