// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// See dsb2.h. Ported from MAME's src/mame/sega/dsb2.cpp (BSD-3-Clause).
//
// Portions derived from Supermodel's Src/Model3/DSB.cpp, Copyright (c)
// 2003-2026 The Supermodel Team, GPL-3.0-or-later: fifo_w_model3 and the
// Model 3 branch of mix(). Supermodel's DSB code is "based on code donated by
// R. Belmont".

#include "hw/dsb2.h"

#include "core/archive.h"
#include "core/log.h"

#include <algorithm>

namespace sm2::hw {
namespace {

// The 68000 runs at 8 MHz (MAME: "unknown clocks"), against the host's 25 MHz.
constexpr u64 kCpuClockNumerator   = 8;
constexpr u64 kCpuClockDenominator = 25;

// The UART's clock, as on the Z80 board: 500 kHz / 16 = the 31.25 kHz Sega rate.
constexpr u32 kUartBitRate     = 31'250;
constexpr u32 kUartBitsPerByte = 10;

// A 1 kHz periodic interrupt on the 68000. In host (25 MHz) cycles that is
// 25000 per tick.
constexpr u64 kTimerHostCycles = 25'000;

constexpr u32 kDecoderRate = 32'000;

constexpr u32 kRomSize  = 0x20000;   // 128 KB program ROM at 0x000000
constexpr u32 kRamBase  = 0xf00000;  // 128 KB RAM
constexpr u32 kRamSize  = 0x20000;

// Play/stop command opcodes on the FIFO. The low bit is the player id and is
// masked off (data & 0xfe), matching MAME.
constexpr u8 kCmdStart = 0x14;
constexpr u8 kCmdEnd   = 0x24;
constexpr u8 kCmdPlay  = 0x74;
constexpr u8 kCmdStop  = 0x84;

}  // namespace

Dsb2::Dsb2() : m_cpu(*this)
{
    m_ram.assign(kRamSize, 0);
}

Dsb2::~Dsb2() = default;

void Dsb2::attach(std::span<const u8> program_rom, std::span<const u8> mpeg_rom)
{
    m_program_rom = program_rom;
    m_mpeg_rom    = mpeg_rom;
}

void Dsb2::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});

    m_start = m_end = 0;
    m_rom_bank      = 0;
    m_audio_pos = m_audio_avail = 0;
    std::fill(std::begin(m_audio_buf), std::end(m_audio_buf), s16{0});
    m_mp_vol   = 0x7f;
    m_mp_start = m_mp_end = m_mp_pan = 0;
    m_mp_pos   = 0;
    m_mp_state = 0;
    m_command  = Command::Idle;
    m_param     = 0;
    m_volume[0] = m_volume[1] = 0xff;  // full, in case the volume commands are missed
    m_stereo    = 0;
    m_cycle_debt    = 0;
    m_timer_debt    = 0;
    m_resample_frac = 0;
    m_prev[0] = m_prev[1] = 0;
    m_counters      = Counters{};

    if (!present()) {
        return;
    }

    m_decoder = std::make_unique<mpeg_audio>(m_mpeg_rom.data(), mpeg_audio::L2,
                                             false, 0);

    m_uart.configure(4'000'000, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();

    // RxRDY drives the 68000's IRQ1 (MAME: rxrdy_handler -> INPUT_LINE_IRQ1).
    m_uart.set_ready_handler([this] {
        m_cpu.set_irq_line(1, m_uart.rxrdy());
    });
    m_uart.set_tx_handler([this](u8 value) {
        if (m_rxd_handler) {
            m_rxd_handler(value);
        }
    });

    m_cpu.reset();
}

void Dsb2::serialize(Archive& ar)
{
    m_cpu.serialize(ar);
    m_uart.serialize(ar);
    ar.bytes(m_ram.data(), m_ram.size());
    ar.raw(m_mp_start);
    ar.raw(m_mp_end);
    ar.raw(m_mp_vol);
    ar.raw(m_mp_pan);
    ar.raw(m_start);
    ar.raw(m_end);
    ar.raw(m_rom_bank);
    ar.raw(m_mp_pos);
    ar.raw(m_audio_pos);
    ar.raw(m_audio_avail);
    ar.raw(m_mp_state);
    ar.raw(m_command);
    ar.raw(m_cycle_debt);
    ar.raw(m_timer_debt);
    ar.raw(m_resample_frac);
    ar.bytes(m_audio_buf, 1152 * 2);

    // The decoder exists only on a set that carries a DSB2.
    bool has_decoder = m_decoder != nullptr;
    ar.raw(has_decoder);
    if (has_decoder && m_decoder) {
        m_decoder->serialize(ar);
    }

    // Model 3 only, so Model 2 save states keep their layout.
    if (m_model3) {
        ar.raw(m_param);
        ar.bytes(m_volume, 2);
        ar.raw(m_stereo);
    }
    if (m_interpolate) {
        ar.bytes(m_prev, 2);
    }
}

