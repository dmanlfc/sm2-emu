//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "hw/model1.h"

#include "core/archive.h"
#include "core/log.h"
#include "hw/save_state_io.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>

namespace sm2::hw {
namespace {

static_assert(std::endian::native == std::endian::little, "Model 1 memory views assume a little-endian host");

constexpr u32 kRomMainCpuSize = 0x2000000;

constexpr u32 kNvram      = 0x400000;  // 64 KB
constexpr u32 kWorkRam    = 0x500000;  // 256 KB
constexpr u32 kListRam    = 0x600000;  // 2 x 64 KB
constexpr u32 kListCtl    = 0x680000;
constexpr u32 kTileRam    = 0x700000;  // 64 KB
constexpr u32 kCharRam    = 0x780000;  // 512 KB
constexpr u32 kPalette    = 0x900000;  // 16 KB
constexpr u32 kColorXlat  = 0x910000;  // 48 KB
constexpr u32 kDualPort   = 0xc00000;  // 2K on the low byte lane
constexpr u32 kUart       = 0xc40000;
constexpr u32 kComm       = 0xb00000;  // the link board, where fitted
constexpr u32 kGlue       = 0xe00000;

/// Slices short enough that the TGP, the I/O board and the UART keep up with
/// the V60 between their handshakes.
constexpr u32 kInterleave = 64;
// Sound board host cycles run per call; flushed at every line end too.
constexpr u32 kAudioBatch = 1024;

/// TGP instructions owed per V60 cycle: 40 MHz / 3 clocks against 16 MHz.
constexpr u64 kTgpNumerator   = 5;
constexpr u64 kTgpDenominator = 6;

constexpr s32 kTgpSyncLimit = 8192;

/// Wait states on a dual-port RAM read. Wing War's boot handshake times out
/// unless the I/O board finishes reading its EEPROM first, which takes it about
/// 2.2 s; at MAME's single wait state the V60's timeout loop is a few percent
/// too short. Four is an estimate, not a measurement.
constexpr s32 kDualPortWaitCycles = 4;

[[nodiscard]] inline std::span<const u32> as_words(std::span<const u8> bytes)
{
    if (bytes.empty()) return {};
    return {reinterpret_cast<const u32*>(bytes.data()), bytes.size() / 4};
}

/// Virtua Racing's settings image with CABINET TYPE moved from STANDARD to
/// SPECIAL, the setting that drives the drive board. The chip holds the bytes
/// swapped in pairs; a CRC-CCITT over 0x0a..0x80 sits at 0x08.
void vr_special_cabinet(std::span<u8> eeprom)
{
    if (eeprom.size() < 0x80) return;
    const auto at  = [&](usize offset) -> u8& { return eeprom[offset ^ 1]; };
    const auto crc = [&] {
        u16 value = 0;
        for (usize i = 0x0a; i < 0x80; ++i) {
            value = static_cast<u16>(value ^ (at(i) << 8));
            for (int bit = 0; bit < 8; ++bit) {
                value = static_cast<u16>((value & 0x8000) != 0 ? (value << 1) ^ 0x1021 : value << 1);
            }
        }
        return value;
    };
    if (at(0x1a) != 1 || (at(0x08) | (at(0x09) << 8)) != crc()) return;
    at(0x1a)      = 0;
    const u16 sum = crc();
    at(0x08)      = static_cast<u8>(sum);
    at(0x09)      = static_cast<u8>(sum >> 8);
}

}  // namespace

Model1::Model1() : m_cpu(*this, &m_io_space), m_io_space(*this) {}

Model1::~Model1() = default;

CpuStatus Model1::main_cpu_status() const
{
    CpuStatus status;
    status.state_string  = const_cast<cpu::v60::V60&>(m_cpu).state_string();
    status.fault_message = m_cpu.fault_message();
    status.instructions  = m_cpu.instructions();
    status.halted        = m_cpu.halted();
    status.faulted       = m_cpu.faulted();
    return status;
}

bool Model1::init(const rom::GameSpec& game, rom::RomSet roms)
{
    m_game = game;
    m_roms = std::move(roms);

    m_rom = m_roms.region("maincpu");
    if (m_rom.size() < kRomMainCpuSize) {
        SM2_ERROR("model1: the 'maincpu' region is missing or short");
        return false;
    }

    m_nvram.assign(0x10000, 0xff);
    m_work_ram.assign(0x40000, 0);
    m_list_words.assign(0x10000, 0);
    m_tile_ram.assign(0x10000, 0);
    m_char_ram.assign(0x80000, 0);
    m_palette_ram.assign(0x2000, 0);
    m_colorxlat.assign(0x6000, 0);

    m_cpu.bind_fetch_window(0, 0xf80000, m_rom.data() + 0xf80000, 0x80000);
    m_cpu.bind_fetch_window(1, 0x000000, m_rom.data(), 0x100000);
    m_cpu.bind_fetch_window(2, 0x200000, m_rom.data() + 0x200000, 0x100000);
    m_cpu.bind_fetch_window(3, kWorkRam, m_work_ram.data(), m_work_ram.size());
    m_cpu.set_irq_cycle(&Model1::irq_cycle, this);

    m_video.set_direct_palette(true);
    m_video.attach(m_tile_ram, m_char_ram, m_palette_ram, m_colorxlat);

    m_tgp.attach(as_words(m_roms.region("tgp_copro")), as_words(m_roms.region("copro_tables")),
                 as_words(m_roms.region("copro_data")));
    if (!m_roms.has("tgp_copro")) {
        SM2_WARN("model1: no TGP program ROM; the 3D will not run");
    }

    // The V60 is the TGP's host: a read from an empty output FIFO retries, a
    // write to a full input FIFO waits.
    m_tgp.fifo_out().set_on_empty_retry([this] { m_cpu.stall(); });
    m_tgp.fifo_out().set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_tgp.fifo_out().set_on_unempty([this] { m_cpu.set_halted(false); });
    m_tgp.fifo_in().set_on_full([this] { m_cpu.set_halted(true); });
    m_tgp.fifo_in().set_on_unfull([this] { m_cpu.set_halted(false); });

    m_geometry.attach(as_words(m_roms.region("polygons")));

    m_uses_io2 = std::find(m_game.device_sets.begin(), m_game.device_sets.end(), std::string{"model1io2"})
                 != m_game.device_sets.end();
    const auto dp_read  = [this](u32 address) { return m_dpram.left_read(address); };
    const auto dp_write = [this](u32 address, u8 value) { m_dpram.left_write(address, value); };
    m_r360    = m_game.name == "wingwar360";
    m_netmerc = m_game.name == "netmerc";
    const std::string& set_family = m_game.parent.empty() ? m_game.name : m_game.parent;
    m_has_comm = set_family == "vr" || set_family == "wingwar";
    if (m_uses_io2) {
        m_ioboard2.attach(m_roms.region("ioboard"));
        m_ioboard2.set_dual_port(dp_read, dp_write);
        m_ioboard2.set_input(0, [this] { return m_inputs.in0; });
        m_ioboard2.set_input(1, [this] { return m_inputs.in1; });
        m_ioboard2.set_input(2, [this] { return m_r360 ? m_r360_state : m_inputs.in2; });
        for (u32 channel = 0; channel < Model1io2::kAnalogCount; ++channel) {
            m_ioboard2.set_analog(channel, [this, channel] {
                return m_r360 && channel == m_r360_throttle ? u8{0} : m_inputs.analog[channel];
            });
        }
        m_ioboard2.set_output([this](u8 value) { lamp_output_w(value); });
        if (m_r360) {
            m_ioboard2.set_drive([this] { return m_r360_state; }, [this](u8 value) { r360_w(value); });
        }
    } else {
        m_ioboard.attach(m_roms.region("ioboard"));
        m_ioboard.set_dual_port(dp_read, dp_write);
        m_ioboard.set_input(0, [this] { return m_inputs.in0; });
        m_ioboard.set_input(1, [this] { return m_inputs.in1; });
        m_ioboard.set_input(2, [this] { return m_inputs.in2; });
        for (u32 channel = 0; channel < Model1io::kAnalogCount; ++channel) {
            m_ioboard.set_analog(channel, [this, channel] { return m_inputs.analog[channel]; });
        }
        m_ioboard.set_output([this](u8 value) { lamp_output_w(value); });
        if (m_game.drive_board) {
            m_ioboard.set_drive([] { return u8{0xff}; }, [this](u8 value) { record_drive_board_write(value); });
        }
    }
    if (!m_roms.has("ioboard")) {
        SM2_WARN("model1: no 'ioboard' region; the program will see no inputs");
    }

    m_m1audio.attach(m_roms.region("m1audio:sndcpu"), m_roms.region("m1audio:pcm1"),
                     m_roms.region("m1audio:pcm2"));
    if (m_roms.has("dsbz80:mpegcpu")) {
        m_m1audio.attach_dsb(m_roms.region("dsbz80:mpegcpu"), m_roms.region("dsbz80:mpeg"));
    }
    // Per-set level in 1/256 units: the loudest in-game peak measured stays under
    // about -0.8 dBFS, and Wing War sits at Virtua Racing's loudness.
    static const std::unordered_map<std::string, u16> kSoundGain = {
        {"vr", 162}, {"vf", 129}, {"swa", 165}, {"wingwar", 398},
    };
    const std::string family = m_game.parent.empty() ? m_game.name : m_game.parent;
    if (const auto it = kSoundGain.find(family); it != kSoundGain.end()) {
        m_m1audio.set_gain(it->second);
    }
    m_uart.set_tx_handler([this](u8 value) { m_m1audio.write_txd(value); });
    m_m1audio.set_rxd_handler([this](u8 value) { m_uart.write_rxd(value); });
    m_uart.set_ready_handler([this] { sound_ready_w(); });

    SM2_INFO("model1: %s", game.title.c_str());
    reset();
    return true;
}

void Model1::reset()
{
    m_bank_offset = 0x1000000;
    m_listctl[0] = m_listctl[1] = 0;
    m_r360_state = 0xff;
    m_irq_status = 0;
    m_irq_mask   = 0xff;
    m_last_irq   = 0;
    m_timer_mode = 0;
    m_timers.fill(Timer{});
    m_fifo_read_latch  = 0;
    m_fifo_write_latch = 0;

    m_cycles          = 0;
    m_frame_start     = 0;
    m_frames          = 0;
    m_tgp_debt        = 0;
    m_peripheral_debt = 0;
    m_audio_pending   = 0;
    m_palette_dirty   = true;

    m_inputs.analog.fill(0);
    for (usize channel = 0; channel < m_inputs.analog.size(); ++channel) {
        if (m_game.analog[channel].control != rom::AnalogControl::None) {
            m_inputs.analog[channel] = m_game.analog[channel].rest;
        }
    }

    m_video.reset();
    m_tgp.reset();
    m_geometry.reset();
    m_render_list.clear();
    m_overlay_list.clear();

    m_dpram.reset();
    if (m_netmerc) {
        // The headset tracker's pose, looking straight ahead: position, then
        // three angles at 8192 to the radian.
        constexpr s16 kPose[6] = {0, 0, 0, 12868, 25736, 12868};
        for (usize i = 0; i < 6; ++i) {
            m_dpram.left_write(static_cast<u32>(0x80 + i * 2), static_cast<u8>(kPose[i]));
            m_dpram.left_write(static_cast<u32>(0x81 + i * 2), static_cast<u8>(kPose[i] >> 8));
        }
    }
    m_ioboard.reset();
    m_ioboard2.reset();
    m_uart.configure(kPeripheralClock, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();
    m_comm.reset();
    m_m1audio.reset();

    m_cpu.reset();
    m_cpu.set_irq_line(cpu::v60::kClearLine);
}

// -- scheduling --------------------------------------------------------------------

void Model1::step_tgp(u32 cycles)
{
    m_tgp_debt += static_cast<u64>(cycles) * kTgpNumerator;
    const s32 owed = static_cast<s32>(m_tgp_debt / kTgpDenominator);
    m_tgp_debt %= kTgpDenominator;
    if (owed > 0) static_cast<void>(m_tgp.run(owed));
}

void Model1::sync_tgp()
{
    if (m_in_tgp_sync) return;
    m_in_tgp_sync = true;
    s32 remaining = kTgpSyncLimit;
    while (remaining > 0 && m_tgp.fifo_out().empty() && !m_tgp.cpu().halted()) {
        const s32 used = m_tgp.run(std::min<s32>(remaining, 32));
        if (used <= 0) break;
        remaining -= used;
        // Borrowed from the TGP's future share, so the time is not spent twice.
        const u64 owed = static_cast<u64>(used) * kTgpDenominator;
        m_tgp_debt     = m_tgp_debt > owed ? m_tgp_debt - owed : 0;
    }
    m_in_tgp_sync = false;
}

void Model1::step_peripherals(u32 cycles)
{
    m_peripheral_debt += static_cast<u64>(cycles) * kPeripheralClock;
    const u32 host = static_cast<u32>(m_peripheral_debt / kCpuClock);
    m_peripheral_debt %= kCpuClock;
    if (host == 0) return;
    if (m_uses_io2) {
        m_ioboard2.run(host);
    } else {
        m_ioboard.run(host);
    }
    m_uart.run(host);
    m_audio_pending += host;
    if (m_audio_pending >= kAudioBatch) flush_audio();
}

void Model1::flush_audio()
{
    if (m_audio_pending == 0) return;
    m_m1audio.run(m_audio_pending);
    m_audio_pending = 0;
}

void Model1::run_frame()
{
    reset_core_profile();
    m_in_frame = true;

    for (u32 line = 0; line < kVerticalTotal; ++line) {
        const u64 line_end = m_frame_start + static_cast<u64>(line + 1) * kCyclesPerLine;
        while (m_cycles < line_end) {
            u64 target = std::min<u64>(line_end, m_cycles + kInterleave);
            for (const Timer& timer : m_timers) {
                if (timer.period != 0 && timer.deadline > m_cycles) target = std::min(target, timer.deadline);
            }
            const s32 slice = static_cast<s32>(target - m_cycles);
            s32       used;
            {
                CoreScope scope(m_core_profile, m_core_profile.i960_ns);
                used = m_cpu.run(slice);
            }
            if (used <= 0) used = slice;
            m_cycles += static_cast<u64>(used);
            {
                CoreScope scope(m_core_profile, m_core_profile.copro_ns);
                step_tgp(static_cast<u32>(used));
            }
            {
                CoreScope scope(m_core_profile, m_core_profile.sound_ns);
                step_peripherals(static_cast<u32>(used));
            }
            service_timers();
        }
        {
            CoreScope scope(m_core_profile, m_core_profile.sound_ns);
            flush_audio();
        }
        sound_ready_w();
        if (line == kVisibleHeight) on_vblank();
    }

    m_frame_start += kCyclesPerFrame;
    ++m_frames;
    m_in_frame = false;
}

void Model1::on_vblank()
{
    if (m_has_comm) m_comm.vblank();
    if (m_video.latch_tile_ram()) ++m_tile_generation;

    const auto start = std::chrono::steady_clock::now();
    if ((m_listctl[1] & 0x1f) == 0x1f) {
        set_current_render_list();
        Model1Geometry::Frame frame;
        frame.list         = current_list();
        frame.palette      = m_palette_ram;
        frame.colour_xlat  = m_colorxlat;
        frame.frame_number = m_frames;
        frame.wide_columns = m_widescreen_columns;
        frame.hud_spread_left  = m_game.hud_spread_left;
        frame.hud_spread_right = m_game.hud_spread_right;
        m_geometry.render(frame, &m_render_list, &m_overlay_list);
        // Farthest first, so a later one in the list draws over an earlier one.
        std::reverse(m_overlay_list.polygons.begin(), m_overlay_list.polygons.end());
        m_video.set_overlay(m_overlay_list.polygons, static_cast<s32>(m_widescreen_columns));
    }
    m_geometry_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());

    // MAME's end_frame: in double-buffered mode the lists swap every other frame.
    if ((m_listctl[0] & 4) != 0 && (m_frames & 1) != 0) m_listctl[0] ^= 0x40;

    if ((m_irq_mask & 0x02) == 0) irq_raise(1);
}

void Model1::set_current_render_list()
{
    if ((m_listctl[0] & 4) == 0) {
        m_listctl[0] = static_cast<u16>((m_listctl[0] & ~0x40) | ((m_listctl[0] & 8) != 0 ? 0x40 : 0));
    }
}

std::span<const u16> Model1::current_list() const
{
    const usize base = (m_listctl[0] & 0x40) != 0 ? 0x8000 : 0;
    return std::span<const u16>(m_list_words).subspan(base, 0x8000);
}

// -- interrupts --------------------------------------------------------------------

void Model1::irq_raise(u32 level)
{
    m_irq_status = static_cast<u8>(m_irq_status | (1u << level));
    irq_update();
}

void Model1::irq_update()
{
    m_cpu.set_irq_line(m_irq_status != 0 ? cpu::v60::kAssertLine : cpu::v60::kClearLine);
}

u8 Model1::irq_cycle(void* context)
{
    auto* self = static_cast<Model1*>(context);
    for (u8 i = 0; i < 8; ++i) {
        if ((self->m_irq_status & (1u << i)) != 0) {
            self->m_last_irq = i;
            break;
        }
    }
    return self->m_last_irq;
}

void Model1::irq_control_w(u8 data)
{
    if (data == 0x10) {
        m_irq_status = 0;
    } else if (data == 0x20) {
        m_irq_status = static_cast<u8>(m_irq_status & ~(1u << m_last_irq));
    }
    irq_update();
}

void Model1::sound_ready_w()
{
    if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_irq_mask & 0x08) == 0) irq_raise(3);
}

