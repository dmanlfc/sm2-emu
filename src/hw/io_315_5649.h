// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2::hw {

/// Sega 315-5649 I/O controller.
///
/// Seven 8-bit ports, a direction register, an auto-incrementing 8-channel
/// analogue mux, and two RS-422 serial channels. On Model 2A it carries the
/// coin and button inputs, the dipswitches, the lamp outputs, and the bit-banged
/// lines to the settings EEPROM. Lightgun games route the gun position through
/// serial channel 2.
///
/// Derived from MAME's src/mame/sega/315_5649.cpp (BSD-3-Clause), reduced to the
/// parts Model 2 uses: the port counter mode and the satellite/loopback modes
/// are not implemented because no Model 2 game selects them.
class Io315_5649 {
public:
    static constexpr u32 kPortCount    = 7;  ///< Ports A through G.
    static constexpr u32 kAnalogCount  = 8;

    using InputHandler  = std::function<u8()>;
    using OutputHandler = std::function<void(u8)>;

    void reset();

    /// Register read. Offsets are register indices, not byte addresses.
    [[nodiscard]] u8 read(u32 offset);

    void write(u32 offset, u8 value);

    // -- wiring ------------------------------------------------------------

    void set_input(u32 port, InputHandler handler)
    {
        if (port < kPortCount) {
            m_input[port] = std::move(handler);
        }
    }

    void set_output(u32 port, OutputHandler handler)
    {
        if (port < kPortCount) {
            m_output[port] = std::move(handler);
        }
    }

    void set_analog(u32 channel, InputHandler handler)
    {
        if (channel < kAnalogCount) {
            m_analog[channel] = std::move(handler);
        }
    }

    /// RS-422 channel 2, which is how the lightgun interface board is reached.
    void set_serial2(InputHandler read_handler, OutputHandler write_handler)
    {
        m_serial2_read  = std::move(read_handler);
        m_serial2_write = std::move(write_handler);
    }

private:
    std::array<InputHandler, kPortCount>   m_input{};
    std::array<OutputHandler, kPortCount>  m_output{};
    std::array<InputHandler, kAnalogCount> m_analog{};

    InputHandler  m_serial2_read;
    OutputHandler m_serial2_write;

    /// Last value written to each port, returned for ports configured as
    /// outputs. Comes up all ones and is *not* cleared by reset, matching
    /// MAME's device, which sets it in its constructor rather than in
    /// device_reset.
    std::array<u8, kPortCount> m_port_value{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    /// Direction register: a set bit marks that port as an input. Every port is
    /// an input after reset. This matters more than it looks: a program that
    /// reads a port before programming the direction register sees the live
    /// input, not a latched zero, and several titles do exactly that.
    u8 m_port_config = 0xff;

    u8 m_mode           = 0;
    u8 m_analog_channel = 0;
};

}  // namespace sm2::hw
