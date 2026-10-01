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
#include "osd/daytona_board.h"

#include <algorithm>
#include <cstdlib>

namespace sm2::osd {
namespace {

/// The operator's strength, (~DIP switches 1-3) * 4: switches left off.
constexpr int kDipPower = 0;

/// The motor's power is 6 bits.
constexpr int kMaxPower = 0x3f;

}  // namespace

void DaytonaBoard::command(u8 value)
{
    // The board takes a byte once it has read it at two interrupts in a row,
    // which a byte the game latches always is.
    const int high = value >> 4;
    if (high == 0x0) {
        // $0a and $05-$07, $0d-$0f turn the forces on; the rest off.
        m_enabled = value == 0x0a || (value & 0x07) >= 5;
        if (!m_enabled) {
            m_boost = 0;
        }
    } else if (high <= 0x6) {
        m_effect = value;
    } else if (high == 0x7) {
        m_boost = ((value & 0x0e) >> 1) + 2;
    }
    // $80-$ff ask for the wheel position, $ff or the DIP switches, or nothing:
    // no effect on the forces.
}

int DaytonaBoard::spring_power(int off, int dead_zone, int full_at, int base) const
{
    // Strength 2n plus the operator's; slope 2 per count, 3 from a strength of
    // $1e; from the dead zone's edge, stopped at the strength from `full_at`.
    const int strength = 2 * (m_effect & 0x07) + kDipPower;
    const int slope    = strength >= 0x1e ? 3 : 2;
    int       power    = strength;
    if (off < full_at) {
        power = slope * (off - dead_zone) - m_boost;
        if (power >= strength) {
            power = strength;
        }
    }
    return power + base;
}

void DaytonaBoard::track_swings(int side, int ticks)
{
    // Coming back to the centre after $31..$124 interrupts away is a swing;
    // ten in a row and the centring springs let go. Staying at the centre $62
    // interrupts, or away $125 ($5b9 while swinging), or a quicker return,
    // clears it.
    if (side != 0) {
        m_away += ticks;
        m_was_centred = false;
        if (m_away >= (m_oscillating ? 0x5b9 : 0x125)) {
            m_oscillating = false;
            m_swings      = 0;
            m_away        = 0;
        }
        return;
    }
    if (m_was_centred) {
        m_centred_for += ticks;
        if (m_centred_for >= 0x62) {
            m_oscillating = false;
            m_swings      = 0;
        }
        return;
    }
    m_centred_for = 0;
    if (!m_oscillating) {
        if (m_away >= 0x31 && m_away < 0x125) {
            m_oscillating = ++m_swings >= 10;
        } else {
            m_swings = 0;
        }
    }
    m_away        = 0;
    m_was_centred = true;
}

DaytonaBoard::Output DaytonaBoard::step(int counts, double ticks, int breakaway)
{
    m_tick_acc += ticks;
    const int interrupts = static_cast<int>(m_tick_acc);
    m_tick_acc -= interrupts;

    const int off  = std::min(std::abs(counts), 0x80);
    const int side = counts >= 3 ? 2 : counts <= -3 ? 1 : 0;  // 1 below, 2 above
    track_swings(side, interrupts);

    // Added to every effect: further from the centre, a little more.
    const int base = off / 16 + breakaway + m_boost;
    const int n    = m_effect & 0x07;

    int  target = 0;
    bool none   = false;
    // Within a spring's centre zone it keeps its way at the breakaway power
    // until the wheel is right on the centre, then lets go of the way.
    const auto centre_zone = [&] {
        if (m_way != Way::None && off < 1) {
            m_way = Way::None;
        }
        target = breakaway;
    };
    const auto push = [&](Way way) {
        if (m_way != Way::None && m_way != way) {
            m_power = 0;  // a push changing way starts again from nothing
        }
        m_way  = way;
        target = 2 * n + kDipPower + base + 10;
    };

    if (!m_enabled) {
        none = true;
    } else {
        switch (m_effect & 0xf8) {
            case 0x20:  // a power and no way: the clutch holds the wheel to the standing motor
                m_way  = Way::None;
                target = 2 * n + kDipPower + base;
                break;
            case 0x30:  // centring spring, +-2 counts
                if (side == 0) {
                    centre_zone();
                } else if (m_oscillating) {
                    none = true;
                } else {
                    m_way  = side == 1 ? Way::Up : Way::Down;
                    target = spring_power(off, 2, 0x15, base);
                }
                break;
            case 0x38:  // centring spring, +-7 counts
                if (off < 8) {
                    centre_zone();
                } else if (m_oscillating) {
                    none = true;
                } else {
                    m_way  = side == 1 ? Way::Up : Way::Down;
                    target = spring_power(off, 7, 0x18, base);
                }
                break;
            case 0x40:  // away from the centre
                push(side == 2 ? Way::Up : Way::Down);
                break;
            case 0x50:
                push(Way::Down);
                break;
            case 0x60:
                push(Way::Up);
                break;
            default:  // $10-$1f, $28-$2f, $48-$4f, $58-$5f, $68-$6f: none
                none = true;
                break;
        }
    }
    if (none) {
        m_way  = Way::None;
        target = 0;
    }

    // The power going out moves one step per interrupt towards the target, and
    // never passes $3f.
    target = std::clamp(target, 0, 0xff);
    if (m_power < target) {
        m_power = std::min(m_power + interrupts, target);
    } else {
        m_power = std::max(m_power - interrupts, target);
    }
    m_power = std::min(m_power, kMaxPower);
    return {m_way, m_power};
}

}  // namespace sm2::osd