void Dsb2::write_txd(u8 value)
{
    ++m_counters.bytes_from_host;
    m_uart.write_rxd(value);
}

void Dsb2::run(u32 host_cycles)
{
    if (!present()) {
        return;
    }

    // 1 kHz timer -> IRQ2 as MAME's HOLD_LINE (pulse, released after the CPU
    // can ack). It must not be held across slices: IRQ2 outranks the UART's
    // RxRDY on IRQ1, so a held timer masks every incoming command byte and the
    // firmware spins forever without one.
    m_timer_debt += host_cycles;
    bool timer_fired = false;
    while (m_timer_debt >= kTimerHostCycles) {
        m_timer_debt -= kTimerHostCycles;
        timer_fired = true;
    }

    m_cycle_debt += static_cast<u64>(host_cycles) * kCpuClockNumerator;
    u64 wanted = m_cycle_debt / kCpuClockDenominator;
    m_cycle_debt %= kCpuClockDenominator;

    if (wanted == 0) {
        return;
    }

    if (timer_fired) {
        m_cpu.set_irq_line(2, true);
    }
    m_uart.run(static_cast<u32>(wanted));
    static_cast<void>(m_cpu.run(static_cast<s32>(wanted)));
    if (timer_fired) {
        m_cpu.set_irq_line(2, false);  // HOLD_LINE: released after acknowledge
    }
}

// ---------------------------------------------------------------------------
// 68000 bus (big-endian). MAME's dsb2_map.
// ---------------------------------------------------------------------------

u8 Dsb2::read8(u32 address)
{
    address &= 0xffffff;

    if (address < kRomSize) {
        return address < m_program_rom.size() ? m_program_rom[address] : 0;
    }
    if (address >= kRamBase && address < kRamBase + kRamSize) {
        return m_ram[address - kRamBase];
    }
    // i8251 at 0xc00000, low byte lane (umask16 0x00ff -> odd byte addresses).
    if (address == 0xc00001) {
        return m_uart.read(0);
    }
    if (address == 0xc00003) {
        return m_uart.read(1);
    }
    // MPEG status: always ready.
    if (address == 0xe80001) {
        return 0x01;
    }
    return 0;
}

u16 Dsb2::read16(u32 address)
{
    return static_cast<u16>((read8(address) << 8) | read8(address + 1));
}

u32 Dsb2::read32(u32 address)
{
    return (static_cast<u32>(read16(address)) << 16) | read16(address + 2);
}

void Dsb2::write8(u32 address, u8 value)
{
    address &= 0xffffff;

    if (address >= kRamBase && address < kRamBase + kRamSize) {
        m_ram[address - kRamBase] = value;
        return;
    }
    if (address == 0xc00001) {
        m_uart.write(0, value);
        return;
    }
    if (address == 0xc00003) {
        m_uart.write(1, value);
        return;
    }
    if (address == 0xd00001) {
        system_control_w(value);
        return;
    }
    if (address == 0xe00003) {
        if (m_model3) {
            fifo_w_model3(value);
        } else {
            fifo_w(value);
        }
        return;
    }
    // ROM and unmapped writes are discarded.
}

void Dsb2::write16(u32 address, u16 value)
{
    write8(address, static_cast<u8>(value >> 8));
    write8(address + 1, static_cast<u8>(value));
}

void Dsb2::write32(u32 address, u32 value)
{
    write16(address, static_cast<u16>(value >> 16));
    write16(address + 2, static_cast<u16>(value));
}

// ---------------------------------------------------------------------------
// MPEG control, MAME's dsb2_device
// ---------------------------------------------------------------------------

