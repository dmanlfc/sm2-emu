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
// The drive board of Indy 500, Touring Car, Over Rev and Super GT 24h, as its
// program (EPR-18261) behaves. The panel is direct drive, with no spring: the
// board drives the motor with two powers, one each way, and works out its
// springs from the wheel's position. The game picks an effect and its strength
// and sets the strength's base, a vibration and the wheel's centre. Read in the
// EPROM; see BackForceFeeder's docs/EPR-18261-Indy500-DriveBoard.
#pragma once

#include "core/types.h"

namespace sm2::osd {

class IndyBoard {
public:
    /// The board's NMI rate, which every time of it counts: the CTC divides
    /// the Z80's clock by 16000, and Supermodel runs that Z80 at 8 MHz.
    static constexpr double kNmiHz = 500.0;

    struct Output {
        /// 0..63 each, the motor's power "up" (the way that raises the wheel
        /// position: right) and "down". Both at once brake the motor.
        int up   = 0;
        int down = 0;
    };

    /// As at power-up: the centre at $80, base 2, effect stop, no vibration.
    void reset() { *this = IndyBoard{}; }

    /// A byte the game wrote to the board.
    void command(u8 value);

    /// One frame: the wheel at `position` on the board's ADC (0..255), over
    /// `nmis` of its interrupts.
    [[nodiscard]] Output step(int position, double nmis);

    [[nodiscard]] u8 effect() const { return m_effect; }
    [[nodiscard]] u8 centre() const { return m_centre; }
    [[nodiscard]] int base() const { return m_base; }

private:
    [[nodiscard]] int strength() const;
    void run_effect(int position);
    void spring(int position, int shift, bool damp_other_side_below);

    // Settings.
    u8  m_effect     = 0x10;  ///< The last command below $70.
    u8  m_centre     = 0x80;  ///< $9x its high nibble, $Cx its low.
    int m_base       = 2;     ///< $7x: x/2 + 2. 1 after an idle centring.
    int m_vib_period = 0;     ///< $Ax & 7: half period in 16 NMIs; 0 none.
    int m_vib_amp    = 0;     ///< $Bx & 7; 0 none.

    // The two powers the effect asks for, kept when it leaves them be.
    int m_up   = 0;
    int m_down = 0;

    // The damping: how far the wheel went down or up in the last 17 NMIs.
    int    m_speed_down = 0;
    int    m_speed_up   = 0;
    int    m_speed_ref  = 0x80;
    int    m_speed_nmis = 0;
    double m_nmi_acc    = 0.0;
    u32    m_nmis       = 0;  ///< Its NMI counter.
};

}  // namespace sm2::osd
