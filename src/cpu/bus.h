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

#include <utility>

// The interface a CPU core uses to reach the rest of the machine. It stands in
// for MAME's address_space and mirrors its shape, so ported instruction code
// stays textually close to upstream.

namespace sm2::cpu {

/// Per-access flags a bus device can report back.
///
/// Model 2 wires the geometry coprocessor's output FIFO into the i960's bus and
/// relies on burst accesses: reading an empty FIFO stalls the CPU mid-burst, and
/// the instruction is re-executed when data arrives. The bus therefore has to
/// tell the core which accesses were burst accesses.
enum : u16 {
    kBusFlagNone  = 0x0000,
    kBusFlagBurst = 0x0001,  ///< MAME's i960_cpu_device::BURST.
};

/// Abstract memory bus.
class Bus {
public:
    virtual ~Bus() = default;

    virtual u8  read8(u32 address)  = 0;
    virtual u16 read16(u32 address) = 0;
    virtual u32 read32(u32 address) = 0;

    virtual void write8(u32 address, u8 value)   = 0;
    virtual void write16(u32 address, u16 value) = 0;
    virtual void write32(u32 address, u32 value) = 0;

    /// Accesses that also report the region's flags. The defaults report none,
    /// so a bus without burst regions need not override them.
    virtual std::pair<u8, u16> read8_flags(u32 address)
    {
        return {read8(address), kBusFlagNone};
    }
    virtual std::pair<u32, u16> read32_flags(u32 address)
    {
        return {read32(address), kBusFlagNone};
    }

    virtual u16 write8_flags(u32 address, u8 value)
    {
        write8(address, value);
        return kBusFlagNone;
    }
    virtual u16 write32_flags(u32 address, u32 value)
    {
        write32(address, value);
        return kBusFlagNone;
    }

    /// Instruction fetch. Separate from read32 so a machine can serve code from
    /// a faster path, and so tracing can distinguish fetches from data.
    virtual u32 fetch32(u32 address) { return read32(address); }

    /// True if reading `address` has no side effect and returns the same
    /// value until the CPU's current run() returns.
    virtual bool read_stable(u32 address) const
    {
        (void)address;
        return false;
    }
};

}  // namespace sm2::cpu
