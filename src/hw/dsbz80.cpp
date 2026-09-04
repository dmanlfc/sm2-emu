// SPDX-License-Identifier: BSD-3-Clause
//
// See dsbz80.h. Ported from MAME's src/mame/sega/dsbz80.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert).

#include "hw/dsbz80.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::hw {
namespace {

// The Z80 runs at 4 MHz (MAME: "unknown clock, but probably pretty slow"),
// against the host i960's 25 MHz. Kept as an exact ratio with the remainder
// carried between run() calls, as the other boards do.
constexpr u64 kZ80ClockNumerator   = 4;
constexpr u64 kZ80ClockDenominator = 25;

// The UART's clock: MAME feeds it a 500 kHz clock, which at the 8251's 16x rate
// is the standard 31.25 kHz Sega sound-data rate. Counted in Z80 cycles.
constexpr u32 kUartBitRate     = 31'250;
constexpr u32 kUartBitsPerByte = 10;  // 8 data + start + stop

constexpr u32 kDecoderRate = 32'000;  // the mpeg_audio stream rate MAME allocs

constexpr u16 kRomSize = 0x8000;   // 32 KB program ROM at 0x0000
constexpr u16 kRamBase = 0x8000;   // 32 KB RAM at 0x8000
constexpr u16 kRamSize = 0x8000;

}  // namespace

DsbZ80::DsbZ80() : m_cpu(*this)
{
    m_ram.assign(kRamSize, 0);
}

DsbZ80::~DsbZ80() = default;

void DsbZ80::attach(std::span<const u8> program_rom, std::span<const u8> mpeg_rom)
{
    m_program_rom = program_rom;
    m_mpeg_rom    = mpeg_rom;
}

void DsbZ80::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});

    m_start = m_end = 0;
    m_audio_pos = m_audio_avail = 0;
    std::fill(std::begin(m_audio_buf), std::end(m_audio_buf), s16{0});
    m_mp_vol   = 0x7f;
    m_mp_state = 0;
    m_mp_start = m_mp_end = m_mp_pan = 0;
    m_lp_start = m_lp_end = 0;
    m_mp_pos = 0;
    m_cycle_debt    = 0;
    m_resample_frac = 0;
    m_counters      = Counters{};

    if (!present()) {
        return;
    }

    // The decoder reads straight out of the MPEG ROM. Rebuilt on every reset so
    // the board can be brought up more than once (the tests do).
    m_decoder = std::make_unique<mpeg_audio>(m_mpeg_rom.data(), mpeg_audio::L2,
                                             false, 0);

    m_uart.configure(4'000'000, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();

    // RxRDY drives the Z80's IRQ0, which is how it learns a command byte
    // arrived. MAME wires rxrdy_handler to INPUT_LINE_IRQ0.
    m_uart.set_ready_handler([this] { m_cpu.set_irq_line(m_uart.rxrdy()); });
    m_uart.set_tx_handler([this](u8 value) {
        if (m_rxd_handler) {
            m_rxd_handler(value);
        }
    });

    m_cpu.reset();
}

void DsbZ80::write_txd(u8 value)
{
    ++m_counters.bytes_from_host;
    m_uart.write_rxd(value);
}

