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
#include "osd/indy_board.h"

#include <algorithm>
#include <cstdlib>

namespace sm2::osd {
namespace {

/// The operator's strength, DIP switches 1-6..8: left off.
constexpr int kDipPower = 0;

/// The motor's powers are 6 bits.
constexpr int kMaxPower = 63;

/// The NMIs between two looks at how far the wheel moved.
constexpr int kSpeedNmis = 18;

/// The positive half of the program's sine table (amplitude $80): a spring's
/// force against its distance from the centre, halved or quartered.
constexpr u8 kSine[128] = {
      0,   3,   6,   9,  12,  15,  18,  21,  24,  28,  31,  34,  37,  40,  43,  46,
     48,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,  81,  83,  85,  88,
     90,  92,  94,  96,  98, 100, 102, 104, 106, 108, 109, 111, 112, 114, 115, 117,
    118, 119, 120, 121, 122, 123, 124, 124, 125, 126, 126, 127, 127, 127, 127, 127,
    128, 127, 127, 127, 127, 127, 126, 126, 125, 124, 124, 123, 122, 121, 120, 119,
    118, 117, 115, 114, 112, 111, 109, 108, 106, 104, 102, 100,  98,  96,  94,  92,
     90,  88,  85,  83,  81,  78,  76,  73,  71,  68,  65,  63,  60,  57,  54,  51,
     48,  46,  43,  40,  37,  34,  31,  28,  24,  21,  18,  15,  12,   9,   6,   3,
};

}  // namespace

void IndyBoard::command(u8 value)
{
    // A command from $70 up is a setting or a question and leaves the effect
    // running.
    switch (value >> 4) {
        case 0x7: m_base = (value & 0x0f) / 2 + 2; return;
        case 0x9: m_centre = static_cast<u8>((m_centre & 0x0f) | ((value & 0x0f) << 4)); return;
        case 0xa: m_vib_period = value & 0x07; return;
        case 0xb: m_vib_amp = value & 0x07; return;
        case 0xc: m_centre = static_cast<u8>((m_centre & 0xf0) | (value & 0x0f)); return;
        default: break;
    }
    if (value < 0x70) {
        m_effect = value;
    }
}

int IndyBoard::strength() const
{
    return (2 * (m_effect & 0x07) + m_base) * 2 + kDipPower;
}

void IndyBoard::spring(int position, int shift, bool damp_other_side_below)
{
    // Towards the centre: the sine of the distance, halved ($30) or quartered
    // ($38), stopped at the strength, plus the damping on the way that brakes
    // the movement. Exactly on the centre it leaves the powers as they were.
    const int s = strength();
    if (position < m_centre) {
        const int curve = kSine[std::min((m_centre - position) >> shift, 127)];
        m_up   = std::min(curve, s) + m_speed_down;
        m_down = damp_other_side_below ? m_speed_up : 0;
    } else if (position > m_centre) {
        const int curve = kSine[std::min((position - m_centre) >> shift, 127)];
        m_down = std::min(curve, s) + m_speed_up;
        m_up   = m_speed_down;
    }
}

void IndyBoard::run_effect(int position)
{
    const int high = m_effect >> 4;
    if (high == 0x0) {
        const int x = m_effect & 0x0f;
        if (x == 1 || x >= 8) {
            // A reset: no force, base 2, no vibration.
            m_up = m_down = 0;
            m_base       = 2;
            m_vib_period = 0;
            m_vib_amp    = 0;
        } else if (x < 6) {
            // An idle centring: base 1, the soft spring at strength 0.
            m_base         = 1;
            const u8 saved = m_effect;
            m_effect       = 0x38;
            spring(position, 2, false);
            m_effect = saved;
        }
        // $06, $07: the powers stay as they are.
        return;
    }
    switch (high) {
        case 0x1:  // stop
            m_up = m_down = 0;
            m_vib_period  = 0;
            m_vib_amp     = 0;
            break;
        case 0x2:  // both ways at once one NMI in two: the motor brakes
            m_up = m_down = strength() / 2;
            break;
        case 0x3:
            if ((m_effect & 0x08) == 0) {
                spring(position, 1, true);
            } else {
                spring(position, 2, false);
            }
            break;
        case 0x4:  // away from the centre
            if (position < m_centre) {
                m_down = strength();
                m_up   = 0;
            } else {
                m_up   = strength();
                m_down = 0;
            }
            break;
        case 0x5:
            m_down = strength();
            m_up   = 0;
            break;
        case 0x6:
            m_up   = strength();
            m_down = 0;
            break;
        default:
            break;
    }
}

IndyBoard::Output IndyBoard::step(int position, double nmis)
{
    position = std::clamp(position, 0, 255);
    m_nmi_acc += nmis;
    const int elapsed = static_cast<int>(m_nmi_acc);
    m_nmi_acc -= elapsed;
    m_nmis += static_cast<u32>(elapsed);

    // How far the wheel went, down or up, between two looks.
    m_speed_nmis += elapsed;
    if (m_speed_nmis >= kSpeedNmis) {
        m_speed_nmis %= kSpeedNmis;
        if (position < m_speed_ref) {
            m_speed_down = m_speed_ref - position;
            m_speed_up   = 0;
        } else {
            m_speed_up   = position - m_speed_ref;
            m_speed_down = 0;
        }
        m_speed_ref = position;
    }

    run_effect(position);

    // With a vibration, an extra push goes one way then the other, each half
    // lasting 16 x the period NMIs; the damping goes on the other way.
    int up   = m_up;
    int down = m_down;
    if (m_vib_period != 0 && m_vib_amp != 0) {
        const int extra = m_vib_amp + std::abs(m_centre - position) / 8 + 1;
        if (((m_nmis >> 4) / static_cast<u32>(m_vib_period)) % 2 != 0) {
            up += extra;
            down += m_speed_up;
        } else {
            up += m_speed_down;
            down += extra;
        }
    }
    return {std::min(up, kMaxPower), std::min(down, kMaxPower)};
}

}  // namespace sm2::osd
