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
// The Model 1 CPU board's TGP: an MB86233 running its program from ROM, fed by
// two FIFOs to the V60 and sharing an 8K-word RAM with it. Memory map from
// MAME's sega/model1_m.cpp.
#pragma once

#include "core/types.h"
#include "cpu/mb86233/mb86233.h"
#include "hw/copro_fifo.h"

#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class Model1Tgp final : public cpu::mb86233::Bus {
public:
    static constexpr u32   kProgramWords = 0x800;
    static constexpr u32   kRamWords     = 0x2000;
    static constexpr usize kFifoDepth    = 16;

    Model1Tgp();
    Model1Tgp(const Model1Tgp&)            = delete;
    Model1Tgp& operator=(const Model1Tgp&) = delete;

    void attach(std::span<const u32> program, std::span<const u32> tables, std::span<const u32> data_rom);
    void reset();

    [[nodiscard]] s32 run(s32 cycles) { return m_cpu.run(cycles); }

    [[nodiscard]] cpu::mb86233::MB86233&       cpu() { return m_cpu; }
    [[nodiscard]] const cpu::mb86233::MB86233& cpu() const { return m_cpu; }
    [[nodiscard]] CoproFifo&                   fifo_in() { return m_fifo_in; }
    [[nodiscard]] CoproFifo&                   fifo_out() { return m_fifo_out; }
    [[nodiscard]] const CoproFifo&             fifo_in() const { return m_fifo_in; }
    [[nodiscard]] const CoproFifo&             fifo_out() const { return m_fifo_out; }

    // -- the V60's ports (d00000-dfffff) ---------------------------------------

    [[nodiscard]] u16 host_ram_adr_r() const { return m_host_ram_adr; }
    void              host_ram_adr_w(u16 value) { m_host_ram_adr = value; }
    [[nodiscard]] u16 host_ram_r(u32 half);
    void              host_ram_w(u32 half, u16 value);

    /// The shared RAM, for the display list walk and the debug dumps.
    [[nodiscard]] std::span<const u32> ram() const { return m_ram; }

    void serialize(Archive& ar);

    // -- cpu::mb86233::Bus -------------------------------------------------------

    u32  fetch(u16 address) override;
    u32  read_program(u16 address) override;
    u32  read_data(u16 address) override;
    void write_data(u16 address, u32 value) override;
    u32  read_io(u16 address) override;
    void write_io(u16 address, u32 value) override;
    u32  read_rf(u8 address) override;
    void write_rf(u8 address, u32 value) override;

private:
    [[nodiscard]] u32 table(u32 index) const { return index < m_tables.size() ? m_tables[index] : 0; }
    [[nodiscard]] u32 sincos_r(u32 offset) const;
    [[nodiscard]] u32 inv_r(u32 offset) const;
    [[nodiscard]] u32 isqrt_r(u32 offset) const;
    [[nodiscard]] u32 atan_r() const;

    cpu::mb86233::MB86233 m_cpu;
    CoproFifo             m_fifo_in;
    CoproFifo             m_fifo_out;

    std::span<const u32> m_program;
    std::span<const u32> m_tables;
    std::span<const u32> m_data_rom;

    std::vector<u32> m_data_low;   ///< 256 words at 0x000
    std::vector<u32> m_data_high;  ///< 512 words at 0x200
    std::vector<u32> m_ram;        ///< shared with the V60

    u32 m_ram_adr[4]{};
    u32 m_sincos_base = 0;
    u32 m_inv_base    = 0;
    u32 m_isqrt_base  = 0;
    u32 m_atan_base[4]{};
    u32 m_data_base = 0;

    u16 m_host_ram_adr = 0;
    u16 m_host_ram_latch[2]{};
};

}  // namespace sm2::hw
