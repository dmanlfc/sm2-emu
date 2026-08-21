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
// Sega Model 2B-CRX.
//
// Derived from MAME's model2b_state in src/mame/sega/model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels).
//
// The address decode follows MAME's model2b_crx_mem region for region so the
// two can be compared directly.

#include "hw/model2b.h"

#include "core/log.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Little-endian access helpers (same as model2.cpp)
// ---------------------------------------------------------------------------

[[nodiscard]] inline u16 load16(const u8* p)
{
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] inline u32 load32(const u8* p)
{
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void store16(u8* p, u16 v) { std::memcpy(p, &v, sizeof(v)); }
inline void store32(u8* p, u32 v) { std::memcpy(p, &v, sizeof(v)); }

[[nodiscard]] std::span<const u16> as_halfwords(std::span<const u8> bytes)
{
    static_assert(std::endian::native == std::endian::little);
    if (bytes.empty()) return {};
    return {reinterpret_cast<const u16*>(bytes.data()), bytes.size() / sizeof(u16)};
}

[[nodiscard]] std::span<const u32> as_words(std::span<const u8> bytes)
{
    static_assert(std::endian::native == std::endian::little);
    if (bytes.empty()) return {};
    return {reinterpret_cast<const u32*>(bytes.data()), bytes.size() / sizeof(u32)};
}

// ---------------------------------------------------------------------------
// Model 2B memory map
// ---------------------------------------------------------------------------
// Differences from Model 2A are noted inline.

constexpr u32 kRomMainCpu      = 0x00000000;  // 2 MB
constexpr u32 kScratchRam      = 0x00200000;  // 256 KB (same as 2A)
constexpr u32 kWorkRam         = 0x00500000;  // 1 MB
constexpr u32 kGeoPort         = 0x00800000;  // 16 KB, Geometrizer function ports
constexpr u32 kGeoProgram      = 0x00804000;  // 16 KB, Geometrizer upload / write-through
constexpr u32 kCoproFunction   = 0x00880000;  // 16 KB, copro function port
constexpr u32 kCoproFifo       = 0x00884000;  // 16 KB, copro FIFO read/write
constexpr u32 kSharcIop        = 0x008c0000;  // 4 KB, SHARC IOP register write
constexpr u32 kBufferRam       = 0x00900000;  // 128 KB, mirror 0x60000
constexpr u32 kVideoRegs       = 0x00980000;  // copro control, geo control, videoctl
constexpr u32 kUart            = 0x009c0000;  // UART (Model 2B position)
constexpr u32 kCpuControl      = 0x00e00000;
constexpr u32 kIrqRegs         = 0x00e80000;
constexpr u32 kTimerRegs       = 0x00f00000;
constexpr u32 kTileRam         = 0x01000000;  // 64 KB, mirror 0x110000
constexpr u32 kCharRam         = 0x01080000;  // 512 KB, mirror 0x100000
constexpr u32 kPaletteRam      = 0x01800000;  // 16 KB
constexpr u32 kColorXlat       = 0x01810000;  // 48 KB
constexpr u32 kZClip           = 0x0181c000;
constexpr u32 kCommRam         = 0x01a00000;  // 16 KB, mirror 0x10000
constexpr u32 kCommCtl         = 0x01a04000;  // CN and FG, mirror 0x10000
constexpr u32 kIoController    = 0x01c00000;  // Same as 2A
constexpr u32 kNvram           = 0x01d00000;  // 16 KB
constexpr u32 kCryptRam        = 0x01d80000;  // 64 KB, 315-5881 staging buffer
constexpr u32 kCryptReady      = 0x01d90000;
constexpr u32 kCryptAddrLo     = 0x01d90008;
constexpr u32 kCryptAddrHi     = 0x01d9000a;
constexpr u32 kCryptSubkey     = 0x01d9000c;
constexpr u32 kCryptData       = 0x01d9000e;
constexpr u32 kRomMainData     = 0x02000000;  // 32 MB window
constexpr u32 kRomMainDataHigh = 0x06000000;  // 16 MB at offset 0x1000000
constexpr u32 kRenderMode      = 0x10000000;
constexpr u32 kPolygonCount    = 0x10400000;
// Model 2B texture RAM: at 0x11000000, not 0x12000000.
constexpr u32 kTextureRam0     = 0x11000000;  // 1 MB + 1 MB mirror
constexpr u32 kTextureRam1     = 0x11200000;  // 1 MB + 1 MB mirror
constexpr u32 kLumaRam         = 0x11400000;  // byte-wide, 0x10000 entries
constexpr u32 kFramebufferA    = 0x11600000;  // 512 KB
constexpr u32 kFramebufferB    = 0x11680000;  // 512 KB

[[nodiscard]] inline bool in_mirrored(u32 address, u32 base, u32 size, u32 mirror)
{
    const u32 folded = address & ~mirror;
    return folded >= base && folded < base + size;
}

}  // namespace

// ===========================================================================
// CoproSharc implementation
// ===========================================================================

CoproSharc::CoproSharc()
    : m_cpu(*this)
{
    m_fifo_in.configure(kFifoDepth);
    m_fifo_out.configure(kFifoDepth);

    // Coprocessor-side flow control (SHARC halts/resumes based on FIFO state).
    m_fifo_in.set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_fifo_in.set_on_unempty([this] { m_cpu.set_halted(false); });

    m_fifo_out.set_on_full([this] { m_cpu.set_halted(true); });
    m_fifo_out.set_on_unfull([this] { m_cpu.set_halted(false); });
}

void CoproSharc::attach(std::span<const u32> data_rom, std::span<u32> buffer_ram)
{
    m_data_rom   = data_rom;
    m_buffer_ram = buffer_ram;
}

void CoproSharc::reset()
{
    m_cpu.reset();
    m_cpu.set_halted(true);

    m_fifo_in.clear();
    m_fifo_out.clear();

    m_control      = 0;
    m_upload_count = 0;

    // SHARC starts with flag0=1 (FIFO-in empty), flag1=0 (FIFO-out not full).
    m_cpu.set_flag_input(0, 1);
    m_cpu.set_flag_input(1, 0);
}

s32 CoproSharc::run(s32 cycles)
{
    if (m_cpu.halted()) return 0;
    return m_cpu.run(cycles);
}

// -- host side -------------------------------------------------------------