// -- timers ------------------------------------------------------------------------

void Model1::timer_period_w(u32 index, u16 value)
{
    Timer& timer = m_timers[index & 1];
    timer.period = value;
    timer.deadline = value != 0 ? m_cycles + 0x800ull * value : 0;
}

u16 Model1::timer_r(u32 index)
{
    Timer& timer = m_timers[index & 1];
    if (timer.period != 0) {
        const u64 remaining = timer.deadline > m_cycles ? timer.deadline - m_cycles : 0;
        timer.value = static_cast<u16>(remaining / 0x800);
    }
    return timer.value;
}

void Model1::service_timers()
{
    for (Timer& timer : m_timers) {
        if (timer.period == 0 || m_cycles < timer.deadline) continue;
        if ((m_irq_mask & 0x01) == 0) irq_raise(0);
        timer.deadline += 0x800ull * timer.period;
    }
}

// -- memory ------------------------------------------------------------------------

u8* Model1::ram_window(u32 address, bool write)
{
    address &= 0xffffff;
    if (address < 0x100000) return write ? nullptr : const_cast<u8*>(m_rom.data()) + address;
    if (address < 0x200000) return write ? nullptr : const_cast<u8*>(m_rom.data()) + m_bank_offset + (address - 0x100000);
    if (address < 0x300000) return write ? nullptr : const_cast<u8*>(m_rom.data()) + address;
    if (address >= kNvram && address < kNvram + 0x10000) return m_nvram.data() + (address - kNvram);
    if (address >= kWorkRam && address < kWorkRam + 0x40000) return m_work_ram.data() + (address - kWorkRam);
    if (address >= kListRam && address < kListRam + 0x20000) {
        return reinterpret_cast<u8*>(m_list_words.data()) + (address - kListRam);
    }
    if (address >= kTileRam && address < kTileRam + 0x10000) return m_tile_ram.data() + (address - kTileRam);
    if (address >= kCharRam && address < kCharRam + 0x80000) return m_char_ram.data() + (address - kCharRam);
    if (address >= kPalette && address < kPalette + 0x4000) {
        return reinterpret_cast<u8*>(m_palette_ram.data()) + (address - kPalette);
    }
    if (address >= kColorXlat && address < kColorXlat + 0xc000) {
        return reinterpret_cast<u8*>(m_colorxlat.data()) + (address - kColorXlat);
    }
    if (address >= 0xf80000) return write ? nullptr : const_cast<u8*>(m_rom.data()) + address;
    return nullptr;
}

