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
// Daytona USA's drive board (EPR-16488A). The game picks an effect; the board
// works out the motor's way and the clutch's power from the wheel's position.
#pragma once

#include "core/types.h"

namespace sm2 {
class Archive;
}

namespace sm2::osd {

class DaytonaBoard {
public:
    /// The motor's ways. Down lowers the wheel's position: left on a cabinet.
    enum class Way : u8 { None, Down, Up };

    struct Output {
        Way way   = Way::None;
        /// 0..63, the clutch's power. With no way set, it brakes the wheel.
        int power = 0;
    };

    /// As at power-up: forces off, no effect.
    void reset()
    {
        const int dip = m_dip_power;
        *this         = DaytonaBoard{};
        m_dip_power   = dip;
    }

    /// The operator's strength, DIP switches 1-3 ON counting 0 to 7: 4 a step
    /// added to every power.
    void set_dip(int switches) { m_dip_power = 4 * switches; }

    /// A byte the game wrote to the board.
    void command(u8 value);

    /// Advances one frame. `counts` is the wheel's distance from the centre
    /// (128 a side, positive is right), `ticks` the number of board interrupts
    /// and `breakaway` the power found by the start-up calibration.
    [[nodiscard]] Output step(int counts, double ticks, int breakaway);

    /// Saves or restores the board's state, for save states.
    void serialize(Archive& ar);

    [[nodiscard]] bool forces_on() const { return m_enabled; }
    [[nodiscard]] u8   effect() const { return m_effect; }
    [[nodiscard]] bool oscillating() const { return m_oscillating; }

private:
    int  spring_power(int off, int dead_zone, int full_at, int base) const;
    void track_swings(int side, int ticks);

    bool m_enabled = false;  ///< $0x: forces on or off.
    u8   m_effect  = 0;      ///< The last $1x-$6x command.
    int  m_dip_power = 0;    ///< (~DIP switches 1-3) * 4, 0..28.
    int  m_boost   = 0;      ///< 2..9 from $7x, 0 after a disabling $0x.

    Way    m_way      = Way::None;  ///< Kept from one interrupt to the next.
    int    m_power    = 0;          ///< The power going out, ramped.
    double m_tick_acc = 0.0;        ///< Interrupts not yet run, a fraction.

    // The oscillation detector.
    bool m_oscillating = false;
    int  m_swings      = 0;
    int  m_away        = 0;  ///< Interrupts since the wheel left the centre.
    int  m_centred_for = 0;  ///< Interrupts at the centre.
    bool m_was_centred = true;
};

}  // namespace sm2::osd