void CoproSharc::control_write(u32 value)
{
    if (((value ^ m_control) & 0x80000000u) != 0) {
        if ((value & 0x80000000u) != 0) {
            SM2_DEBUG("copro_sharc: microcode upload started");
            m_upload_count = 0;
            // MAME's model2b_state::copro_halt() is deliberately empty — unlike
            // the TGP on Model 2A, the SHARC is not halted for the upload. It
            // is already halted from machine reset and stays that way until the
            // boot write below.
        } else {
            SM2_DEBUG("copro_sharc: booting, %u 16-bit word(s) uploaded", m_upload_count);
            // model2b_state::copro_boot() only clears the halt line. It must
            // NOT reset the core: a reset would clear the on-chip SRAM banks
            // that the microcode was just DMA'd into.
            m_cpu.set_halted(false);
        }
    }
    m_control = value;
}

void CoproSharc::host_fifo_write(u32 value)
{
    if ((m_control & 0x80000000u) != 0) {
        // Upload mode: 16-bit words sent to SHARC via external DMA.
        m_cpu.external_dma_write(m_upload_count, static_cast<u64>(value & 0xffff));
        ++m_upload_count;
        return;
    }
    m_fifo_in.push(value);
    // Update FLAG0: tells the SHARC whether fifo_in is empty.
    m_cpu.set_flag_input(0, m_fifo_in.empty() ? 1 : 0);
}

u32 CoproSharc::host_fifo_read()
{
    u32 value = m_fifo_out.pop();
    // Update FLAG1: tells the SHARC whether fifo_in is full (back-pressure).
    m_cpu.set_flag_input(1, m_fifo_in.full() ? 1 : 0);
    return value;
}

void CoproSharc::function_port_write(u32 byte_offset, u32 value)
{
    // The function address lives in the offset (MAME: a = (offset >> 2) & 0xff,
    // where offset is the dword count, so: a = (byte_offset >> 4) & 0xff).
    const u32 function = (byte_offset >> 4) & 0xff;
    const u32 command  = (value & 0x800fffffu) | (function << 23);
    m_fifo_in.push(command);
    m_cpu.set_flag_input(0, m_fifo_in.empty() ? 1 : 0);
}

void CoproSharc::iop_write(u32 offset, u32 data)
{
    m_cpu.external_iop_write(offset, data);
}

// -- sharc::Bus implementation ---------------------------------------------

// The Model 2B board does not map anything into the SHARC's program space —
// MAME only installs copro_sharc_map on AS_DATA. Program accesses are serviced
// entirely by the chip's internal SRAM banks inside cpu::sharc::SHARC, so these
// only ever see out-of-range addresses.
u64 CoproSharc::pm_read48(u32 /*address*/) { return 0; }

void CoproSharc::pm_write48(u32 /*address*/, u64 /*data*/) {}

u32 CoproSharc::pm_read32(u32 /*address*/) { return 0; }

void CoproSharc::pm_write32(u32 /*address*/, u32 /*data*/) {}

u32 CoproSharc::dm_read32(u32 address)
{
    // MAME's copro_sharc_map:
    //   0x0400000–0x0bfffff  FIFO-in read
    //   0x0c00000–0x13fffff  FIFO-out write (read returns 0)
    //   0x1400000–0x1bfffff  buffer RAM
    //   0x1c00000–0x1dfffff  copro_data ROM
    if (address >= 0x0400000 && address <= 0x0bfffff) {
        u32 value = m_fifo_in.pop();
        // Update FLAG0 after consuming a word.
        m_cpu.set_flag_input(0, m_fifo_in.empty() ? 1 : 0);
        return value;
    }
    if (address >= 0x0c00000 && address <= 0x13fffff) {
        return 0;  // Write-only region
    }
    if (address >= 0x1400000 && address <= 0x1bfffff) {
        const u32 index = (address - 0x1400000) & 0x7fff;
        if (index < m_buffer_ram.size()) {
            return m_buffer_ram[index];
        }
        return 0;
    }
    if (address >= 0x1c00000 && address <= 0x1dfffff) {
        const u32 index = address - 0x1c00000;
        if (index < m_data_rom.size()) {
            return m_data_rom[index];
        }
        return 0;
    }
    return 0;
}

void CoproSharc::dm_write32(u32 address, u32 data)
{
    if (address >= 0x0c00000 && address <= 0x13fffff) {
        m_fifo_out.push(data);
        // Update FLAG1.
        m_cpu.set_flag_input(1, m_fifo_in.full() ? 1 : 0);
        return;
    }
    if (address >= 0x1400000 && address <= 0x1bfffff) {
        const u32 index = (address - 0x1400000) & 0x7fff;
        if (index < m_buffer_ram.size()) {
            m_buffer_ram[index] = data;
        }
        return;
    }
}

// Unused: the host DMA upload path goes through cpu::sharc::SHARC::
// external_dma_write, which applies the 16/48 packing and writes into the
// chip's own program memory. This Bus hook exists only for boards that map
// external memory into the SHARC's program space; Model 2B does not.
void CoproSharc::external_dma_write(u32 /*address*/, u64 /*data*/) {}

// ===========================================================================
// Model2B implementation
// ===========================================================================

Model2B::Model2B() : m_cpu(*this) {}

Model2B::~Model2B() = default;

