// SPDX-License-Identifier: GPL-3.0-or-later
//
// NCR 53C810 SCSI controller (SCRIPTS processor only).
//
// Derived from Supermodel's Src/Model3/53C810.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// VF3 writes DSP, which starts SCRIPTS automatically since MAN=0, and only then
// sets single-step mode, expecting an interrupt. Either the processor enters
// single-step mode during the (very short) transfer, which is not feasible to
// emulate, or single-stepping works even while halted, executing an invalid
// instruction once or twice without the VF3 driver noticing. Disabling
// automatic execution when single-stepping is enabled breaks Scud Race (jerky,
// glitchy graphics). Automatic execution plus single-stepping while halted
// works, but hits invalid instructions each time.
//
// Page 2-22 (42) of the manual describes how to clear interrupts.

#include "hw/model3/scsi_53c810.h"

#include "core/archive.h"
#include "hw/model3/byteswap.h"
#include "core/log.h"
#include "cpu/ppc/ppc.h"
#include "hw/model3/irq.h"

#include <bit>

namespace sm2::hw::model3 {

// -- SCRIPTS -----------------------------------------------------------------------

u32 Scsi53c810::fetch(u32 offset)
{
    // The bus is big endian; SCRIPTS words are little endian.
    return bswap(m_bus->read32(m_dsp + offset));
}

// TODO: what happens if an interrupt instruction runs in single-step mode?
void Scsi53c810::int_intfly()
{
    m_halt = true;   // halt SCRIPTS execution
    m_istat |= 1;    // DMA interrupt pending
    m_dstat |= 4;    // SCRIPTS interrupt instruction received
    if ((m_dien & 4) != 0) m_irq->assert_irq(m_irq_bit);
    if ((m_dbc & 0x100000) != 0) SM2_WARN("53C810: INTFLY instruction not emulated");
    // DSP not incremented (VF3 relies on this)
}

void Scsi53c810::move_memory()
{
    u32 src  = m_dsps;
    u32 dest = m_temp = fetch(8);  // word 3
    u32 num_bytes = m_dbc;
    // Not implemented: illegal instruction interrupt when src and dest are not
    // aligned the same way.
    SM2_TRACE("53C810: Move Memory %08X -> %08X, %X", src, dest, num_bytes);

    for (u32 i = 0; i < num_bytes / 4; ++i) {
        m_bus->write32(dest, m_bus->read32(src));
        dest += 4;
        src += 4;
    }
    num_bytes &= 3;
    while (num_bytes != 0) {
        m_bus->write8(dest++, m_bus->read8(src++));
        --num_bytes;
    }

    m_dbc  = 0;
    m_dsps = src;
    m_temp = dest;
    m_dsp += 12;
}

bool Scsi53c810::execute()
{
    if ((m_dcmd & 0xE0) == 0xC0) {
        move_memory();
        return true;
    }
    if ((m_dcmd & 0xF8) == 0x98) {
        int_intfly();
        return true;
    }
    SM2_DEBUG("53C810: unrecognized instruction (%02X%06X, DSP=%08X)", m_dcmd, m_dbc, m_dsp);
    return false;
}

void Scsi53c810::run(bool single_step)
{
    if (single_step) {
        const u32 op = fetch(0);
        m_dbc  = op & 0x00FFFFFF;
        m_dcmd = static_cast<u8>(op >> 24);
        m_dsps = fetch(4);
        (void)execute();

        m_istat |= 1;  // DMA interrupt pending
        m_dstat |= 8;  // single step interrupt
        if ((m_dien & 8) != 0) m_irq->assert_irq(m_irq_bit);
    } else {
        // Automatic mode: run as long as the processor is not halted.
        for (int i = 0; i < 100 && !m_halt; ++i) {
            const u32 op = fetch(0);
            m_dbc  = op & 0x00FFFFFF;
            m_dcmd = static_cast<u8>(op >> 24);
            m_dsps = fetch(4);
            if (!execute()) break;
        }
    }
}

// -- registers ---------------------------------------------------------------------

namespace {

void set_byte(u32& reg, unsigned index, u8 data)
{
    const unsigned shift = index * 8;
    reg = (reg & ~(0xFFu << shift)) | (u32{data} << shift);
}

}  // namespace

void Scsi53c810::write_register(unsigned reg, u8 data)
{
    if (reg >= 0x60) {
        SM2_WARN("53C810: write to invalid register %02X", reg);
        return;
    }
    m_regs[reg] = data;

    switch (reg) {
        case 0x14: m_istat = data; break;
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: set_byte(m_temp, reg - 0x1C, data); break;
        case 0x24: case 0x25: case 0x26: set_byte(m_dbc, reg - 0x24, data); break;
        case 0x27: m_dcmd = data; break;
        case 0x2C: case 0x2D: case 0x2E: set_byte(m_dsp, reg - 0x2C, data); break;
        case 0x2F:
            set_byte(m_dsp, 3, data);
            m_halt = false;  // writing this register un-halts operation (pg.6-31 of LSI manual)
            if ((m_dmode & 1) == 0) {
                // MAN=0: start SCRIPTS automatically. TODO: should single-step be
                // tested first?
                run(false);
            }
            break;
        case 0x30: case 0x31: case 0x32: case 0x33: set_byte(m_dsps, reg - 0x30, data); break;
        case 0x38: m_dmode = data; break;
        case 0x39: m_dien = data; break;
        case 0x3B:
            m_dcntl = data;
            if ((m_dcntl & 0x14) == 0x14) {
                run(true);  // single step
            } else if ((m_dcntl & 0x04) != 0) {
                run(false);  // start DMA bit
            }
            break;
        default: break;
    }
}

u8 Scsi53c810::read_register(unsigned reg)
{
    if (reg >= 0x60) {
        SM2_WARN("53C810: read from invalid register %02X", reg);
        return 0;
    }
    switch (reg) {
        case 0x0C: {
            // Stacked interrupts are not generated, so always clear the IRQ
            // status. The manual says these should be cleared here, but MAME
            // never clears them.
            const u8 ret = m_dstat;
            m_istat &= 0xFE;  // clear DIP (DMA interrupt)
            m_dstat &= 0xF7;  // clear SSI (single step interrupt)
            m_irq->deassert_irq(m_irq_bit);
            return ret;
        }
        case 0x14: return m_istat;
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: return static_cast<u8>(m_temp >> ((reg - 0x1C) * 8));
        case 0x24: case 0x25: case 0x26: return static_cast<u8>(m_dbc >> ((reg - 0x24) * 8));
        case 0x27: return m_dcmd;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: return static_cast<u8>(m_dsp >> ((reg - 0x2C) * 8));
        case 0x30: case 0x31: case 0x32: case 0x33: return static_cast<u8>(m_dsps >> ((reg - 0x30) * 8));
        case 0x38: return m_dmode;
        case 0x39: return m_dien;
        case 0x3B: return m_dcntl;
        default: break;
    }
    return m_regs[reg];
}

u32 Scsi53c810::read_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
    (void)device;
    if (bits == 8) return 0;
    if (reg == 0x00) {
        u32 d = bswap(u32{0x00011000});  // 0x1000 = LSI Logic, 0x0001 = 53c810a
        if (bits == 16) d = (d >> ((2 - (offset & 2)) * 8)) & 0xFFFF;
        return d;
    }
    return 0;
}

