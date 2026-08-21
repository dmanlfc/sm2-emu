// SPDX-License-Identifier: BSD-3-Clause
//
// See z80pio.h.

#include "hw/z80pio.h"

#include "core/log.h"

namespace sm2::hw {

void Z80Pio::reset()
{
    for (u32 index = 0; index < kPorts; ++index) {
        Port& port = m_port[index];
        // Keep the wiring; clear the programmed state. MAME's device_reset leaves
        // both ports in input mode with the interrupt enable clear.
        port.mode   = kModeInput;
        port.input  = 0;
        port.output = 0;
        port.ior    = 0xff;
        port.icw    = 0;
        port.mask   = 0;
        port.vector = 0;
        port.next   = NextControl::Any;
    }
}

u8 Z80Pio::read_alt(u32 offset)
{
    const u32 index = (offset >> 1) & 1;
    return (offset & 1) != 0 ? control_read() : data_read(index);
}

void Z80Pio::write_alt(u32 offset, u8 value)
{
    const u32 index = (offset >> 1) & 1;
    if ((offset & 1) != 0) {
        control_write(index, value);
    } else {
        data_write(index, value);
    }
}

u8 Z80Pio::control_read() const
{
    return static_cast<u8>((m_port[0].icw & 0xc0) | (m_port[1].icw >> 4));
}

u8 Z80Pio::data_read(u32 index)
{
    Port& port = m_port[index];
    switch (port.mode) {
        case kModeOutput:
            return port.output;

        case kModeInput:
        case kModeBidirectional:
            if (port.input_cb) port.input = port.input_cb();
            return port.input;

        case kModeBitControl:
        default:
            // Input lines come from the pin, output lines read back the latch.
            if (port.input_cb) port.input = port.input_cb();
            return static_cast<u8>((port.input & port.ior)
                                   | (port.output & static_cast<u8>(port.ior ^ 0xff)));
    }
}

void Z80Pio::data_write(u32 index, u8 value)
{
    Port& port = m_port[index];
    port.output = value;
    if (port.mode == kModeOutput || port.mode == kModeBitControl
        || port.mode == kModeBidirectional) {
        if (port.output_cb) port.output_cb(value);
    }
}

void Z80Pio::control_write(u32 index, u8 value)
{
    Port& port = m_port[index];

    switch (port.next) {
        case NextControl::DataDirection:
            port.ior  = value;
            port.next = NextControl::Any;
            return;

        case NextControl::Mask:
            port.mask = value;
            port.next = NextControl::Any;
            return;

        case NextControl::Any:
            break;
    }

    if ((value & 0x01) == 0) {
        port.vector = value;
        return;
    }

    switch (value & 0x0f) {
        case 0x0f:  // operating mode
            port.mode = static_cast<u8>(value >> 6);
            if (port.mode == kModeBidirectional && index == 1) {
                SM2_WARN("z80pio: port B was put into bidirectional mode, "
                         "which the part does not have");
                port.mode = kModeBitControl;
            }
            if (port.mode == kModeBitControl) {
                // Bit control needs its direction mask before it means anything.
                port.next = NextControl::DataDirection;
            }
            return;

        case 0x07:  // interrupt control word
            port.icw = value;
            if ((value & 0x10) != 0) {
                port.next = NextControl::Mask;
            }
            if ((value & 0x80) != 0) {
                SM2_WARN("z80pio: port %c enabled interrupts, which are not modelled",
                         index == 0 ? 'A' : 'B');
            }
            return;

        case 0x03:  // interrupt enable flip-flop alone
            port.icw = static_cast<u8>((value & 0x80) | (port.icw & 0x7f));
            if ((value & 0x80) != 0) {
                SM2_WARN("z80pio: port %c enabled interrupts, which are not modelled",
                         index == 0 ? 'A' : 'B');
            }
            return;

        default:
            SM2_DEBUG("z80pio: port %c ignored control word %02x",
                      index == 0 ? 'A' : 'B', value);
            return;
    }
}

}  // namespace sm2::hw
