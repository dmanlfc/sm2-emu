// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "hw/model1_tgp.h"

#include "core/archive.h"

#include <algorithm>

namespace sm2::hw {

Model1Tgp::Model1Tgp()
    : m_cpu(*this)
    , m_data_low(0x100, 0)
    , m_data_high(0x200, 0)
    , m_ram(kRamWords, 0)
{
    m_fifo_in.configure(kFifoDepth);
    m_fifo_out.configure(kFifoDepth);

    m_fifo_in.set_on_empty_retry([this] { m_cpu.stall(); });
    m_fifo_in.set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_fifo_in.set_on_unempty([this] { m_cpu.set_halted(false); });

    m_fifo_out.set_on_full([this] { m_cpu.set_halted(true); });
    m_fifo_out.set_on_unfull([this] { m_cpu.set_halted(false); });
}

void Model1Tgp::attach(std::span<const u32> program, std::span<const u32> tables, std::span<const u32> data_rom)
{
    m_program  = program;
    m_tables   = tables;
    m_data_rom = data_rom;
}

void Model1Tgp::reset()
{
    m_cpu.reset();
    m_cpu.set_halted(false);
    m_fifo_in.clear();
    m_fifo_out.clear();
    std::fill(m_data_low.begin(), m_data_low.end(), 0u);
    std::fill(m_data_high.begin(), m_data_high.end(), 0u);
    std::fill(m_ram.begin(), m_ram.end(), 0u);
    std::fill(std::begin(m_ram_adr), std::end(m_ram_adr), 0u);
    std::fill(std::begin(m_atan_base), std::end(m_atan_base), 0u);
    m_sincos_base     = 0;
    m_inv_base        = 0;
    m_isqrt_base      = 0;
    m_data_base       = 0;
    m_host_ram_adr    = 0;
    m_host_ram_latch[0] = 0;
    m_host_ram_latch[1] = 0;
}

void Model1Tgp::serialize(Archive& ar)
{
    m_cpu.serialize(ar);
    m_fifo_in.serialize(ar);
    m_fifo_out.serialize(ar);
    ar.bytes(m_data_low.data(), m_data_low.size());
    ar.bytes(m_data_high.data(), m_data_high.size());
    ar.bytes(m_ram.data(), m_ram.size());
    ar.bytes(m_ram_adr, 4);
    ar.raw(m_sincos_base);
    ar.raw(m_inv_base);
    ar.raw(m_isqrt_base);
    ar.bytes(m_atan_base, 4);
    ar.raw(m_data_base);
    ar.raw(m_host_ram_adr);
    ar.bytes(m_host_ram_latch, 2);
}

// -- V60 side -------------------------------------------------------------------

u16 Model1Tgp::host_ram_r(u32 half)
{
    const u32 word = m_ram[m_host_ram_adr & (kRamWords - 1)];
    if (half == 0) return static_cast<u16>(word);
    if ((m_host_ram_adr & 0x8000) != 0) ++m_host_ram_adr;
    return static_cast<u16>(word >> 16);
}

void Model1Tgp::host_ram_w(u32 half, u16 value)
{
    m_host_ram_latch[half & 1] = value;
    if (half != 0) {
        m_ram[m_host_ram_adr & (kRamWords - 1)] = m_host_ram_latch[0] | (static_cast<u32>(m_host_ram_latch[1]) << 16);
        if ((m_host_ram_adr & 0x8000) != 0) ++m_host_ram_adr;
    }
}

// -- TGP side -------------------------------------------------------------------

u32 Model1Tgp::fetch(u16 address)
{
    const u32 index = address & (kProgramWords - 1);
    return index < m_program.size() ? m_program[index] : 0;
}

u32 Model1Tgp::read_program(u16 address)
{
    return fetch(address);
}

u32 Model1Tgp::read_data(u16 address)
{
    if (address < 0x100) return m_data_low[address];
    if (address == 0x100) return m_fifo_in.pop();
    if (address >= 0x200 && address < 0x400) return m_data_high[address - 0x200];
    return 0;
}

void Model1Tgp::write_data(u16 address, u32 value)
{
    if (address < 0x100) {
        m_data_low[address] = value;
    } else if (address >= 0x200 && address < 0x400) {
        m_data_high[address - 0x200] = value;
    } else if (address == 0x400) {
        m_fifo_out.push(value);
    }
}

u32 Model1Tgp::read_io(u16 address)
{
    if (address >= 0x8000) {
        if (m_data_rom.empty()) return 0;
        const u32 index = ((m_data_base & ~0x7fffu) | (address & 0x7fff)) & (m_data_rom.size() - 1);
        return m_data_rom[index];
    }
    if (address < 0x20) {
        // 0x00/0x01 with A3-A4 selecting one of four address registers.
        const u32 sel = (address >> 3) & 3;
        if ((address & 7) == 0) return m_ram_adr[sel];
        if ((address & 7) == 1) {
            const u32 value = m_ram[m_ram_adr[sel] & (kRamWords - 1)];
            m_ram_adr[sel] += (m_ram_adr[sel] & 0x40000) != 0 ? 4 : 1;
            return value;
        }
        return 0;
    }
    if (address <= 0x23) return sincos_r(address - 0x20);
    if (address <= 0x27) return atan_r();
    if (address <= 0x29) return inv_r(address - 0x28);
    if (address <= 0x2b) return isqrt_r(address - 0x2a);
    return 0;
}

void Model1Tgp::write_io(u16 address, u32 value)
{
    if (address < 0x20) {
        const u32 sel = (address >> 3) & 3;
        if ((address & 7) == 0) {
            m_ram_adr[sel] = value;
        } else if ((address & 7) == 1) {
            m_ram[m_ram_adr[sel] & (kRamWords - 1)] = value;
            m_ram_adr[sel] += (m_ram_adr[sel] & 0x40000) != 0 ? 4 : 1;
        }
        return;
    }
    if (address <= 0x23) {
        m_sincos_base = value;
    } else if (address <= 0x27) {
        m_atan_base[address - 0x24] = value;
    } else if (address <= 0x29) {
        m_inv_base = value;
    } else if (address <= 0x2b) {
        m_isqrt_base = value;
    } else if (address == 0x2e) {
        m_data_base = value;
    }
}

u32 Model1Tgp::read_rf(u8 /*address*/)
{
    return 0;
}

void Model1Tgp::write_rf(u8 /*address*/, u32 /*value*/)
{
    // LEDs.
}

u32 Model1Tgp::sincos_r(u32 offset) const
{
    const u32 ang   = m_sincos_base + offset * 0x4000;
    u32       index = ang & 0x3fff;
    if ((ang & 0x4000) != 0) index = std::min<u32>(0x4000 - index, 0x3fff);
    u32 result = table(index);
    if ((ang & 0x8000) != 0) result ^= 0x80000000u;
    return result;
}

u32 Model1Tgp::inv_r(u32 offset) const
{
    const u32 index  = ((m_inv_base >> 9) & 0x3ffe) | (offset & 1);
    u32       result = table(index | 0x8000);
    const u8  bexp   = static_cast<u8>((m_inv_base >> 23) & 0xff);
    const u8  exp    = static_cast<u8>((result >> 23) + (0x7f - bexp));
    result           = (result & 0x807fffffu) | (static_cast<u32>(exp) << 23);
    if ((m_inv_base & 0x80000000u) != 0) result ^= 0x80000000u;
    return result;
}

u32 Model1Tgp::isqrt_r(u32 offset) const
{
    const u32 index  = 0x2000 ^ (((m_isqrt_base >> 10) & 0x3ffe) | (offset & 1));
    u32       result = table(index | 0xc000);
    const u8  bexp   = static_cast<u8>((m_isqrt_base >> 24) & 0x7f);
    const u8  exp    = static_cast<u8>((result >> 23) + (0x3f - bexp));
    result           = (result & 0x807fffffu) | (static_cast<u32>(exp) << 23);
    if ((offset & 1) == 0) result &= 0x7fffffffu;
    return result;
}

u32 Model1Tgp::atan_r() const
{
    u32 idx = m_atan_base[3] & 0xffff;
    if ((idx & 0xc000) != 0) idx = 0x3fff;
    u32 result = table(idx | 0x4000);

    // The table ROM has a carry error in some entries; correct it as MAME does.
    const u16 dt = static_cast<u16>((result >> 16) + result);
    if ((dt & 0x001) != 0) {
        if ((result & 0x00f) == 0x00e) result -= 0x00000001;
        else result -= 0x00010000;
    }
    if ((dt & 0x010) != 0) {
        if ((result & 0x0f0) == 0x0e0) result -= 0x00000010;
        else result -= 0x00100000;
    }
    if ((dt & 0x100) != 0) {
        if ((result & 0xf00) == 0xe00) result -= 0x00000100;
        else result -= 0x01000000;
    }

    const bool s0 = (m_atan_base[0] & 0x80000000u) != 0;
    const bool s1 = (m_atan_base[1] & 0x80000000u) != 0;
    const bool s2 = (m_atan_base[2] & 0x80000000u) != 0;
    if (s0 ^ s1 ^ s2) result >>= 16;
    if (s2) result += 0x4000;
    if ((s0 && !s2) || (s1 && s2)) result += 0x8000;
    return result & 0xffff;
}

}  // namespace sm2::hw
