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
// The EPR-18261 drive board (Indy 500, Touring Car, Over Rev, Super GT 24h).
// A direct-drive panel with no spring: the board works out two motor powers,
// one each way, from the game's effect and the wheel's position.
#pragma once

#include "core/types.h"
#include "hw/indy_drive_reply.h"

namespace sm2 {
class Archive;
}

namespace sm2::osd {

class IndyBoard {
public:
    /// The NMI rate: the CTC divides the Z80's 8 MHz clock by 16000.
    static constexpr double kNmiHz = 500.0;

    struct Output {
        /// Each 0..63. Up raises the wheel position (to the right); both at
        /// once brake the motor.
        int up   = 0;
        int down = 0;
    };

    /// As at power-up: the centre at $80, base 2, effect stop, no vibration.
    void reset()
    {
        const int dip = m_dip_power;
        *this         = IndyBoard{};
        m_dip_power   = dip;
    }

    /// The operator's strength, DIP switches 1-1..1-3 ON counting 0 to 7, added
    /// to every strength.
    void set_dip(int switches) { m_dip_power = switches; }

    /// A byte the game wrote to the board.
    void command(u8 value);

    /// Advances one frame, with the wheel at `position` (0..255), over `nmis`
    /// interrupts.
    [[nodiscard]] Output step(int position, double nmis);

    /// Saves or restores the board's state, for save states.
    void serialize(Archive& ar);

    [[nodiscard]] u8 effect() const { return m_effect; }
    [[nodiscard]] u8 centre() const { return hw::indy_board_centre(m_centre); }
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
    int m_dip_power  = 0;     ///< DIP switches 1-1..1-3, 0..7.

    // The two powers the effect asks for, kept when it leaves them be.
    int m_up   = 0;
    int m_down = 0;

    // Damping: how far the wheel moved down or up during the last kSpeedNmis
    // NMIs.
    int    m_speed_down = 0;
    int    m_speed_up   = 0;
    int    m_speed_ref  = 0x80;
    int    m_speed_nmis = 0;
    double m_nmi_acc    = 0.0;
    u32    m_nmis       = 0;  ///< Its NMI counter.
};

}  // namespace sm2::osd
