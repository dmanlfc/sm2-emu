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

#include <utility>

// The interface a CPU core uses to reach the rest of the machine.
//
// This replaces MAME's address_space. The ported cores were written against an
// object offering read_byte/read_word/read_dword plus flag-returning variants,
// so the shape here deliberately mirrors that: it keeps the ported instruction
// implementations textually close to upstream, which is what makes future MAME
// fixes diffable.

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
///
/// Virtual dispatch costs a few nanoseconds per access. At the 25 MHz the real
/// i960KB runs, and with the machine's own dispatch being a table lookup
/// underneath, that is not close to being the limiting factor; measured before
/// choosing anything more intrusive.
class Bus {
public:
    virtual ~Bus() = default;

    virtual u8  read8(u32 address)  = 0;
    virtual u16 read16(u32 address) = 0;
    virtual u32 read32(u32 address) = 0;

    virtual void write8(u32 address, u8 value)   = 0;
    virtual void write16(u32 address, u16 value) = 0;
    virtual void write32(u32 address, u32 value) = 0;

    /// Reads that also report the accessed region's flags.
    ///
    /// Default implementations ignore flags, so a bus that has no burst regions
    /// need not implement them.
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
};

}  // namespace sm2::cpu
