// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
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

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class Msm6253 {
public:
    static constexpr u32 kChannelCount = 4;

    /// An analogue channel's current voltage, quantised to eight bits.
    using InputHandler = std::function<u8()>;

    void reset();

    void set_input(u32 channel, InputHandler handler);

    /// Start a conversion on the channel in the low two address bits. The result
    /// is latched immediately, as in MAME: the Z80 takes longer than a
    /// conversion to reach its first read.
    void address_write(u32 offset);

    /// Read the next bit of the latched conversion into D0. The other bits float
    /// high (MAME's `space.unmap() & 0xfe`); the firmware rotates the byte, so
    /// they must read as ones.
    [[nodiscard]] u8 d0_read();

    /// Save/restore the shift register (the only durable state).
    void serialize(Archive& ar);

private:
    /// Shift one bit out of the register, zero filling behind it.
    [[nodiscard]] bool shift_out();

    std::array<InputHandler, kChannelCount> m_input{};

    u8 m_shift_register = 0;
};

}  // namespace sm2::hw