void DsbZ80::run(u32 host_cycles)
{
    if (!present()) {
        return;
    }

    m_cycle_debt += static_cast<u64>(host_cycles) * kZ80ClockNumerator;
    u64 wanted = m_cycle_debt / kZ80ClockDenominator;
    m_cycle_debt %= kZ80ClockDenominator;

    if (wanted == 0) {
        return;
    }

    m_uart.run(static_cast<u32>(wanted));
    static_cast<void>(m_cpu.run(static_cast<s32>(wanted)));
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------

u8 DsbZ80::read8(u16 address)
{
    if (address < kRomSize) {
        return address < m_program_rom.size() ? m_program_rom[address] : 0;
    }
    if (address >= kRamBase) {
        return m_ram[address - kRamBase];
    }
    return 0;
}

void DsbZ80::write8(u16 address, u8 value)
{
    if (address >= kRamBase) {
        m_ram[address - kRamBase] = value;
    }
    // Below 0x8000 is ROM; writes are discarded.
}

u8 DsbZ80::io_read8(u16 port)
{
    switch (port & 0xff) {
        case 0xe2:
            return mpeg_pos_r(0);
        case 0xe3:
            return mpeg_pos_r(1);
        case 0xe4:
            return mpeg_pos_r(2);
        case 0xf0:
            return m_uart.read(0);
        case 0xf1:
            return m_uart.read(1);
        default:
            return 0;
    }
}

void DsbZ80::io_write8(u16 port, u8 value)
{
    switch (port & 0xff) {
        case 0xe0:
            mpeg_trigger_w(value);
            break;
        case 0xe2:
            mpeg_start_w(0, value);
            break;
        case 0xe3:
            mpeg_start_w(1, value);
            break;
        case 0xe4:
            mpeg_start_w(2, value);
            break;
        case 0xe5:
            mpeg_end_w(0, value);
            break;
        case 0xe6:
            mpeg_end_w(1, value);
            break;
        case 0xe7:
            mpeg_end_w(2, value);
            break;
        case 0xe8:
            mpeg_volume_w(value);
            break;
        case 0xe9:
            mpeg_stereo_w(value);
            break;
        case 0xf0:
            m_uart.write(0, value);
            break;
        case 0xf1:
            m_uart.write(1, value);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// MPEG control ports, MAME's dsbz80_device handlers
// ---------------------------------------------------------------------------

void DsbZ80::mpeg_trigger_w(u8 data)
{
    m_mp_state = data;

    if (data == 0) {  // stop
        m_mp_state    = 0;
        m_audio_pos   = 0;
        m_audio_avail = 0;
    } else if (data == 1) {  // play without loop
        m_mp_pos = m_mp_start * 8;
    } else if (data == 2) {  // play with loop
        m_mp_pos = m_mp_start * 8;
    }
}

u8 DsbZ80::mpeg_pos_r(u32 offset) const
{
    const u32 mp_prg = static_cast<u32>(m_mp_pos) >> 3;
    switch (offset) {
        case 0:
            return (mp_prg >> 16) & 0xff;
        case 1:
            return (mp_prg >> 8) & 0xff;
        case 2:
            return mp_prg & 0xff;
        default:
            return 0;
    }
}

// Writes to start/end while playback is in progress get latched and take effect
// when the current stream ends, which is how the Z80 does looping and multi-part
// songs. See MAME's comment.
void DsbZ80::mpeg_start_w(u32 offset, u8 data)
{
    switch (offset) {
        case 0:
            m_start = (m_start & 0x00ffff) | (static_cast<u32>(data) << 16);
            break;
        case 1:
            m_start = (m_start & 0xff00ff) | (static_cast<u32>(data) << 8);
            break;
        case 2:
            m_start = (m_start & 0xffff00) | data;
            if (m_mp_state == 0) {
                m_mp_start = m_start;
            } else {
                m_lp_start = m_start;
            }
            break;
        default:
            break;
    }
}

void DsbZ80::mpeg_end_w(u32 offset, u8 data)
{
    switch (offset) {
        case 0:
            m_end = (m_end & 0x00ffff) | (static_cast<u32>(data) << 16);
            break;
        case 1:
            m_end = (m_end & 0xff00ff) | (static_cast<u32>(data) << 8);
            break;
        case 2:
            m_end = (m_end & 0xffff00) | data;
            if (m_mp_state == 0) {
                m_mp_end = m_end;
            } else {
                m_lp_end = m_end;
            }
            break;
        default:
            break;
    }
}

void DsbZ80::mpeg_volume_w(u8 data)
{
    m_mp_vol = static_cast<u32>(~data & 0x7f);
}

void DsbZ80::mpeg_stereo_w(u8 data)
{
    m_mp_pan = data & 3;  // 0 = stereo, 1 = left on both, 2 = right on both
}

// ---------------------------------------------------------------------------
// Decode and mix
// ---------------------------------------------------------------------------

void DsbZ80::decode_next()
{
    if (m_mp_state == 0 || !m_decoder) {
        m_audio_avail = 0;
        return;
    }

    int sample_rate    = 0;
    int channel_count  = 0;
    int output_samples = 0;
    const bool ok = m_decoder->decode_buffer(m_mp_pos, static_cast<int>(m_mp_end) * 8,
                                             m_audio_buf, output_samples,
                                             sample_rate, channel_count);
    if (ok) {
        m_audio_pos   = 0;
        m_audio_avail = output_samples;
        ++m_counters.mpeg_frames;
        return;
    }

    // End of the current stream: loop or stop, MAME's fallback.
    if (m_mp_state == 2) {
        if (m_mp_pos == static_cast<s32>(m_lp_start) * 8) {
            m_mp_state = 0;  // looping on undecodable data, give up
        }
        m_mp_pos = static_cast<s32>(m_lp_start) * 8;
        if (m_lp_end != 0) {
            m_mp_end = m_lp_end;
        }
    } else {
        m_mp_state = 0;
    }
    m_audio_avail = 0;
}

void DsbZ80::mix(s16* dst, u32 frames, u32 out_rate)
{
    if (!present() || out_rate == 0) {
        return;
    }

    for (u32 i = 0; i < frames; ++i) {
        // Produce one decoder-rate stereo frame's worth of source samples on
        // demand, then linearly resample to the output rate.
        while (m_audio_pos >= m_audio_avail) {
            decode_next();
            if (m_audio_avail == 0) {
                return;  // nothing more to play this call
            }
        }

        const s16 lraw = m_audio_buf[m_audio_pos * 2];
        const s16 rraw = m_audio_buf[m_audio_pos * 2 + 1];

        // Volume, MAME: sample * m_mp_vol, full-scale 32768*128.
        s32 l;
        s32 r;
        switch (m_mp_pan) {
            case 1:  // left only, on both channels
                l = r = static_cast<s32>(lraw) * static_cast<s32>(m_mp_vol);
                break;
            case 2:  // right only, on both channels
                l = r = static_cast<s32>(rraw) * static_cast<s32>(m_mp_vol);
                break;
            default:  // stereo
                l = static_cast<s32>(lraw) * static_cast<s32>(m_mp_vol);
                r = static_cast<s32>(rraw) * static_cast<s32>(m_mp_vol);
                break;
        }
        // Down from the 32768*128 full-scale to 16 bits.
        l >>= 7;
        r >>= 7;

        const s32 ol = static_cast<s32>(dst[i * 2]) + l;
        const s32 or_ = static_cast<s32>(dst[i * 2 + 1]) + r;
        dst[i * 2]     = static_cast<s16>(std::clamp(ol, -32768, 32767));
        dst[i * 2 + 1] = static_cast<s16>(std::clamp(or_, -32768, 32767));

        // Advance the resampler: consume out_rate ticks, step the source
        // position once per decoder-rate tick.
        m_resample_frac += kDecoderRate;
        while (m_resample_frac >= out_rate) {
            m_resample_frac -= out_rate;
            ++m_audio_pos;
        }
    }
}

}  // namespace sm2::hw
