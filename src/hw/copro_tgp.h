// SPDX-License-Identifier: BSD-3-Clause
//
// The Model 2 / Model 2A geometry coprocessor subsystem: the MB86234 together
// with everything wired around it.
//
// Derived from MAME's src/mame/sega/model2.cpp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
#pragma once

#include "core/types.h"
#include "cpu/mb86233/bus.h"
#include "cpu/mb86233/mb86233.h"
#include "hw/copro_fifo.h"

#include <span>
#include <vector>

namespace sm2::hw {

/// The coprocessor and its surroundings.
///
/// The MB86234 has no memory of its own beyond its register file. Everything it
/// touches is provided here:
///
///   program   4096 words of RAM that the host CPU fills with microcode
///   data      two RAM banks, 256 words at 0x000 and 512 words at 0x200
///   io        the mathematical lookup tables, or a window onto the display list
///             buffer and the coprocessor's data ROM, depending on a bank register
///   rf        the two host FIFOs and that bank register
///
/// The lookup tables are the reason a Model 2 can do trigonometry at all: there
/// is no such instruction. The program writes an argument to a port and reads the
/// result back, and the tables come from ROMs on the CPU board.
class CoproTgp final : public cpu::mb86233::Bus {
public:
    /// Words of microcode RAM.
    static constexpr u32 kProgramWords = 0x1000;

    /// Depth of each host FIFO.
    static constexpr usize kFifoDepth = 8;

    CoproTgp();

    CoproTgp(const CoproTgp&)            = delete;
    CoproTgp& operator=(const CoproTgp&) = delete;

    /// Wire up the ROM tables and the display list buffer.
    ///
    /// `tables` is the CPU board's 64K-word table ROM: sine and cosine at 0x0000,
    /// arctangent at 0x4000, reciprocal at 0x8000 and inverse square root at
    /// 0xc000. `data_rom` is the game's coprocessor data ROM, which holds
    /// collision and height-map data and is empty on some games. `buffer_ram` is
    /// the host's display list buffer, which the coprocessor writes its results
    /// into; it must outlive this object.
    void attach(std::span<const u32> tables, std::span<const u32> data_rom,
                std::span<u32> buffer_ram);

    void reset();

    [[nodiscard]] cpu::mb86233::MB86233& cpu() { return m_cpu; }
    [[nodiscard]] const cpu::mb86233::MB86233& cpu() const { return m_cpu; }

    [[nodiscard]] CoproFifo& fifo_in() { return m_fifo_in; }
    [[nodiscard]] CoproFifo& fifo_out() { return m_fifo_out; }
    [[nodiscard]] const CoproFifo& fifo_in() const { return m_fifo_in; }
    [[nodiscard]] const CoproFifo& fifo_out() const { return m_fifo_out; }

    /// Advance the coprocessor by up to this many of its own cycles.
    [[nodiscard]] s32 run(s32 cycles);

    // -- host side ---------------------------------------------------------

    /// Write to the control register at 0x00980000.
    ///
    /// The top bit selects microcode upload. On the rising edge the coprocessor is
    /// halted and the word counter reset; on the falling edge it is released and
    /// reset, which starts the uploaded program.
    void control_write(u32 value);
    [[nodiscard]] u32 control_read() const { return m_control; }

    /// Write to the FIFO port at 0x00884000. During an upload this is the
    /// microcode itself rather than a command.
    void host_fifo_write(u32 value);

    /// Read a result from the coprocessor.
    [[nodiscard]] u32 host_fifo_read();

    /// Write to a function port at 0x00880000. The port number becomes part of the
    /// command word, which is how one write both selects a function and passes an
    /// argument.
    void function_port_write(u32 byte_offset, u32 value);

    /// True when there is no result waiting. This is what the host polls.
    [[nodiscard]] bool output_empty() const { return m_fifo_out.empty(); }

    /// Words of microcode uploaded since the last upload began.
    [[nodiscard]] u32 uploaded_words() const { return m_upload_count; }

    /// Counts of what the coprocessor did, for diagnostics.
    ///
    /// These separate "the coprocessor is running" from "the coprocessor is doing
    /// the work it exists for". A program can retire millions of instructions in a
    /// polling loop without ever touching the tables or writing a result.
    struct Activity {
        u64 commands_received = 0;  ///< Words popped from the host FIFO.
        u64 results_sent      = 0;  ///< Words pushed to the host FIFO.
        u64 buffer_writes     = 0;  ///< Words written into the display list.
        u64 buffer_reads      = 0;
        u64 data_rom_reads    = 0;
        u64 table_reads       = 0;  ///< Trigonometry and reciprocal lookups.
    };
    [[nodiscard]] const Activity& activity() const { return m_activity; }

    [[nodiscard]] std::span<const u32> program() const { return m_program; }

    // -- cpu::mb86233::Bus -------------------------------------------------

    u32  fetch(u16 address) override;
    u32  read_program(u16 address) override;
    u32  read_data(u16 address) override;
    void write_data(u16 address, u32 value) override;
    u32  read_io(u16 address) override;
    void write_io(u16 address, u32 value) override;
    u32  read_rf(u8 address) override;
    void write_rf(u8 address, u32 value) override;

private:
    /// The external memory window, reachable through the io space when the bank
    /// register selects it.
    [[nodiscard]] u32 external_read(u16 offset) const;
    void external_write(u16 offset, u32 value);

    /// True when the bank register points the io space at external memory instead
    /// of the mathematical tables.
    [[nodiscard]] bool external_window_enabled() const
    {
        return (m_bank & 0xc00000) != 0;
    }

    // The mathematical units. Each takes an argument through a write and returns
    // a result through a read, with the port offset selecting which part of the
    // answer is wanted.
    [[nodiscard]] u32 sincos_read(u16 offset) const;
    [[nodiscard]] u32 atan_read() const;
    [[nodiscard]] u32 inverse_read(u16 offset) const;
    [[nodiscard]] u32 inverse_sqrt_read(u16 offset) const;

    [[nodiscard]] u32 table(u32 index) const
    {
        ++m_activity.table_reads;
        return index < m_tables.size() ? m_tables[index] : 0;
    }

    cpu::mb86233::MB86233 m_cpu;

    CoproFifo m_fifo_in;
    CoproFifo m_fifo_out;

    std::vector<u32> m_program;
    std::vector<u32> m_data_low;   ///< 256 words at 0x000
    std::vector<u32> m_data_high;  ///< 512 words at 0x200

    std::span<const u32> m_tables;
    std::span<const u32> m_data_rom;
    std::span<u32>       m_buffer_ram;

    u32 m_control      = 0;
    u32 m_upload_count = 0;

    /// External memory bank register, written through rf port 3.
    u32 m_bank = 0;

    /// Arguments latched by the mathematical units.
    u32 m_sincos_base = 0;
    u32 m_inverse_base = 0;
    u32 m_inverse_sqrt_base = 0;
    u32 m_atan_base[4]{};

    /// Mutable because the table lookups are const reads that still count.
    mutable Activity m_activity;
};

}  // namespace sm2::hw
