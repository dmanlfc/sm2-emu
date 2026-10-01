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
// Daytona USA's drive board as its program (EPR-16488A) behaves. The game does
// not send torques: it turns the forces on or off and picks an effect and its
// strength, and the board works out, from the wheel's position and at every
// interrupt, the way its AC motor turns and the power of the clutch that
// passes the torque to the wheel. Read in the EPROM; see
// BackForceFeeder's docs/EPR-16488A-Daytona-DriveBoard.
#pragma once

#include "core/types.h"

namespace sm2::osd {

class DaytonaBoard {
public:
    /// The motor's ways. Down lowers the wheel's position: left on a cabinet.
    enum class Way : u8 { None, Down, Up };

    struct Output {
        Way way   = Way::None;
        /// 0..63, the clutch's power. With no way, the motor stands and the
        /// clutch holds the wheel to it: a brake.
        int power = 0;
    };

    /// As at power-up: forces off, no effect.
    void reset() { *this = DaytonaBoard{}; }

    /// A byte the game wrote to the board.
    void command(u8 value);

    /// One frame: the wheel `counts` off the centre in the board's ADC counts
    /// (128 a side, positive up, i.e. right), over `ticks` of its interrupts,
    /// with `breakaway` the power its start-up calibration found.
    [[nodiscard]] Output step(int counts, double ticks, int breakaway);

    [[nodiscard]] bool forces_on() const { return m_enabled; }
    [[nodiscard]] u8   effect() const { return m_effect; }
    [[nodiscard]] bool oscillating() const { return m_oscillating; }

private:
    int  spring_power(int off, int dead_zone, int full_at, int base) const;
    void track_swings(int side, int ticks);

    bool m_enabled = false;  ///< $0x: forces on or off.
    u8   m_effect  = 0;      ///< The last $1x-$6x command.
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
