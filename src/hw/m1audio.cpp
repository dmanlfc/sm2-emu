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

#include "hw/m1audio.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::hw {

namespace {

// ---------------------------------------------------------------------------
// Memory map, from MAME's segam1audio_device::segam1audio_map
// ---------------------------------------------------------------------------

constexpr u32 kRomBase      = 0x000000;  // 256 KB
constexpr u32 kRomSize      = 0x040000;
constexpr u32 kRomMirror    = 0x080000;  // 128 KB view of the region at 0x20000
constexpr u32 kRomMirrorSize = 0x020000;
constexpr u32 kUartBase     = 0xc20000;  // two registers, byte lane
constexpr u32 kPcm1Base     = 0xc40000;  // four registers
constexpr u32 kPcm1Unknown  = 0xc40012;  // written, does nothing
constexpr u32 kPcm1Bank     = 0xc50000;
constexpr u32 kPcm2Base     = 0xc60000;
constexpr u32 kPcm2Bank     = 0xc70000;
constexpr u32 kYmBase       = 0xd00000;  // four registers
constexpr u32 kRamBase      = 0xf00000;  // 64 KB
constexpr u32 kRamSize      = 0x010000;

/// The 68000 runs at 20 MHz / 2 against the host i960's 25 MHz.
constexpr u64 kCpuNumerator   = 2;
constexpr u64 kCpuDenominator = 5;

/// One output frame per 560 host cycles: the MultiPCMs' 10 MHz over their 224
/// divider is 44642.857 Hz, and 25 MHz / 560 is the same number exactly.
constexpr u64 kHostCyclesPerFrame = 560;

/// The sound chips are clocked from their own crystals, not the host's.
/// MAME: MULTIPCM(config, ..., 20_MHz_XTAL / 2) and YM3438(config, ..., 16_MHz_XTAL / 2).
constexpr u32 kPcmClock = 20'000'000 / 2;
constexpr u32 kYmClock  = 16'000'000 / 2;

/// What generate() produces, and what sample_rate() reports: the MultiPCMs' own
/// rate, 10 MHz / 224. The YM3438 runs faster than this and is resampled onto it.
constexpr u32 kOutputRate = kPcmClock / MultiPcm::kClockDivider;

/// Cap on buffered audio, so a headless run with nothing draining does not grow
/// without bound. Four frames' worth at ~760 frames each.
constexpr usize kMaxPendingFrames = 4 * 800;

/// The board's mixer, as percentages of full scale. MAME's segam1audio routes
/// each MultiPCM to the speaker at 0.5 and the YM3438 at 0.30; those are the
/// numbers, and they are what keeps two 16-bit PCM chips from clipping the bus
/// the moment they both sound.
constexpr s32 kGainPcm = 50;
constexpr s32 kGainYm  = 30;

/// Whether an address falls in a device's register block of `count` 16-bit words.
[[nodiscard]] inline bool in_block(u32 address, u32 base, u32 count)
{
    return address >= base && address < base + count * 2;
}

}  // namespace

M1Audio::M1Audio()
    : m_cpu(*this)
    , m_pcm{MultiPcm(kPcmClock), MultiPcm(kPcmClock)}
    , m_ym(kYmClock, kOutputRate)
{
    m_ram.assign(kRamSize, 0);
    m_pending.reserve(kMaxPendingFrames * 2);
}

M1Audio::~M1Audio() = default;

void M1Audio::attach(std::span<const u8> program_rom,
                     std::span<const u8> pcm1,
                     std::span<const u8> pcm2)
{
    m_program_rom = program_rom;
    m_pcm[0].attach(pcm1);
    m_pcm[1].attach(pcm2);
}

u32 M1Audio::sample_rate() const
{
    return m_pcm[0].sample_rate();
}

u32 M1Audio::active_voices() const
{
    return m_pcm[0].active_voices() + m_pcm[1].active_voices();
}

void M1Audio::set_rxd_handler(std::function<void(u8)> handler)
{
    m_rxd_handler = std::move(handler);
}

void M1Audio::write_txd(u8 value)
{
    ++m_counters.bytes_from_host;
    m_uart.write_rxd(value);
}