void Model1::note_ram_write(u32 address)
{
    address &= 0xffffff;
    if (address >= kTileRam && address < kTileRam + 0x10000) {
        ++m_tile_generation;
    } else if (address >= kCharRam && address < kCharRam + 0x80000) {
        m_video.tiles().note_char_write(address - kCharRam, 2);
        ++m_char_generation;
    } else if (address >= kPalette && address < kColorXlat + 0xc000) {
        m_palette_dirty = true;
        ++m_table_generation;
    }
}

u16 Model1::copro_fifo_read(u32 half, bool from_io)
{
    if (half == 0) {
        if (m_tgp.fifo_out().empty()) sync_tgp();
        if (m_tgp.fifo_out().empty() && !from_io) {
            // Only IN can be retried; a plain load would take a zero.
            return 0;
        }
        m_fifo_read_latch = m_tgp.fifo_out().pop();
        return static_cast<u16>(m_fifo_read_latch);
    }
    return static_cast<u16>(m_fifo_read_latch >> 16);
}

void Model1::copro_fifo_write(u32 half, u16 value)
{
    if (half != 0) {
        m_fifo_write_latch = (m_fifo_write_latch & 0x0000ffffu) | (static_cast<u32>(value) << 16);
        m_tgp.fifo_in().push(m_fifo_write_latch);
    } else {
        m_fifo_write_latch = (m_fifo_write_latch & 0xffff0000u) | value;
    }
}

