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

#include "hw/ym3438.h"

#include "core/archive.h"

#include <algorithm>
#include <vector>

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

void Ym3438::serialize(Archive& ar)
{
    // The FM engine's state goes through ymfm's own save_restore, carried as a
    // length-prefixed byte blob so the layout is ymfm's to define.
    std::vector<u8> blob;
    if (ar.saving()) {
        ymfm::ymfm_saved_state ss(blob, /*saving=*/true);
        m_chip.save_restore(ss);
    }
    ar.vector_pod(blob);
    if (ar.loading() && !ar.failed()) {
        ymfm::ymfm_saved_state ss(blob, /*saving=*/false);
        m_chip.save_restore(ss);
    }

    ar.bytes(m_timer_deadline, 2);
    ar.bytes(m_timer_running, 2);
    ar.raw(m_samples_run);
    ar.raw(m_step);
    ar.raw(m_position);
    ar.bytes(m_prev, 2);
    ar.bytes(m_next, 2);
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
    // ymfm counts in input clocks; time here is in FM samples. A duration that
    // rounds to nothing still takes a sample, or a zero-period timer would fire
    // in a loop without time moving.
    const u64 samples     = static_cast<u64>(duration_in_clocks) / kClocksPerSample;
    m_timer_deadline[tnum] = m_samples_run + std::max<u64>(1, samples);
    m_timer_running[tnum]  = true;
}

void Ym3438::ymfm_update_irq(bool asserted)
{
    // Recorded, not delivered: the IRQ pin is unconnected on this board and the
    // sound driver polls the status register.
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
            // m_engine is ymfm_interface's back-pointer, set by the engine.
            m_engine->engine_timer_expired(tnum);
        }
    }
}

void Ym3438::generate(s32* accum, u32 frames, s32 gain_percent)
{
    for (u32 frame = 0; frame < frames; ++frame) {
        // Interpolated between the two chip samples either side of this output
        // frame. The output is already within 16 bits, so nothing to clamp.
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

        // Advance by one output frame (m_step / m_output_rate of a chip sample),
        // consuming one or two chip samples.
        m_position += m_step;
        while (m_position >= m_output_rate) {
            m_position -= m_output_rate;
            run_one_sample();
        }
    }
}

}  // namespace sm2::hw
