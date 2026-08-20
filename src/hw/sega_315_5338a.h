// SPDX-License-Identifier: BSD-3-Clause
//
// Sega 315-5338A I/O controller.
//
// Derived from MAME's src/mame/sega/315_5338a.{h,cpp} (BSD-3-Clause,
// copyright-holders Dirk Best).
//
// Seven 8-bit ports with a direction register, plus a byte-wide serial side
// consisting of an address register, a data register and a command register. On
// the Model 1 I/O board the ports carry the cabinet's switches, the lamps and the
// bit-banged lines to the settings EEPROM, and the serial side is how the Z80
// reaches the dual-port RAM it shares with the main board: it loads an address a
// byte at a time, puts a byte in the data register, then writes a command that
// performs the transfer.
//
// This is not the same part as the 315-5649 in hw/io_315_5649.h, despite the
// similar port layout. The 5649 has its own analogue mux and two RS-422 channels;
// this one has neither, and instead of a mux it has the command register above.
// The two are kept as separate classes for that reason rather than being
// generalised: the register maps only overlap for the ports themselves.
//
// What is deliberately not modelled: the unknown bits of the status register at
// offset 0x0d, which MAME returns as a fixed 0x08 ("transfer finished, command
// acknowledged"), and any conversion timing behind the command register. Both
// sides of the transfer are synchronous here, so a command has always completed
// by the time the status register can be read.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2::hw {

class Sega3155338a {
public:
    static constexpr u32 kPortCount = 7;  ///< Ports A through G.

    using InputHandler  = std::function<u8()>;
    using OutputHandler = std::function<void(u8)>;

    /// The serial side's destination, which on the Model 1 I/O board is the
    /// left-hand port of the dual-port RAM.
    using SerialReadHandler  = std::function<u8(u32 address)>;
    using SerialWriteHandler = std::function<void(u32 address, u8 value)>;

    void reset();

    /// Register read. Offsets are register indices, not byte addresses.
    [[nodiscard]] u8 read(u32 offset);

    void write(u32 offset, u8 value);

    // -- wiring ------------------------------------------------------------

    void set_input(u32 port, InputHandler handler);
    void set_output(u32 port, OutputHandler handler);

    void set_serial_read(SerialReadHandler handler) { m_serial_read = std::move(handler); }
    void set_serial_write(SerialWriteHandler handler) { m_serial_write = std::move(handler); }

private:
    std::array<InputHandler, kPortCount>  m_input{};
    std::array<OutputHandler, kPortCount> m_output{};

    SerialReadHandler  m_serial_read;
    SerialWriteHandler m_serial_write;

    /// Last value written to each port, returned for ports configured as
    /// outputs. Comes up all ones and is *not* cleared by reset, matching MAME's
    /// device, which fills it in its constructor rather than in device_reset.
    std::array<u8, kPortCount> m_port_value{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    /// Direction register: a set bit marks that port as an input. Zero after
    /// reset, so every port starts as an output -- the opposite of the 315-5649,
    /// and taken from MAME, whose m_port_config starts at 0 in the constructor.
    u8 m_port_config = 0;

    /// The serial side's latched state.
    u8  m_serial_output = 0;
    u8  m_command       = 0;
    u16 m_address       = 0;
};

}  // namespace sm2::hw
