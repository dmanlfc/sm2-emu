// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315_5649.cpp, BSD-3-Clause.

#include "hw/io_315_5649.h"

#include "core/archive.h"
#include "core/log.h"

namespace sm2::hw {

void Io315_5649::reset()
{
    // As in MAME, the port latches survive a reset.
    m_port_config    = 0xff;
    m_mode           = 0;
    m_analog_channel = 0;
}

void Io315_5649::serialize(Archive& ar)
{
    ar.bytes(m_port_value.data(), m_port_value.size());
    ar.raw(m_port_config);
    ar.raw(m_mode);
    ar.raw(m_analog_channel);
}

u8 Io315_5649::read(u32 offset)
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

        // RS-422 channel 1 input: undriven on Model 2A, reads as zero as in MAME.
        case 0x0b:
            data = 0x00;
            break;

        // RS-422 channel 2 input, used by the lightgun interface board.
        case 0x0c:
            data = m_serial2_read ? m_serial2_read() : 0x00;
            break;

        // RS-422 status: receive full, transmit empty, so polling always
        // proceeds. MAME's hack; nothing on Model 2 needs the real handshake.
        case 0x0d:
            data = 0x0c;
            break;

        // Analogue input. Reading advances the mux; an unbound channel reads 0xff.
        case 0x0f: {
            const InputHandler& handler = m_analog[m_analog_channel];
            data = handler ? handler() : 0xff;
            m_analog_channel = (m_analog_channel + 1) & 0x07;
            break;
        }

        default:
            break;
    }

    return data;
}

void Io315_5649::write(u32 offset, u8 value)
{
    switch (offset) {
        // Ports A to G. Writing latches the value and drives the output, even
        // for a port configured as an input; reads of an input port ignore it.
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

        // Direction register: a set bit marks the port as an input.
        case 0x08:
            m_port_config = value;
            break;

        // RS-422 channel 1 output. Unconnected on Model 2A.
        case 0x09:
            break;

        // RS-422 channel 2 output.
        case 0x0a:
            if (m_serial2_write) {
                m_serial2_write(value);
            }
            break;

        // Mode register: port G counter mode, RS-422 satellite and loopback
        // modes, satellite number. No Model 2 game selects any of them.
        case 0x0e:
            if ((value & 0xf0) != 0) {
                SM2_DEBUG("315-5649: unimplemented mode bits %02x", value & 0xf0);
            }
            m_mode = value;
            break;

        // Analogue mux channel select.
        case 0x0f:
            m_analog_channel = value & 0x07;
            break;

        default:
            break;
    }
}

}  // namespace sm2::hw
