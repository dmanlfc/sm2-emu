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

#include <deque>
#include <vector>

namespace sm2::hw {

class Machine;

/// Recent machine states, for stepping back through play.
///
/// The newest state is kept whole. Each older one is stored as the bytes that
/// differ from the state after it, since most of the machine does not change
/// between snapshots.
class Rewind {
public:
    /// Keep at most `budget_bytes` of history, one snapshot every
    /// `interval_frames` frames. Drops the history when either changes.
    void configure(usize budget_bytes, u32 interval_frames);

    void clear();

    /// Call after every frame the machine runs forward.
    void capture(const Machine& machine);

    /// Restore the newest snapshot and make the one before it the newest. The
    /// oldest is kept, so holding rewind stops there. False when there is
    /// nothing to restore.
    bool step_back(Machine& machine);

    [[nodiscard]] usize snapshots() const { return m_current.empty() ? 0 : m_deltas.size() + 1; }
    [[nodiscard]] usize bytes_used() const { return m_current.size() + m_delta_bytes; }

private:
    usize m_budget   = 0;
    u32   m_interval = 1;
    u32   m_counter  = 0;

    std::vector<u8>             m_current;
    std::deque<std::vector<u8>> m_deltas;  ///< back() turns m_current into the snapshot before it
    usize                       m_delta_bytes = 0;
    std::vector<u8>             m_scratch;
};

/// What turns `newer` back into `older`: the size of `older`, then
/// (offset, length, bytes) runs where the two differ. Exposed for tests.
[[nodiscard]] std::vector<u8> rewind_delta(const std::vector<u8>& newer,
                                           const std::vector<u8>& older);

/// Apply a rewind_delta to `state` in place.
void apply_rewind_delta(std::vector<u8>& state, const std::vector<u8>& delta);

}  // namespace sm2::hw
