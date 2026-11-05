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
//
// Main-CPU wait states per memory area.
//
// The game programs a wait state for each area through the CPU-control
// registers at 0x00E00000 (Sega's "Area Wait Time and Memory Type Setting"
// table): one word per area, 01/02/04/08/10 for 0-4 waits, 20 for a device that
// generates its own ready, and bit 6 for DRAM. Data accesses pay the area's
// waits, except in DRAM; instruction fetches do not, as the i960KB's cache
// covers most of them.

#pragma once

#include "core/types.h"

#include <span>

namespace sm2::hw {

/// CPU cycles per programmed wait state, fitted to hardware captures: attract
/// scenes that load from data ROM (Dynamite Cop, Motor Raid) then keep pace
/// with the real board.
constexpr s32 kCyclesPerWait = 3;

/// Extra cycles one data access to address costs, given the 0x38-byte register
/// block.
[[nodiscard]] inline s32 area_wait_cycles(u32 address, std::span<const u8> regs)
{
    int offset;
    if (address < 0x00200000) {
        offset = 0x00;  // program ROM
    } else if (address >= 0x00500000 && address < 0x00600000) {
        offset = 0x08;  // work RAM
    } else if (address >= 0x00900000 && address < 0x00980000) {
        offset = 0x04;  // download (buffer) RAM
    } else if (address >= 0x01800000 && address < 0x0181c000) {
        offset = 0x1c;  // colour
    } else if (address >= 0x01a00000 && address < 0x01a20000) {
        offset = 0x20;  // communication
    } else if (address >= 0x01c00000 && address < 0x01c80000) {
        offset = 0x24;  // I/O
    } else if (address >= 0x01c80000 && address < 0x01d00000) {
        offset = 0x28;  // sound
    } else if (address >= 0x01d00000 && address < 0x01d80000) {
        offset = 0x2c;  // backup
    } else if (address >= 0x01d80000 && address < 0x01e00000) {
        offset = 0x30;  // security
    } else if ((address >= 0x02000000 && address < 0x04000000)
               || (address >= 0x06000000 && address < 0x07000000)) {
        offset = 0x34;  // data ROM
    } else {
        return 0;  // external devices and zero-wait control
    }
    if (static_cast<usize>(offset) >= regs.size()) {
        return 0;
    }
    const u8 setting = regs[static_cast<usize>(offset)];
    if ((setting & 0x40) != 0) {
        return 0;  // DRAM: page-mode hits dominate, so the wait rarely applies
    }
    switch (setting & 0x3f) {
        case 0x02: return 1 * kCyclesPerWait;
        case 0x04: return 2 * kCyclesPerWait;
        case 0x08: return 3 * kCyclesPerWait;
        case 0x10: return 4 * kCyclesPerWait;
        default:   return 0;  // 0 wait, external ready, or unprogrammed
    }
}

}  // namespace sm2::hw
