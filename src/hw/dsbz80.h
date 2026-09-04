// SPDX-License-Identifier: BSD-3-Clause
//
// Sega Z80 Digital Sound Board (DSB), the MPEG music board.
//
// Ported from MAME's src/mame/sega/dsbz80.{h,cpp} (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert). A Z80 with 32 KB of program
// ROM and 32 KB of RAM, an i8251 taking sound commands from the host on the
// same serial link the SCSP listens to, and an MPEG-1 Layer II decoder that
// streams a game's background music from a dedicated ROM. The Z80 drives the
// decoder through a handful of I/O ports (start/end address, trigger, volume,
// stereo mode) and reads back the current playback position.
//
// Several Model 2C titles carry it -- Sega Touring Car, Sega Water Ski, Dynamite
// Baseball, Sega Ski Super G. Their sound effects come from the SCSP as usual;
// only the music is on this board, which is why a set with a DSB is silent in
// the music while its effects play when the board is absent.
//
// Changes from upstream: MAME's device/sound-stream plumbing is replaced by the
// project's Z80 Bus and i8251, and an explicit generate() that decodes and
// resamples into a caller's buffer, mixed with whatever the SCSP produced.

#pragma once

#include "core/types.h"
#include "cpu/z80/z80.h"
#include "hw/i8251.h"
#include "hw/mpeg_audio.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace sm2::hw {

class DsbZ80 final : public cpu::z80::Bus {
public:
    DsbZ80();
    ~DsbZ80() override;

    DsbZ80(const DsbZ80&)            = delete;
    DsbZ80& operator=(const DsbZ80&) = delete;

    /// The Z80 program ROM ("dsbz80:mpegcpu", 32 KB) and the MPEG data ROM
    /// ("dsbz80:mpeg", up to 8 MB). A board with no program ROM is inert.
    void attach(std::span<const u8> program_rom, std::span<const u8> mpeg_rom);

    [[nodiscard]] bool present() const { return !m_program_rom.empty(); }

    void reset();

    /// A serial byte from the host's i8251, the same one the SCSP receives.
    void write_txd(u8 value);

    /// A byte the board sends back to the host (unused by the Model 2 titles,
    /// kept for completeness).
    void set_rxd_handler(std::function<void(u8)> handler) { m_rxd_handler = std::move(handler); }

    /// Advance the Z80 (and its UART) by the equivalent of `host_cycles` of the
    /// host i960's 25 MHz clock.
    void run(u32 host_cycles);

    /// Decode and mix `frames` stereo frames at `out_rate` Hz into `dst`
    /// (interleaved, added to whatever is already there). The decoder runs at
    /// 32000 Hz; the difference is bridged by a linear resampler.
    void mix(s16* dst, u32 frames, u32 out_rate);

    // -- cpu::z80::Bus -------------------------------------------------------

    u8   read8(u16 address) override;
    void write8(u16 address, u8 value) override;
    u8   io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

    // -- inspection ----------------------------------------------------------

    struct Counters {
        u64 bytes_from_host = 0;
        u64 mpeg_frames     = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }
    [[nodiscard]] u32 mp_state() const { return m_mp_state; }

private:
    // I/O port handlers, MAME's dsbz80io_map.
    void mpeg_trigger_w(u8 data);
    void mpeg_start_w(u32 offset, u8 data);
    void mpeg_end_w(u32 offset, u8 data);
    void mpeg_volume_w(u8 data);
    void mpeg_stereo_w(u8 data);
    [[nodiscard]] u8 mpeg_pos_r(u32 offset) const;

    /// Fill m_audio_buf with the next decoded MPEG frame, or stop. Returns the
    /// number of stereo samples now available, 0 at end of stream.
    void decode_next();

    cpu::z80::Z80 m_cpu;
    I8251         m_uart;

    std::span<const u8> m_program_rom;
    std::span<const u8> m_mpeg_rom;
    std::vector<u8>     m_ram;  ///< 32 KB at 0x8000.

    std::unique_ptr<mpeg_audio> m_decoder;
    s16 m_audio_buf[1152 * 2] = {};

    // Playback state, MAME's dsbz80_device members.
    u32 m_mp_start = 0, m_mp_end = 0, m_mp_vol = 0x7f, m_mp_pan = 0, m_mp_state = 0;
    u32 m_lp_start = 0, m_lp_end = 0, m_start = 0, m_end = 0;
    s32 m_mp_pos = 0, m_audio_pos = 0, m_audio_avail = 0;

    // Host-clock to Z80-clock conversion, carried like the other boards.
    u64 m_cycle_debt = 0;

    // Linear resampler state (decoder 32000 Hz -> output rate).
    u64 m_resample_frac = 0;

    std::function<void(u8)> m_rxd_handler;
    Counters                m_counters;
    bool                    m_warned_no_program = false;
};

}  // namespace sm2::hw
