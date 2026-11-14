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

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// Per-stage CPU timing over a fixed set of named stages, reporting p50/p95/p99.

namespace sm2::core {

/// One named stage's collected samples, in milliseconds.
class StageTimer {
public:
    explicit StageTimer(std::string name) : m_name(std::move(name)) {}

    /// RAII scope over a stage. Default-constructible and movable so maybe_scope()
    /// can return an inert one.
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

    /// Record an already-measured duration, e.g. a GPU timestamp delta.
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
[[nodiscard]] inline StageTimer::Scope maybe_scope(StageTimer& timer, bool enabled)
{
    return enabled ? StageTimer::Scope(timer) : StageTimer::Scope();
}

/// p50/p95/p99/mean over a stage's samples. Nearest-rank, matching main.cpp's
/// frame-rate summary.
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
