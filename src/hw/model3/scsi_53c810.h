// SPDX-License-Identifier: GPL-3.0-or-later
//
// NCR 53C810 SCSI controller. The Model 3 uses only its SCRIPTS processor, as
// a DMA engine: Step 1.x games move display lists and textures to the Real3D
// with Memory Move instructions.
//
// Derived from Supermodel's Src/Model3/53C810.cpp and 53C810.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team (53C810.cpp) and Copyright (c) 2011 Bart
// Trzynadlowski, Nik Henson (53C810.h).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The PCI base-address byte starts at zero and is saved, as is the halt flag.
#pragma once

#include "core/types.h"
#include "hw/model3/mpc10x.h"

#include <array>

namespace sm2 {
class Archive;
}

namespace sm2::cpu::ppc {
class PpcBus;
}

namespace sm2::hw::model3 {

class IrqController;

class Scsi53c810 : public PciDevice {
public:
    void attach(cpu::ppc::PpcBus* bus, IrqController* irq, u32 irq_bit)
    {
        m_bus     = bus;
        m_irq     = irq;
        m_irq_bit = irq_bit;
    }

    void reset();

    void write_register(unsigned reg, u8 data);
    [[nodiscard]] u8 read_register(unsigned reg);

    [[nodiscard]] u32 read_pci_config(unsigned device, unsigned reg, unsigned bits,
                                      unsigned offset) const override;
    void write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset,
                          u32 data) override;

    /// The byte PCI configuration register 4 was last written with. On
    /// Step 1.x, 0xC0 places the registers at 0xC0000000.
    [[nodiscard]] u8 base_address() const { return m_base_address; }

    void serialize(Archive& ar);

    [[nodiscard]] u32 dsp() const { return m_dsp; }
    [[nodiscard]] bool halted() const { return m_halt; }

private:
    void run(bool single_step);
    /// Execute the instruction in DCMD/DBC/DSPS. False on an invalid opcode.
    bool execute();
    void int_intfly();
    void move_memory();
    [[nodiscard]] u32 fetch(u32 offset);

    cpu::ppc::PpcBus* m_bus     = nullptr;
    IrqController*    m_irq     = nullptr;
    u32               m_irq_bit = 0;

    std::array<u8, 0x60> m_regs{};
    u32  m_temp  = 0;
    u32  m_dsp   = 0;
    u32  m_dsps  = 0;
    u32  m_dbc   = 0;
    u8   m_dcmd  = 0;
    u8   m_dcntl = 0;
    u8   m_dmode = 0;
    u8   m_dstat = 0;
    u8   m_dien  = 0;
    u8   m_istat = 0;
    bool m_halt  = false;
    u8   m_base_address = 0;
};

}  // namespace sm2::hw::model3
