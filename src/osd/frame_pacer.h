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
#pragma once

#include "core/types.h"

namespace sm2::osd {

/// Holds the emulator to the machine's own frame rate.
///
/// Model 2 runs at 57.5245 Hz. A monitor runs at 60, or 120, or whatever the user
/// set, and none of those is a multiple of it. Presenting one emulated frame per
/// display refresh therefore runs the game four percent fast at 60 Hz: fights end
/// early, music would play sharp, and anyone who knows the game notices.
///
/// So the machine is paced against real time instead, and every emulated frame is
/// presented exactly once: none is duplicated and none is dropped, so no input is
/// ever lost. On a 60 Hz display that means a frame is occasionally held for two
/// refreshes, which is unavoidable at this rate without inventing frames, and is
/// what the hardware would have looked like on a monitor it was not built for.
///
/// The deadline advances by exactly one period each frame rather than being
/// recomputed from the current time, so rounding cannot accumulate and the long-run
/// rate is exact.
class FramePacer {
public:
    /// Frames of lateness tolerated before the debt is written off.
    ///
    /// A window drag, a shader compile or a disc stall can leave the emulator far
    /// behind. Catching up by running frames flat out would then sprint the game
    /// through whatever the player missed, which is worse than losing that time.
    static constexpr u32 kMaxLagFrames = 4;

    /// Begin pacing at `period_ns` per frame. Also resets the rate measurement.
    void start(u64 period_ns);

    /// Sleep until the next frame is due.
    ///
    /// Returns how many whole frame periods were written off because the emulator
    /// had fallen too far behind, which is zero unless something stalled. Returns
    /// immediately when unthrottled.
    u32 wait();

    /// Abandon the current deadline and start again from now. Call after anything
    /// that stopped the loop for a while, such as a pause.
    void resync();

    /// Whether to pace at all. Turning it off runs as fast as the host can, which
    /// is what a capture or a fast-forward key wants.
    void set_throttled(bool throttled);
    [[nodiscard]] bool throttled() const { return m_throttled; }

    /// Frames per second actually achieved, averaged over the last second, or zero
    /// before the first full second. Measures the whole loop, so it reports what
    /// the player is seeing rather than what was asked for.
    [[nodiscard]] double measured_hz() const { return m_measured_hz; }

    /// Rate the machine is being asked to run at, for the same comparison.
    [[nodiscard]] double target_hz() const;

private:
    void account_for_frame();

    u64  m_period_ns = 0;
    u64  m_next_ns   = 0;
    bool m_throttled = true;
    bool m_started   = false;

    u64    m_window_start_ns = 0;
    u32    m_window_frames   = 0;
    double m_measured_hz     = 0.0;
};

}  // namespace sm2::osd
