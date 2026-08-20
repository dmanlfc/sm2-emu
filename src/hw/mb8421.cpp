// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/devices/machine/mb8421.{h,cpp}, BSD-3-Clause.

#include "hw/mb8421.h"

namespace sm2::hw {

void Mb8421::reset()
{
    m_ram.fill(0);
}

u8 Mb8421::left_read(u32 offset) const
{
    return m_ram[offset & kAddressMask];
}

void Mb8421::left_write(u32 offset, u8 value)
{
    m_ram[offset & kAddressMask] = value;
}

u8 Mb8421::right_read(u32 offset) const
{
    return m_ram[offset & kAddressMask];
}

void Mb8421::right_write(u32 offset, u8 value)
{
    m_ram[offset & kAddressMask] = value;
}

}  // namespace sm2::hw
