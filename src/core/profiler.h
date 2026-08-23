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

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// Per-stage CPU timing for the phase 8 benchmark (design.md, requirement 1).
//
// Deliberately not a general-purpose profiling library: it exists to answer one
// question, "which stage does a frame's CPU time go to", over a fixed set of
// named stages, and to report p50/p95/p99 rather than a running mean. There is
// no sampling or instrumentation framework here on purpose -- std::chrono around
// a call site is the whole mechanism, because that is all a handful of named
// stages, measured for a few thousand frames, need.

namespace sm2::core {

/// One named stage's collected samples, in milliseconds.
class StageTimer {
public:
    explicit StageTimer(std::string name) : m_name(std::move(name)) {}

    /// RAII scope: construct at the top of the stage, let it go out of scope at
    /// the bottom. Matches how the call sites already read (a block per stage),
    /// rather than a begin()/end() pair that can be mismatched.
    ///
    /// Default-constructible and movable, deliberately, so a call site can build
    /// one conditionally (see maybe_scope() below) and still hold it by value
    /// rather than needing std::optional, which would require Scope to be
    /// movable anyway once returned from a factory function.
    class Scope {
    public:
        Scope() = default;
        explicit Scope(StageTimer& owner)
            : m_owner(&owner), m_start(std::chrono::steady_clock::now())
        {
        }
        ~Scope()
        {
            if (m_owner != nullptr) {
                const auto elapsed = std::chrono::steady_clock::now() - m_start;
                m_owner->m_samples_ms.push_back(
                    std::chrono::duration<double, std::milli>(elapsed).count());
            }
        }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept : m_owner(other.m_owner), m_start(other.m_start)
        {
            other.m_owner = nullptr;
        }
        Scope& operator=(Scope&&) = delete;

    private:
        StageTimer*                          m_owner = nullptr;
        std::chrono::steady_clock::time_point m_start{};
    };

    [[nodiscard]] Scope scope() { return Scope(*this); }

    /// Record an already-measured duration directly, for a figure computed
    /// elsewhere (a GPU timestamp delta, or a hardware stage's own
    /// last_run_nanoseconds()-style accessor) rather than timed around a call
    /// made here.
    void record_ms(double milliseconds) { m_samples_ms.push_back(milliseconds); }

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::vector<double>& samples_ms() const { return m_samples_ms; }

    void reserve(usize count) { m_samples_ms.reserve(count); }
    void clear() { m_samples_ms.clear(); }

private:
    std::string         m_name;
    std::vector<double> m_samples_ms;
};

/// Scope(timer) if `enabled`, otherwise an inert Scope that records nothing.
/// Lets a call site write one line unconditionally rather than branching on
/// whether profiling is active.
[[nodiscard]] inline StageTimer::Scope maybe_scope(StageTimer& timer, bool enabled)
{
    return enabled ? StageTimer::Scope(timer) : StageTimer::Scope();
}

/// p50/p95/p99/mean over a stage's collected samples. Nearest-rank on a sorted
/// copy, matching the same convention main.cpp's frame-rate summary uses, so the
/// two reports read the same way.
struct StageStats {
    std::string name;
    double      mean_ms = 0.0;
    double      p50_ms  = 0.0;
    double      p95_ms  = 0.0;
    double      p99_ms  = 0.0;
    usize       samples = 0;
};

[[nodiscard]] inline double percentile_ms(std::vector<double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const usize rank =
        static_cast<usize>(fraction * static_cast<double>(values.size() - 1));
    return values[rank];
}

[[nodiscard]] inline StageStats summarise(const StageTimer& timer)
{
    StageStats stats;
    stats.name    = timer.name();
    stats.samples = timer.samples_ms().size();
    if (stats.samples == 0) {
        return stats;
    }
    double sum = 0.0;
    for (const double ms : timer.samples_ms()) {
        sum += ms;
    }
    stats.mean_ms = sum / static_cast<double>(stats.samples);
    stats.p50_ms  = percentile_ms(timer.samples_ms(), 0.50);
    stats.p95_ms  = percentile_ms(timer.samples_ms(), 0.95);
    stats.p99_ms  = percentile_ms(timer.samples_ms(), 0.99);
    return stats;
}

}  // namespace sm2::core
