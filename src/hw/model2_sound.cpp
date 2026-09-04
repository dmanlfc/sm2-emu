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
// See model2_sound.h.

#include "hw/model2_sound.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------
// MAME's model2_snd, region for region.

constexpr u32 kAddressMask = 0x00ffffff;  ///< The 68000 has 24 address lines.

constexpr u32 kRamBase     = 0x000000;
constexpr u32 kRamSize     = 0x080000;  // 512 KB, shared with the SCSP
constexpr u32 kScspBase    = 0x100000;
constexpr u32 kScspSize    = 0x001000;
constexpr u32 kSndCtrl     = 0x400000;  // sample banking, write only
constexpr u32 kProgramBase = 0x600000;
constexpr u32 kProgramSize = 0x080000;
constexpr u32 kSamplesBase = 0x800000;  // samples + 0x000000, 2 MB
constexpr u32 kSamplesSize = 0x200000;
constexpr u32 kBank4Base   = 0xa00000;  // samples + 0x200000, 4 MB
constexpr u32 kBank4Size   = 0x400000;
constexpr u32 kBank5Base   = 0xe00000;  // samples + 0x600000, 2 MB
constexpr u32 kBank5Size   = 0x200000;

/// The SCSP's own view of memory is 20 bits wide, of which the Model 2 sound
/// board wires up the low 512 KB.
constexpr u32 kScspAddressMask = 0x000fffff;

/// 68000 clock over host clock, exactly. 45.1584 MHz / 4 over 25 MHz reduces to
/// 7056/15625, and 5^6 shares no factor with 2^4 * 3^2 * 7^2, so this is lowest
/// terms and the remainder has to be carried rather than rounded away.
/// Note: MAME applies a 1-cycle wait state to every RAM and SCSP register
/// access, effectively halving the 68000's throughput. The numerator here
/// accounts for that.
constexpr u64 kCpuClockNumerator   = 3528;  // 7056 / 2
constexpr u64 kCpuClockDenominator = 15625;

/// Sample rate over host clock, exactly: 44100/25000000 reduces to 441/250000,
/// and 3^2 * 7^2 shares no factor with 2^4 * 5^6.
constexpr u64 kSampleNumerator   = 441;
constexpr u64 kSampleDenominator = 250000;

/// The SCSP is clocked at half the 45.1584 MHz crystal on the video board, which
/// divided by 512 is 44100 Hz exactly.
constexpr u32 kScspClock = 22'579'200;

/// Longest run of audio kept while nothing is draining it, in frames. Four video
/// frames is far more than a healthy consumer needs and small enough that a
/// headless run does not accumulate a gigabyte of samples.
constexpr usize kMaxPendingFrames = 4 * 800;

/// The reset vector, 16 bytes, is copied from the start of the sound ROM into
/// RAM. MAME's reset_model2_scsp does the same, with the note that the hardware
/// presumably arranges it in some way that amounts to this.
constexpr usize kResetVectorBytes = 16;

}  // namespace

Model2Sound::Model2Sound() : m_cpu(*this), m_scsp(*this, kScspClock)
{
    m_ram.assign(kRamSize, 0);
    m_pending.reserve(kMaxPendingFrames * 2);

    // The SCSP's interrupts go to the sound 68000, and only there: on Model 2 the
    // main_irq callback, which on a Saturn would reach the SH-2, is left unwired,
    // as MAME leaves it. The host's sound interrupt comes from the UART instead.
    m_scsp.set_irq_handler([this](int level, bool assert) {
        if (level <= 0) {
            m_cpu.clear_irq_lines();
            return;
        }
        m_cpu.set_irq_line(level, assert);
    });
}

Model2Sound::~Model2Sound() = default;

void Model2Sound::attach(std::span<const u8> program_rom, std::span<const u8> samples)
{
    m_program_rom = program_rom;
    m_samples     = samples;
}

void Model2Sound::attach_dsb(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg)
{
    m_dsb.attach(dsb_program, dsb_mpeg);
}

