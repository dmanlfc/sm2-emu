// SPDX-License-Identifier: BSD-3-Clause
//
// See dsb2.h. Ported from MAME's src/mame/sega/dsb2.cpp (BSD-3-Clause).

#include "hw/dsb2.h"

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
    m_playing  = false;
    m_command  = Command::Idle;
    m_cycle_debt    = 0;
    m_timer_debt    = 0;
    m_resample_frac = 0;
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
        fifo_w(value);
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
    // Bit 3 (inverted) is the MPEG ROM bank (A24). Only meaningful with more
    // than 16 MB of MPEG ROM, which no Model 2 DSB2 title has, so this is
    // effectively always bank 0 here -- kept for fidelity.
    m_rom_bank = (~data >> 3) & 1;
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
            m_start = (m_start & 0xffff00) | data;
            m_mp_start = m_start;
            m_command  = Command::Idle;
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
            m_mp_end  = m_end;
            m_command = Command::Idle;
            break;
        case Command::Idle:
        default:
            if ((data & 0xfe) == kCmdStart) {
                m_command = Command::StartHi;
            } else if ((data & 0xfe) == kCmdEnd) {
                m_command = Command::EndHi;
            } else if ((data & 0xfe) == kCmdPlay) {
                const u32 rom_bytes = static_cast<u32>(m_mpeg_rom.size());
                u32 start = m_mp_start & 0xffffff;
                u32 end   = m_mp_end & 0xffffff;
                if (rom_bytes > 0x1000000 && m_rom_bank != 0) {
                    start |= 0x1000000;
                    end |= 0x1000000;
                }
                // Guard against an out-of-range window: the decoder's bit reader
                // would otherwise run off the end of the ROM buffer.
                if (start >= rom_bytes || end > rom_bytes || end <= start) {
                    SM2_WARN("dsb2: refusing play window start=%07x end=%07x "
                             "(romsz=%07x)",
                             start, end, rom_bytes);
                    m_playing     = false;
                    m_audio_pos   = 0;
                    m_audio_avail = 0;
                } else {
                    m_mp_start = start;
                    m_mp_end   = end;
                    m_mp_pos   = static_cast<s32>(m_mp_start) * 8;
                    m_playing  = true;
                }
            } else if ((data & 0xfe) == kCmdStop) {
                m_playing     = false;
                m_audio_pos   = 0;
                m_audio_avail = 0;
            }
            break;
    }
}

void Dsb2::decode_next()
{
    if (!m_playing || !m_decoder) {
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
    } else {
        m_playing     = false;  // DSB2 does not loop (MAME leaves it stopped)
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

        const s16 lraw = m_audio_buf[m_audio_pos * 2];
        const s16 rraw = m_audio_buf[m_audio_pos * 2 + 1];

        s32 l;
        s32 r;
        switch (m_mp_pan) {
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
        l >>= 7;
        r >>= 7;

        const s32 ol = static_cast<s32>(dst[i * 2]) + l;
        const s32 orr = static_cast<s32>(dst[i * 2 + 1]) + r;
        dst[i * 2]     = static_cast<s16>(std::clamp(ol, -32768, 32767));
        dst[i * 2 + 1] = static_cast<s16>(std::clamp(orr, -32768, 32767));

        m_resample_frac += kDecoderRate;
        while (m_resample_frac >= out_rate) {
            m_resample_frac -= out_rate;
            ++m_audio_pos;
        }
    }
}

}  // namespace sm2::hw
