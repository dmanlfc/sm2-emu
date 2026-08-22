//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
//
// Yamaha YM3438 (OPN2C), the FM half of the Model 1 audio board.
//
// The synthesis is ymfm's, the same library MAME uses -- see cmake/Dependencies
// for why. This is the host side of ymfm's interface: the two timers, the busy
// flag, the IRQ pin, and the resampler that gets the chip's 55556 Hz output onto
// the board's 44643 Hz bus.
//
// The timers are the reason this chip cannot be left out. The board's sound
// driver runs its sequencer off Timer B's overflow flag, polling the status
// register for it, and MAME binds no IRQ handler for this chip so polling is the
// only route. With the status register stubbed to zero the driver initialises both
// MultiPCMs, waits for a tick that never comes and never keys a voice on. That
// was measured both ways: stock MAME keys on 35 voices in Daytona's first ten
// seconds, and MAME with YM reads forced to zero keys on none.

#pragma once

#include "core/types.h"

#include <ymfm_opn.h>

namespace sm2::hw {

class Ym3438 final : private ymfm::ymfm_interface {
public:
    /// `clock` is the chip's own crystal -- 8 MHz on this board, MAME's
    /// 16_MHz_XTAL / 2 -- and `output_rate` the rate the board's bus runs at.
    Ym3438(u32 clock, u32 output_rate);

    Ym3438(const Ym3438&)            = delete;
    Ym3438& operator=(const Ym3438&) = delete;

    void reset();

    [[nodiscard]] u8 read(u32 offset);
    void write(u32 offset, u8 value);

    /// Add `frames` interleaved stereo frames into `accum`, two s32 per frame,
    /// at `gain_percent` of full scale.
    ///
    /// This is where time passes for the chip: the timers are counted in output
    /// samples, so it has to be called whether or not anyone is listening.
    void generate(s32* accum, u32 frames, s32 gain_percent);

    /// The chip's native output rate, clock / 144. Not the rate generate()
    /// produces -- that is the output_rate given to the constructor.
    [[nodiscard]] u32 native_rate() const { return m_clock / kClocksPerSample; }

    struct Stats {
        u64 samples          = 0;
        u64 timer_a_expiries = 0;
        u64 timer_b_expiries = 0;
        u64 irq_changes      = 0;
        s32 peak_output      = 0;
        bool irq_asserted    = false;
    };
    [[nodiscard]] const Stats& stats() const { return m_stats; }

private:
    /// One FM sample is 24 operator slots at a prescale of 6. ymfm hands timer
    /// durations over in input clocks, and always as a multiple of this, so the
    /// conversion to samples is exact.
    static constexpr u64 kClocksPerSample = 24 * 6;

    // -- ymfm::ymfm_interface --------------------------------------------------
    //
    // ymfm_sync_mode_write and ymfm_sync_check_interrupts are deliberately not
    // overridden. MAME defers both onto its scheduler so that a CPU mid-write
    // does not reorder against the sound stream; there is no scheduler here and
    // the base class's default is to call the engine straight away, which is what
    // is wanted.
    //
    // ymfm_set_busy_end is not overridden either: ym2612 and ym3438 never arm the
    // busy flag -- only the older OPN parts do -- so the default "never busy" is
    // the chip's real behaviour rather than a simplification.

    void ymfm_set_timer(u32 tnum, s32 duration_in_clocks) override;
    void ymfm_update_irq(bool asserted) override;

    void run_one_sample();

    ymfm::ym3438 m_chip;

    u32 m_clock       = 0;
    u32 m_output_rate = 0;

    /// Deadlines in FM samples since reset, or zero when the timer is stopped.
    u64  m_timer_deadline[2] = {0, 0};
    bool m_timer_running[2]  = {false, false};
    u64  m_samples_run       = 0;

    // Linear resampler from the chip's rate to the board's. The two rates are
    // exact ratios of their crystals, so the position is kept as a fraction with
    // m_step over m_output_rate rather than in floating point.
    u32 m_step        = 0;  ///< Numerator: chip samples per output frame.
    u32 m_position    = 0;  ///< Fraction between m_prev and m_next, / m_output_rate.
    s32 m_prev[2]     = {0, 0};
    s32 m_next[2]     = {0, 0};

    Stats m_stats;
};

}  // namespace sm2::hw
