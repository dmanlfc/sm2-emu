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
// See model2_sound.h.

#include "hw/model2_sound.h"

#include "core/archive.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <unordered_map>

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

/// The SCSP's own view of memory is 20 bits wide.
constexpr u32 kScspAddressMask = 0x000fffff;

/// 68000 clock over host clock, exactly: 45.1584 MHz / 4 over 25 MHz in lowest
/// terms. The remainder is carried, not rounded.
constexpr u64 kCpuClockNumerator   = 7056;
constexpr u64 kCpuClockDenominator = 15625;

/// The 68000 and the SCSP share the 45.1584 MHz crystal, so one 44100 Hz sample
/// is exactly 256 68000 cycles.
constexpr u64 kCyclesPerSample = 256;

/// Wait states on a 68000 access to RAM or the SCSP: a fixed two, plus up to
/// kContentionCycles more as the SCSP's sample fetches take the RAM bus. Both
/// are fitted to hardware captures; without them the music runs fast.
constexpr u32 kBusWaitCycles    = 2;
constexpr u32 kContentionCycles = 12;

/// The SCSP is clocked at half the 45.1584 MHz crystal on the video board, which
/// divided by 512 is 44100 Hz exactly.
constexpr u32 kScspClock = 22'579'200;

/// Longest run of audio kept while nothing is draining it, in frames.
constexpr usize kMaxPendingFrames = 4 * 800;

/// The 16-byte reset vector copied from the sound ROM into RAM, as MAME's
/// reset_model2_scsp does.
constexpr usize kResetVectorBytes = 16;

}  // namespace

Model2Sound::Model2Sound() : m_cpu(*this), m_scsp(*this, kScspClock)
{
    m_ram.assign(kRamSize, 0);
    m_pending.reserve(kMaxPendingFrames * 2);

    // The SCSP interrupts only the sound 68000; main_irq is unwired on Model 2.
    // The host's sound interrupt comes from the UART.
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

void Model2Sound::attach_dsb2(std::span<const u8> dsb_program, std::span<const u8> dsb_mpeg)
{
    m_dsb2.attach(dsb_program, dsb_mpeg);
}

void Model2Sound::set_midi_out_handler(Scsp::MidiOutHandler handler)
{
    m_scsp.set_midi_out_handler(std::move(handler));
}

void Model2Sound::midi_in(u8 value)
{
    // The host serial link drives the SCSP's MIDI port and, where fitted, the
    // DSB.
    m_scsp.midi_in(value);
    m_dsb.write_txd(value);
    m_dsb2.write_txd(value);
}

void Model2Sound::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});

    m_bank4_offset = 0x200000;
    m_bank5_offset = 0x600000;

    m_cycle_debt   = 0;
    m_cycle_budget = 0;
    m_sample_phase = kCyclesPerSample;
    m_counters     = Counters{};
    m_pending.clear();

    m_scsp.reset();
    m_dsb.reset();
    m_dsb2.reset();

    // Seed the vector table, or the 68000 boots from cleared RAM.
    const usize copy = std::min(kResetVectorBytes, m_program_rom.size());
    if (copy != 0) {
        std::copy_n(m_program_rom.begin(), copy, m_ram.begin());
    }

    m_cpu.reset();
}

