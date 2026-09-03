// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/devices/machine/gen_fifo.cpp (BSD-3-Clause,
// Olivier Galibert).

#include "hw/copro_fifo.h"

#include <algorithm>

namespace sm2::hw {

void CoproFifo::configure(usize depth)
{
    clear();
    m_depth = depth;
}

void CoproFifo::clear()
{
    m_values.clear();
    m_overflow.clear();

    // A cleared FIFO is empty, so a destination waiting on it stays waiting; but
    // it is no longer full, so a halted source has to be released or it would
    // never run again.
    if (m_full_halted) {
        m_full_halted = false;
        if (m_on_unfull) {
            m_on_unfull();
        }
    }
    m_empty_halted = false;
}

u32 CoproFifo::pop()
{
    if (empty()) {
        if (m_on_empty_retry) {
            m_on_empty_retry();
        }
        if (!m_empty_halted) {
            m_empty_halted = true;
            if (m_on_empty_halt) {
                m_on_empty_halt();
            }
        }
        return 0;
    }

    const u32 value = m_values.front();
    m_values.pop_front();

    // Take one back from the overflow queue, and release the source once the
    // queue has drained.
    if (!m_overflow.empty()) {
        m_values.push_back(m_overflow.front());
        m_overflow.pop_front();

        if (m_overflow.empty() && m_full_halted) {
            m_full_halted = false;
            if (m_on_unfull) {
                m_on_unfull();
            }
        }
    }
    return value;
}

u32 CoproFifo::peek(usize offset) const
{
    if (offset < m_values.size()) {
        return m_values[offset];
    }
    offset -= m_values.size();
    return offset < m_overflow.size() ? m_overflow[offset] : 0;
}

void CoproFifo::push(u32 value)
{
    if (!m_overflow.empty() || full()) {
        m_overflow.push_back(value);
        m_peak_overflow = std::max(m_peak_overflow, m_overflow.size());

        if (!m_full_halted) {
            m_full_halted = true;
            if (m_on_full) {
                m_on_full();
            }
        }
        return;
    }

    m_values.push_back(value);

    if (m_empty_halted) {
        m_empty_halted = false;
        if (m_on_unempty) {
            m_on_unempty();
        }
    }
}

}  // namespace sm2::hw