void Scsi53c810::write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset,
                                  u32 data)
{
    (void)device;
    (void)bits;
    (void)offset;
    if (reg == 4) m_base_address = static_cast<u8>(data & 0xFF);  // base address of the device
}

void Scsi53c810::reset()
{
    m_regs.fill(0);
    m_regs[0x00] = 0xC0;  // SCNTL0
    m_regs[0x0C] = 0x80;  // DSTAT
    m_regs[0x0F] = 0x02;  // SSTAT2
    m_regs[0x18] = 0xFF;  // reserved
    m_regs[0x19] = 0xF0;  // CTEST1
    m_regs[0x1A] = 0x01;  // CTEST2
    m_regs[0x46] = 0x60;  // MACNTL
    m_regs[0x47] = 0x0F;  // GPCNTL
    m_regs[0x4C] = 0x03;  // STEST0
    m_temp  = 0;
    m_dsp   = 0;
    m_dsps  = 0;
    m_dbc   = 0;
    m_dcmd  = 0;
    m_dcntl = 0;
    m_dmode = 0;
    m_dstat = 0x80;  // DMA FIFO empty
    m_dien  = 0;
    m_istat = 0;
    m_halt  = false;
}

void Scsi53c810::serialize(Archive& ar)
{
    ar.bytes(m_regs.data(), m_regs.size());
    ar.raw(m_temp);
    ar.raw(m_dsp);
    ar.raw(m_dsps);
    ar.raw(m_dbc);
    ar.raw(m_dcmd);
    ar.raw(m_dcntl);
    ar.raw(m_dmode);
    ar.raw(m_dstat);
    ar.raw(m_dien);
    ar.raw(m_istat);
    ar.raw(m_halt);
    ar.raw(m_base_address);
}

}  // namespace sm2::hw::model3