bool Model2B::init(const rom::GameSpec& game, rom::RomSet roms)
{
    m_game = game;
    m_roms = std::move(roms);

    m_rom_maincpu   = m_roms.region("maincpu");
    m_rom_main_data = m_roms.region("main_data");

    if (m_rom_maincpu.empty()) {
        SM2_ERROR("model2b: the 'maincpu' region is missing");
        return false;
    }

    // Allocate RAM.
    m_work_ram.assign(0x100000, 0);
    m_scratch_ram.assign(0x40000, 0);
    m_buffer_ram.assign(0x20000 / 4, 0);
    m_tile_ram.assign(0x10000, 0);
    m_char_ram.assign(0x80000, 0);
    m_palette_ram.assign(0x4000 / 2, 0);
    m_colorxlat.assign(0xc000 / 2, 0);
    m_luma_ram.assign(0x10000, 0);
    m_texture_ram0.assign(0x100000 / 4, 0);
    m_texture_ram1.assign(0x100000 / 4, 0);
    m_framebuffer_a.assign(0x80000 / 2, 0);
    m_framebuffer_b.assign(0x80000 / 2, 0);
    m_nvram.assign(0x4000, 0xff);
    m_cpu_control.assign(0x40, 0);
    m_comm_ram.assign(0x4000, 0);
    m_crypt_ram.assign(0x10000, 0);

    // 315-5881 protection chip setup.
    if (game.protection == rom::Protection::Sega315_5881 && game.protection_key == 0) {
        SM2_WARN("model2b: %s needs a 315-5881 key and the database has none",
                 game.name.c_str());
    }
    m_crypt.set_key(game.protection_key);
    m_crypt.set_read_callback([this](u32 word_address) {
        const u32 offset = (word_address * 2) & 0xffffu;
        return static_cast<u16>((m_crypt_ram[offset] << 8) | m_crypt_ram[offset + 1]);
    });

    // Video stage.
    m_video.attach(m_tile_ram, m_char_ram, m_palette_ram, m_colorxlat);

    // The link board's 16 KB is the same storage the i960 reaches at
    // 0x01a00000; the board keeps it so the access stays a burst window.
    m_comm.attach_shared(m_comm_ram);

    // SHARC coprocessor: reads copro_data ROM and writes into the display list
    // buffer. The table ROM is not used by the SHARC (it had its own math).
    m_rom_copro_data = m_roms.region("copro_data");
    m_copro.attach(as_words(m_rom_copro_data), m_buffer_ram);

    // Geometry engine (high-level): polygon ROM + texture ROM + buffer RAM.
    m_rom_polygons = m_roms.region("polygons");
    m_rom_textures = m_roms.region("textures");
    m_geometry.attach(as_words(m_rom_polygons), as_halfwords(m_rom_textures),
                      m_buffer_ram);

    // Sound board.
    m_sound.attach(m_roms.region("audiocpu"), m_roms.region("samples"));
    m_uart.set_tx_handler([this](u8 value) { m_sound.midi_in(value); });
    m_sound.set_midi_out_handler([this](u8 value) { m_uart.write_rxd(value); });
    m_uart.set_ready_handler([this] { sound_ready_w(); });

    // Host-side FIFO flow control: halt/release the i960 on FIFO events.
    m_copro.fifo_out().set_on_empty_retry([this] { m_cpu.stall(); });
    m_copro.fifo_out().set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_copro.fifo_out().set_on_unempty([this] { m_cpu.set_halted(false); });
    m_copro.fifo_in().set_on_full([this] { m_cpu.set_halted(true); });
    m_copro.fifo_in().set_on_unfull([this] { m_cpu.set_halted(false); });

    SM2_INFO("model2b: %s board, %s", rom::board_name(game.board), game.title.c_str());
    reset();
    return true;
}

