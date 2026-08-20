// SPDX-License-Identifier: BSD-3-Clause
//
// OKI MSM6253 8-bit 4-channel A/D converter.
//
// Derived from MAME's src/devices/machine/msm6253.{h,cpp} (BSD-3-Clause,
// copyright-holders AJR).
//
// The part sits on the Model 1 I/O board, where the Z80 reads the cabinet's
// analogue controls through it. Its data path is serial, not parallel: there is
// one data pin, and a conversion is retrieved one bit at a time, most significant
// first. That is the whole reason this is a device rather than a table lookup --
// a read does not return a channel's value, it returns one bit of it and
// destroys that bit, so eight reads make one sample and a program that reads
// nine times gets a zero.
//
// What is deliberately not modelled: the D7 output pin (MAME's d7_r), which no
// Model 2 board wires, the conversion time, and the select_w addressing variant
// used by boards that latch the channel from the data bus instead of the address
// bus. The Model 1 I/O board uses address_w.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2::hw {

class Msm6253 {
public:
    static constexpr u32 kChannelCount = 4;

    /// An analogue channel's current voltage, quantised to eight bits.
    using InputHandler = std::function<u8()>;

    void reset();

    void set_input(u32 channel, InputHandler handler);

    /// Start a conversion. The channel comes from the low two bits of the
    /// address, and the result is latched into the shift register immediately:
    /// MAME does not model the conversion delay, and neither does the hardware
    /// well enough for anything to notice, because the Z80 spends more than a
    /// conversion time getting to its first read.
    void address_write(u32 offset);

    /// Read the next bit of the latched conversion into D0.
    ///
    /// The other seven bits of the byte read back come from whatever the bus
    /// floats at, which on this board is all ones -- MAME writes that as
    /// `space.unmap() & 0xfe`. Returning them as ones rather than zeros matters:
    /// the I/O board's firmware rotates the byte it read, and zeros there would
    /// corrupt the bits already assembled if it ever masked the wrong way.
    [[nodiscard]] u8 d0_read();

private:
    /// Shift one bit out of the register, zero filling behind it.
    [[nodiscard]] bool shift_out();

    std::array<InputHandler, kChannelCount> m_input{};

    u8 m_shift_register = 0;
};

}  // namespace sm2::hw