u16 Model1::register_read16(u32 address, bool from_io)
{
    address &= 0xffffff;
    if (address >= 0xd00000 && address < 0xe00000) {
        switch (address & 0xfe0000) {
            case 0xd00000: return m_tgp.host_ram_adr_r();
            case 0xd20000: return m_tgp.host_ram_r((address >> 1) & 1);
            case 0xd80000: return copro_fifo_read((address >> 1) & 1, from_io);
            case 0xdc0000: return 0xffff;
            default: break;
        }
    }
    if (address >= kDualPort && address < kDualPort + 0x1000) {
        m_cpu.add_wait_cycles(kDualPortWaitCycles);
        return m_dpram.right_read((address - kDualPort) >> 1);
    }
    if (address >= kUart && address < kUart + 4) return m_uart.read((address - kUart) >> 1);
    if (m_has_comm && address >= kComm && address < kComm + 0x1004) {
        const u32 offset = address - kComm;
        if (offset < M1Comm::kSharedSize) {
            return static_cast<u16>(m_comm.share_read(offset) | (m_comm.share_read(offset + 1) << 8));
        }
        return offset == 0x1000 ? m_comm.cn_read() : m_comm.fg_read();
    }
    if (address >= kListCtl && address < kListCtl + 4) {
        return (address & 2) == 0 ? static_cast<u16>(m_listctl[0] | 0x30) : m_listctl[1];
    }
    if (address == kGlue + 2) return m_irq_mask;
    if (address >= kGlue + 0xc && address < kGlue + 0x10) return timer_r((address - (kGlue + 0xc)) >> 1);

    ++m_unmapped;
    if (m_log_unmapped) SM2_WARN("model1: unmapped read %06x (pc %06x)", address, m_cpu.ppc());
    return 0;
}