void Model2B::reset()
{
    m_intreq = 0;
    m_intena = 0;
    m_pending_intena_valid = false;
    m_timers.fill(Timer{});

    m_videocontrol = 0;
    m_render_mode  = false;
    m_render_test  = false;
    m_render_unk   = false;
    m_geoctl       = 0;
    m_geocnt       = 0;
    m_geo_write_start_address = 0;
    m_geo_read_start_address  = 0;
    m_ctrlmode      = false;
    m_palette_dirty = true;

    m_cycles      = 0;
    m_frame_start = 0;
    m_frames      = 0;

    m_unmapped_reads.clear();
    m_unmapped_writes.clear();

    m_video.reset();
    m_copro.reset();
    m_geometry.reset();
    m_sound.reset();
    m_render_list.clear();

    m_copro_debt    = 0;
    m_in_copro_sync = false;

    // Display-list buffer pattern (same as 2A).
    std::fill(m_buffer_ram.begin(), m_buffer_ram.end(), 0x07800f0fu);

    // Analogue controls at rest.
    m_inputs.analog.fill(0);
    for (usize channel = 0; channel < m_inputs.analog.size(); ++channel) {
        if (m_game.analog[channel].control != rom::AnalogControl::None) {
            m_inputs.analog[channel] = m_game.analog[channel].rest;
        }
    }
    m_inputs.gun_p1x = m_game.lightgun.p1x.rest;
    m_inputs.gun_p1y = m_game.lightgun.p1y.rest;
    m_inputs.gun_p2x = m_game.lightgun.p2x.rest;
    m_inputs.gun_p2y = m_game.lightgun.p2y.rest;
    m_inputs.gears   = 0;
    m_gear_selected  = 0;

    m_io.reset();
    m_crypt.reset();

    m_uart.configure(kCpuClock, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();
    m_comm.reset();

    // I/O controller callbacks (same as 2A).
    m_io.set_output(0, [this](u8 value) { io_port_a_write(value); });
    m_io.set_input(1, [this] { return io_port_b_read(); });
    m_io.set_input(2, [this] { return io_port_c_read(); });
    m_io.set_input(3, [this] { return m_inputs.in2; });
    m_io.set_output(5, [this](u8 value) { lamp_output_w(value); });
    m_io.set_input(6, [this] { return m_inputs.dipswitches; });
    for (u32 channel = 0; channel < Io315_5649::kAnalogCount; ++channel) {
        m_io.set_analog(channel, [this, channel] { return m_inputs.analog[channel]; });
    }

    if (m_game.drive_board) {
        m_io.set_output(4, [this](u8 value) { drive_board_write(value); });
    }

    if (m_game.motion_base) {
        // Rail Chase 2 talks to its motion base over ports D and E and will not
        // start until the base answers. MAME does not emulate the board either:
        // rchase2_drive_board_r acknowledges four commands and nothing else, with
        // the comment "simulate this so that it passes the initial checks". Port D
        // carries the answer rather than IN2 on this cabinet.
        m_io.set_output(4, [this](u8 value) { m_motion_command = value; });
        m_io.set_input(3, [this] { return motion_base_read(); });
    }

    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

void Model2B::run_frame()
{
    for (u32 line = 0; line < kVerticalTotal; ++line) {
        const u64 line_end   = m_frame_start + static_cast<u64>(line + 1) * kCyclesPerLine;
        const u64 line_start = m_cycles;

        while (m_cycles < line_end) {
            u64 target = std::min(line_end, next_timer_deadline());
            if (target <= m_cycles) target = m_cycles + 1;
            target = std::min(target, m_cycles + kCoproInterleave);

            const s32 slice = static_cast<s32>(std::min<u64>(target - m_cycles, 1u << 20));
            const s32 used  = m_cpu.run(slice);
            m_cycles += static_cast<u64>(used);
            step_copro(static_cast<u32>(used > 0 ? used : 0));

            m_uart.run(static_cast<u32>(used > 0 ? used : 0));
            service_timers();

            if (m_pending_intena_valid && m_cycles >= m_pending_intena_cycle) {
                m_intena = m_pending_intena;
                m_pending_intena_valid = false;
                sound_ready_w();
                irq_update();
            }
        }

        const u32 line_cycles = static_cast<u32>(m_cycles - line_start);
        m_sound.run(line_cycles);
        sound_ready_w();

        if (line == kVisibleHeight) {
            on_vblank_start();
        }
    }

    m_frame_start += kCyclesPerFrame;
    ++m_frames;
}

void Model2B::step_copro(u32 host_cycles)
{
    // The SHARC runs at 32 MHz vs the i960's 25 MHz, so the ratio is 32/25.
    // To keep integer arithmetic: debt accumulates in 25ths.
    m_copro_debt += host_cycles * 32;
    const s32 cycles = static_cast<s32>(m_copro_debt / 25);
    m_copro_debt %= 25;

    if (cycles > 0) {
        static_cast<void>(m_copro.run(cycles));
    }
}

void Model2B::sync_copro()
{
    if (m_in_copro_sync) return;
    m_in_copro_sync = true;

    s32 remaining = kCoproSyncLimit;
    while (remaining > 0 && m_copro.output_empty() && !m_copro.cpu().halted()) {
        const s32 slice = std::min<s32>(remaining, 32);
        const s32 used  = m_copro.run(slice);
        if (used <= 0) break;
        remaining -= used;
        const u32 owed = static_cast<u32>(used) * 25;
        m_copro_debt = m_copro_debt > owed ? m_copro_debt - owed : 0;
    }

    m_in_copro_sync = false;
}

u64 Model2B::next_timer_deadline() const
{
    u64 deadline = ~0ULL;
    for (const Timer& timer : m_timers) {
        if (timer.running) {
            deadline = std::min(deadline, timer.start_cycle + timer.original);
        }
    }
    return deadline;
}

void Model2B::service_timers()
{
    for (u32 index = 0; index < m_timers.size(); ++index) {
        Timer& timer = m_timers[index];
        if (!timer.running) continue;
        if (m_cycles < timer.start_cycle + timer.original) continue;

        timer.running = false;
        timer.value   = 0xfffff;
        raise_interrupt(1u << (index + 2));
    }
}

void Model2B::on_vblank_start()
{
    if ((m_videocontrol & 1) == 0 || (m_frames & 1) == 0) {
        m_geometry.set_read_start_address(m_geo_read_start_address);
        // The CRTC sync registers move the projected image relative to the
        // monitor, and MAME applies them in model2_3d_project on every board.
        // Leaving them at zero here shifted the whole 3D scene: Wave Runner's
        // came out 90 lines high and 8 columns right, leaving the bottom of the
        // screen empty.
        m_geometry.set_crtc_offsets(m_video.crtc_x_offset(), m_video.crtc_y_offset());
        m_geometry.run(&m_render_list);
    }
    raise_interrupt(1u << 0);
    // MAME's screen_vblank calls the link board here, after the geometry pass
    // and the interrupt. It is what steps the ring protocol.
    m_comm.vblank();
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void Model2B::raise_interrupt(u32 line)
{
    if ((m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

void Model2B::irq_update()
{
    const u32 active = m_intreq & m_intena;
    m_cpu.set_irq_line(cpu::i960::I960_IRQ0,
                       (active & 0x0001) != 0 ? cpu::i960::kAssertLine
                                              : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ1,
                       (active & 0x0002) != 0 ? cpu::i960::kAssertLine
                                              : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ2,
                       (active & 0x03fc) != 0 ? cpu::i960::kAssertLine
                                              : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ3,
                       (active & 0x0c00) != 0 ? cpu::i960::kAssertLine
                                              : cpu::i960::kClearLine);
}

void Model2B::sound_ready_w()
{
    const u32 line = 1u << 10;
    if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

// ---------------------------------------------------------------------------
// Timer registers
// ---------------------------------------------------------------------------

u32 Model2B::timers_r(u32 index)
{
    if (index >= m_timers.size()) return 0;
    const Timer& timer = m_timers[index];
    if (!timer.running) return timer.value;
    const u64 elapsed = m_cycles - timer.start_cycle;
    if (elapsed >= timer.original) return 0;
    return static_cast<u32>(timer.original - elapsed);
}

void Model2B::timers_w(u32 index, u32 value)
{
    if (index >= m_timers.size()) return;
    Timer& timer   = m_timers[index];
    timer.value    = value & 0xfffff;
    timer.original = timer.value;
    timer.start_cycle = m_cycles;
    timer.running  = true;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

u8 Model2B::io_port_b_read()
{
    const u8 panel = m_inputs.in0;
    if (!m_ctrlmode) {
        return panel;
    }
    return static_cast<u8>(0xc0 | (m_eeprom.data_out() ? 0x20 : 0x00) | 0x10
                           | (panel & 0x0f));
}

u8 Model2B::io_port_c_read()
{
    return m_inputs.in1;
}

void Model2B::io_port_a_write(u8 value)
{
    m_ctrlmode = bit(value, 0) != 0;
    m_eeprom.set_di(bit(value, 5) != 0);
    m_eeprom.set_cs(bit(value, 6) != 0);
    m_eeprom.set_clk(bit(value, 7) != 0);
}

void Model2B::lamp_output_w(u8 /*value*/) {}

void Model2B::drive_board_write(u8 value)
{
    m_drive_board_latch = value;
}

u8 Model2B::motion_base_read() const
{
    // MAME's rchase2_drive_board_r, verbatim. Each of the four cylinders reports
    // ready by pulling its bit low, and the command that asks about it is
    // accepted in either nibble.
    u8 data = 0xff;
    if (m_motion_command == 0xe0 || m_motion_command == 0x0e) data &= ~u8{1};
    if (m_motion_command == 0xd0 || m_motion_command == 0x0d) data &= ~u8{2};
    if (m_motion_command == 0xb0 || m_motion_command == 0x0b) data &= ~u8{4};
    if (m_motion_command == 0x70 || m_motion_command == 0x07) data &= ~u8{8};
    return data;
}

// ---------------------------------------------------------------------------
// Address decode — flat memory windows
// ---------------------------------------------------------------------------

Model2B::Window Model2B::resolve(u32 address)
{
    const auto window = [](auto& storage, u32 offset, bool writable, u16 flags,
                           Notify notify = Notify::None) {
        Window result;
        auto* bytes = reinterpret_cast<u8*>(storage.data());
        const usize total = storage.size() * sizeof(typename std::remove_reference_t<
                                                    decltype(storage)>::value_type);
        if (offset >= total) return Window{};
        result.base     = bytes + offset;
        result.size     = total - offset;
        result.writable = writable;
        result.flags    = flags;
        result.notify   = notify;
        result.offset   = offset;
        return result;
    };

    const auto rom_window = [](std::span<const u8> storage, u32 offset) {
        Window result;
        if (offset >= storage.size()) return Window{};
        result.base     = const_cast<u8*>(storage.data()) + offset;
        result.size     = storage.size() - offset;
        result.writable = false;
        result.rom      = true;
        result.flags    = cpu::kBusFlagBurst;
        return result;
    };

    if (address >= kRomMainCpu && address < kRomMainCpu + 0x200000) {
        return rom_window(m_rom_maincpu, address - kRomMainCpu);
    }
    if (address >= kWorkRam && address < kWorkRam + 0x100000) {
        return window(m_work_ram, address - kWorkRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kScratchRam && address < kScratchRam + 0x40000) {
        return window(m_scratch_ram, address - kScratchRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kRomMainData && address < kRomMainData + 0x2000000) {
        return rom_window(m_rom_main_data, address - kRomMainData);
    }
    if (address >= kRomMainDataHigh && address < kRomMainDataHigh + 0x1000000) {
        return rom_window(m_rom_main_data, (address - kRomMainDataHigh) + 0x1000000);
    }
    if (in_mirrored(address, kBufferRam, 0x20000, 0x60000)) {
        return window(m_buffer_ram, address & 0x1ffff, true, cpu::kBusFlagBurst);
    }
    if (in_mirrored(address, kTileRam, 0x10000, 0x110000)) {
        return window(m_tile_ram, address & 0xffff, true, cpu::kBusFlagBurst,
                      Notify::TileRam);
    }
    if (in_mirrored(address, kCharRam, 0x80000, 0x100000)) {
        return window(m_char_ram, address & 0x7ffff, true, cpu::kBusFlagBurst,
                      Notify::CharRam);
    }
    if (address >= kPaletteRam && address < kPaletteRam + 0x4000) {
        return window(m_palette_ram, address - kPaletteRam, true, cpu::kBusFlagBurst,
                      Notify::Palette);
    }
    if (address >= kColorXlat && address < kColorXlat + 0xc000) {
        return window(m_colorxlat, address - kColorXlat, true, cpu::kBusFlagBurst,
                      Notify::Palette);
    }
    if (in_mirrored(address, kCommRam, 0x4000, 0x10000)) {
        return window(m_comm_ram, address & 0x3fff, true, cpu::kBusFlagBurst);
    }
    if (address >= kNvram && address < kNvram + 0x4000) {
        return window(m_nvram, address - kNvram, true, cpu::kBusFlagBurst);
    }
    if (m_game.protection == rom::Protection::Sega315_5881
        && address >= kCryptRam && address < kCryptRam + 0x10000) {
        return window(m_crypt_ram, address - kCryptRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kCpuControl && address < kCpuControl + 0x38) {
        return window(m_cpu_control, address - kCpuControl, true, cpu::kBusFlagNone);
    }
    if (address >= kFramebufferA && address < kFramebufferA + 0x80000) {
        return window(m_framebuffer_a, address - kFramebufferA, true, cpu::kBusFlagBurst);
    }
    if (address >= kFramebufferB && address < kFramebufferB + 0x80000) {
        return window(m_framebuffer_b, address - kFramebufferB, true, cpu::kBusFlagBurst);
    }
    // Model 2B texture RAM: 1 MB each at 0x11000000 and 0x11200000, with mirrors.
    //
    // Plain memory, unlike the original and 2A boards. Those decode texture RAM
    // through a write handler that packs two 16-bit halves into each 32-bit entry
    // (MAME's tex0_w/tex1_w); 2B and 2C map it as .ram() with no handler at all,
    // so a write lands where it is addressed. Applying the 2A transform here
    // halves every destination address and drops the upper half of each sheet,
    // which is what was streaking Wave Runner's textures.
    if (in_mirrored(address, kTextureRam0, 0x100000, 0x100000)) {
        return window(m_texture_ram0, address & 0xfffff, true, cpu::kBusFlagBurst,
                      Notify::TextureRam);
    }
    if (in_mirrored(address, kTextureRam1, 0x100000, 0x100000)) {
        return window(m_texture_ram1, address & 0xfffff, true, cpu::kBusFlagBurst,
                      Notify::TextureRam);
    }

    return {};
}

u16 Model2B::register_flags(u32 address)
{
    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        return cpu::kBusFlagBurst;
    }
    if (address >= kCoproFunction && address < kCoproFunction + 0x4000) {
        return cpu::kBusFlagBurst;
    }
    if (in_mirrored(address, 0x01020000, 4, 0x100000)) {
        return cpu::kBusFlagBurst;
    }
    if (address >= kLumaRam && address < kLumaRam + 0x20000) {
        return cpu::kBusFlagBurst;
    }
    return cpu::kBusFlagNone;
}

// ---------------------------------------------------------------------------
// Register reads
// ---------------------------------------------------------------------------

u32 Model2B::register_read(u32 address, u32 width)
{
    if (address >= kGeoPort && address < kGeoPort + 0x4000) {
        const u32 offset = address - kGeoPort;
        if (offset == 0x2008) return m_geo_write_start_address;
        if (offset == 0x3008) return m_geo_read_start_address;
        return 0;
    }

    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        return 0xffffffff;
    }

    if (address >= kCoproFifo && address < kCoproFifo + 0x4000) {
        if (m_copro.output_empty()) sync_copro();
        return m_copro.host_fifo_read();
    }

    if (address >= kVideoRegs && address < kVideoRegs + 0x40) {
        const u32 offset = address - kVideoRegs;
        if (offset < 0x04) return m_copro.control_read();
        if (offset < 0x08) {
            if (m_copro.output_empty()) sync_copro();
            return m_copro.output_empty() ? 1 : 0;
        }
        if (offset >= 0x0c && offset < 0x10) {
            const u8 parity = m_render_mode ? static_cast<u8>((m_frames & 1) << 2)
                                            : static_cast<u8>((m_frames & 2) << 1);
            return parity | (m_videocontrol & 3);
        }
        if (offset >= 0x14 && offset < 0x18) {
            // Copro status: returns -1 if upload count is zero (MAME).
            return m_copro.status();
        }
        if (offset >= 0x30 && offset < 0x40) {
            static constexpr u8 kId[16] = {
                0, 'T', 'A', 'H', 0, 'A', 'K', 'O',
                0, 'Z', 'A', 'K', 0, 'M', 'T', 'K',
            };
            u32 value = 0;
            for (u32 byte = 0; byte < width && (offset - 0x30 + byte) < 16; ++byte) {
                value |= static_cast<u32>(kId[offset - 0x30 + byte]) << (byte * 8);
            }
            return value;
        }
        return 0;
    }

    // UART at 0x009c0000 (Model 2B position).
    if (address >= kUart && address < kUart + 0x08) {
        if (width == 4) {
            const u32 data   = m_uart.read(0);
            const u32 status = m_uart.read(1);
            return data | (status << 16);
        }
        return m_uart.read((address - kUart) >> 1);
    }

    if (address >= kIrqRegs && address < kIrqRegs + 0x08) {
        return (address - kIrqRegs) < 4 ? m_intreq : m_intena;
    }

    if (address >= kTimerRegs && address < kTimerRegs + 0x10) {
        return timers_r((address - kTimerRegs) / 4);
    }

    // Link board handshake registers, on byte lanes 0 and 2 of one dword and
    // mirrored at 0x01a14000 exactly as MAME maps them. Without these the i960
    // reads an unmapped bus and a linked title never finishes its network check.
    if (in_mirrored(address, kCommCtl, 4, 0x10000)) {
        const u32 offset = address & 3;
        if (width == 4) {
            return static_cast<u32>(m_comm.cn_read())
                 | (static_cast<u32>(m_comm.fg_read()) << 16);
        }
        if (offset == 0) return m_comm.cn_read();
        if (offset == 2) return m_comm.fg_read();
        return 0xffffffff;
    }


    if (address >= kIoController && address < kIoController + 0x20) {
        const u32 offset = address - kIoController;
        if (width == 4) {
            const u32 reg = offset >> 1;
            return static_cast<u32>(m_io.read(reg))
                 | (static_cast<u32>(m_io.read(reg + 1)) << 16);
        }
        if ((offset & 1) != 0) return 0xffffffff;
        return m_io.read(offset >> 1);
    }

    if (address >= kRenderMode && address < kRenderMode + 0x200000) {
        return (static_cast<u32>(m_render_unk) << 14)
             | (static_cast<u32>(m_render_mode) << 2)
             | static_cast<u32>(m_render_test);
    }

    if (address >= kPolygonCount && address < kPolygonCount + 0x200000) {
        return m_geometry.polygon_count();
    }

    if (address >= 0x10800000 && address < 0x10800004) {
        return 0;
    }

    if (address >= kLumaRam && address < kLumaRam + 0x20000) {
        // Model 2B luma: umask16(0x00ff), so one byte per 16-bit half-word.
        const u32 index = (address - kLumaRam) / 2;
        return index < m_luma_ram.size() ? m_luma_ram[index] : 0;
    }

    if (m_game.protection == rom::Protection::Sega315_5881) {
        if (address >= kCryptReady && address < kCryptReady + 2) {
            return m_crypt.ready_r();
        }
        if (address >= kCryptData && address < kCryptData + 2) {
            return m_crypt.decrypt_le_r();
        }
    }

    note_unmapped_read(address, width);
    return 0;
}

// ---------------------------------------------------------------------------
// Register writes
// ---------------------------------------------------------------------------

void Model2B::register_write(u32 address, u32 value, u32 width)
{
    if (address >= kGeoPort && address < kGeoPort + 0x4000) {
        const u32 offset = address - kGeoPort;
        if (offset < 0x1000) {
            u32 word = 0;
            if ((value & 0x80000000) != 0) {
                word = (value & 0x800fffff) | (((offset >> 4) & 0x3f) << 23);
            } else if ((offset & 0xf) == 0) {
                word = (value & 0x000fffff) | (((offset >> 4) & 0x3f) << 23);
                if (((offset >> 4) & 0xc0) != 0 && ((offset >> 4) & 0x3f) == 1) {
                    word |= ((offset >> 10) & 3) << 29;
                }
            } else {
                return;
            }
            const u32 index = m_geo_write_start_address / 4;
            if (index < m_buffer_ram.size()) {
                m_buffer_ram[index] = word;
            }
            m_geo_write_start_address += 4;
            return;
        }
        if (offset == 0x1008) { m_geo_write_start_address = value & 0xfffff; return; }
        if (offset == 0x3008) { m_geo_read_start_address = value & 0xfffff; return; }
        return;
    }

    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        if ((m_geoctl & 0x80000000) != 0) {
            ++m_geocnt;
        } else {
            const u32 index = m_geo_write_start_address / 4;
            if (index < m_buffer_ram.size()) {
                m_buffer_ram[index] = value;
            }
            m_geo_write_start_address += 4;
        }
        return;
    }

    if (address >= kCoproFunction && address < kCoproFunction + 0x4000) {
        m_copro.function_port_write(address - kCoproFunction, value);
        return;
    }
    if (address >= kCoproFifo && address < kCoproFifo + 0x4000) {
        m_copro.host_fifo_write(value);
        return;
    }

    // SHARC IOP register write at 0x008c0000. MAME maps this as a dword
    // handler (external_iop_write), so the register index it receives is the
    // byte offset divided by four, not the byte offset itself.
    //
    // This is also how Last Bronx uploads its microcode: it configures DMA
    // channel 6 through these registers and then streams the program through
    // register 4, the external port buffer, instead of using the FIFO port with
    // the upload bit set the way every other Model 2B game does.
    if (address >= kSharcIop && address < kSharcIop + 0x1000) {
        m_copro.iop_write((address - kSharcIop) >> 2, value);
        return;
    }

    if (address >= kVideoRegs && address < kVideoRegs + 0x40) {
        const u32 offset = address - kVideoRegs;
        if (offset < 0x04) {
            m_copro.control_write(value);
            return;
        }
        if (offset >= 0x08 && offset < 0x0c) {
            if (((value ^ m_geoctl) & 0x80000000) != 0) {
                if ((value & 0x80000000) != 0) {
                    m_geocnt = 0;
                    SM2_DEBUG("model2b: geometrizer upload started");
                } else {
                    SM2_DEBUG("model2b: geometrizer boot, %u words uploaded", m_geocnt);
                }
            }
            m_geoctl = value;
            return;
        }
        if (offset >= 0x0c && offset < 0x10) {
            m_videocontrol = value;
            return;
        }
        // 0x00980020: bank control, always written as 0. Ignored.
        return;
    }

    // UART at 0x009c0000 (Model 2B position).
    if (address >= kUart && address < kUart + 0x08) {
        m_uart.write((address - kUart) >> 1, static_cast<u8>(value));
        return;
    }

    if (address >= kIrqRegs && address < kIrqRegs + 0x08) {
        if ((address - kIrqRegs) < 4) {
            m_intreq &= value;
            irq_update();
        } else {
            m_pending_intena       = value;
            m_pending_intena_cycle = m_cycles + 2;
            m_pending_intena_valid = true;
        }
        return;
    }

    if (address >= kTimerRegs && address < kTimerRegs + 0x10) {
        timers_w((address - kTimerRegs) / 4, value);
        return;
    }

    if (in_mirrored(address, 0x01020000, 4, 0x100000)) return;
    if (in_mirrored(address, 0x01040000, 2, 0x100000)) {
        m_video.set_horizontal_sync(static_cast<u16>(value));
        return;
    }
    if (in_mirrored(address, 0x01060000, 2, 0x100000)) {
        m_video.set_vertical_sync(static_cast<u16>(value));
        return;
    }
    if (in_mirrored(address, 0x01070000, 4, 0x100000)) return;

    if (address >= kZClip && address < kZClip + 4) {
        m_geometry.set_z_clip(static_cast<u8>(value));
        return;
    }

    if (in_mirrored(address, kCommCtl, 4, 0x10000)) {
        const u32 offset = address & 3;
        if (width == 4) {
            m_comm.cn_write(static_cast<u8>(value & 0xff));
            m_comm.fg_write(static_cast<u8>((value >> 16) & 0xff));
            return;
        }
        if (offset == 0) m_comm.cn_write(static_cast<u8>(value & 0xff));
        else if (offset == 2) m_comm.fg_write(static_cast<u8>(value & 0xff));
        return;
    }

    if (address >= kIoController && address < kIoController + 0x20) {
        const u32 offset = address - kIoController;
        if (width == 4) {
            const u32 reg = offset >> 1;
            m_io.write(reg, static_cast<u8>(value & 0xff));
            m_io.write(reg + 1, static_cast<u8>((value >> 16) & 0xff));
            return;
        }
        if ((offset & 1) == 0) {
            m_io.write(offset >> 1, static_cast<u8>(value & 0xff));
        }
        return;
    }

    if (address >= 0x01c00040 && address < 0x01c00044) return;

    if (address >= kRenderMode && address < kRenderMode + 0x200000) {
        if ((bit(value, 0) != 0) != m_render_test) {
            m_render_test = bit(value, 0) != 0;
            SM2_DEBUG("model2: render test mode %s", m_render_test ? "on" : "off");
        }
        m_render_mode = bit(value, 2) != 0;
        m_render_unk  = bit(value, 14) != 0;
        return;
    }

    if (address >= kLumaRam && address < kLumaRam + 0x20000) {
        // Model 2B: umask16(0x00ff) means one byte per 16-bit half-word.
        const u32 index = (address - kLumaRam) / 2;
        if (index < m_luma_ram.size()) {
            m_luma_ram[index] = static_cast<u8>(value & 0xff);
            ++m_table_generation;
        }
        return;
    }

    if (m_game.protection == rom::Protection::Sega315_5881) {
        if (address >= kCryptAddrLo && address < kCryptAddrLo + 2) {
            m_crypt.addrlo_w(static_cast<u16>(value));
            if (width == 4) {
                m_crypt.addrhi_w(static_cast<u16>(value >> 16));
            }
            return;
        }
        if (address >= kCryptAddrHi && address < kCryptAddrHi + 2) {
            m_crypt.addrhi_w(static_cast<u16>(value));
            return;
        }
        if (address >= kCryptSubkey && address < kCryptSubkey + 2) {
            m_crypt.subkey_le_w(static_cast<u16>(value));
            return;
        }
    }

    note_unmapped_write(address, value, width);
}

// ---------------------------------------------------------------------------
// Bus interface
// ---------------------------------------------------------------------------

u8 Model2B::read8(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr) return *w.base;
    return static_cast<u8>(register_read(address, 1) & 0xff);
}

u16 Model2B::read16(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 2) return load16(w.base);
    return static_cast<u16>(register_read(address, 2) & 0xffff);
}

u32 Model2B::read32(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 4) return load32(w.base);
    return register_read(address, 4);
}

// The i960 reaches an unaligned multi-word access one byte at a time and takes
// the region's burst capability from the first of those bytes, so a byte access
// has to report the same flags a dword access would. Without this the base class
// default of "no burst" applies, the address stops advancing part-way through an
// unaligned ldl/ldt/ldq or stl/stt/stq, and the rest of the transfer collapses
// onto one location -- which shows up as a table read from ROM arriving with
// every entry equal to the first. MAME has the same structure and gets the flags
// right because its read_byte_flags goes through the same dispatch table as
// read_dword_flags.
std::pair<u8, u16> Model2B::read8_flags(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr) {
        if ((w.flags & cpu::kBusFlagBurst) == 0) {
            ++m_no_burst_reads[address >> 20];
        }
        return {*w.base, w.flags};
    }
    const u16 flags = register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_reads[address >> 20];
    }
    return {static_cast<u8>(register_read(address, 1) & 0xff), flags};
}

u16 Model2B::write8_flags(u32 address, u8 value)
{
    const Window w = resolve(address);
    const u16    flags = w.base != nullptr ? w.flags : register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_writes[address >> 20];
    }
    write8(address, value);
    return flags;
}

std::pair<u32, u16> Model2B::read32_flags(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 4) {
        if ((w.flags & cpu::kBusFlagBurst) == 0) ++m_no_burst_reads[address >> 20];
        return {load32(w.base), w.flags};
    }
    const u16 flags = register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) ++m_no_burst_reads[address >> 20];
    return {register_read(address, 4), flags};
}

