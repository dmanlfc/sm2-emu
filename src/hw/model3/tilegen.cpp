// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 2D tile generator, emulation side.
//
// Derived from Supermodel's Src/Model3/TileGen.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2012 Bart Trzynadlowski, Nik Henson.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.

#include "hw/model3/tilegen.h"

#include "core/archive.h"
#include "core/log.h"
#include "hw/model3/irq.h"

namespace sm2::hw::model3 {

TileGen::TileGen()
    : m_vram(kVramBytes, 0)
{
}

void TileGen::reset()
{
    std::fill(m_vram.begin(), m_vram.end(), u8{0});
    m_regs.fill(0);
    m_lines.fill(TileGenLine{});
}

void TileGen::write_register(unsigned reg, u32 data)
{
    reg &= 0xFF;
    switch (reg) {
        case 0x00: case 0x08: case 0x0C:  // 0x0C: end of frame
        case 0x20: case 0x40: case 0x44:  // 0x40/0x44: layer colour offsets (renderer's)
        case 0x60: case 0x64: case 0x68: case 0x6C:
            break;
        case 0x10:  // IRQ acknowledge
            // MAME believes only the lower 4 bits should be cleared.
            if (m_irq != nullptr) m_irq->deassert_irq(data & 0xFF);
            break;
        default:
            SM2_TRACE("tilegen: reg %02X = %08X", reg, data);
            break;
    }
    m_regs[reg / 4] = data;
}

void TileGen::latch_line(u32 line)
{
    if (line >= m_lines.size()) return;
    TileGenLine& l = m_lines[line];
    l.reg20 = m_regs[0x20 / 4];
    l.reg40 = m_regs[0x40 / 4];
    l.reg44 = m_regs[0x44 / 4];
    for (u32 i = 0; i < 4; ++i) l.scroll[i] = m_regs[0x60 / 4 + i];
}

void TileGen::serialize(Archive& ar)
{
    ar.bytes(m_vram.data(), m_vram.size());
    ar.bytes(m_regs.data(), m_regs.size());
    ar.bytes(m_lines.data(), m_lines.size());
}

}  // namespace sm2::hw::model3