void Model1::register_write16(u32 address, u16 value, u16 mask)
{
    address &= 0xffffff;
    const bool low = (mask & 0x00ff) != 0;
    if (address >= 0xd00000 && address < 0xe00000) {
        switch (address & 0xfe0000) {
            case 0xd00000: m_tgp.host_ram_adr_w(static_cast<u16>((m_tgp.host_ram_adr_r() & ~mask) | (value & mask))); return;
            case 0xd20000: m_tgp.host_ram_w((address >> 1) & 1, value); return;
            case 0xd80000: copro_fifo_write((address >> 1) & 1, value); return;
            default: break;
        }
    }
    if (address >= kDualPort && address < kDualPort + 0x1000) {
        if (low) m_dpram.right_write((address - kDualPort) >> 1, static_cast<u8>(value));
        return;
    }
    if (address >= kUart && address < kUart + 4) {
        if (low) {
            m_uart.write((address - kUart) >> 1, static_cast<u8>(value));
            sound_ready_w();
        }
        return;
    }
    if (m_has_comm && address >= kComm && address < kComm + 0x1004) {
        const u32 offset = address - kComm;
        if (offset < M1Comm::kSharedSize) {
            if (low) m_comm.share_write(offset, static_cast<u8>(value));
            if ((mask & 0xff00) != 0) m_comm.share_write(offset + 1, static_cast<u8>(value >> 8));
        } else if (low) {
            if (offset == 0x1000) m_comm.cn_write(static_cast<u8>(value));
            else m_comm.fg_write(static_cast<u8>(value));
        }
        return;
    }
    if (address >= kListCtl && address < kListCtl + 4) {
        u16& reg = m_listctl[(address >> 1) & 1];
        reg      = static_cast<u16>((reg & ~mask) | (value & mask));
        return;
    }
    if (address >= kGlue && address < kGlue + 0x10) {
        switch (address - kGlue) {
            case 0x0:
                if (low) irq_control_w(static_cast<u8>(value));
                return;
            case 0x2:
                if (low) {
                    m_irq_mask = static_cast<u8>(value);
                    sound_ready_w();
                }
                return;
            case 0x4:
                if (low && (value & 0xf) == 1) m_bank_offset = 0x1000000 + 0x100000 * ((value >> 4) & 7);
                return;
            case 0x6: m_timer_mode = static_cast<u16>((m_timer_mode & ~mask) | (value & mask)); return;
            case 0x8:
            case 0xa: timer_period_w((address - (kGlue + 8)) >> 1, static_cast<u16>((m_timers[((address - (kGlue + 8)) >> 1) & 1].period & ~mask) | (value & mask))); return;
            default: return;  // 0xc/0xe: the count registers ignore writes
        }
    }
    // Sync registers and the rest of the 0x72-0x77 block are not used.
    if (address >= 0x720000 && address < 0x780000) return;

    ++m_unmapped;
    if (m_log_unmapped) SM2_WARN("model1: unmapped write %06x = %04x (pc %06x)", address, value, m_cpu.ppc());
}

