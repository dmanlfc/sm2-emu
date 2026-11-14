// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
#pragma once

#include "core/types.h"

namespace sm2::cpu::mb86233 {

/// The four address spaces of an MB86233/MB86234, as separate buses. Every
/// access is a 32-bit word and addresses count words; the part has no byte
/// addressing.
///
///   program   4096 words of writable microcode, which the host CPU uploads
///   data      two independent RAM banks, at 0x000 and 0x200
///   io        external memory and the mathematical lookup tables
///   rf        sixteen "register file" ports, which is where the host FIFOs
///             and the external memory bank register live
///
/// Implementations must mask each address to the width of their space, as
/// MAME's address spaces do; the core applies only the encoding's mask.
class Bus {
public:
    virtual ~Bus() = default;

    /// Instruction fetch, separate from read_program as the hottest access.
    [[nodiscard]] virtual u32 fetch(u16 address) = 0;

    /// Program memory read as data. Used by the table-lookup instructions.
    [[nodiscard]] virtual u32 read_program(u16 address) = 0;

    [[nodiscard]] virtual u32 read_data(u16 address) = 0;
    virtual void write_data(u16 address, u32 value) = 0;

    [[nodiscard]] virtual u32 read_io(u16 address) = 0;
    virtual void write_io(u16 address, u32 value) = 0;

    /// Register file port. Four bits of address on this part.
    [[nodiscard]] virtual u32 read_rf(u8 address) = 0;
    virtual void write_rf(u8 address, u32 value) = 0;
};

}  // namespace sm2::cpu::mb86233
