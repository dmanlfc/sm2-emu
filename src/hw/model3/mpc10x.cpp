// SPDX-License-Identifier: GPL-3.0-or-later
//
// Motorola MPC105 / MPC106 PCI bridge and the Model 3's PCI bus.
//
// Derived from Supermodel's Src/Model3/MPC10x.cpp and PCI.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2019 Bart Trzynadlowski, Nik Henson, Ian Curtis (MPC10x) and Copyright
// (c) 2011 Bart Trzynadlowski, Nik Henson (PCI).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// A minimal implementation for the Model 3, not a full emulation of the
// device. Accesses smaller than 32 bits are probably handled wrongly,
// particularly for register numbers that are not 32-bit aligned. Little-endian
// mode is not supported (though the internal registers seem to be stored
// little-endian). External configuration registers are not implemented.
// Multiple PCI buses are not supported; everything is assumed to be on bus 0.
// Reference: "MPC106 PCI/Bridge/Memory Controller User's Manual" (MPC106UM/D
// Rev.1).

#include "hw/model3/mpc10x.h"

#include "core/archive.h"
#include "hw/model3/byteswap.h"
#include "core/log.h"

#include <bit>

namespace sm2::hw::model3 {

// -- PCI bus ----------------------------------------------------------------------

void PciBus::attach(unsigned device, PciDevice* object)
{
    if (m_count < m_links.size()) {
        m_links[m_count++] = Link{device, object};
    }
}

u32 PciBus::read_config(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
    for (usize i = 0; i < m_count; ++i) {
        if (m_links[i].device == device) {
            return m_links[i].object->read_pci_config(device, reg, bits, offset);
        }
    }
    SM2_DEBUG("model3 pci: read from unknown device %u reg %02x", device, reg);
    return 0;
}

void PciBus::write_config(unsigned device, unsigned reg, unsigned bits, unsigned offset, u32 data)
{
    for (usize i = 0; i < m_count; ++i) {
        if (m_links[i].device == device) {
            m_links[i].object->write_pci_config(device, reg, bits, offset, data);
            return;
        }
    }
    SM2_DEBUG("model3 pci: write to unknown device %u reg %02x = %08x", device, reg, data);
}

// -- MPC10x -----------------------------------------------------------------------

namespace {

void put16le(std::array<u8, 256>& regs, unsigned reg, u16 v)
{
    regs[reg & 0xFF]       = static_cast<u8>(v);
    regs[(reg + 1) & 0xFF] = static_cast<u8>(v >> 8);
}

void put32le(std::array<u8, 256>& regs, unsigned reg, u32 v)
{
    for (unsigned i = 0; i < 4; ++i) regs[(reg + i) & 0xFF] = static_cast<u8>(v >> (8 * i));
}

}  // namespace

void Mpc10x::set_model(int model)
{
    if (model != 0x105 && model != 0x106) {
        SM2_ERROR("model3: invalid MPC10x model number (%X)", model);
        model = 0x105;
    }
    m_model = model;
}

void Mpc10x::write_config_address(u32 data)
{
    const u32 d    = bswap(data);
    m_pci_bus      = (d >> 16) & 0xFF;
    m_pci_device   = (d >> 11) & 0x1F;
    m_pci_function = (d >> 8) & 7;
    // For actual PCI devices (device > 0) the register is shifted right by 2
    // and clamped to 0x3F when used; device 0 uses it as a byte index.
    m_pci_reg = d & 0xFF;
    if (m_pci_bus != 0) SM2_DEBUG("model3: MPC10x access to PCI bus %u", m_pci_bus);
}

u32 Mpc10x::read_config_data(unsigned bits, unsigned offset) const
{
    if (m_pci_device == 0) {
        const unsigned r = m_pci_reg + offset;
        switch (bits) {
            case 8:  return m_regs[r & 0xFF];
            case 16: return (u32{m_regs[r & 0xFF]} << 8) | m_regs[(r + 1) & 0xFF];
            case 32:
                return (u32{m_regs[r & 0xFF]} << 24) | (u32{m_regs[(r + 1) & 0xFF]} << 16)
                       | (u32{m_regs[(r + 2) & 0xFF]} << 8) | m_regs[(r + 3) & 0xFF];
            default: return 0;
        }
    }
    return m_bus != nullptr ? m_bus->read_config(m_pci_device, (m_pci_reg >> 2) & 0x3C, bits, offset)
                            : 0;
}

void Mpc10x::write_config_data(unsigned bits, unsigned offset, u32 data)
{
    if (m_pci_device == 0) {
        const unsigned r = m_pci_reg + offset;
        switch (bits) {
            case 8: m_regs[r & 0xFF] = static_cast<u8>(data); break;
            case 16:
                m_regs[r & 0xFF]       = static_cast<u8>(data >> 8);
                m_regs[(r + 1) & 0xFF] = static_cast<u8>(data);
                break;
            case 32:
                m_regs[r & 0xFF]       = static_cast<u8>(data >> 24);
                m_regs[(r + 1) & 0xFF] = static_cast<u8>(data >> 16);
                m_regs[(r + 2) & 0xFF] = static_cast<u8>(data >> 8);
                m_regs[(r + 3) & 0xFF] = static_cast<u8>(data);
                break;
            default: break;
        }
        return;
    }
    if (m_bus != nullptr) m_bus->write_config(m_pci_device, (m_pci_reg >> 2) & 0x3C, bits, offset, data);
}

void Mpc10x::write_register(unsigned reg, u8 data)
{
    m_regs[reg & 0xFF] = data;
    if ((reg & 0xFF) == 0xA8 && (data & 0x20) != 0) {
        SM2_WARN("model3: MPC10x little endian mode not implemented");
    }
}

void Mpc10x::reset()
{
    m_regs.fill(0);
    put16le(m_regs, 0x00, 0x1057);                             // vendor ID (Motorola)
    put16le(m_regs, 0x02, m_model == 0x106 ? 0x0002 : 0x0001);  // device ID
    if (m_model == 0x106) {
        put32le(m_regs, 0x04, 0x00800006);  // PCI command and PCI status
        put32le(m_regs, 0x08, 0x00060000);  // class code and revision ID
        put32le(m_regs, 0x0C, 0x00000800);  // cache line size
        put32le(m_regs, 0x70, 0x00CD0000);  // output driver control
        put32le(m_regs, 0xA8, 0x0010FF00);  // processor interface config. 1
        put32le(m_regs, 0xAC, 0x060C000C);  // processor interface config. 2
        put32le(m_regs, 0xB8, 0x04000000);
        put32le(m_regs, 0xC0, 0x00000100);  // error enabling 1
        put32le(m_regs, 0xE0, 0x00420FFF);  // emulation support configuration 1
        put32le(m_regs, 0xE8, 0x00200000);  // emulation support configuration 2
        put32le(m_regs, 0xF0, 0x0000FF02);  // memory control config. 1
        put32le(m_regs, 0xF4, 0x00030000);  // memory control config. 2
        put32le(m_regs, 0xFC, 0x00000010);  // memory control config. 4
    } else {
        put32le(m_regs, 0x04, 0x00800006);
        put32le(m_regs, 0x08, 0x00060000);
        put32le(m_regs, 0xA8, 0x0010FF00);
        put32le(m_regs, 0xAC, 0x060C000C);
        put32le(m_regs, 0xB8, 0x04000000);
        put32le(m_regs, 0xF0, 0x0000FF02);
        put32le(m_regs, 0xF4, 0x00030000);
        put32le(m_regs, 0xFC, 0x00000010);
    }
    m_pci_bus      = 0;
    m_pci_device   = 0;
    m_pci_function = 0;
    m_pci_reg      = 0;
}

void Mpc10x::serialize(Archive& ar)
{
    ar.bytes(m_regs.data(), m_regs.size());
    ar.raw(m_pci_bus);
    ar.raw(m_pci_device);
    ar.raw(m_pci_function);
    ar.raw(m_pci_reg);
}

}  // namespace sm2::hw::model3