u8 Model1::read8(u32 address)
{
    if (const u8* p = ram_window(address, false)) return *p;
    const u16 word = register_read16(address & ~1u, false);
    return static_cast<u8>((address & 1) != 0 ? word >> 8 : word);
}

u16 Model1::read16(u32 address)
{
    if (const u8* p = ram_window(address, false)) {
        u16 v;
        std::memcpy(&v, p, 2);
        return v;
    }
    return register_read16(address, false);
}

u32 Model1::read32(u32 address)
{
    return read16(address) | (static_cast<u32>(read16(address + 2)) << 16);
}

void Model1::write8(u32 address, u8 value)
{
    if (u8* p = ram_window(address, true)) {
        *p = value;
        note_ram_write(address);
        return;
    }
    if ((address & 1) != 0) {
        register_write16(address & ~1u, static_cast<u16>(value << 8), 0xff00);
    } else {
        register_write16(address, value, 0x00ff);
    }
}

void Model1::write16(u32 address, u16 value)
{
    if (u8* p = ram_window(address, true)) {
        std::memcpy(p, &value, 2);
        note_ram_write(address);
        return;
    }
    register_write16(address, value, 0xffff);
}

void Model1::write32(u32 address, u32 value)
{
    write16(address, static_cast<u16>(value));
    write16(address + 2, static_cast<u16>(value >> 16));
}

