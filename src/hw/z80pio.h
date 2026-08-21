// SPDX-License-Identifier: BSD-3-Clause
//
// Zilog Z80 PIO (Z8420), two 8-bit parallel ports.
//
// Derived from MAME's src/devices/machine/z80pio.{h,cpp} (BSD-3-Clause,
// copyright-holder Curt Coder).
//
// Here for the TMPZ84C015 on Virtua Cop's I/O board, where the two ports read
// the board's dipswitch banks. What that firmware asks of the PIO was measured
// (tools/mame/iotap.lua): it puts both ports into bit-control mode with every
// line an input, writes an interrupt control word with the mask-follows bit and
// a zero mask, writes an interrupt vector, and then only ever reads the two data
// registers. Nothing drives a handshake and no interrupt is ever enabled.
//
// So this covers all four modes' data paths, because they are a few lines each,
// and models the interrupt logic only as far as "the enable is off, so nothing
// is requested". The ARDY/BRDY handshake lines and the bit-control match logic
// that would raise an interrupt are not modelled; anything reaching them says so.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

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

    /// The TMPZ84C015's "alternate" register layout, which is what its internal
    /// I/O map uses: bit 1 of the offset selects the port and bit 0 selects the
    /// control register over the data register.
    [[nodiscard]] u8 read_alt(u32 offset);
    void             write_alt(u32 offset, u8 value);

    /// Never asserted here, because the firmware leaves both enables clear. Kept
    /// so the daisy chain has one shape for every peripheral on it.
    [[nodiscard]] bool interrupt_pending() const { return false; }

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
