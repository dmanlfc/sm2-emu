// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Sega 315-5338A I/O controller.
//
// Derived from MAME's src/mame/sega/315_5338a.{h,cpp} (BSD-3-Clause,
// copyright-holders Dirk Best).
//
// Seven 8-bit ports with a direction register, plus a serial side (address,
// data and command registers). On the Model 1 I/O board the ports carry the
// switches, lamps and EEPROM lines, and the serial side is how the Z80 reaches
// the dual-port RAM: load an address a byte at a time, a data byte, then a
// command that performs the transfer.
//
// Not the same part as the 315-5649 (hw/io_315_5649.h): only the ports overlap.
//
// Transfers are synchronous, so the status register at 0x0d reads a fixed 0x08
// (finished, acknowledged), as in MAME.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2 {
class Archive;
}

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

    /// Save/restore the latched port values, direction register and serial-side
    /// state. Callbacks excluded (re-bound by the owner).
    void serialize(Archive& ar);

private:
    std::array<InputHandler, kPortCount>  m_input{};
    std::array<OutputHandler, kPortCount> m_output{};

    SerialReadHandler  m_serial_read;
    SerialWriteHandler m_serial_write;

    /// Last value written to each port, returned for output ports. All ones at
    /// power-on and not cleared by reset, as in MAME.
    std::array<u8, kPortCount> m_port_value{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    /// Direction register: a set bit marks that port as an input. Zero after
    /// reset, so every port starts as an output, unlike the 315-5649.
    u8 m_port_config = 0;

    /// The serial side's latched state.
    u8  m_serial_output = 0;
    u8  m_command       = 0;
    u16 m_address       = 0;
};

}  // namespace sm2::hw
