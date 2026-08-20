// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/devices/machine/msm6253.cpp, BSD-3-Clause.

#include "hw/msm6253.h"

namespace sm2::hw {

void Msm6253::reset()
{
    m_shift_register = 0;
}

void Msm6253::set_input(u32 channel, InputHandler handler)
{
    if (channel < kChannelCount) {
        m_input[channel] = std::move(handler);
    }
}

void Msm6253::address_write(u32 offset)
{
    // MAME's address_w: fill the shift register from the internal A/D latch. The
    // written value is ignored; only which of the four addresses was written
    // matters. An unbound channel reads as 0xff, matching MAME's port_read
    // fallback for an unassigned input.
    const InputHandler& handler = m_input[offset & 3];
    m_shift_register            = handler ? handler() : 0xff;
}

bool Msm6253::shift_out()
{
    const bool msb = (m_shift_register & 0x80) != 0;
    m_shift_register = static_cast<u8>(m_shift_register << 1);
    return msb;
}

u8 Msm6253::d0_read()
{
    // The offset is ignored: any address in the device's window reads the next
    // bit. Bits 1 to 7 are the undriven bus.
    return static_cast<u8>((shift_out() ? 0x01 : 0x00) | 0xfe);
}

}  // namespace sm2::hw