void Model2B::write8(u32 address, u8 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable) {
        *w.base = value;
        note_video_write(w, 1);
        return;
    }
    if (w.base != nullptr && w.rom) return;
    register_write(address, value, 1);
}

void Model2B::write16(u32 address, u16 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 2) {
        store16(w.base, value);
        note_video_write(w, 2);
        return;
    }
    if (w.base != nullptr && w.rom) return;
    register_write(address, value, 2);
}

void Model2B::write32(u32 address, u32 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 4) {
        store32(w.base, value);
        note_video_write(w, 4);
        return;
    }
    if (w.base != nullptr && w.rom) return;
    register_write(address, value, 4);
}

u16 Model2B::write32_flags(u32 address, u32 value)
{
    const Window w = resolve(address);
    const u16 flags = w.base != nullptr ? w.flags : register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) ++m_no_burst_writes[address >> 20];

    if (w.base != nullptr && w.writable && w.size >= 4) {
        store32(w.base, value);
        note_video_write(w, 4);
        return flags;
    }
    if (w.base != nullptr && w.rom) return flags;
    register_write(address, value, 4);
    return flags;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

void Model2B::note_video_write(const Window& w, u32 width)
{
    switch (w.notify) {
        case Notify::None: return;
        case Notify::Palette:
            m_palette_dirty = true;
            ++m_table_generation;
            return;
        case Notify::TileRam:
            m_video.tiles().note_tile_write(w.offset, width);
            return;
        case Notify::CharRam:
            m_video.tiles().note_char_write(w.offset, width);
            return;
        case Notify::TextureRam:
            ++m_texture_generation;
            return;
    }
}