void M1Audio::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});

    m_cpu_debt        = 0;
    m_sample_debt     = 0;
    m_cycle_overshoot = 0;
    m_counters        = Counters{};
    m_pending.clear();

    m_pcm[0].reset();
    m_pcm[1].reset();
    m_ym.reset();

    // The board's own UART. Its clock is 16 MHz / 2 divided by 16, which is the
    // same 31.25 kHz the host end runs at -- the two have to agree or the link
    // would not work on hardware either. Counted in this board's 68000 cycles,
    // because that is what run() advances it by.
    m_uart.configure(kPcmClock, 31'250, 10);
    m_uart.reset();

    // RxRDY drives the sound CPU's IRQ 2, which is how the program learns a
    // command byte has arrived rather than polling for it.
    m_uart.set_ready_handler([this] {
        m_cpu.set_irq_line(2, m_uart.rxrdy());
    });
    m_uart.set_tx_handler([this](u8 value) {
        ++m_counters.bytes_to_host;
        if (m_rxd_handler) {
            m_rxd_handler(value);
        }
    });

    // Last, because the 68000 fetches its stack pointer and PC from the first
    // eight bytes of ROM as part of reset.
    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

M1Audio::Window M1Audio::resolve(u32 address)
{
    if (address >= kRomBase && address < kRomBase + kRomSize) {
        if (address < m_program_rom.size()) {
            return {const_cast<u8*>(m_program_rom.data()) + address,
                    m_program_rom.size() - address, false};
        }
        return {};
    }

    if (address >= kRomMirror && address < kRomMirror + kRomMirrorSize) {
        // The upper ROM socket appears twice: once in the low 256 KB and again
        // here. MAME maps it as a second view of region offset 0x20000.
        const u32 offset = 0x20000u + (address - kRomMirror);
        if (offset < m_program_rom.size()) {
            return {const_cast<u8*>(m_program_rom.data()) + offset,
                    m_program_rom.size() - offset, false};
        }
        return {};
    }

    if (address >= kRamBase && address < kRamBase + kRamSize) {
        const u32 offset = address - kRamBase;
        return {m_ram.data() + offset, kRamSize - offset, true};
    }

    return {};
}

// ---------------------------------------------------------------------------
// cpu::Bus -- big-endian
// ---------------------------------------------------------------------------

u16 M1Audio::read16(u32 address)
{
    address &= 0xffffff;

    if (const Window window = resolve(address); window.base != nullptr && window.size >= 2) {
        return static_cast<u16>((window.base[0] << 8) | window.base[1]);
    }

    if (in_block(address, kUartBase, 2)) {
        ++m_counters.uart_reads;
        return m_uart.read((address - kUartBase) >> 1);
    }
    if (in_block(address, kPcm1Base, 4)) {
        return m_pcm[0].read();
    }
    if (in_block(address, kPcm2Base, 4)) {
        return m_pcm[1].read();
    }
    if (in_block(address, kYmBase, 4)) {
        ++m_counters.ym_reads;
        const u8 value = m_ym.read((address - kYmBase) >> 1);
        if (value != 0) {
            ++m_counters.ym_status_reads;
        }
        return value;
    }

    ++m_counters.unmapped_reads;
    return 0;
}

u8 M1Audio::read8(u32 address)
{
    const u16 word = read16(address & ~1u);
    return static_cast<u8>((address & 1) != 0 ? (word & 0xff) : (word >> 8));
}

u32 M1Audio::read32(u32 address)
{
    return (static_cast<u32>(read16(address)) << 16) | read16(address + 2);
}

void M1Audio::write16(u32 address, u16 value)
{
    address &= 0xffffff;

    if (const Window window = resolve(address);
        window.base != nullptr && window.writable && window.size >= 2) {
        window.base[0] = static_cast<u8>(value >> 8);
        window.base[1] = static_cast<u8>(value & 0xff);
        return;
    }

    // Every device on this board is eight bits wide on the low half of the word.
    const u8 low = static_cast<u8>(value & 0xff);

    if (in_block(address, kUartBase, 2)) {
        ++m_counters.uart_writes;
        m_uart.write((address - kUartBase) >> 1, low);
        return;
    }
    if (in_block(address, kPcm1Base, 4)) {
        ++m_counters.pcm_writes[0];
        m_pcm[0].write((address - kPcm1Base) >> 1, low);
        return;
    }
    if (in_block(address, kPcm2Base, 4)) {
        ++m_counters.pcm_writes[1];
        m_pcm[1].write((address - kPcm2Base) >> 1, low);
        return;
    }
    if (address == kPcm1Bank || address == kPcm1Bank + 1) {
        ++m_counters.bank_writes[0];
        m_pcm[0].set_bank(value & 3);
        return;
    }
    if (address == kPcm2Bank || address == kPcm2Bank + 1) {
        ++m_counters.bank_writes[1];
        m_pcm[1].set_bank(value & 3);
        return;
    }
    if (address == kPcm1Unknown) {
        // MAME maps this as nopw. Written by the program, does nothing.
        return;
    }
    if (in_block(address, kYmBase, 4)) {
        ++m_counters.ym_writes;
        m_ym.write((address - kYmBase) >> 1, low);
        return;
    }

    ++m_counters.unmapped_writes;
}

void M1Audio::write8(u32 address, u8 value)
{
    if (const Window window = resolve(address);
        window.base != nullptr && window.writable && window.size >= 1) {
        window.base[0] = value;
        return;
    }
    // The devices sit on the low byte of each word, which on a big-endian bus is
    // the odd address. A write to the even half is the other, unpopulated lane.
    if ((address & 1) != 0) {
        write16(address & ~1u, value);
    }
}

void M1Audio::write32(u32 address, u32 value)
{
    write16(address, static_cast<u16>(value >> 16));
    write16(address + 2, static_cast<u16>(value & 0xffff));
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void M1Audio::uart_tick(u32 host_cycles)
{
    // The UART is configured in this board's own cycles, so convert as the CPU
    // does. Approximate within a slice is fine: the byte timer only decides when
    // a transmitted byte completes.
    m_uart.run(static_cast<u32>(static_cast<u64>(host_cycles) * kCpuNumerator
                                / kCpuDenominator));
}

void M1Audio::generate_audio(u32 host_cycles)
{
    m_sample_debt += host_cycles;
    const u64 frames = m_sample_debt / kHostCyclesPerFrame;
    m_sample_debt %= kHostCyclesPerFrame;
    if (frames == 0) {
        return;
    }

    // Mixed in 32 bits so the three gained streams can sum past full scale and be
    // clamped once at the end, which is where MAME's speaker clamps.
    static thread_local std::vector<s32> accum;
    accum.assign(static_cast<usize>(frames) * 2, 0);

    m_pcm[0].generate(accum.data(), static_cast<u32>(frames), kGainPcm);
    m_pcm[1].generate(accum.data(), static_cast<u32>(frames), kGainPcm);
    m_ym.generate(accum.data(), static_cast<u32>(frames), kGainYm);

    const usize before = m_pending.size();
    m_pending.resize(before + static_cast<usize>(frames) * 2);
    for (usize i = 0; i < static_cast<usize>(frames) * 2; ++i) {
        m_pending[before + i] =
            static_cast<s16>(std::clamp(accum[i], -32768, 32767));
    }

    // Nothing is draining in a headless run, so drop the oldest rather than grow.
    if (m_pending.size() > kMaxPendingFrames * 2) {
        const usize excess = m_pending.size() - kMaxPendingFrames * 2;
        m_pending.erase(m_pending.begin(),
                        m_pending.begin() + static_cast<std::ptrdiff_t>(excess));
        m_counters.samples_dropped += excess;
    }
}

void M1Audio::run(u32 host_cycles)
{
    if (m_program_rom.empty()) {
        if (!m_warned_no_program) {
            m_warned_no_program = true;
            SM2_WARN("m1audio: no sound program ROM; the board is inert");
        }
        return;
    }

    // Audio first, for the same reason the CRX board does it: the chips' state
    // advances per output frame, so generating before the CPU runs keeps a write
    // this slice from retroactively changing samples already produced. It is also
    // what advances the YM3438's timers, which this board's sound driver uses as
    // its sequencer clock, so it has to happen before the CPU polls for them.
    generate_audio(host_cycles);
    uart_tick(host_cycles);

    m_cpu_debt += static_cast<u64>(host_cycles) * kCpuNumerator;
    u64 owed = m_cpu_debt / kCpuDenominator;
    m_cpu_debt %= kCpuDenominator;

    // 68000 instructions are not interruptible, so a slice usually runs a little
    // past its allowance. Carrying the overshoot keeps the board's clock honest
    // instead of letting it drift fast.
    if (owed <= m_cycle_overshoot) {
        m_cycle_overshoot -= owed;
        return;
    }
    owed -= m_cycle_overshoot;
    m_cycle_overshoot = 0;

    const s32 used = m_cpu.run(static_cast<s32>(owed));
    if (used > static_cast<s32>(owed)) {
        m_cycle_overshoot = static_cast<u64>(used) - owed;
    }
}

}  // namespace sm2::hw
