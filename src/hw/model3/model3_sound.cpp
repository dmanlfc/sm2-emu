// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 sound board. See model3_sound.h.
//
// Derived from Supermodel's Src/Model3/SoundBoard.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c) 2011
// Bart Trzynadlowski, Nik Henson. Port handling and mixing from Supermodel's
// Src/Model3/Model3.cpp, Src/Model3/DSB.cpp and Src/OSD/SDL/Audio.cpp,
// Copyright (c) 2003-2026 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Sample ROM banking is not fully understood. The low 2 MB is presumed
// unbanked (MAME). Sound test captures from dayto2pe suggest that when
// (400001) & 0x10 is set:
//
//       ROM A00000-DFFFFF -> A00000-DFFFFF
//       ROM E00000-FFFFFF -> E00000-FFFFFF
//
//   and when it is clear (the default, upper 6 MB of ROM):
//
//       ROM 200000-5FFFFF -> A00000-DFFFFF
//       ROM 600000-7FFFFF -> E00000-FFFFFF
//
// As implemented, the 8 MB window at 0x800000 maps the low or high 8 MB of a
// 16 MB sample space.

#include "hw/model3/model3_sound.h"

#include "core/archive.h"
#include "core/log.h"

#include <algorithm>

namespace sm2::hw::model3 {
namespace {

constexpr u32 kAddressMask = 0x00ffffff;  // 24 address lines

constexpr u32 kRamSize       = 0x100000;  // 1 MB per SCSP
constexpr u32 kProgramMask   = 0x07ffff;  // 512 KB, mirrored through 0x6fffff
constexpr u32 kSampleWindow  = 0x800000;  // 8 MB at 0x800000
constexpr u32 kSampleSpace   = 0x1000000; // 16 MB, the high half banked in
constexpr u32 kScspRegMask   = 0x000fff;  // registers mirrored through each 1 MB
constexpr u32 kBankRegister  = 0x400001;

/// Both SCSPs on the 45.1584 MHz crystal, halved.
constexpr u32 kScspClock = 22'579'200;

/// The DSBs count Model 2's 25 MHz host cycles: 25 / 11.2896 = 15625/7056.
constexpr u64 kDsbClockNumerator   = 15625;
constexpr u64 kDsbClockDenominator = 7056;

/// Samples between DSB timeslices (one command byte each).
constexpr u32 kChunkFrames = 8;

/// Below the SCSP MIDI FIFO's 31-byte capacity.
constexpr u32 kScspFifoTarget = 16;

/// Longest run of audio kept while nothing drains it.
constexpr usize kMaxPendingFrames = 4 * 800;

constexpr usize kResetVectorBytes = 16;

}  // namespace

// ---------------------------------------------------------------------------
// Small pieces
// ---------------------------------------------------------------------------

bool Model3Sound::Fifo::push(u8 value)
{
    if (count == data.size()) {
        return false;
    }
    data[(head + count) % data.size()] = value;
    ++count;
    return true;
}

u8 Model3Sound::Fifo::pop()
{
    const u8 value = data[head];
    head = (head + 1) % static_cast<u32>(data.size());
    --count;
    return value;
}

u8 Model3Sound::ScspRam::scsp_read_byte(u32 address)
{
    return (*m_ram)[address & (kRamSize - 1)];
}

u16 Model3Sound::ScspRam::scsp_read_word(u32 address)
{
    address &= (kRamSize - 1) & ~1u;
    return static_cast<u16>(((*m_ram)[address] << 8) | (*m_ram)[address + 1]);
}

void Model3Sound::ScspRam::scsp_write_word(u32 address, u16 value)
{
    address &= (kRamSize - 1) & ~1u;
    (*m_ram)[address]     = static_cast<u8>(value >> 8);
    (*m_ram)[address + 1] = static_cast<u8>(value);
}

// ---------------------------------------------------------------------------
// Construction and configuration
// ---------------------------------------------------------------------------

Model3Sound::Model3Sound()
    : m_cpu(*this),
      m_ram1(kRamSize, 0),
      m_ram2(kRamSize, 0),
      m_ram1_view(m_ram1),
      m_ram2_view(m_ram2),
      m_master(m_ram1_view, kScspClock),
      m_slave(m_ram2_view, kScspClock)
{
    m_pending.reserve(kMaxPendingFrames * 2);
    m_front.reserve(kChunkFrames * 2);
    m_rear.reserve(kChunkFrames * 2);

    // Only the master drives the 68000's IPL; the slave has no handler.
    m_master.set_irq_handler([this](int level, bool assert) {
        if (level <= 0) {
            m_cpu.clear_irq_lines();
            return;
        }
        m_cpu.set_irq_line(level, assert);
    });

    // 68000 MIDI out: nothing listens.
    m_master.set_midi_out_handler([this](u8) { ++m_counters.midi_out_bytes; });
}

Model3Sound::~Model3Sound() = default;

void Model3Sound::attach(std::span<const u8> program_rom, std::span<const u8> samples)
{
    m_program_rom = program_rom;
    m_samples     = samples;
}

void Model3Sound::attach_mpeg(rom::Model3MpegBoard board, std::span<const u8> program,
                              std::span<const u8> music)
{
    m_mpeg = program.empty() ? rom::Model3MpegBoard::None : board;
    m_dsb1.attach({}, {});
    m_dsb2.attach({}, {});
    switch (m_mpeg) {
        case rom::Model3MpegBoard::Dsb1:
            m_dsb1.set_interpolate(true);
            m_dsb1.attach(program, music);
            break;
        case rom::Model3MpegBoard::Dsb2:
            m_dsb2.set_model3_commands(true);
            m_dsb2.set_interpolate(true);
            m_dsb2.attach(program, music);
            break;
        case rom::Model3MpegBoard::None:
            break;
    }
}

void Model3Sound::set_host_clock(u32 hz)
{
    m_host_hz    = hz != 0 ? hz : 66'000'000;
    m_cycle_debt = 0;
}

void Model3Sound::set_sound_volume(u32 percent)
{
    m_sound_gain = static_cast<s32>(std::min<u32>(percent, 200) * 256 / 100);
}

void Model3Sound::set_music_volume(u32 percent)
{
    const u32 clamped = std::min<u32>(percent, 200);
    m_dsb1.set_music_volume(clamped);
    m_dsb2.set_music_volume(clamped);
}

void Model3Sound::reset()
{
    std::fill(m_ram1.begin(), m_ram1.end(), u8{0});
    std::fill(m_ram2.begin(), m_ram2.end(), u8{0});

    // Vector table: start of program ROM copied to RAM 1.
    const usize copy = std::min(kResetVectorBytes, m_program_rom.size());
    std::copy_n(m_program_rom.begin(), copy, m_ram1.begin());

    m_bank       = 0;
    m_control    = 0;
    m_scsp_fifo  = Fifo{};
    m_dsb_fifo   = Fifo{};
    m_cycle_debt   = 0;
    m_cycle_budget = 0;
    m_sample_phase = kCyclesPerSample;  // the first sample is due at once
    m_dsb_debt     = 0;
    m_counters     = Counters{};
    m_front.clear();
    m_rear.clear();
    m_pending.clear();
    m_quad.clear();

    m_master.reset();
    m_slave.reset();
    m_dsb1.reset();
    m_dsb2.reset();
    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// The PowerPC side
// ---------------------------------------------------------------------------
// The board's ports at offset 0x08 of the PowerPC's 0xF0 I/O space.

void Model3Sound::write_port(u32 offset, u8 value)
{
    switch (offset & 0xf) {
        case 0x0: write_midi(value); break;
        case 0x4: write_control(value); break;
        default: break;
    }
}

u8 Model3Sound::read_port(u32 offset) const
{
    switch (offset & 0xf) {
        // Possibly Magical Truck Adventure's region lock (0 locked, 1 unlocked).
        case 0x0: return 0x00;
        // Magical Truck Adventure's country check.
        case 0x4: return 0x83;
        default: return 0x00;
    }
}

void Model3Sound::write_midi(u8 value)
{
    ++m_counters.midi_bytes;

    if (!m_scsp_fifo.push(value)) {
        ++m_counters.fifo_overflows;
    }
    if (m_mpeg != rom::Model3MpegBoard::None && !m_dsb_fifo.push(value)) {
        ++m_counters.fifo_overflows;
    }

    if (m_irq_handler) {
        m_irq_handler(false);
    }
}

void Model3Sound::write_control(u8 value)
{
    ++m_counters.control_writes;
    m_control = value;
    if (!midi_irq_enabled() && m_irq_handler) {
        m_irq_handler(false);
    }
}

void Model3Sound::midi_irq_pulse()
{
    if (!midi_irq_enabled()) {
        return;
    }
    ++m_counters.irq_pulses;
    if (m_irq_handler) {
        m_irq_handler(true);
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void Model3Sound::run(u32 host_cycles)
{
    if (!present()) {
        if (!m_warned_no_program) {
            m_warned_no_program = true;
            SM2_WARN("model3 sound: no 'sound_program' region, the sound board will stay silent");
        }
        return;
    }
    m_cycle_debt += static_cast<u64>(host_cycles) * kCpuClock;
    m_cycle_budget += static_cast<s64>(m_cycle_debt / m_host_hz);
    m_cycle_debt %= m_host_hz;
    advance();
}

void Model3Sound::run_samples(u32 samples)
{
    if (!present()) {
        return;
    }
    m_cycle_budget += static_cast<s64>(samples) * kCyclesPerSample;
    advance();
}

void Model3Sound::advance()
{
    // SCSPs run one sample ahead of the 68000, as on Model 2, so a timer
    // interrupts at the start of the sample it expires in.
    while (m_cycle_budget > 0) {
        if (m_sample_phase >= kCyclesPerSample) {
            generate_sample();
            m_sample_phase -= kCyclesPerSample;
            continue;
        }
        const s64 slice = std::min<s64>(m_cycle_budget,
                                        static_cast<s64>(kCyclesPerSample - m_sample_phase));
        const s32 used = m_cpu.run(static_cast<s32>(slice));
        m_sample_phase += static_cast<u64>(used);
        m_cycle_budget -= used;
    }

    // Nothing staged between calls, so states need not carry it.
    if (!m_front.empty()) {
        flush_chunk();
    }

    const usize limit = kMaxPendingFrames * 2;
    if (m_pending.size() > limit) {
        const usize excess = m_pending.size() - limit;
        m_counters.samples_dropped += excess / 2;
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

void Model3Sound::feed_scsp()
{
    while (m_scsp_fifo.count != 0 && m_master.midi_in_pending() < kScspFifoTarget) {
        m_master.midi_in(m_scsp_fifo.pop());
    }
}

void Model3Sound::generate_sample()
{
    feed_scsp();

    s16 master[2];
    s16 slave[2];
    m_master.generate(master, 1);
    m_slave.generate(slave, 1);
    ++m_counters.samples;

    // DAC18B: hw::Scsp scales 18-bit output to 16 bits, but the games are
    // mastered for the unscaled level, so restore the 4x. The sound volume
    // applies to the SCSPs only.
    const s32 master_gain = m_sound_gain * (m_master.dac18() ? 4 : 1);
    const s32 slave_gain  = m_sound_gain * (m_slave.dac18() ? 4 : 1);
    const auto scale = [](s16 v, s32 gain) {
        return static_cast<s16>(std::clamp((v * gain) >> 8, -32768, 32767));
    };
    m_front.push_back(scale(master[0], master_gain));
    m_front.push_back(scale(master[1], master_gain));
    m_rear.push_back(scale(slave[0], slave_gain));
    m_rear.push_back(scale(slave[1], slave_gain));

    if (m_front.size() >= kChunkFrames * 2) {
        flush_chunk();
    }
}

void Model3Sound::flush_chunk()
{
    const u32 frames = static_cast<u32>(m_front.size() / 2);

    // DSB music goes to the front pair.
    if (m_mpeg != rom::Model3MpegBoard::None) {
        m_dsb_debt += static_cast<u64>(frames) * kCyclesPerSample * kDsbClockNumerator;
        const u32 host = static_cast<u32>(m_dsb_debt / kDsbClockDenominator);
        m_dsb_debt %= kDsbClockDenominator;

        if (m_mpeg == rom::Model3MpegBoard::Dsb1) {
            if (m_dsb_fifo.count != 0 && m_dsb1.ready_for_byte()) {
                m_dsb1.write_txd(m_dsb_fifo.pop());
            }
            m_dsb1.run(host);
            m_dsb1.mix(m_front.data(), frames, kSampleRate);
        } else {
            if (m_dsb_fifo.count != 0 && m_dsb2.ready_for_byte()) {
                m_dsb2.write_txd(m_dsb_fifo.pop());
            }
            m_dsb2.run(host);
            m_dsb2.mix(m_front.data(), frames, kSampleRate);
        }
    }

    if (m_keep_quad) {
        for (u32 i = 0; i < frames; ++i) {
            m_quad.push_back(m_front[i * 2]);
            m_quad.push_back(m_front[i * 2 + 1]);
            m_quad.push_back(m_rear[i * 2]);
            m_quad.push_back(m_rear[i * 2 + 1]);
        }
    }

    mix_down(frames);
    m_front.clear();
    m_rear.clear();
}

void Model3Sound::mix_down(u32 frames)
{
    // The stereo fold is (front + rear) / 2 per side. QuadMix puts the master
    // in the centre and keeps the slave's stereo.
    for (u32 i = 0; i < frames; ++i) {
        const s32 fl = m_front[i * 2];
        const s32 fr = m_front[i * 2 + 1];
        const s32 rl = m_rear[i * 2];
        const s32 rr = m_rear[i * 2 + 1];
        s32 l;
        s32 r;
        if (m_layout == rom::Model3Audio::QuadMix) {
            const s32 centre = (fl + fr) / 4;
            l = centre + rl / 2;
            r = centre + rr / 2;
        } else {
            l = (fl + rl) / 2;
            r = (fr + rr) / 2;
        }
        m_pending.push_back(static_cast<s16>(std::clamp(l, -32768, 32767)));
        m_pending.push_back(static_cast<s16>(std::clamp(r, -32768, 32767)));
    }
}

// ---------------------------------------------------------------------------
// The 68000's bus
// ---------------------------------------------------------------------------
// The 68000 address map, decoded on address bits 20-23:
//
//   000000-0FFFFF  RAM 1 (master SCSP)
//   100000-1FFFFF  master SCSP registers (4 KB, mirrored)
//   200000-2FFFFF  RAM 2 (slave SCSP)
//   300000-3FFFFF  slave SCSP registers
//   400001         sample bank register, write only
//   600000-6FFFFF  program ROM (512 KB, mirrored)
//   800000-FFFFFF  sample ROM window, banked

u8 Model3Sound::sample_byte(u32 offset) const
{
    // A sample ROM of 8 MB or less is mirrored.
    offset &= kSampleSpace - 1;
    if (m_samples.size() <= kSampleWindow) {
        offset &= kSampleWindow - 1;
    }
    return offset < m_samples.size() ? m_samples[offset] : u8{0};
}

void Model3Sound::bank_write(u8 value)
{
    ++m_counters.bank_writes;
    m_bank = value;
}

u16 Model3Sound::read16(u32 address)
{
    address &= kAddressMask & ~1u;
    switch ((address >> 20) & 0xf) {
        case 0x0:
            return static_cast<u16>((m_ram1[address] << 8) | m_ram1[address + 1]);
        case 0x1:
            return m_master.read((address & kScspRegMask) >> 1);
        case 0x2: {
            const u32 a = address & (kRamSize - 1);
            return static_cast<u16>((m_ram2[a] << 8) | m_ram2[a + 1]);
        }
        case 0x3:
            return m_slave.read((address & kScspRegMask) >> 1);
        case 0x6: {
            const u32 a = address & kProgramMask;
            if (a + 1 >= m_program_rom.size()) {
                return 0;
            }
            return static_cast<u16>((m_program_rom[a] << 8) | m_program_rom[a + 1]);
        }
        case 0x8: case 0x9: case 0xa: case 0xb:
        case 0xc: case 0xd: case 0xe: case 0xf: {
            const u32 bank = (m_bank & 0x10) != 0 ? kSampleWindow : 0;
            const u32 a    = bank + (address & (kSampleWindow - 1));
            return static_cast<u16>((sample_byte(a) << 8) | sample_byte(a + 1));
        }
        default:
            ++m_counters.unmapped_reads;
            return 0;
    }
}

u8 Model3Sound::read8(u32 address)
{
    const u16 word = read16(address & ~1u);
    return (address & 1) != 0 ? static_cast<u8>(word) : static_cast<u8>(word >> 8);
}

u32 Model3Sound::read32(u32 address)
{
    const u32 high = read16(address);
    const u32 low  = read16(address + 2);
    return (high << 16) | low;
}

void Model3Sound::write16(u32 address, u16 value)
{
    address &= kAddressMask & ~1u;
    switch ((address >> 20) & 0xf) {
        case 0x0:
            m_ram1[address]     = static_cast<u8>(value >> 8);
            m_ram1[address + 1] = static_cast<u8>(value);
            return;
        case 0x1:
            m_master.write((address & kScspRegMask) >> 1, value, 0xffff);
            return;
        case 0x2: {
            const u32 a = address & (kRamSize - 1);
            m_ram2[a]     = static_cast<u8>(value >> 8);
            m_ram2[a + 1] = static_cast<u8>(value);
            return;
        }
        case 0x3:
            m_slave.write((address & kScspRegMask) >> 1, value, 0xffff);
            return;
        default:
            // A word write drives the low byte lane.
            if (address == (kBankRegister & ~1u)) {
                bank_write(static_cast<u8>(value));
                return;
            }
            ++m_counters.unmapped_writes;
            return;
    }
}

void Model3Sound::write8(u32 address, u8 value)
{
    address &= kAddressMask;
    switch ((address >> 20) & 0xf) {
        case 0x0:
            m_ram1[address] = value;
            return;
        case 0x2:
            m_ram2[address & (kRamSize - 1)] = value;
            return;
        case 0x1:
        case 0x3: {
            // A byte write to a 16-bit register must not disturb the other half.
            Scsp& scsp = ((address >> 20) & 0xf) == 0x1 ? m_master : m_slave;
            const bool low  = (address & 1) != 0;
            const u16  mask = low ? u16{0x00ff} : u16{0xff00};
            const u16  wide = low ? static_cast<u16>(value) : static_cast<u16>(value << 8);
            scsp.write((address & kScspRegMask) >> 1, wide, mask);
            return;
        }
        default:
            if (address == kBankRegister) {
                bank_write(value);
                return;
            }
            ++m_counters.unmapped_writes;
            return;
    }
}

void Model3Sound::write32(u32 address, u32 value)
{
    write16(address, static_cast<u16>(value >> 16));
    write16(address + 2, static_cast<u16>(value));
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------

void Model3Sound::serialize(Archive& ar)
{
    ar.bytes(m_ram1.data(), m_ram1.size());
    ar.bytes(m_ram2.data(), m_ram2.size());
    ar.raw(m_bank);
    m_cpu.serialize(ar);
    m_master.serialize(ar);
    m_slave.serialize(ar);
    // Only the board the set has: the other is inert and its state constant.
    if (m_mpeg == rom::Model3MpegBoard::Dsb1) {
        m_dsb1.serialize(ar);
    } else if (m_mpeg == rom::Model3MpegBoard::Dsb2) {
        m_dsb2.serialize(ar);
    }
    ar.raw(m_control);
    ar.raw(m_scsp_fifo);
    ar.raw(m_dsb_fifo);
    ar.raw(m_cycle_debt);
    ar.raw(m_cycle_budget);
    ar.raw(m_sample_phase);
    ar.raw(m_dsb_debt);
}

}  // namespace sm2::hw::model3
