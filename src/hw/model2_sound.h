//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
//
// Model 2 sound board: a 68000 with 512 KB of RAM and an SCSP.
//
// The memory map follows MAME's model2_snd in src/mame/sega/model2.cpp
// (BSD-3-Clause) region for region.
//
// The board is almost independent of the CPU board. It has its own ROM, its own
// RAM and its own clock; the only link is a serial one, an i8251 on the host side
// talking to the SCSP's MIDI port, which carries the sound commands. There is no
// reset line from the host either, so the sound program runs from power-on and
// this can be brought up and verified without the host being involved at all.

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
#include <vector>

namespace sm2::hw {

class Model2Sound final : public SoundBoard, public cpu::Bus, public ScspMemory {
public:
    Model2Sound();
    ~Model2Sound() override;

    Model2Sound(const Model2Sound&)            = delete;
    Model2Sound& operator=(const Model2Sound&) = delete;

    /// Sound program ROM (MAME's "audiocpu") and PCM sample ROM ("samples").
    ///
    /// Either may be empty. A board with no program ROM is inert: run() does
    /// nothing, which keeps a synthetic machine in the tests usable.
    void attach(std::span<const u8> program_rom, std::span<const u8> samples);

    /// Attach the Z80 Digital Sound Board's ROMs, for the sets that carry one
    /// (Sega Touring Car and other DSB titles). The board plays the music as
    /// MPEG audio; its output mixes into this board's stream. Absent for most
    /// sets, in which case the DSB stays inert.
    void attach_dsb(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg);

    /// Attach the 68000-based DSB2 music board's ROMs, for the sets that carry
    /// one (Top Skater). Same role as the Z80 DSB, different board; a set has at
    /// most one. Empty regions leave it inert.
    void attach_dsb2(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg);

    void reset();

    /// Advance the board by the sound-clock equivalent of `host_cycles` of the
    /// host i960's 25 MHz clock.
    ///
    /// The two ratios involved are kept exactly, with the remainders carried
    /// between calls: 45.1584 MHz / 4 over 25 MHz for the 68000, and 44100 Hz
    /// over 25 MHz for the SCSP. Rounding either per call would drift audibly
    /// over a few minutes.
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

    /// SCSP slots currently keyed on. The Model 1 board counts MultiPCM channels
    /// instead; neither number means anything precise, they just say whether the
    /// board is making noise.
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

    /// Generate the SCSP's share of `host_cycles` worth of samples.
    void generate_audio(u32 host_cycles);

    cpu::m68000::M68000 m_cpu;

    /// Constructed with *this as its memory. Safe in the initialiser list: the
    /// SCSP's constructor only stores the pointer.
    Scsp m_scsp;

    /// The MPEG music boards, present only on DSB titles. A set carries at
    /// most one; both are inert otherwise. m_dsb is the Z80 board (stcc),
    /// m_dsb2 the 68000 board (topskatr).
    DsbZ80 m_dsb;
    Dsb2   m_dsb2;

    std::span<const u8> m_program_rom;
    std::span<const u8> m_samples;

    std::vector<u8> m_ram;  ///< 512 KB, shared with the SCSP's own address space.

    /// Offsets into the sample ROM for the two banked windows. Fixed for every
    /// Model 2 game whose sample region is 8 MB or smaller, which is all of them
    /// that matter here; see snd_ctrl_write.
    u32 m_bank4_offset = 0x200000;
    u32 m_bank5_offset = 0x600000;

    u64 m_cycle_debt = 0;  ///< Numerator carried between run() calls.

    /// Sound cycles the last slice ran past its allowance, owed to the next one.
    u64 m_cycle_overshoot = 0;

    u64 m_sample_debt = 0;  ///< Numerator for the 44100 Hz sample clock.

    std::vector<s16> m_pending;

    Counters m_counters;
    bool     m_warned_no_program = false;
};

}  // namespace sm2::hw