u16 Model1::io_read16(u32 address)
{
    return register_read16(address, true);
}

void Model1::io_write16(u32 address, u16 value)
{
    register_write16(address, value, 0xffff);
}

// -- video -------------------------------------------------------------------------

void Model1::compose_video()
{
    if (m_palette_dirty) {
        m_video.refresh_pens();
        m_palette_dirty = false;
    }
    m_video.compose();
}

void Model1::lamp_output_w(u8 value)
{
    record_lamp_latch(value);
}

void Model1::r360_w(u8 value)
{
    // Command group B is the game state; the base answers with a status, or with
    // the throttle for 0xaf. MAME's model1_state::r360_w.
    record_drive_board_write(value);
    switch (value) {
        case 0xbf:
        case 0xbe:
        case 0xba:
        case 0xb9: m_r360_state = static_cast<u8>(~0x40); break;
        case 0xbd: m_r360_state = static_cast<u8>(~0x44); break;
        case 0xbc: m_r360_state = static_cast<u8>(~0x45); break;
        case 0xbb: m_r360_state = static_cast<u8>(~0x46); break;
        case 0xaf: m_r360_state = static_cast<u8>(~m_inputs.analog[m_r360_throttle]); break;
        default: break;
    }
}

void Model1::log_unmapped_summary() const
{
    if (m_unmapped != 0) SM2_INFO("model1: %llu unmapped accesses", static_cast<unsigned long long>(m_unmapped));
}

// -- persistence -------------------------------------------------------------------

