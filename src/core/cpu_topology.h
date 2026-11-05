//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Which cores are the fast ones, on machines where it matters.
#pragma once

#include "core/types.h"

#include <vector>

namespace sm2::core {

/// Cores split by the kernel's cpu_capacity: below 60% of the fastest is
/// "slow". Homogeneous or unknown means every core is fast. `fast` lists the
/// fastest first.
struct CpuTopology {
    std::vector<int> fast;
    std::vector<int> slow;

    [[nodiscard]] static CpuTopology detect();
};

/// Pin the calling thread to `cpus`; a no-op where unsupported or refused.
void pin_current_thread(const std::vector<int>& cpus);

}  // namespace sm2::core
