//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "osd/drive_command.h"

namespace sm2::osd {
namespace {

using Effect = DriveCommand::Effect;

DriveCommand make(Effect effect, int steps, int full_steps, bool held = false)
{
    return {effect, steps * kDriveFull / full_steps, held};
}

// Daytona's bytes (EPR-16488A), as read for pads and plain rumble. A
// force-feedback wheel runs DaytonaBoard instead.
//   0x1x  motor off
//   0x2x  friction     0..7
//   0x3x  centring     0..7; 8..15 are the same strengths with a wider deadzone
//   0x4x  kick away from the centre  0..7
//   0x5x  push left    0..7
//   0x6x  push right   0..7
// All strengths share the board's scale (0..63), including its gain setting
// of 2. A push or kick is 12 + 2n, so even step 0 is a firm push; only 0x1x
// releases the wheel. A spring is 2n + 2, reached by a ramp of two a pot step
// from just inside its deadzone. Steps 8..15 of the other families match
// nothing on the board and also stop the motor. 0x0x and 0x7x are the boot
// handshake and the gain setting.
DriveCommand decode_daytona(u8 value)
{
    constexpr DriveCommand kOther{Effect::Other};
    constexpr DriveCommand kOff{};
    constexpr int kPotStep = 256;  // one step of the board's 8-bit pot, in axis units
    const int low = value & 0x0f;
    const auto push = [low](Effect effect) { return make(effect, 6 + low, 13); };
    switch (value & 0xf0) {
        case 0x10: return kOff;
        case 0x20: return low <= 7 ? make(Effect::Friction, low + 1, 8) : kOff;
        case 0x30: {
            DriveCommand spring = make(Effect::Spring, (low & 7) + 1, 13);
            spring.deadzone     = (low < 8 ? 3 : 8) * kPotStep;
            spring.ramp_from    = (low < 8 ? 2 : 7) * kPotStep;
            spring.full_at      = (low < 8 ? 21 : 24) * kPotStep;
            return spring;
        }
        case 0x40: return low <= 7 ? push(Effect::Kick) : kOff;
        case 0x50: return low <= 7 ? push(Effect::PushLeft) : kOff;
        case 0x60: return low <= 7 ? push(Effect::PushRight) : kOff;
        default:   return kOther;
    }
}

// EPR-18261's bytes (Indy 500, Touring Car, Over Rev, Super GT 24h), as read
// for pads and plain rumble. A force-feedback wheel runs IndyBoard instead.
//   0x1x  motor off
//   0x2x  friction     0..7
//   0x3x  centring     0..7; 8..15 repeat them
//   0x5x  push left    0..7, where 0 is no push
//   0x6x  push right   0..7, where 0 is no push
// A push fades to nothing at step 0 (Indy 500 streams 0x54, 0x53 .. 0x50 as a
// jolt dies away), so strengths run from zero. 0x0x is game state and 0x7x the
// motor strength. Only IndyBoard plays 0x4x, 0x9x/0xcx and 0xax/0xbx.
DriveCommand decode_indy(u8 value)
{
    constexpr DriveCommand kOther{Effect::Other};
    constexpr int kPotStep = 256;
    const int low = value & 0x0f;
    const auto push = [low](Effect effect) {
        return low == 0 ? DriveCommand{} : make(effect, low, 7);
    };
    switch (value & 0xf0) {
        case 0x10: return DriveCommand{};
        case 0x20: return make(Effect::Friction, (low & 7) + 1, 8);
        case 0x30: {
            DriveCommand spring = make(Effect::Spring, (low & 7) + 1, 13);
            spring.deadzone     = 3 * kPotStep;
            spring.ramp_from    = 2 * kPotStep;
            spring.full_at      = 21 * kPotStep;
            return spring;
        }
        case 0x50: return push(Effect::PushLeft);
        case 0x60: return push(Effect::PushRight);
        default:   return kOther;
    }
}

// Sega Rally streams a torque every frame, strength in the low five bits,
// decoded the way its board's program (EPR-17891) does:
//   0x80..0x9f  push right  1..32
//   0xc0..0xdf  push left   1..32
//   0x40..0x5f  brake       1..32
//   0x10..0x17  chop the torque on and off, 0x10 stops
// Holding the wheel off-centre makes the game push it back. 0x00 releases.
DriveCommand decode_rally(u8 value)
{
    const int low = value & 0x1f;
    switch (value & 0xe0) {
        case 0x00:
            if (value == 0x00) {
                return DriveCommand{};
            }
            if ((value & 0xf8) == 0x10) {
                DriveCommand chop{Effect::Other};
                chop.pulse = value & 7;
                return chop;
            }
            return DriveCommand{Effect::Other};
        case 0x40: return make(Effect::Friction, low + 1, 32);
        case 0x80: return make(Effect::PushRight, low + 1, 32, true);
        case 0xc0: return make(Effect::PushLeft, low + 1, 32, true);
        default:   return DriveCommand{Effect::Other};
    }
}

}  // namespace

std::optional<u8> vr_to_daytona(u8 value)
{
    const u8 group = value & 0xf0;
    const u8 low   = value & 0x0f;
    switch (value) {
        // The handle modes, as the game's DRIVE BD TEST labels them.
        case 0x20:
        case 0x21: return u8{0x33};  // centering
        case 0x22: return u8{0x53};  // roll left
        case 0x23: return u8{0x63};  // roll right
        case 0x24: return u8{0x10};  // free
        default:   break;
    }
    switch (group) {
        case 0x20:
        case 0x40:
        case 0x50:
        case 0x60: return static_cast<u8>(group | (low >> 1));
        case 0x30: return static_cast<u8>(0x38 | (low >> 1));
        case 0x70:
        case 0x90: return std::nullopt;
        default:   return value;
    }
}

DriveCommand decode_drive_command(rom::DriveProtocol protocol, u8 value)
{
    switch (protocol) {
        case rom::DriveProtocol::Vr: {
            const std::optional<u8> board = vr_to_daytona(value);
            return board ? decode_daytona(*board) : DriveCommand{Effect::Other};
        }
        case rom::DriveProtocol::Indy:  return decode_indy(value);
        case rom::DriveProtocol::Rally: return decode_rally(value);
        case rom::DriveProtocol::Daytona:
        default: return decode_daytona(value);
    }
}

}  // namespace sm2::osd
