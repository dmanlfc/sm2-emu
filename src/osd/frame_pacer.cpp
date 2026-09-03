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
#include "osd/frame_pacer.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace sm2::osd {
namespace {

/// One second of nanoseconds, and the window the rate average is taken over.
constexpr u64 kNanosecondsPerSecond = 1'000'000'000ULL;

}  // namespace

void FramePacer::start(u64 period_ns)
{
    m_period_ns       = period_ns;
    m_next_ns         = SDL_GetTicksNS() + period_ns;
    m_started         = true;
    m_window_start_ns = SDL_GetTicksNS();
    m_window_frames   = 0;
    m_measured_hz     = 0.0;
}

void FramePacer::resync()
{
    if (!m_started) {
        return;
    }
    m_next_ns         = SDL_GetTicksNS() + m_period_ns;
    m_window_start_ns = SDL_GetTicksNS();
    m_window_frames   = 0;
}

void FramePacer::set_throttled(bool throttled)
{
    if (throttled && !m_throttled) {
        // Coming back from unthrottled, the deadline is far in the past. Starting
        // again from now avoids a burst of instant frames.
        resync();
    }
    m_throttled = throttled;
}

void FramePacer::set_sync_adjust(double fraction)
{
    m_sync_adjust = std::clamp(fraction, -kMaxSyncFraction, kMaxSyncFraction);
}

double FramePacer::target_hz() const
{
    if (m_period_ns == 0) {
        return 0.0;
    }
    return static_cast<double>(kNanosecondsPerSecond) / static_cast<double>(m_period_ns);
}

void FramePacer::account_for_frame()
{
    ++m_window_frames;
    const u64 now     = SDL_GetTicksNS();
    const u64 elapsed = now - m_window_start_ns;
    if (elapsed >= kNanosecondsPerSecond) {
        m_measured_hz = static_cast<double>(m_window_frames)
                      * static_cast<double>(kNanosecondsPerSecond)
                      / static_cast<double>(elapsed);
        m_window_start_ns = now;
        m_window_frames   = 0;
    }
}

u32 FramePacer::wait()
{
    if (!m_started || m_period_ns == 0) {
        return 0;
    }

    if (!m_throttled) {
        account_for_frame();
        return 0;
    }

    const u64 now = SDL_GetTicksNS();

    // Too far behind to make up. Write the debt off rather than running the game
    // fast to recover it.
    const u64 limit = m_next_ns + static_cast<u64>(kMaxLagFrames) * m_period_ns;
    if (now > limit) {
        const u32 written_off = static_cast<u32>((now - m_next_ns) / m_period_ns);
        m_next_ns             = now + m_period_ns;
        account_for_frame();
        return written_off;
    }

    if (now < m_next_ns) {
        // Precise rather than the plain delay: SDL sleeps for the bulk of the wait
        // and spins out the last fraction, which a millisecond-granularity sleep
        // cannot do and which at 17.384 ms a frame is the difference between the
        // right rate and a visibly wrong one.
        SDL_DelayPrecise(m_next_ns - now);
    }

    // Advanced by exactly one period, not recomputed from the clock, so the
    // fractional millisecond in the period cannot be rounded away frame after
    // frame. m_sync_adjust is a transient stretch and leaves m_period_ns intact.
    const u64 effective = m_period_ns
        + static_cast<s64>(static_cast<double>(m_period_ns) * m_sync_adjust);
    m_next_ns += effective;
    account_for_frame();
    return 0;
}

}  // namespace sm2::osd
