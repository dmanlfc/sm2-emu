// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315_5338a.cpp, BSD-3-Clause.

#include "hw/sega_315_5338a.h"

#include "core/log.h"

namespace sm2::hw {

void Sega3155338a::reset()
{
    // MAME has no device_reset here at all: everything is set once in the
    // constructor and survives a reset. Only the serial side is cleared, because
    // a half-loaded address surviving a reset would send the first transfer after
    // it somewhere arbitrary.
    m_port_config   = 0;
    m_serial_output = 0;
    m_command       = 0;
    m_address       = 0;
}

void Sega3155338a::set_input(u32 port, InputHandler handler)
{
    if (port < kPortCount) {
        m_input[port] = std::move(handler);
    }
}

void Sega3155338a::set_output(u32 port, OutputHandler handler)
{
    if (port < kPortCount) {
        m_output[port] = std::move(handler);
    }
}

u8 Sega3155338a::read(u32 offset)
{
    u8 data = 0xff;

    switch (offset) {
        // Ports A to G.
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
            if (bit(m_port_config, offset) != 0) {
                const InputHandler& handler = m_input[offset];
                data = handler ? handler() : 0xff;
            } else {
                data = m_port_value[offset];
            }
            break;

        // Port direction register.
        case 0x08:
            data = m_port_config;
            break;

        // The data register reads back what was last written to it. MAME marks
        // this as a guess; nothing depends on it beyond the firmware's own
        // read-after-write check.
        case 0x0a:
            data = m_serial_output;
            break;

        // Likewise the command register.
        case 0x0b:
            data = m_command;
            break;

        // Serial data input: fetch the byte at the latched address.
        case 0x0c:
            data = m_serial_read ? m_serial_read(m_address) : 0xff;
            break;

        // Status register.
        //   7654----  unknown
        //   ----3---  transfer finished
        //   -----21-  unknown
        //   -------0  command acknowledged (0 = ack)
        //
        // Always "finished and acknowledged", as MAME returns: every transfer
        // here completes inside the write that starts it.
        case 0x0d:
            data = 0x08;
            break;

        default:
            break;
    }

    return data;
}

void Sega3155338a::write(u32 offset, u8 value)
{
    switch (offset) {
        // Ports A to G. MAME drives the output callback even for a port
        // configured as an input, with a note that bingoct's sound needs it.
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06: {
            m_port_value[offset] = value;
            const OutputHandler& handler = m_output[offset];
            if (handler) {
                handler(value);
            }
            break;
        }

        // Port direction register: a set bit marks the port as an input. A port
        // that has just been switched from input to output re-drives its latched
        // value, which is what stops a lamp changing state when the firmware
        // finishes configuring the chip.
        case 0x08: {
            const u8 changed = static_cast<u8>(value ^ m_port_config);
            for (u32 port = 0; port < kPortCount; ++port) {
                if (bit(changed, port) != 0 && bit(value, port) == 0) {
                    const OutputHandler& handler = m_output[port];
                    if (handler) {
                        handler(m_port_value[port]);
                    }
                }
            }
            m_port_config = value;
            break;
        }

        // Command register. This is the serial side's whole control surface: the
        // address is loaded a byte at a time out of the data register, and a
        // transfer is then either a write to that address or, for the 0x7n
        // family, a write to one of eight fixed addresses.
        case 0x09:
            m_command = value;
            switch (value) {
                case 0x00:
                    m_address = static_cast<u16>((m_address & 0xff00) | m_serial_output);
                    break;
                case 0x01:
                    m_address = static_cast<u16>((m_address & 0x00ff)
                                                 | (static_cast<u16>(m_serial_output) << 8));
                    break;
                case 0x07:
                    if (m_serial_write) {
                        m_serial_write(m_address, m_serial_output);
                    }
                    break;
                case 0x70:
                case 0x71:
                case 0x72:
                case 0x73:
                case 0x74:
                case 0x75:
                case 0x76:
                case 0x77:
                    if (m_serial_write) {
                        m_serial_write(value & 0x07u, m_serial_output);
                    }
                    break;
                case 0x87:
                    // Sent after setting up an address and before reading serial
                    // data back. Nothing to do: the read at offset 0x0c fetches
                    // from the latched address directly.
                    break;
                default:
                    SM2_DEBUG("315-5338a: unknown command %02x", value);
                    break;
            }
            break;

        // Serial data output register.
        case 0x0a:
            m_serial_output = value;
            break;

        default:
            break;
    }
}

}  // namespace sm2::hw
