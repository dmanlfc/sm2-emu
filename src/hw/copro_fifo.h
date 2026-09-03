// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "core/types.h"

#include <deque>
#include <functional>

namespace sm2::hw {

/// A hardware FIFO with flow control, between two processors.
///
/// Derived from MAME's src/devices/machine/gen_fifo.{h,cpp} (BSD-3-Clause,
/// Olivier Galibert). The data behaviour is the same; the scheduling is not.
///
/// MAME resolves the two processors' relative progress with timers: a pop from an
/// empty FIFO schedules a machine-wide synchronisation, and only if the FIFO is
/// still empty afterwards is the destination halted. That is necessary when the
/// scheduler is free to run devices in any order. Here the caller drives both
/// processors itself and can run the other side on demand, so the synchronisation
/// step does not exist and the halt is applied straight away.
///
/// The consequence is that a processor may be halted slightly earlier than on
/// hardware. No value is ever lost and the order is unchanged, because an
/// overflowing push is queued rather than dropped, exactly as in MAME.
class CoproFifo {
public:
    /// Set the depth and clear. Handlers survive.
    void configure(usize depth);

    void clear();

    /// Remove the oldest value.
    ///
    /// On an empty FIFO this returns zero and asks the destination to try the
    /// instruction again, then halts it. Returning a value the hardware never
    /// produced would corrupt the geometry silently; re-executing is what the
    /// real part does.
    [[nodiscard]] u32 pop();

    /// Read the oldest value without removing it or disturbing flow control.
    [[nodiscard]] u32 peek(usize offset = 0) const;

    void push(u32 value);

    [[nodiscard]] bool empty() const { return m_values.empty(); }
    [[nodiscard]] bool full() const { return m_values.size() >= m_depth; }

    /// Values held, including any that overflowed the configured depth.
    [[nodiscard]] usize size() const { return m_values.size() + m_overflow.size(); }

    /// How far the FIFO has ever overflowed. A non-zero figure means the two
    /// processors are being interleaved too coarsely, not that anything is wrong.
    [[nodiscard]] usize peak_overflow() const { return m_peak_overflow; }

    // -- flow control ------------------------------------------------------

    /// Called on every pop from an empty FIFO: the destination must re-execute.
    void set_on_empty_retry(std::function<void()> handler)
    {
        m_on_empty_retry = std::move(handler);
    }

    /// Called once when the FIFO becomes empty on a pop: halt the destination.
    void set_on_empty_halt(std::function<void()> handler)
    {
        m_on_empty_halt = std::move(handler);
    }

    /// Called when a push makes a halted destination runnable again.
    void set_on_unempty(std::function<void()> handler)
    {
        m_on_unempty = std::move(handler);
    }

    /// Called once when a push overflows: halt the source.
    void set_on_full(std::function<void()> handler) { m_on_full = std::move(handler); }

    /// Called when a pop drains the overflow: the source can resume.
    void set_on_unfull(std::function<void()> handler) { m_on_unfull = std::move(handler); }

private:
    /// Values within the configured depth, oldest first. A deque gives O(1)
    /// front pop where a vector would pay a linear erase(begin()).
    std::deque<u32> m_values;

    /// Values pushed while full. Kept rather than dropped so no command is lost
    /// when the interleave lets the source get ahead.
    std::deque<u32> m_overflow;

    usize m_depth = 0;

    bool m_empty_halted = false;
    bool m_full_halted  = false;

    usize m_peak_overflow = 0;

    std::function<void()> m_on_empty_retry;
    std::function<void()> m_on_empty_halt;
    std::function<void()> m_on_unempty;
    std::function<void()> m_on_full;
    std::function<void()> m_on_unfull;
};

}  // namespace sm2::hw
