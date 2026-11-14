// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Fujitsu MB8421 dual-port SRAM, 2K x 8.
//
// Derived from MAME's src/devices/machine/mb8421.{h,cpp} (BSD-3-Clause,
// copyright-holders hap, AJR), reduced to the one configuration a Model 2 board
// uses: the 2K x 8 part with byte-wide ports on both sides.
//
// The original Model 2's i960 talks to its I/O board through this; the CRX
// boards use a 315-5649 instead.
//
// Not modelled: the _BUSY arbitration pins (MAME omits them too) and the two
// mailbox interrupts, which Model 2 leaves unwired. Both sides poll, so the
// mailbox addresses are plain RAM.
#pragma once

#include "core/types.h"

#include <array>
#include <span>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// 2048 bytes reachable from two independent byte-wide ports, with no
/// arbitration: the two CPUs are stepped in turn, so accesses never collide.
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

    /// Save/restore the 2 KB shared RAM.
    void serialize(Archive& ar);

private:
    static constexpr u32 kAddressMask = kSize - 1;

    std::array<u8, kSize> m_ram{};
};

}  // namespace sm2::hw
