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
//
// The Model 3 deadline list, in PowerPC cycles. Event ids (not callbacks) so
// it serialises. Each id is pending at most once; ties fire in schedule order.
#pragma once

#include "core/types.h"

#include <array>
#include <optional>

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

class Scheduler {
public:
    static constexpr u32 kMaxEvents = 16;
    static constexpr u64 kNever     = ~u64{0};

    void reset();

    [[nodiscard]] u64 now() const { return m_now; }
    void advance(u64 cycles) { m_now += cycles; }

    /// Schedule (or move) `id` to fire at absolute cycle `when`.
    void schedule(u32 id, u64 when);
    void cancel(u32 id);
    [[nodiscard]] bool pending(u32 id) const;
    [[nodiscard]] u64 deadline(u32 id) const;

    /// The earliest pending deadline, or kNever.
    [[nodiscard]] u64 next_deadline() const;

    struct Due {
        u32 id   = 0;
        u64 when = 0;  ///< The cycle it was scheduled for (<= now()).
    };

    /// The earliest event due at or before now(), removed from the list.
    [[nodiscard]] std::optional<Due> pop_due();

    void serialize(Archive& ar);

private:
    struct Slot {
        u64  when    = 0;
        u64  seq     = 0;
        bool pending = false;
    };
    std::array<Slot, kMaxEvents> m_slots{};
    u64 m_now = 0;
    u64 m_seq = 0;
};

}  // namespace sm2::hw::model3
