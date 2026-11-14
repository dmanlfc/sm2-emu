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
#include "hw/model3/scheduler.h"

#include "core/archive.h"

namespace sm2::hw::model3 {

void Scheduler::reset()
{
    m_slots.fill(Slot{});
    m_now = 0;
    m_seq = 0;
}

void Scheduler::schedule(u32 id, u64 when)
{
    if (id >= kMaxEvents) return;
    m_slots[id] = Slot{when, m_seq++, true};
}

void Scheduler::cancel(u32 id)
{
    if (id < kMaxEvents) m_slots[id].pending = false;
}

bool Scheduler::pending(u32 id) const
{
    return id < kMaxEvents && m_slots[id].pending;
}

u64 Scheduler::deadline(u32 id) const
{
    return pending(id) ? m_slots[id].when : kNever;
}

u64 Scheduler::next_deadline() const
{
    u64 best = kNever;
    for (const Slot& s : m_slots) {
        if (s.pending && s.when < best) best = s.when;
    }
    return best;
}

std::optional<Scheduler::Due> Scheduler::pop_due()
{
    std::optional<u32> best;
    for (u32 i = 0; i < kMaxEvents; ++i) {
        const Slot& s = m_slots[i];
        if (!s.pending || s.when > m_now) continue;
        if (!best || s.when < m_slots[*best].when
            || (s.when == m_slots[*best].when && s.seq < m_slots[*best].seq)) {
            best = i;
        }
    }
    if (!best) return std::nullopt;
    m_slots[*best].pending = false;
    return Due{*best, m_slots[*best].when};
}

void Scheduler::serialize(Archive& ar)
{
    ar.raw(m_now);
    ar.raw(m_seq);
    for (Slot& s : m_slots) {
        ar.raw(s.when);
        ar.raw(s.seq);
        ar.raw(s.pending);
    }
}

}  // namespace sm2::hw::model3
