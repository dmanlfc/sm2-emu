// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315-5838_317-0229_comp.cpp
// (BSD-3-Clause, copyright-holders David Haywood, Samuel Neves, Peter
// Wilhelmsen, Morten Shearman Kirkegaard).

#include "hw/sega_315_5838_comp.h"

namespace sm2::hw {
namespace {

// The 50-byte string Dead or Alive's boot check reads. Two leading spaces are
// part of the real data, not padding.
constexpr char kDoaString[] = "  TECMO LTD.  DEAD OR ALIVE  1996.10.22  VER. 1.00";
constexpr u32  kDoaStringLength = 50;

}  // namespace

void Sega3155838Comp::reset()
{
    m_srcoffset = 0;
}

u8 Sega3155838Comp::next_byte()
{
    const u8 value = m_srcoffset < kDoaStringLength
                         ? static_cast<u8>(kDoaString[m_srcoffset])
                         : 0x00;
    m_srcoffset = (m_srcoffset + 1) & 0x007fffff;
    return value;
}

u16 Sega3155838Comp::data_r()
{
    return static_cast<u16>((next_byte() << 8) | next_byte());
}

}  // namespace sm2::hw