void Model1::load_nvram()
{
    if (m_nvram_directory.empty() || m_game.name.empty()) return;
    (void)apply_default_image(m_roms.region("nvram"), m_nvram);
    (void)apply_default_image(m_roms.region("eeprom"), m_uses_io2 ? m_ioboard2.eeprom().bytes() : m_ioboard.eeprom().bytes());
    if ((m_game.parent.empty() ? m_game.name : m_game.parent) == "vr") {
        vr_special_cabinet(m_uses_io2 ? m_ioboard2.eeprom().bytes() : m_ioboard.eeprom().bytes());
    }

    const std::filesystem::path base = m_nvram_directory;
    const std::filesystem::path path = base / (m_game.name + ".nv");
    if (std::FILE* handle = std::fopen(path.string().c_str(), "rb")) {
        const usize read = std::fread(m_nvram.data(), 1, m_nvram.size(), handle);
        std::fclose(handle);
        if (read != m_nvram.size()) {
            SM2_WARN("nvram '%s' is the wrong size; starting blank", path.string().c_str());
            std::fill(m_nvram.begin(), m_nvram.end(), u8{0xff});
        }
    }
    const std::string eeprom = (base / (m_game.name + ".eeprom")).string();
    (void)(m_uses_io2 ? m_ioboard2.eeprom().load(eeprom) : m_ioboard.eeprom().load(eeprom));
}

void Model1::save_nvram() const
{
    if (m_nvram_directory.empty() || m_game.name.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(m_nvram_directory, error);
    const std::filesystem::path base = m_nvram_directory;
    const std::filesystem::path path = base / (m_game.name + ".nv");
    if (std::FILE* handle = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(m_nvram.data(), 1, m_nvram.size(), handle);
        std::fclose(handle);
    }
    const std::string eeprom = (base / (m_game.name + ".eeprom")).string();
    (void)(m_uses_io2 ? m_ioboard2.eeprom().save(eeprom) : m_ioboard.eeprom().save(eeprom));
}

void Model1::serialize(Archive& ar)
{
    m_cpu.serialize(ar);
    m_tgp.serialize(ar);
    m_geometry.serialize(ar);
    m_video.serialize(ar);
    if (m_uses_io2) {
        m_ioboard2.serialize(ar);
    } else {
        m_ioboard.serialize(ar);
    }
    m_dpram.serialize(ar);
    m_uart.serialize(ar);
    m_m1audio.serialize(ar);
    if (m_has_comm) m_comm.serialize(ar);

    ar.bytes(m_nvram.data(), m_nvram.size());
    ar.bytes(m_work_ram.data(), m_work_ram.size());
    ar.bytes(m_list_words.data(), m_list_words.size());
    ar.bytes(m_tile_ram.data(), m_tile_ram.size());
    if (ar.loading()) m_video.latch_tile_ram();
    ar.bytes(m_char_ram.data(), m_char_ram.size());
    ar.bytes(m_palette_ram.data(), m_palette_ram.size());
    ar.bytes(m_colorxlat.data(), m_colorxlat.size());

    ar.raw(m_bank_offset);
    ar.bytes(m_listctl, 2);
    ar.raw(m_r360_state);
    ar.raw(m_irq_status);
    ar.raw(m_irq_mask);
    ar.raw(m_last_irq);
    ar.raw(m_timer_mode);
    ar.bytes(m_timers.data(), m_timers.size());
    ar.raw(m_fifo_read_latch);
    ar.raw(m_fifo_write_latch);
    ar.raw(m_cycles);
    ar.raw(m_frame_start);
    ar.raw(m_frames);
    ar.raw(m_tgp_debt);
    ar.raw(m_peripheral_debt);
    ar.raw(m_inputs);
}

bool Model1::save_state_data(std::vector<u8>& out) const
{
    auto* self = const_cast<Model1*>(this);
    return save_state_to_buffer(out, m_game.name, static_cast<u32>(m_game.board), m_in_frame,
                                [self](Archive& ar) { self->serialize(ar); });
}

bool Model1::load_state_data(std::span<const u8> data)
{
    if (!load_state_from_buffer(data, m_game.name, static_cast<u32>(m_game.board), m_in_frame,
                                [this](Archive& ar) { serialize(ar); })) {
        return false;
    }
    ++m_table_generation;
    ++m_tile_generation;
    ++m_char_generation;
    m_palette_dirty = true;
    irq_update();
    return true;
}

}  // namespace sm2::hw
