// SPDX-License-Identifier: GPL-3.0-or-later
//
// Motorola MPC105 / MPC106 PCI bridge and memory controller, and the Model 3's
// PCI bus (configuration space only).
//
// Derived from Supermodel's Src/Model3/MPC10x.cpp, MPC10x.h, PCI.cpp and PCI.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2019 Bart Trzynadlowski, Nik Henson, Ian Curtis (MPC10x) and Copyright
// (c) 2011 Bart Trzynadlowski, Nik Henson (PCI).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The register file is filled byte-wise as little-endian, whatever the host,
// and register indices wrap to its 256 bytes. PCI devices are a fixed
// (number, interface) table.
#pragma once

#include "core/types.h"

#include <array>

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

/// A device on the PCI bus. `reg` is the configuration register's byte
/// offset (a multiple of 4), `bits` 8/16/32, `offset` the byte within it.
class PciDevice {
public:
    virtual ~PciDevice() = default;
    [[nodiscard]] virtual u32 read_pci_config(unsigned device, unsigned reg, unsigned bits,
                                              unsigned offset) const                    = 0;
    virtual void write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset,
                                  u32 data)                                             = 0;
};

class PciBus {
public:
    void attach(unsigned device, PciDevice* object);
    [[nodiscard]] u32 read_config(unsigned device, unsigned reg, unsigned bits,
                                  unsigned offset) const;
    void write_config(unsigned device, unsigned reg, unsigned bits, unsigned offset, u32 data);

private:
    struct Link {
        unsigned   device = 0;
        PciDevice* object = nullptr;
    };
    std::array<Link, 8> m_links{};
    usize               m_count = 0;
};

class Mpc10x {
public:
    /// 0x105 (MPC105, steps 1.x) or 0x106 (MPC106, steps 2.x).
    void set_model(int model);
    [[nodiscard]] int model() const { return m_model; }

    void attach_bus(PciBus* bus) { m_bus = bus; }

    void reset();

    /// `data` as the PowerPC stored it; byte-reversed here.
    void write_config_address(u32 data);
    [[nodiscard]] u32 read_config_data(unsigned bits, unsigned offset) const;
    void write_config_data(unsigned bits, unsigned offset, u32 data);

    /// The bridge's own register space at 0xF8FFF000 (MPC105), byte-wide.
    void write_register(unsigned reg, u8 data);
    [[nodiscard]] u8 register_byte(unsigned reg) const { return m_regs[reg & 0xFF]; }

    void serialize(Archive& ar);

private:
    int                  m_model = 0x105;
    PciBus*              m_bus   = nullptr;
    std::array<u8, 256>  m_regs{};
    u32                  m_pci_bus      = 0;
    u32                  m_pci_device   = 0;
    u32                  m_pci_function = 0;
    u32                  m_pci_reg      = 0;
};

}  // namespace sm2::hw::model3