void Dsb2::system_control_w(u8 data)
{
    // Bit 3 (inverted) is the MPEG ROM bank (A24), meaningful only with more
    // than 16 MB of MPEG ROM.
    m_rom_bank = (~data >> 3) & 1;
}

void Dsb2::play()
{
    m_mp_state = open_window(m_mp_start, m_mp_end) ? 1 : 0;
}

void Dsb2::stop()
{
    m_mp_state    = 0;
    m_audio_pos   = 0;
    m_audio_avail = 0;
}

// The Model 3 command set: MAME's address sequences plus opcodes that take a
// one-byte parameter, mostly speaker volumes (Daytona 2 sets its rear pair with
// B6/A7 and its front pair with B2/A3). dayto2pe plays its attract tune by
// writing A4 75.
void Dsb2::fifo_w_model3(u8 data)
{
    if (m_param != 0) {
        const u8 opcode = m_param;
        m_param = 0;
        switch (opcode) {
            case 0xa0: m_stereo = data != 0 ? 1 : 0; break;  // mono, left
            case 0xb1: m_stereo = data != 0 ? 2 : 0; break;  // mono, right
            case 0xa4: if (data == 0x75) { play(); } break;
            case 0xb4: if (data == 0x96) { stop(); } break;
            case 0xb0:                                       // left volume
            case 0xb6: m_volume[0] = data; break;            // rear left(?)
            case 0xa1:                                       // right volume
            case 0xa7: m_volume[1] = data; break;            // rear right(?)
            default: break;  // a3, a5, b2, b5: accepted and ignored
        }
        return;
    }

    if (m_command != Command::Idle) {
        const bool end_lo = m_command == Command::EndLo;
        fifo_w(data);
        if (end_lo) {
            m_stereo = 0;  // each end address resets to stereo
        }
        return;
    }

    switch (data) {
        case 0x14: case 0x15: m_command = Command::StartHi; break;
        case 0x24: case 0x25: m_command = Command::EndHi; break;
        case 0x74: case 0x75: play(); break;
        case 0x84: case 0x85: stop(); break;
        case 0xa0: case 0xa1: case 0xa3: case 0xa4: case 0xa5: case 0xa7:
        case 0xb0: case 0xb1: case 0xb2: case 0xb4: case 0xb5: case 0xb6:
            m_param = data;
            break;
        default: break;
    }
}

void Dsb2::fifo_w(u8 data)
{

    switch (m_command) {
        case Command::StartHi:
            m_start = (m_start & 0x00ffff) | (static_cast<u32>(data) << 16);
            m_command = Command::StartMid;
            break;
        case Command::StartMid:
            m_start = (m_start & 0xff00ff) | (static_cast<u32>(data) << 8);
            m_command = Command::StartLo;
            break;
        case Command::StartLo:
            // Written while playing, it is where the stream loops to at its end.
            m_start = (m_start & 0xffff00) | data;
            if (m_mp_state == 0) {
                m_mp_start = m_start;
            } else {
                m_mp_state = 2;
            }
            m_command = Command::Idle;
            break;
        case Command::EndHi:
            m_end = (m_end & 0x00ffff) | (static_cast<u32>(data) << 16);
            m_command = Command::EndMid;
            break;
        case Command::EndMid:
            m_end = (m_end & 0xff00ff) | (static_cast<u32>(data) << 8);
            m_command = Command::EndLo;
            break;
        case Command::EndLo:
            m_end = (m_end & 0xffff00) | data;
            if (m_mp_state == 0) {
                m_mp_end = m_end;
            }
            m_command = Command::Idle;
            break;
        case Command::Idle:
        default:
            if ((data & 0xfe) == kCmdStart) {
                m_command = Command::StartHi;
            } else if ((data & 0xfe) == kCmdEnd) {
                m_command = Command::EndHi;
            } else if ((data & 0xfe) == kCmdPlay) {
                m_mp_state = open_window(m_mp_start, m_mp_end) ? 1 : 0;
            } else if ((data & 0xfe) == kCmdStop) {
                m_mp_state    = 0;
                m_audio_pos   = 0;
                m_audio_avail = 0;
            }
            break;
    }
}

