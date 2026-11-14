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
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// Model 2 sound board: a 68000 with 512 KB of RAM and an SCSP.
//
// The memory map follows MAME's model2_snd in src/mame/sega/model2.cpp
// (BSD-3-Clause) region for region.
//
// The board has its own ROM, RAM and clock. Its only link to the host is serial,
// from the host's i8251 to the SCSP's MIDI port, and there is no reset line.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"
#include "cpu/m68000/m68000.h"
#include "hw/scsp.h"
#include "hw/scsp_dsp.h"
#include "hw/dsbz80.h"
#include "hw/dsb2.h"
#include "hw/sound_board.h"

#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class Model2Sound final : public SoundBoard, public cpu::Bus, public ScspMemory {
public:
    Model2Sound();
    ~Model2Sound() override;

    Model2Sound(const Model2Sound&)            = delete;
    Model2Sound& operator=(const Model2Sound&) = delete;

    /// Sound program ROM (MAME's "audiocpu") and PCM sample ROM ("samples").
    /// Either may be empty; without a program ROM the board is inert.
    void attach(std::span<const u8> program_rom, std::span<const u8> samples);

    /// Attach the Z80 Digital Sound Board's ROMs (Sega Touring Car). Its MPEG
    /// music mixes into this board's stream; empty regions leave it inert.
    void attach_dsb(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg);

    /// Attach the 68000-based DSB2 music board's ROMs (Top Skater). Empty
    /// regions leave it inert.
    void attach_dsb2(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg);

    void set_music_volume(u32 percent)
    {
        m_dsb.set_music_volume(percent);
        m_dsb2.set_music_volume(percent);
    }

    void reset();

    /// Apply the per-set flat gain that levels loudness across the library.
    void configure_balance(const std::string& game_name);

    /// Advance the board by the equivalent of `host_cycles` of the i960's 25 MHz
    /// clock. Remainders are carried between calls so the ratios stay exact.
    void run(u32 host_cycles);

    [[nodiscard]] bool present() const override { return !m_program_rom.empty(); }

    // -- the serial link to the CPU board ------------------------------------

    /// A command byte from the host's i8251, bound for the SCSP's MIDI port.
    void midi_in(u8 value);

    /// Called when the SCSP sends a byte back to the host.
    void set_midi_out_handler(Scsp::MidiOutHandler handler);

    // -- audio ---------------------------------------------------------------

    /// Samples generated since the last clear, interleaved stereo at 44100 Hz.
    [[nodiscard]] std::span<const s16> pending_samples() const override { return m_pending; }
    void clear_pending_samples() override { m_pending.clear(); }

    [[nodiscard]] u32 sample_rate() const override { return m_scsp.sample_rate(); }

    /// SCSP slots currently keyed on; a rough sign of activity.
    [[nodiscard]] u32 active_voices() const override { return m_scsp.active_slots(); }

    // -- cpu::Bus ----------------------------------------------------------
    //
    // The 68000's bus is big-endian, unlike every other bus in the machine.

    u8  read8(u32 address) override;
    u16 read16(u32 address) override;
    u32 read32(u32 address) override;

    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;

    // -- ScspMemory ---------------------------------------------------------
    //
    // The SCSP addresses the same 512 KB the 68000 uses as work RAM.

    u8   scsp_read_byte(u32 address) override;
    u16  scsp_read_word(u32 address) override;
    void scsp_write_word(u32 address, u16 value) override;

    // -- inspection, for the headless bring-up test -------------------------

    struct Counters {
        u64 scsp_reads      = 0;
        u64 scsp_writes     = 0;
        u64 snd_ctrl_writes = 0;
        u64 sample_reads    = 0;
        u64 unmapped_reads  = 0;
        u64 unmapped_writes = 0;
        /// Samples thrown away because nothing drained pending_samples(). Only
        /// non-zero in a headless run, where there is no audio device.
        u64 samples_dropped = 0;
    };

    [[nodiscard]] const Counters& counters() const { return m_counters; }
    [[nodiscard]] const cpu::m68000::M68000& cpu() const { return m_cpu; }
    [[nodiscard]] const Scsp& scsp() const { return m_scsp; }
    [[nodiscard]] std::span<const u8> ram() const { return m_ram; }

    /// Save/restore the whole board except the ROM spans and pending samples.
    /// Between frames only.
    void serialize(Archive& ar);

private:
    /// Resolves a 68000 address to a byte pointer, or null for anything that is
    /// not plain memory. `writable` is false for ROM.
    struct Window {
        u8*   base     = nullptr;
        usize size     = 0;
        bool  writable = false;
    };
    [[nodiscard]] Window resolve(u32 address);

    void snd_ctrl_write(u16 value);

    /// Charge the 68000 the wait state RAM and the SCSP impose on an access.
    void bus_wait(u32 address);

    /// Generate the SCSP's next sample into m_pending.
    void generate_sample();

    cpu::m68000::M68000 m_cpu;

    /// Constructed with *this as its memory. Safe in the initialiser list: the
    /// SCSP's constructor only stores the pointer.
    Scsp m_scsp;

    /// The MPEG music boards; a set carries at most one.
    DsbZ80 m_dsb;
    Dsb2   m_dsb2;

    std::span<const u8> m_program_rom;
    std::span<const u8> m_samples;

    std::vector<u8> m_ram;  ///< 512 KB, shared with the SCSP's own address space.

    /// Sample ROM offsets of the two banked windows; see snd_ctrl_write.
    u32 m_bank4_offset = 0x200000;
    u32 m_bank5_offset = 0x600000;

    u64 m_cycle_debt = 0;  ///< Numerator carried between run() calls.

    /// 68000 cycles granted but not yet run; negative once an instruction has
    /// overrun the grant, which the next grant then pays back.
    s64 m_cycle_budget = 0;

    /// How far the 68000 is into the most recently generated sample, in cycles.
    /// Reaching kCyclesPerSample means the next sample is due.
    u64 m_sample_phase = 0;

    std::vector<s16> m_pending;

    Counters m_counters;
    bool     m_warned_no_program = false;
};

}  // namespace sm2::hw