void Model2B::compose_video()
{
    if (m_palette_dirty) {
        m_video.refresh_pens();
        m_palette_dirty = false;
    }
    m_video.compose();

    // Render test mode replaces the 3D output with a framebuffer bank, chosen by
    // frame parity as MAME's draw_framebuffer chooses it. Drawn here rather than
    // in the renderer because it is opaque 2D output that belongs under the
    // category-one tilemap layers, which is exactly where the tilemap surface is.
    if (m_render_test) {
        m_video.draw_framebuffer(framebuffer((m_frames & 1) != 0 ? 1 : 0));
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void Model2B::log_burst_summary() const
{
    bool any = false;
    for (u32 region = 0; region < kBurstRegions; ++region) {
        const u32 reads  = m_no_burst_reads[region];
        const u32 writes = m_no_burst_writes[region];
        if (reads == 0 && writes == 0) continue;
        if (!any) {
            SM2_INFO("multi-word accesses without burst capability, by 1 MB region:");
            any = true;
        }
        SM2_INFO("  %08x  %10u read(s)  %10u write(s)", region << 20, reads, writes);
    }
}

void Model2B::note_unmapped_read(u32 address, u32 width)
{
    ++m_unmapped_reads[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2b: unmapped read%u at %08x (ip %08x)", width * 8, address,
                  m_cpu.pip());
    }
}

void Model2B::note_unmapped_write(u32 address, u32 value, u32 width)
{
    ++m_unmapped_writes[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2b: unmapped write%u at %08x = %08x (ip %08x)", width * 8,
                  address, value, m_cpu.pip());
    }
}