void Model2Sound::set_midi_out_handler(Scsp::MidiOutHandler handler)
{
    m_scsp.set_midi_out_handler(std::move(handler));
}

void Model2Sound::midi_in(u8 value)
{
    // The host serial link drives both the SCSP's MIDI port and, on the sets
    // that carry one, the DSB. MAME fans the same txd out to both.
    m_scsp.midi_in(value);
    m_dsb.write_txd(value);
}

void Model2Sound::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});

    m_bank4_offset = 0x200000;
    m_bank5_offset = 0x600000;

    m_cycle_debt      = 0;
    m_cycle_overshoot = 0;
    m_sample_debt     = 0;
    m_counters        = Counters{};
    m_pending.clear();

    m_scsp.reset();
    m_dsb.reset();

    // Seed the vector table. Without this the 68000 would fetch its stack
    // pointer and entry point out of cleared RAM and immediately run off into
    // nothing.
    const usize copy = std::min(kResetVectorBytes, m_program_rom.size());
    if (copy != 0) {
        std::copy_n(m_program_rom.begin(), copy, m_ram.begin());
    }

    m_cpu.reset();
}

void Model2Sound::run(u32 host_cycles)
{
    if (!present()) {
        if (!m_warned_no_program) {
            m_warned_no_program = true;
            SM2_WARN("sound: no 'audiocpu' region, the sound board will stay silent");
        }
        return;
    }

    // Audio first, then the CPU. The SCSP's timers advance with the samples, so
    // generating this interval's audio before running the 68000 over the same
    // interval means a timer interrupt is visible to the program within the same
    // slice rather than the next one. MAME gets the same effect from the other
    // direction, by catching the stream up whenever the program touches a
    // register.
    generate_audio(host_cycles);

    m_cycle_debt += static_cast<u64>(host_cycles) * kCpuClockNumerator;
    u64 wanted = m_cycle_debt / kCpuClockDenominator;
    m_cycle_debt %= kCpuClockDenominator;

    // Instructions are not interruptible, so the last one of a slice runs past
    // the end of it. Left uncorrected that overshoot compounds: at one call per
    // scanline it made the board run nearly three percent fast, which is a
    // quarter-tone sharp and audible. Paying it back out of the next slice keeps
    // the long-run rate exact.
    if (m_cycle_overshoot >= wanted) {
        m_cycle_overshoot -= wanted;
        return;
    }
    wanted -= m_cycle_overshoot;
    m_cycle_overshoot = 0;

    const s32 used = m_cpu.run(static_cast<s32>(wanted));
    if (used > 0 && static_cast<u64>(used) > wanted) {
        m_cycle_overshoot = static_cast<u64>(used) - wanted;
    }
}