bool Dsb2::open_window(u32 start, u32 end)
{
    const u32 rom_bytes = static_cast<u32>(m_mpeg_rom.size());
    start &= 0xffffff;
    end &= 0xffffff;
    if (rom_bytes > 0x1000000 && m_rom_bank != 0) {
        start |= 0x1000000;
        end |= 0x1000000;
    }
    // Guard against an out-of-range window: the decoder's bit reader would
    // otherwise run off the end of the ROM buffer.
    if (start >= rom_bytes || end > rom_bytes || end <= start) {
        SM2_WARN("dsb2: refusing play window start=%07x end=%07x (romsz=%07x)",
                 start, end, rom_bytes);
        m_audio_pos   = 0;
        m_audio_avail = 0;
        return false;
    }
    m_mp_start = start;
    m_mp_end   = end;
    m_mp_pos   = static_cast<s32>(start) * 8;
    return true;
}

void Dsb2::decode_next()
{
    if (m_mp_state == 0 || !m_decoder) {
        m_audio_avail = 0;
        return;
    }

    int sample_rate    = 0;
    int channel_count  = 0;
    int output_samples = 0;
    bool ok = m_decoder->decode_buffer(m_mp_pos, static_cast<int>(m_mp_end) * 8,
                                       m_audio_buf, output_samples, sample_rate,
                                       channel_count);
    if (!ok && m_mp_state == 2 && open_window(m_start, m_end)) {
        ok = m_decoder->decode_buffer(m_mp_pos, static_cast<int>(m_mp_end) * 8,
                                      m_audio_buf, output_samples, sample_rate,
                                      channel_count);
    }
    if (ok) {
        m_audio_pos   = 0;
        m_audio_avail = output_samples;
        ++m_counters.mpeg_frames;
    } else {
        m_mp_state    = 0;
        m_audio_avail = 0;
    }
}

void Dsb2::mix(s16* dst, u32 frames, u32 out_rate)
{
    if (!present() || out_rate == 0) {
        return;
    }

    for (u32 i = 0; i < frames; ++i) {
        while (m_audio_pos >= m_audio_avail) {
            decode_next();
            if (m_audio_avail == 0) {
                return;
            }
        }

        s16 lraw = m_audio_buf[m_audio_pos * 2];
        s16 rraw = m_audio_buf[m_audio_pos * 2 + 1];
        if (m_interpolate) {
            // Linear interpolation from the previous decoded sample.
            const s64 t = static_cast<s64>(m_resample_frac);
            lraw = static_cast<s16>(m_prev[0] + (lraw - m_prev[0]) * t / out_rate);
            rraw = static_cast<s16>(m_prev[1] + (rraw - m_prev[1]) * t / out_rate);
        }

        s32 l;
        s32 r;
        if (m_model3) {
            // Per-channel volume and mono routing, scaled to MAME's 32768*128 full scale.
            s32 src_l = lraw, src_r = rraw, vol_l = m_volume[0], vol_r = m_volume[1];
            if (m_stereo == 1) {
                src_r = lraw;
                vol_r = vol_l;
            } else if (m_stereo == 2) {
                src_l = rraw;
                vol_l = vol_r;
            }
            l = src_l * vol_l * 128 / 255;
            r = src_r * vol_r * 128 / 255;
        } else switch (m_mp_pan) {
            case 1:
                l = r = static_cast<s32>(lraw) * static_cast<s32>(m_mp_vol);
                break;
            case 2:
                l = r = static_cast<s32>(rraw) * static_cast<s32>(m_mp_vol);
                break;
            default:
                l = static_cast<s32>(lraw) * static_cast<s32>(m_mp_vol);
                r = static_cast<s32>(rraw) * static_cast<s32>(m_mp_vol);
                break;
        }
        l = ((l >> 7) * m_gain >> 8) * m_music >> 8;
        r = ((r >> 7) * m_gain >> 8) * m_music >> 8;

        const s32 ol = static_cast<s32>(dst[i * 2]) + l;
        const s32 orr = static_cast<s32>(dst[i * 2 + 1]) + r;
        dst[i * 2]     = static_cast<s16>(std::clamp(ol, -32768, 32767));
        dst[i * 2 + 1] = static_cast<s16>(std::clamp(orr, -32768, 32767));

        m_resample_frac += kDecoderRate;
        while (m_resample_frac >= out_rate) {
            m_resample_frac -= out_rate;
            m_prev[0] = m_audio_buf[m_audio_pos * 2];
            m_prev[1] = m_audio_buf[m_audio_pos * 2 + 1];
            ++m_audio_pos;
        }
    }
}

}  // namespace sm2::hw