void Model2B::log_unmapped_summary() const
{
    if (m_unmapped_reads.empty() && m_unmapped_writes.empty()) return;
    SM2_INFO("unmapped accesses, by 1 MB region:");
    for (const auto& [region, count] : m_unmapped_reads) {
        SM2_INFO("  %08x  %llu read(s)", region, static_cast<unsigned long long>(count));
    }
    for (const auto& [region, count] : m_unmapped_writes) {
        SM2_INFO("  %08x  %llu write(s)", region, static_cast<unsigned long long>(count));
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void Model2B::set_nvram_directory(const std::string& directory)
{
    m_nvram_directory = directory;
}

void Model2B::seed_eeprom_from_rom()
{
    // ROM_REGION16_LE, so the words are already in the order the chip
    // stores them and a straight copy is right on a little-endian host.
    (void)apply_default_image(m_roms.region("eeprom"), m_eeprom.bytes());
}

void Model2B::load_nvram()
{
    if (m_nvram_directory.empty() || m_game.name.empty()) return;
    // A set can ship power-on images for both, and MAME applies those before it
    // reads any saved file. Do the same, so a saved image still wins.
    (void)apply_default_image(m_roms.region("backup1"), m_nvram);
    seed_eeprom_from_rom();

    const std::filesystem::path base = std::filesystem::path(m_nvram_directory);

    const std::filesystem::path nvram_path = base / (m_game.name + ".nv");
    std::FILE* handle = std::fopen(nvram_path.string().c_str(), "rb");
    if (handle != nullptr) {
        const usize read = std::fread(m_nvram.data(), 1, m_nvram.size(), handle);
        std::fclose(handle);
        if (read == m_nvram.size()) {
            SM2_INFO("loaded nvram from %s", nvram_path.string().c_str());
        } else {
            SM2_WARN("nvram '%s' is the wrong size; starting blank",
                     nvram_path.string().c_str());
            std::fill(m_nvram.begin(), m_nvram.end(), u8{0xff});
        }
    }

    (void)m_eeprom.load((base / (m_game.name + ".eeprom")).string());
}

void Model2B::save_nvram() const
{
    if (m_nvram_directory.empty() || m_game.name.empty()) return;

    std::error_code error;
    std::filesystem::create_directories(m_nvram_directory, error);
    if (error) {
        SM2_WARN("could not create '%s': %s", m_nvram_directory.c_str(),
                 error.message().c_str());
        return;
    }

    const std::filesystem::path base = std::filesystem::path(m_nvram_directory);
    const std::filesystem::path nvram_path = base / (m_game.name + ".nv");
    std::FILE* handle = std::fopen(nvram_path.string().c_str(), "wb");
    if (handle != nullptr) {
        std::fwrite(m_nvram.data(), 1, m_nvram.size(), handle);
        std::fclose(handle);
    } else {
        SM2_WARN("could not write nvram to '%s'", nvram_path.string().c_str());
    }

    (void)m_eeprom.save((base / (m_game.name + ".eeprom")).string());
}

}  // namespace sm2::hw
