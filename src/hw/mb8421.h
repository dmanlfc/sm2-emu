// SPDX-License-Identifier: BSD-3-Clause
//
// Fujitsu MB8421 dual-port SRAM, 2K x 8.
//
// Derived from MAME's src/devices/machine/mb8421.{h,cpp} (BSD-3-Clause,
// copyright-holders hap, AJR), reduced to the one configuration a Model 2 board
// uses: the 2K x 8 part with byte-wide ports on both sides.
//
// This is how the original Model 2's i960 talks to its I/O board. The CRX boards
// replaced it with a 315-5649 wired straight onto the i960's bus, so no other
// board in this project needs one.
//
// What is deliberately not modelled: the _BUSY arbitration pins, which MAME also
// leaves out, and the two mailbox interrupt lines. The interrupts are real -- a
// write to the top address of one side raises an interrupt on the other -- but
// neither side of a Model 2 uses them: MAME's model2o machine config binds
// neither intl_callback nor intr_callback, and the I/O board's Z80 has no
// interrupt input wired at all. Both sides poll. The mailbox addresses are
// therefore plain RAM here, which is what makes this file short.
#pragma once

#include "core/types.h"

#include <array>
#include <span>

namespace sm2::hw {

/// 2048 bytes reachable from two independent byte-wide ports.
///
/// Both ports address the same storage with no arbitration, which is what the
/// real part does for every address pair except a simultaneous access to the
/// same cell. Nothing here can observe that case: the two CPUs are stepped in
/// turn rather than concurrently, so an access from one always completes before
/// the other begins.
class Mb8421 {
public:
    static constexpr u32 kSize = 0x800;

    /// Erase to zero, matching MAME's make_unique_clear.
    void reset();

    // -- left port, the I/O board's Z80 ------------------------------------

    [[nodiscard]] u8 left_read(u32 offset) const;
    void             left_write(u32 offset, u8 value);

    // -- right port, the i960 ----------------------------------------------

    [[nodiscard]] u8 right_read(u32 offset) const;
    void             right_write(u32 offset, u8 value);

    /// Contents, for diagnostics. MAME's peek().
    [[nodiscard]] std::span<const u8> bytes() const { return m_ram; }

private:
    static constexpr u32 kAddressMask = kSize - 1;

    std::array<u8, kSize> m_ram{};
};

}  // namespace sm2::hw
