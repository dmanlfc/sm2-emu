// SPDX-License-Identifier: BSD-3-Clause
//
// Sega Digital Sound Board 2 (DSB2), the 68000-based MPEG music board.
//
// Ported from MAME's src/mame/sega/dsb2.{cpp,h} (BSD-3-Clause). A 68000 with
// 128 KB of RAM, an i8251 taking sound commands from the host on the same
// serial link the SCSP listens to, a 1 kHz timer interrupt, and the same MPEG-1
// Layer II decoder the Z80 DSB uses (hw/mpeg_audio). Where the Z80 board takes
// its play parameters as raw I/O-port writes, the DSB2's 68000 firmware drives
// the decoder through a small command FIFO: a START/END opcode followed by a
// three-byte address, then a play or stop opcode.
//
// On Model 2 only the Top Skater family carries a DSB2. Their sound effects come
// from the SCSP; only the music is on this board.

#pragma once

#include "core/types.h"
#include "cpu/m68000/m68000.h"
#include "hw/i8251.h"
#include "hw/mpeg_audio.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace sm2::hw {

class Dsb2 final : public cpu::Bus {
public:
    Dsb2();
    ~Dsb2() override;

    Dsb2(const Dsb2&)            = delete;
    Dsb2& operator=(const Dsb2&) = delete;

    /// The 68000 program ROM ("dsb2:mpegcpu", 128 KB) and the MPEG data ROM
    /// ("dsb2:mpeg", up to 16 MB on Model 2). Inert with no program ROM.
    void attach(std::span<const u8> program_rom, std::span<const u8> mpeg_rom);

    [[nodiscard]] bool present() const { return !m_program_rom.empty(); }

    void reset();

    /// A serial byte from the host's i8251, the same one the SCSP receives.
    void write_txd(u8 value);

    void set_rxd_handler(std::function<void(u8)> handler) { m_rxd_handler = std::move(handler); }

    /// Advance the 68000 (and its UART and 1 kHz timer) by the equivalent of
    /// `host_cycles` of the host i960's 25 MHz clock.
    void run(u32 host_cycles);

    /// Decode and mix `frames` stereo frames at `out_rate` Hz into `dst`
    /// (added to what is there). The decoder runs at 32000 Hz; a linear
    /// resampler bridges the difference.
    void mix(s16* dst, u32 frames, u32 out_rate);

    // -- cpu::Bus (68000, big-endian) ---------------------------------------

    u8   read8(u32 address) override;
    u16  read16(u32 address) override;
    u32  read32(u32 address) override;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;

    // -- inspection ----------------------------------------------------------

    struct Counters {
        u64 bytes_from_host = 0;
        u64 mpeg_frames     = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

private:
    // The FIFO command state machine, MAME's mpeg_command_t.
    enum class Command : u8 {
        Idle,
        StartHi,
        StartMid,
        StartLo,
        EndHi,
        EndMid,
        EndLo,
    };

    void system_control_w(u8 data);  ///< 0xd00001: MPEG ROM bank (A24)
    void fifo_w(u8 data);            ///< 0xe00003: command / address FIFO

    void decode_next();

    cpu::m68000::M68000 m_cpu;
    I8251               m_uart;

    std::span<const u8> m_program_rom;
    std::span<const u8> m_mpeg_rom;
    std::vector<u8>     m_ram;  ///< 128 KB at 0xf00000.

    std::unique_ptr<mpeg_audio> m_decoder;
    s16 m_audio_buf[1152 * 2] = {};

    u32 m_mp_start = 0, m_mp_end = 0, m_mp_vol = 0x7f, m_mp_pan = 0;
    u32 m_start = 0, m_end = 0, m_rom_bank = 0;
    s32 m_mp_pos = 0, m_audio_pos = 0, m_audio_avail = 0;
    bool    m_playing = false;
    Command m_command = Command::Idle;

    u64 m_cycle_debt     = 0;  ///< host->68000 clock remainder.
    u64 m_timer_debt     = 0;  ///< host cycles toward the next 1 kHz tick.
    u64 m_resample_frac  = 0;

    std::function<void(u8)> m_rxd_handler;
    Counters                m_counters;
};

}  // namespace sm2::hw
