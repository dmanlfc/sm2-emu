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
//
#pragma once

#include "core/types.h"

namespace sm2::osd {

/// Holds the emulator to the machine's own frame rate (57.5245 Hz on Model 2)
/// against real time rather than the display refresh. Every emulated frame is
/// presented exactly once, so some are held for two refreshes at 60 Hz.
///
/// The deadline advances by exactly one period each frame rather than being
/// recomputed from the current time, so rounding cannot accumulate.
class FramePacer {
public:
    /// Frames of lateness tolerated before the debt is written off, rather than
    /// sprinting through the missed time after a stall.
    static constexpr u32 kMaxLagFrames = 4;

    /// Begin pacing at `period_ns` per frame. Also resets the rate measurement.
    void start(u64 period_ns);

    /// Sleep until the next frame is due.
    ///
    /// Returns how many whole frame periods were written off for falling too far
    /// behind. Returns immediately when unthrottled.
    u32 wait();

    /// Abandon the current deadline and start again from now. Call after anything
    /// that stopped the loop for a while, such as a pause.
    void resync();

    /// Whether to pace at all; off runs as fast as the host can.
    void set_throttled(bool throttled);
    [[nodiscard]] bool throttled() const { return m_throttled; }

    /// Stretch the frame period by a small fraction to lock the emulator to the
    /// audio device's rate (audio clock pull). Clamped here, and a no-op while
    /// unthrottled.
    void set_sync_adjust(double fraction);

    /// Frames per second achieved over the whole loop, averaged over the last
    /// second, or zero before the first full second.
    [[nodiscard]] double measured_hz() const { return m_measured_hz; }

    /// Rate the machine is being asked to run at, for the same comparison.
    [[nodiscard]] double target_hz() const;

private:
    void account_for_frame();

    /// Sync nudge clamp: 0.5 %, well below an audible pitch shift.
    static constexpr double kMaxSyncFraction = 0.005;

    u64    m_period_ns    = 0;
    u64    m_next_ns      = 0;
    double m_sync_adjust  = 0.0;
    bool   m_throttled    = true;
    bool   m_started      = false;

    u64    m_window_start_ns = 0;
    u32    m_window_frames   = 0;
    double m_measured_hz     = 0.0;
};

}  // namespace sm2::osd
