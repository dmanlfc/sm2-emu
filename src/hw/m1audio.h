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
// Sega Model 1 audio board: a 68000 with a YM3438 and two MultiPCMs.
//
// Derived from MAME's src/mame/shared/segam1audio.cpp (BSD-3-Clause,
// copyright-holders R. Belmont) region for region.
//
// All three sound chips are present. The two MultiPCMs are a port of MAME's own
// device; the YM3438 is ymfm, which is the library MAME's ym3438_device wraps, so
// the FM is the reference implementation rather than an approximation of it.
//
// Used by Model 1 and by the early Model 2 sets -- Daytona USA, Desert Tank and
// Virtua Cop -- which is why it lives here rather than with the CRX family's
// 68000/SCSP board. The two have nothing in common but their host CPU: different
// sound chips, different clocks, different sample rate.
//
// As with the CRX board the only link to the host is serial, an i8251 at each end,
// so this can be brought up and verified without the host doing anything.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"
#include "cpu/m68000/m68000.h"
#include "hw/i8251.h"
#include "hw/multipcm.h"
#include "hw/sound_board.h"
#include "hw/ym3438.h"

#include <functional>
#include <span>
#include <vector>

namespace sm2::hw {

class M1Audio final : public SoundBoard, public cpu::Bus {
public:
    M1Audio();
    ~M1Audio() override;

    M1Audio(const M1Audio&)            = delete;
    M1Audio& operator=(const M1Audio&) = delete;

    /// The board's own ROMs: the 68000 program ("m1audio:sndcpu") and one sample
    /// region per MultiPCM ("m1audio:pcm1", "m1audio:pcm2").
    ///
    /// Any of them may be empty. A board with no program ROM is inert, which is
    /// what a set that has not had its audio ROMs declared yet looks like.
    void attach(std::span<const u8> program_rom,
                std::span<const u8> pcm1,
                std::span<const u8> pcm2);

    void reset();

    /// Advance the board by the equivalent of `host_cycles` of the host i960's
    /// 25 MHz clock.
    ///
    /// The 68000 runs at 10 MHz (MAME: 20_MHz_XTAL / 2), so the ratio is 2/5 and
    /// the remainder is carried between calls rather than rounded per call.
    void run(u32 host_cycles);

    // -- the serial link to the CPU board ------------------------------------

    /// A byte from the host's i8251, arriving at this board's receiver.
    void write_txd(u8 value);

    /// Called when this board's transmitter sends a byte back to the host.
    void set_rxd_handler(std::function<void(u8)> handler);

    // -- SoundBoard ----------------------------------------------------------

    [[nodiscard]] std::span<const s16> pending_samples() const override { return m_pending; }
    void clear_pending_samples() override { m_pending.clear(); }
    [[nodiscard]] u32  sample_rate() const override;
    [[nodiscard]] bool present() const override { return !m_program_rom.empty(); }
    [[nodiscard]] u32  active_voices() const override;

    // -- cpu::Bus ------------------------------------------------------------
    //
    // Big-endian, like the CRX board's 68000 and unlike everything else here.

    u8  read8(u32 address) override;
    u16 read16(u32 address) override;
    u32 read32(u32 address) override;

    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;

    // -- inspection, for the headless bring-up report ------------------------

    struct Counters {
        u64 ym_reads        = 0;
        u64 ym_writes       = 0;
        u64 ym_status_reads = 0;  ///< Of ym_reads, those that came back non-zero.
        u64 pcm_writes[2]   = {0, 0};
        u64 bank_writes[2]  = {0, 0};
        u64 uart_reads      = 0;
        u64 uart_writes     = 0;
        u64 bytes_from_host = 0;
        u64 bytes_to_host   = 0;
        u64 unmapped_reads  = 0;
        u64 unmapped_writes = 0;
        /// Samples thrown away because nothing drained pending_samples(). Only
        /// non-zero in a headless run, where there is no audio device.
        u64 samples_dropped = 0;
    };

    [[nodiscard]] const Counters& counters() const { return m_counters; }
    [[nodiscard]] const cpu::m68000::M68000& cpu() const { return m_cpu; }
    [[nodiscard]] std::span<const u8> ram() const { return m_ram; }
    [[nodiscard]] const MultiPcm& pcm(u32 index) const { return m_pcm[index & 1]; }
    [[nodiscard]] const Ym3438& ym() const { return m_ym; }

private:
    /// Plain memory behind an address, or null for anything with side effects.
    struct Window {
        u8*   base     = nullptr;
        usize size     = 0;
        bool  writable = false;
    };
    [[nodiscard]] Window resolve(u32 address);

    /// Generate this slice's share of samples and mix the three chips.
    void generate_audio(u32 host_cycles);

    void uart_tick(u32 host_cycles);

    cpu::m68000::M68000 m_cpu;

    /// Board-side end of the serial link. The host's own i8251 lives on the CPU
    /// board; this is the second one, at the other end of the same two wires.
    I8251 m_uart;

    MultiPcm m_pcm[2];
    Ym3438   m_ym;

    std::span<const u8> m_program_rom;

    std::vector<u8> m_ram;  ///< 64 KB at 0xf00000.

    std::function<void(u8)> m_rxd_handler;

    u64 m_cpu_debt    = 0;  ///< Numerator carried between run() calls, 2/5.
    u64 m_sample_debt = 0;  ///< Numerator for the 44643 Hz sample clock.

    /// Sound cycles the last slice ran past its allowance, owed to the next one.
    u64 m_cycle_overshoot = 0;

    std::vector<s16> m_pending;

    Counters m_counters;
    bool     m_warned_no_program = false;
};

}  // namespace sm2::hw