void Model2Sound::serialize(Archive& ar)
{
    ar.bytes(m_ram.data(), m_ram.size());
    m_cpu.serialize(ar);
    m_scsp.serialize(ar);
    m_dsb.serialize(ar);
    m_dsb2.serialize(ar);
    // Retired per-slot balance state, kept so existing save states still load.
    bool retired_active = false;
    u16  retired_gains[4] = {256, 256, 256, 256};
    ar.raw(retired_active);
    for (u16& gain : retired_gains) {
        ar.raw(gain);
    }
    ar.raw(m_bank4_offset);
    ar.raw(m_bank5_offset);
    ar.raw(m_cycle_debt);
    ar.raw(m_cycle_budget);
    ar.raw(m_sample_phase);
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

    m_cycle_debt += static_cast<u64>(host_cycles) * kCpuClockNumerator;
    m_cycle_budget += static_cast<s64>(m_cycle_debt / kCpuClockDenominator);
    m_cycle_debt %= kCpuClockDenominator;

    // The 68000 runs a sample at a time with the SCSP one sample ahead, so a
    // timer interrupts at the start of the sample it expires in and a reload
    // counts from the next one. The sequencers' tempo depends on it.
    const usize offset = m_pending.size();
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
    const u32 frames = static_cast<u32>((m_pending.size() - offset) / 2);

    // The DSB music board, where fitted, runs over the same interval and mixes
    // its MPEG audio into the frames the SCSP just produced.
    if (m_dsb.present()) {
        m_dsb.run(host_cycles);
        m_dsb.mix(m_pending.data() + offset, frames, m_scsp.sample_rate());
    }
    if (m_dsb2.present()) {
        m_dsb2.run(host_cycles);
        m_dsb2.mix(m_pending.data() + offset, frames, m_scsp.sample_rate());
    }

    // Nothing draining the buffer means a headless run. The SCSP must still run
    // for its timers and envelopes, so drop the oldest samples.
    const usize limit = kMaxPendingFrames * 2;
    if (m_pending.size() > limit) {
        const usize excess = m_pending.size() - limit;
        m_counters.samples_dropped += excess / 2;
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

void Model2Sound::generate_sample()
{
    const usize offset = m_pending.size();
    m_pending.resize(offset + 2);
    m_scsp.generate(m_pending.data() + offset, 1);
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

Model2Sound::Window Model2Sound::resolve(u32 address)
{
    // The read-only sample windows differ only in their start in the ROM.
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

void Model2Sound::bus_wait(u32 address)
{
    if (address < kRamBase + kRamSize
        || (address >= kScspBase && address < kScspBase + kScspSize)) {
        const u32 contention = (kContentionCycles * m_scsp.active_slots() + 16) / 32;
        m_cpu.stall(static_cast<s32>(kBusWaitCycles + contention));
    }
}

void Model2Sound::snd_ctrl_write(u16 value)
{
    ++m_counters.snd_ctrl_writes;

    // Sample banking applies only to sets with more than 8 MB of samples, as in
    // MAME.
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
// read16/write16 are the primitives: the bus is 16 bits wide, so longword
// accesses are two word accesses, which also handles region straddles.

u16 Model2Sound::read16(u32 address)
{
    address &= kAddressMask;
    bus_wait(address);

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

    // The banking register is write-only; reads return 0 like other unmapped
    // reads.
    if (address >= kSndCtrl && address < kSndCtrl + 2) {
        return 0;
    }

    ++m_counters.unmapped_reads;
    SM2_TRACE("sound: unmapped read16 at %06x", address);
    return 0;
}

void Model2Sound::configure_balance(const std::string& game_name)
{
    // Per-set flat SCSP gain in 1/256 units (256 = unity), levelling loudness
    // against daytona. A mastering choice, not a hardware property.
    static const std::unordered_map<std::string, u16> kFlatGain = {
        {"airwlkrs", 793},
        {"bel", 902},
        {"desert", 254},
        {"doa", 500}, {"doaa", 500}, {"doaab", 500}, {"doaae", 500}, {"doab", 500},
        {"dynabb", 1751},
        {"dynabb97", 1029},
        {"dynamcop", 705}, {"dynamcopb", 705}, {"dynamcopc", 705},
        {"dyndeka2", 705}, {"dyndeka2b", 705},
        {"fvipers", 403}, {"fvipersa", 403}, {"fvipersb", 403},
        {"gunblade", 1000},
        {"hotd", 1400}, {"hotdo", 1400}, {"hotdp", 1100},
        {"hpyagu98", 925},
        {"indy500", 2110}, {"indy500d", 2110}, {"indy500to", 2110},
        {"lastbrnx", 2095}, {"lastbrnxj", 2095}, {"lastbrnxu", 2095},
        {"manxtt", 799}, {"manxttc", 799}, {"manxttdx", 799},
        {"motoraid", 1239}, {"motoraiddx", 1239},
        {"overrev", 3892}, {"overrevb", 4022}, {"overrevba", 3852},
        {"pltkids", 2673}, {"pltkidsa", 2673},
        {"rchase2", 745}, {"rchase2a", 745},
        {"schamp", 403}, {"sfight", 403},
        {"segawski", 1095},
        {"sgt24h", 2319},
        {"skisuprg", 953},
        {"skytargt", 927},
        {"srallyc", 739}, {"srallycb", 739}, {"srallycc", 739},
        {"srallycdx", 739}, {"srallycdxa", 739},
        {"stcc", 384}, {"stcca", 384}, {"stccb", 384}, {"stcco", 384},
        {"topskatr", 232}, {"topskatrj", 232}, {"topskatru", 232}, {"topskatruo", 232},
        {"vcop", 321}, {"vcopa", 321},
        {"vcop2", 641},
        {"vf2", 840}, {"vf2a", 840}, {"vf2b", 840}, {"vf2o", 840},
        {"von", 1042}, {"vonj", 1042}, {"vonr", 1042}, {"vonu", 1042},
        {"vstriker", 899}, {"vstrikero", 899},
        {"waverunr", 1181},
        {"zerogun", 1912}, {"zerogunj", 1912},
        {"zeroguna", 2824}, {"zerogunaj", 2824},
    };

    // Top Skater's music board level against the effects, matched to arcade
    // captures.
    static const std::unordered_map<std::string, u16> kDsbGain = {
        {"topskatr", 64}, {"topskatrj", 64}, {"topskatru", 64}, {"topskatruo", 64},
    };
    if (const auto dsb = kDsbGain.find(game_name); dsb != kDsbGain.end()) {
        m_dsb2.set_gain(dsb->second);
    }

    const auto it = kFlatGain.find(game_name);
    if (it != kFlatGain.end()) {
        std::array<u16, 32> gains;
        gains.fill(it->second);
        m_scsp.set_slot_gains(gains.data());
    }
}

void Model2Sound::write16(u32 address, u16 value)
{
    address &= kAddressMask;
    bus_wait(address);

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
    bus_wait(masked);

    // Registers are told which lane was driven, so a byte write to a 16-bit
    // register does not clear the other half.
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
// A 20-bit big-endian space: sound RAM at 0x00000-0x7ffff, then the first
// 512 KB of sample ROM (MAME's device_rom_interface fallback).

u8 Model2Sound::scsp_read_byte(u32 address)
{
    address &= kScspAddressMask;
    if (address < m_ram.size()) {
        return m_ram[address];
    }
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
