// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Zilog Z80 PIO (Z8420), two 8-bit parallel ports.
//
// Derived from MAME's src/devices/machine/z80pio.{h,cpp} (BSD-3-Clause,
// copyright-holder Curt Coder).
//
// Here for the TMPZ84C015 on Virtua Cop's I/O board, whose firmware sets both
// ports to bit-control inputs, leaves interrupts disabled and only reads the
// dipswitch banks. All four modes' data paths are covered; the ARDY/BRDY
// handshake and bit-control interrupt logic are not modelled, and anything
// reaching them says so.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class Z80Pio {
public:
    static constexpr u32 kPorts = 2;

    using InputHandler  = std::function<u8()>;
    using OutputHandler = std::function<void(u8)>;

    enum Mode : u8 {
        kModeOutput        = 0,
        kModeInput         = 1,
        kModeBidirectional = 2,
        kModeBitControl    = 3,
    };

    void reset();

    void set_input(u32 port, InputHandler handler)
    {
        if (port < kPorts) m_port[port].input_cb = std::move(handler);
    }
    void set_output(u32 port, OutputHandler handler)
    {
        if (port < kPorts) m_port[port].output_cb = std::move(handler);
    }

    /// The TMPZ84C015's "alternate" register layout: offset bit 1 selects the
    /// port, bit 0 the control register over the data register.
    [[nodiscard]] u8 read_alt(u32 offset);
    void             write_alt(u32 offset, u8 value);

    /// Never asserted: the firmware leaves both enables clear.
    [[nodiscard]] bool interrupt_pending() const { return false; }

    /// Save/restore both ports' register state. The port callbacks are excluded.
    void serialize(Archive& ar);

private:
    enum class NextControl : u8 { Any, DataDirection, Mask };

    struct Port {
        u8 mode   = kModeInput;
        u8 input  = 0;
        u8 output = 0;
        u8 ior    = 0xff;  ///< Bit-control direction mask; a set bit is an input.
        u8 icw    = 0;
        u8 mask   = 0;
        u8 vector = 0;
        NextControl next = NextControl::Any;

        InputHandler  input_cb;
        OutputHandler output_cb;
    };

    [[nodiscard]] u8 data_read(u32 index);
    void             data_write(u32 index, u8 value);
    void             control_write(u32 index, u8 value);
    [[nodiscard]] u8 control_read() const;

    std::array<Port, kPorts> m_port{};
};

}  // namespace sm2::hw