void Model2Sound::generate_audio(u32 host_cycles)
{
    m_sample_debt += static_cast<u64>(host_cycles) * kSampleNumerator;
    const u64 frames = m_sample_debt / kSampleDenominator;
    m_sample_debt %= kSampleDenominator;

    if (frames == 0) {
        return;
    }

    // Nothing draining the buffer means a headless run. The SCSP still has to be
    // stepped, because that is where its timers and envelopes advance, so the
    // samples are generated and then the oldest are dropped.
    const usize offset = m_pending.size();
    m_pending.resize(offset + static_cast<usize>(frames) * 2);
    m_scsp.generate(m_pending.data() + offset, static_cast<u32>(frames));

    // The DSB (music board, DSB titles only) runs its Z80 over the same host
    // interval and mixes its decoded MPEG audio into the frames the SCSP just
    // produced. Inert -- and free -- for the sets without one.
    if (m_dsb.present()) {
        m_dsb.run(host_cycles);
        m_dsb.mix(m_pending.data() + offset, static_cast<u32>(frames),
                  m_scsp.sample_rate());
    }

    const usize limit = kMaxPendingFrames * 2;
    if (m_pending.size() > limit) {
        const usize excess = m_pending.size() - limit;
        m_counters.samples_dropped += excess / 2;
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

Model2Sound::Window Model2Sound::resolve(u32 address)
{
    // A helper for the read-only sample windows, which differ only in where they
    // start in the sample ROM.
    const auto sample_window = [this](u32 offset, u32 span) -> Window {
        if (offset >= m_samples.size()) {
            return {};
        }
        Window result;
        result.base     = const_cast<u8*>(m_samples.data()) + offset;
        result.size     = std::min<usize>(span, m_samples.size() - offset);
        result.writable = false;
        return result;
    };

    if (address < kRamBase + kRamSize) {
        return Window{m_ram.data() + address, kRamSize - address, true};
    }

    if (address >= kProgramBase && address < kProgramBase + kProgramSize) {
        const u32 offset = address - kProgramBase;
        if (offset >= m_program_rom.size()) {
            return {};
        }
        return Window{const_cast<u8*>(m_program_rom.data()) + offset,
                      m_program_rom.size() - offset, false};
    }

    if (address >= kSamplesBase && address < kSamplesBase + kSamplesSize) {
        const u32 offset = address - kSamplesBase;
        return sample_window(offset, kSamplesSize - offset);
    }
    if (address >= kBank4Base && address < kBank4Base + kBank4Size) {
        const u32 offset = address - kBank4Base;
        return sample_window(m_bank4_offset + offset, kBank4Size - offset);
    }
    if (address >= kBank5Base && address < kBank5Base + kBank5Size) {
        const u32 offset = address - kBank5Base;
        return sample_window(m_bank5_offset + offset, kBank5Size - offset);
    }

    return {};
}

void Model2Sound::snd_ctrl_write(u16 value)
{
    ++m_counters.snd_ctrl_writes;

    // Sample banking, and only for the handful of games whose sample region is
    // larger than 8 MB. Everything below that, Virtua Fighter 2 included, has
    // its whole sample ROM permanently visible and this register does nothing;
    // MAME guards it the same way.
    if (m_samples.size() <= 0x800000) {
        return;
    }
    if ((value & 0x20) != 0) {
        m_bank4_offset = 0x200000;
        m_bank5_offset = 0x600000;
    } else {
        m_bank4_offset = 0x800000;
        m_bank5_offset = 0xa00000;
    }
}

// ---------------------------------------------------------------------------
// The 68000's bus
// ---------------------------------------------------------------------------
// read16/write16 are the primitives: the 68000's bus is 16 bits wide, so a
// longword access really is two word accesses, and building the wider ones out
// of them keeps accesses that straddle two regions correct for free.

u16 Model2Sound::read16(u32 address)
{
    address &= kAddressMask;

    if (const Window window = resolve(address); window.base != nullptr) {
        if (address >= kSamplesBase) {
            ++m_counters.sample_reads;
        }
        if (window.size >= 2) {
            return static_cast<u16>((window.base[0] << 8) | window.base[1]);
        }
        return static_cast<u16>(window.base[0] << 8);
    }

    if (address >= kScspBase && address < kScspBase + kScspSize) {
        ++m_counters.scsp_reads;
        return m_scsp.read((address - kScspBase) >> 1);
    }

    // The banking register is write-only. MAME maps no reader at all, so a read
    // is an open bus; zero is as good a guess as any and is what an unmapped
    // read gives elsewhere.
    if (address >= kSndCtrl && address < kSndCtrl + 2) {
        return 0;
    }

    ++m_counters.unmapped_reads;
    SM2_TRACE("sound: unmapped read16 at %06x", address);
    return 0;
}

void Model2Sound::write16(u32 address, u16 value)
{
    address &= kAddressMask;

    if (const Window window = resolve(address); window.base != nullptr) {
        if (!window.writable) {
            // ROM. Discarded, as MAME's .rom() does.
            return;
        }
        window.base[0] = static_cast<u8>(value >> 8);
        if (window.size >= 2) {
            window.base[1] = static_cast<u8>(value);
        }
        return;
    }

    if (address >= kScspBase && address < kScspBase + kScspSize) {
        ++m_counters.scsp_writes;
        m_scsp.write((address - kScspBase) >> 1, value, 0xffff);
        return;
    }

    if (address >= kSndCtrl && address < kSndCtrl + 2) {
        snd_ctrl_write(value);
        return;
    }

    ++m_counters.unmapped_writes;
    SM2_TRACE("sound: unmapped write16 at %06x = %04x", address, value);
}

u8 Model2Sound::read8(u32 address)
{
    // Byte lane 0 of the word is the high half on a big-endian bus.
    const u16 word = read16(address & ~1u);
    return (address & 1) != 0 ? static_cast<u8>(word) : static_cast<u8>(word >> 8);
}

void Model2Sound::write8(u32 address, u8 value)
{
    const u32 masked = address & kAddressMask;

    // Plain memory takes the byte directly; anything with side effects has to be
    // told which lane was driven, because a byte write to a 16-bit register must
    // not clear the other half.
    if (const Window window = resolve(masked); window.base != nullptr) {
        if (window.writable) {
            *window.base = value;
        }
        return;
    }

    if (masked >= kScspBase && masked < kScspBase + kScspSize) {
        ++m_counters.scsp_writes;
        const u16 mask = (masked & 1) != 0 ? u16{0x00ff} : u16{0xff00};
        const u16 wide = (masked & 1) != 0 ? static_cast<u16>(value)
                                           : static_cast<u16>(value << 8);
        m_scsp.write((masked - kScspBase) >> 1, wide, mask);
        return;
    }

    if (masked >= kSndCtrl && masked < kSndCtrl + 2) {
        snd_ctrl_write(value);
        return;
    }

    ++m_counters.unmapped_writes;
    SM2_TRACE("sound: unmapped write8 at %06x = %02x", masked, value);
}

u32 Model2Sound::read32(u32 address)
{
    const u32 high = read16(address);
    const u32 low  = read16(address + 2);
    return (high << 16) | low;
}

void Model2Sound::write32(u32 address, u32 value)
{
    write16(address, static_cast<u16>(value >> 16));
    write16(address + 2, static_cast<u16>(value));
}

// ---------------------------------------------------------------------------
// The SCSP's bus
// ---------------------------------------------------------------------------
// MAME gives the SCSP a 20-bit big-endian space of its own and maps the same
// "soundram" into it, which is how a slot's sample address reaches data the 68000
// loaded. Only the low 512 KB is wired, so anything above that reads as nothing.

u8 Model2Sound::scsp_read_byte(u32 address)
{
    address &= kScspAddressMask;
    if (address < m_ram.size()) {
        return m_ram[address];
    }
    // Addresses above the 512 KB RAM window read from the sample ROM, which is
    // where the SCSP finds PCM data that the 68000 did not copy into RAM. On
    // Model 2 the SCSP's 20-bit address space maps RAM at 0x00000–0x7FFFF and
    // the first 512 KB of sample ROM at 0x80000–0xFFFFF. This is what MAME's
    // device_rom_interface provides as a fallback behind the scsp_map RAM.
    const u32 rom_offset = address - static_cast<u32>(m_ram.size());
    if (rom_offset < m_samples.size()) {
        return m_samples[rom_offset];
    }
    return 0;
}

u16 Model2Sound::scsp_read_word(u32 address)
{
    address &= kScspAddressMask & ~1u;
    if (address + 1 < m_ram.size()) {
        return static_cast<u16>((m_ram[address] << 8) | m_ram[address + 1]);
    }
    // Fall through to sample ROM for addresses above the RAM window.
    const u32 rom_offset = address - static_cast<u32>(m_ram.size());
    if (rom_offset + 1 < m_samples.size()) {
        return static_cast<u16>((m_samples[rom_offset] << 8) | m_samples[rom_offset + 1]);
    }
    return 0;
}

void Model2Sound::scsp_write_word(u32 address, u16 value)
{
    address &= kScspAddressMask & ~1u;
    if (address + 1 >= m_ram.size()) {
        return;  // Writes above RAM are discarded (ROM is read-only).
    }
    m_ram[address]     = static_cast<u8>(value >> 8);
    m_ram[address + 1] = static_cast<u8>(value);
}

}  // namespace sm2::hw
