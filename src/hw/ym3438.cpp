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

#include "hw/ym3438.h"

#include <algorithm>

namespace sm2::hw {

Ym3438::Ym3438(u32 clock, u32 output_rate)
    : m_chip(*this)
    , m_clock(clock)
    , m_output_rate(output_rate)
{
    m_step = native_rate();
}

void Ym3438::reset()
{
    m_timer_running[0] = false;
    m_timer_running[1] = false;
    m_timer_deadline[0] = 0;
    m_timer_deadline[1] = 0;
    m_samples_run       = 0;
    m_position          = 0;
    m_prev[0] = m_prev[1] = 0;
    m_next[0] = m_next[1] = 0;
    m_stats               = Stats{};

    m_chip.reset();
}

u8 Ym3438::read(u32 offset)
{
    return m_chip.read(offset);
}

void Ym3438::write(u32 offset, u8 value)
{
    m_chip.write(offset, value);
}

// ---------------------------------------------------------------------------
// ymfm::ymfm_interface
// ---------------------------------------------------------------------------

void Ym3438::ymfm_set_timer(u32 tnum, s32 duration_in_clocks)
{
    if (tnum > 1) {
        return;
    }
    if (duration_in_clocks < 0) {
        m_timer_running[tnum] = false;
        return;
    }
    // ymfm counts in input clocks; time here is counted in FM samples, which is
    // the granularity the chip actually advances at. A duration that rounds to
    // nothing still has to take a sample, or a timer with a zero period would
    // fire in a loop without time moving.
    const u64 samples     = static_cast<u64>(duration_in_clocks) / kClocksPerSample;
    m_timer_deadline[tnum] = m_samples_run + std::max<u64>(1, samples);
    m_timer_running[tnum]  = true;
}

void Ym3438::ymfm_update_irq(bool asserted)
{
    // Recorded, not delivered. MAME's segam1audio binds no irq_handler for this
    // chip, so its IRQ pin goes nowhere on this board and the sound driver polls
    // the status register instead.
    ++m_stats.irq_changes;
    m_stats.irq_asserted = asserted;
}

// ---------------------------------------------------------------------------
// Time and output
// ---------------------------------------------------------------------------

void Ym3438::run_one_sample()
{
    ymfm::ym3438::output_data out{};
    m_chip.generate(&out, 1);

    m_prev[0] = m_next[0];
    m_prev[1] = m_next[1];
    m_next[0] = out.data[0];
    m_next[1] = out.data[1];

    ++m_samples_run;
    ++m_stats.samples;

    // Checked after the sample rather than before, so a deadline set during this
    // sample's own register writes is not mistaken for one that has already
    // passed. engine_timer_expired re-arms through ymfm_set_timer.
    for (u32 tnum = 0; tnum < 2; ++tnum) {
        if (m_timer_running[tnum] && m_samples_run >= m_timer_deadline[tnum]) {
            if (tnum == 0) {
                ++m_stats.timer_a_expiries;
            } else {
                ++m_stats.timer_b_expiries;
            }
            m_timer_running[tnum] = false;
            // m_engine is ymfm_interface's own back-pointer, set by the engine at
            // construction. Same route MAME's wrapper takes from its timer
            // callback.
            m_engine->engine_timer_expired(tnum);
        }
    }
}

void Ym3438::generate(s32* accum, u32 frames, s32 gain_percent)
{
    for (u32 frame = 0; frame < frames; ++frame) {
        // Interpolated between the two chip samples the current output frame
        // falls between. The chip's own output is already inside 16 bits, so
        // unlike the MultiPCMs there is nothing to clamp before the gain.
        const s32 left =
            m_prev[0]
            + static_cast<s32>((static_cast<s64>(m_next[0] - m_prev[0]) * m_position)
                               / m_output_rate);
        const s32 right =
            m_prev[1]
            + static_cast<s32>((static_cast<s64>(m_next[1] - m_prev[1]) * m_position)
                               / m_output_rate);

        accum[frame * 2 + 0] += left * gain_percent / 100;
        accum[frame * 2 + 1] += right * gain_percent / 100;

        m_stats.peak_output =
            std::max({m_stats.peak_output, std::abs(left), std::abs(right)});

        // Advance by one output frame, which is m_step / m_output_rate of a chip
        // sample. The chip's rate is the higher of the two, so this consumes one
        // or two samples per frame.
        m_position += m_step;
        while (m_position >= m_output_rate) {
            m_position -= m_output_rate;
            run_one_sample();
        }
    }
}

}  // namespace sm2::hw
