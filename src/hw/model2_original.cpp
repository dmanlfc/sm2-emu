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
// The original Sega Model 2.
//
// The address decode, interrupt latch and timer behaviour follow MAME's
// src/mame/sega/model2.cpp (BSD-3-Clause), whose model2o_mem this mirrors region
// for region so the two can be compared directly. model2o_mem is model2_tgp_mem
// -- the map Model 2A also uses -- plus four overrides, and those four are the
// only places this file departs from hw/model2.cpp's decode.

#include "hw/model2_original.h"

#include "core/log.h"

#include <algorithm>
#include <string>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Little-endian access helpers
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
    static_assert(std::endian::native == std::endian::little,
                  "the ROM word views assume a little-endian host");
    if (bytes.empty()) {
        return {};
    }
    return {reinterpret_cast<const u16*>(bytes.data()), bytes.size() / sizeof(u16)};
}

[[nodiscard]] std::span<const u32> as_words(std::span<const u8> bytes)
{
    static_assert(std::endian::native == std::endian::little,
                  "the ROM word views assume a little-endian host");
    if (bytes.empty()) {
        return {};
    }
    return {reinterpret_cast<const u32*>(bytes.data()), bytes.size() / sizeof(u32)};
}

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------

constexpr u32 kRomMainCpu      = 0x00000000;  // 2 MB
constexpr u32 kScratchRam      = 0x00200000;  // 128 KB, half of Model 2A's
constexpr u32 kRomMirror       = 0x00220000;  // 128 KB view of maincpu at 0x20000
constexpr u32 kWorkRam         = 0x00500000;  // 1 MB
constexpr u32 kGeoPort         = 0x00800000;  // 16 KB, Geometrizer function ports
constexpr u32 kGeoProgram      = 0x00804000;  // 16 KB, Geometrizer upload
constexpr u32 kCoproFunction   = 0x00880000;  // 16 KB, coprocessor function port
constexpr u32 kCoproFifo       = 0x00884000;  // 16 KB, coprocessor FIFO
constexpr u32 kBufferRam       = 0x00900000;  // 128 KB, mirror 0x60000
constexpr u32 kVideoRegs       = 0x00980000;
constexpr u32 kCpuControl      = 0x00e00000;  // 0x38 bytes of wait-state registers
constexpr u32 kIrqRegs         = 0x00e80000;
constexpr u32 kTimerRegs       = 0x00f00000;
constexpr u32 kTileRam         = 0x01000000;  // 64 KB, mirror 0x110000
constexpr u32 kCharRam         = 0x01080000;  // 512 KB, mirror 0x100000
constexpr u32 kPaletteRam      = 0x01800000;  // 16 KB
constexpr u32 kColorXlat       = 0x01810000;  // 48 KB
constexpr u32 kZClip           = 0x0181c000;
constexpr u32 kCommRam         = 0x01a00000;  // 16 KB, mirror 0x10000
constexpr u32 kCommCtl         = 0x01a04000;  // CN and FG, mirror 0x10000
constexpr u32 kDualPortRam     = 0x01c00000;  // 2K x 8 on byte lanes 0 and 2
constexpr u32 kDualPortWindow  = 0x1000;      // i960 bytes the 2K occupies
constexpr u32 kUart            = 0x01c80000;
constexpr u32 kNvram           = 0x01d00000;  // 16 KB
constexpr u32 kRomMainData     = 0x02000000;  // 32 MB window
constexpr u32 kRomMainDataHigh = 0x06000000;  // 16 MB window at region offset 0x1000000
constexpr u32 kRenderMode      = 0x10000000;
constexpr u32 kPolygonCount    = 0x10400000;
constexpr u32 kFramebufferA    = 0x11600000;  // 512 KB
constexpr u32 kFramebufferB    = 0x11680000;  // 512 KB
constexpr u32 kTextureRam0     = 0x12000000;  // 2 MB window, mirror 0x200000
constexpr u32 kTextureRam1     = 0x12400000;  // 2 MB window, mirror 0x200000
constexpr u32 kLumaRam         = 0x12800000;  // byte-wide on lane 0

/// True when `address`, with the mirrored bits masked out, falls in
/// [base, base + size).
[[nodiscard]] inline bool in_mirrored(u32 address, u32 base, u32 size, u32 mirror)
{
    const u32 folded = address & ~mirror;
    return folded >= base && folded < base + size;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Model2Original::Model2Original() : m_cpu(*this) {}

Model2Original::~Model2Original() = default;

bool Model2Original::init(const rom::GameSpec& game, rom::RomSet roms)
{
    m_game = game;
    m_roms = std::move(roms);

    m_rom_maincpu   = m_roms.region("maincpu");
    m_rom_main_data = m_roms.region("main_data");

    if (m_rom_maincpu.empty()) {
        SM2_ERROR("model2o: the 'maincpu' region is missing");
        return false;
    }

    m_work_ram.assign(0x100000, 0);
    m_scratch_ram.assign(0x20000, 0);
    m_buffer_ram.assign(0x20000 / 4, 0);
    m_tile_ram.assign(0x10000, 0);
    m_char_ram.assign(0x80000, 0);
    m_palette_ram.assign(0x4000 / 2, 0);
    m_colorxlat.assign(0xc000 / 2, 0);
    m_luma_ram.assign(0x8000, 0);
    m_texture_ram0.assign(0x200000 / 4, 0);
    m_texture_ram1.assign(0x200000 / 4, 0);
    m_framebuffer_a.assign(0x80000 / 2, 0);
    m_framebuffer_b.assign(0x80000 / 2, 0);
    m_nvram.assign(0x4000, 0xff);
    m_cpu_control.assign(0x40, 0);
    m_comm_ram.assign(0x4000, 0);

    // The video stage keeps decoded copies of this memory, so it has to be
    // attached after the allocation above and before anything can write to it.
    m_video.attach(m_tile_ram, m_char_ram, m_palette_ram, m_colorxlat);

    // The link board's 16 KB is the same storage the i960 reaches at
    // 0x01a00000; the board keeps it so the access stays a burst window.
    m_comm.attach_shared(m_comm_ram);

    // The coprocessor. Identical to Model 2A's, because MAME's model2o_state
    // derives from model2_tgp_state and inherits its copro maps unchanged.
    m_rom_copro_tables = m_roms.region("copro_tgp_tables");
    m_rom_copro_data   = m_roms.region("copro_data");
    m_copro.attach(as_words(m_rom_copro_tables), as_words(m_rom_copro_data),
                   m_buffer_ram);

    m_rom_polygons = m_roms.region("polygons");
    m_rom_textures = m_roms.region("textures");
    m_geometry.attach(as_words(m_rom_polygons), as_halfwords(m_rom_textures),
                      m_buffer_ram);

    // The I/O board. Its firmware is a device ROM set rather than part of the
    // game's own chips, so it arrives in its own region; the loader has already
    // fetched it from model1io.zip beside the game archive.
    if (!m_roms.has("ioboard")) {
        SM2_WARN("model2o: no 'ioboard' region; the I/O board has no firmware and "
                 "the program will see no inputs");
    }
    // Virtua Cop's cabinet fits the advanced board -- MAME replaces the device
    // outright in model2o_state::vcop -- and its firmware is not interchangeable
    // with the plain one: a different CPU, a different memory map, and a window
    // onto the FPGA that digitises the guns.
    m_uses_advanced_io = std::find(m_game.device_sets.begin(), m_game.device_sets.end(),
                                  std::string{"model1io2"}) != m_game.device_sets.end();

    if (m_uses_advanced_io) {
        m_ioboard2.attach(m_roms.region("ioboard"));
        wire_advanced_io_board();
    } else {
        m_ioboard.attach(m_roms.region("ioboard"));
    }

    // The 2K dual-port RAM between the two. The i960 reaches its right-hand port
    // through the memory map; the I/O board's expander reaches the left-hand port
    // over the serial side of its 315-5338A. That is the entire connection between
    // the two computers.
    if (!m_uses_advanced_io) {
    m_ioboard.set_dual_port([this](u32 address) { return m_dpram.left_read(address); },
                            [this](u32 address, u8 value) {
                                m_dpram.left_write(address, value);
                            });

    // MAME's model2o binds in_callback<0> and <1> only: IN2 is unused by every
    // title on this board that is in scope, and an unbound port reads as an open
    // input, which is the same thing an idle panel gives.
    m_ioboard.set_input(0, [this] { return m_inputs.in0; });
    m_ioboard.set_input(1, [this] { return m_inputs.in1; });

    // Desert Tank's three analogue controls, as model2o_state::desert binds them:
    // an_callback<0> STEER, <1> ACCEL, <2> BRAKE. The remaining five channels stay
    // unbound and read as open inputs.
    for (u32 channel = 0; channel < 3; ++channel) {
        m_ioboard.set_analog(channel, [this, channel] { return m_inputs.analog[channel]; });
    }

    m_ioboard.set_output([this](u8 value) { lamp_output_w(value); });

    // Only Daytona has a force-feedback board behind the expander's port E.
    if (m_game.drive_board) {
        m_ioboard.set_drive([this] { return m_drive_board_latch; },
                            [this](u8 value) { drive_board_write(value); });
    }
    }  // !m_uses_advanced_io

    // Host side of the coprocessor FIFO flow control, exactly as Model 2A wires
    // it: only the machine can halt the host CPU, which is why this is not in
    // CoproTgp.
    m_copro.fifo_out().set_on_empty_retry([this] { m_cpu.stall(); });
    m_copro.fifo_out().set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_copro.fifo_out().set_on_unempty([this] { m_cpu.set_halted(false); });
    m_copro.fifo_in().set_on_full([this] { m_cpu.set_halted(true); });
    m_copro.fifo_in().set_on_unfull([this] { m_cpu.set_halted(false); });

    // The sound board and the two wires that reach it. Nothing on the CPU board
    // addresses it directly: the host's UART transmitter feeds the sound board's
    // receiver and the sound board's transmitter comes back into this receiver,
    // which is the whole connection.
    m_m1audio.attach(m_roms.region("m1audio:sndcpu"), m_roms.region("m1audio:pcm1"),
                     m_roms.region("m1audio:pcm2"));
    m_uart.set_tx_handler([this](u8 value) { m_m1audio.write_txd(value); });
    m_m1audio.set_rxd_handler([this](u8 value) { m_uart.write_rxd(value); });
    m_uart.set_ready_handler([this] { sound_ready_w(); });

    SM2_INFO("model2o: %s board, %s", rom::board_name(game.board), game.title.c_str());
    if (!m_m1audio.present()) {
        SM2_WARN("model2o: this set declares no M1 audio ROMs; there will be no sound");
    }
    reset();
    return true;
}

void Model2Original::reset()
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
    m_palette_dirty = true;

    m_cycles      = 0;
    m_frame_start = 0;
    m_frames      = 0;

    m_unmapped_reads.clear();
    m_unmapped_writes.clear();

    m_video.reset();
    m_copro.reset();
    m_geometry.reset();
    m_render_list.clear();

    m_copro_debt    = 0;
    m_in_copro_sync = false;

    // The display-list buffer comes up holding a specific pattern. MAME notes
    // that the hardware probably derives this itself; without it, a game that
    // parses the buffer before writing it reads nonsense.
    std::fill(m_buffer_ram.begin(), m_buffer_ram.end(), 0x07800f0fu);

    // Bring the analogue controls up where the hardware has them with nobody at
    // the cabinet, from the same per-title data MAME takes from each PORT_BIT's
    // default. A channel the database does not describe stays at zero.
    m_inputs.analog.fill(0);
    for (usize channel = 0; channel < m_inputs.analog.size(); ++channel) {
        if (m_game.analog[channel].control != rom::AnalogControl::None) {
            m_inputs.analog[channel] = m_game.analog[channel].rest;
        }
    }
    m_inputs.gears  = 0;

    m_dpram.reset();
    m_ioboard.reset();
    m_ioboard2.reset();

    m_uart.configure(kCpuClock, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();
    m_comm.reset();

    // After the UART, because reset() re-installs the sound board's own handlers.
    m_m1audio.reset();

    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

void Model2Original::run_frame()
{
    reset_core_profile();  // per-core --profile split; see Model2::run_frame

    for (u32 line = 0; line < kVerticalTotal; ++line) {
        const u64 line_end   = m_frame_start + static_cast<u64>(line + 1) * kCyclesPerLine;
        const u64 line_start = m_cycles;

        while (m_cycles < line_end) {
            u64 target = std::min(line_end, next_timer_deadline());
            if (target <= m_cycles) {
                target = m_cycles + 1;
            }
            target = std::min(target, m_cycles + kCoproInterleave);

            const s32 slice = static_cast<s32>(std::min<u64>(target - m_cycles, 1u << 20));
            s32 used;
            {
                CoreScope scope(m_core_profile, m_core_profile.i960_ns);
                used = m_cpu.run(slice);
            }
            m_cycles += static_cast<u64>(used);

            const u32 spent = static_cast<u32>(used > 0 ? used : 0);
            {
                CoreScope scope(m_core_profile, m_core_profile.copro_ns);
                step_copro(spent);
            }

            // The I/O board is interleaved at the same granularity as the
            // coprocessor. It has to be fine: the only thing between the two
            // computers is 2K of RAM neither side locks, and a whole scanline of
            // i960 time is over a hundred Z80 instructions, which is long enough
            // for the board to overwrite a structure the program is halfway
            // through reading.
            if (m_uses_advanced_io) {
                m_ioboard2.run(spent);
            } else {
                m_ioboard.run(spent);
            }

            // TxRDY going high is what raises the sound interrupt, so the UART is
            // ticked with the CPU rather than once a scanline.
            m_uart.run(spent);

            service_timers();

            if (m_pending_intena_valid && m_cycles >= m_pending_intena_cycle) {
                m_intena = m_pending_intena;
                m_pending_intena_valid = false;
                sound_ready_w();
                irq_update();
            }
        }

        // The sound board shares nothing with the host but the serial line, so a
        // scanline is fine enough: the only thing on it with a deadline is the
        // output sample clock, and that is counted in host cycles either way.
        {
            CoreScope scope(m_core_profile, m_core_profile.sound_ns);
            m_m1audio.run(static_cast<u32>(m_cycles - line_start));
        }

        // TxRDY and RxRDY are levels and the interrupt latch samples them. Doing
        // it once a scanline as well as on every UART tick is what lets the link
        // start at all: the program enables the transmitter first and unmasks the
        // interrupt afterwards, so an edge-triggered latch would never see the
        // transmitter become ready.
        sound_ready_w();

        if (line == kVisibleHeight) {
            on_vblank_start();
        }
    }

    m_frame_start += kCyclesPerFrame;
    ++m_frames;
}

void Model2Original::step_copro(u32 host_cycles)
{
    // Two coprocessor instructions per three host cycles, as on Model 2A: the
    // same MB86234 on the same 50 MHz crystal, taking three clocks per
    // instruction against the i960's 25 MHz.
    m_copro_debt += host_cycles * 2;
    const s32 cycles = static_cast<s32>(m_copro_debt / 3);
    m_copro_debt %= 3;

    if (cycles > 0) {
        static_cast<void>(m_copro.run(cycles));
    }
}

void Model2Original::sync_copro()
{
    if (m_in_copro_sync) {
        return;
    }
    m_in_copro_sync = true;

    s32 remaining = kCoproSyncLimit;
    while (remaining > 0 && m_copro.output_empty() && !m_copro.cpu().halted()) {
        const s32 slice = std::min<s32>(remaining, 32);
        const s32 used  = m_copro.run(slice);
        if (used <= 0) {
            break;
        }
        remaining -= used;
        // Borrow from the coprocessor's future share so this time is not spent
        // twice. The debt is in thirds of a host cycle, times two.
        const u32 owed = static_cast<u32>(used) * 3;
        m_copro_debt   = m_copro_debt > owed ? m_copro_debt - owed : 0;
    }

    m_in_copro_sync = false;
}

u64 Model2Original::next_timer_deadline() const
{
    u64 deadline = ~0ULL;
    for (const Timer& timer : m_timers) {
        if (timer.running) {
            deadline = std::min(deadline, timer.start_cycle + timer.original);
        }
    }
    return deadline;
}

void Model2Original::service_timers()
{
    for (u32 index = 0; index < m_timers.size(); ++index) {
        Timer& timer = m_timers[index];
        if (!timer.running) {
            continue;
        }
        if (m_cycles < timer.start_cycle + timer.original) {
            continue;
        }

        timer.running = false;
        timer.value   = 0xfffff;
        raise_interrupt(1u << (index + 2));
    }
}

void Model2Original::on_vblank_start()
{
    // Bit 0 of the video control register selects 30 Hz geometry, which every
    // original Model 2 title uses -- MAME's render_mode_w notes desert among
    // them -- so this runs on even frames only and the renderer shows the
    // previous result again on the others.
    if ((m_videocontrol & 1) == 0 || (m_frames & 1) == 0) {
        m_geometry.set_read_start_address(m_geo_read_start_address);
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

void Model2Original::sound_ready_w()
{
    // MAME's model2_state::sound_ready_w, which model2o_state binds to both of
    // the UART's ready lines.
    const u32 line = 1u << 10;
    if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

void Model2Original::raise_interrupt(u32 line)
{
    if ((m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

void Model2Original::irq_update()
{
    m_cpu.set_irq_line(cpu::i960::I960_IRQ0,
                       (m_intreq & 0b0000'0000'0001) != 0 ? cpu::i960::kAssertLine
                                                          : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ1,
                       (m_intreq & 0b0000'0000'0010) != 0 ? cpu::i960::kAssertLine
                                                          : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ2,
                       (m_intreq & 0b0011'1111'1100) != 0 ? cpu::i960::kAssertLine
                                                          : cpu::i960::kClearLine);
    m_cpu.set_irq_line(cpu::i960::I960_IRQ3,
                       (m_intreq & 0b1100'0000'0000) != 0 ? cpu::i960::kAssertLine
                                                          : cpu::i960::kClearLine);
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

u32 Model2Original::timers_r(u32 index)
{
    Timer& timer = m_timers[index];
    if (timer.running) {
        const u64 elapsed = m_cycles - timer.start_cycle;
        timer.value = elapsed >= timer.original
                          ? 0
                          : static_cast<u32>(timer.original - elapsed);
    }
    return timer.value;
}

void Model2Original::timers_w(u32 index, u32 value)
{
    Timer& timer      = m_timers[index];
    timer.value       = value;
    timer.original    = value;
    timer.start_cycle = m_cycles;
    timer.running     = true;
}

// ---------------------------------------------------------------------------
// I/O board callbacks
// ---------------------------------------------------------------------------

void Model2Original::lamp_output_w(u8 value)
{
    // Two coin counters and six outputs. On Desert Tank those are the cannon and
    // machine-gun recoil motors and four lamps, per MAME's comment block. Recorded
    // for a future output layer; nothing consumes it yet.
    SM2_TRACE("model2o: outputs = %02x", value);
}

void Model2Original::drive_board_write(u8 value)
{
    m_drive_board_latch = value;
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

Model2Original::Window Model2Original::resolve(u32 address)
{
    const auto window = [](auto& storage, u32 offset, bool writable, u16 flags,
                           Notify notify = Notify::None) {
        Window result;
        auto* bytes = reinterpret_cast<u8*>(storage.data());
        const usize total = storage.size() * sizeof(typename std::remove_reference_t<
                                                    decltype(storage)>::value_type);
        if (offset >= total) {
            return Window{};
        }
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
        if (offset >= storage.size()) {
            return Window{};
        }
        result.base     = const_cast<u8*>(storage.data()) + offset;
        result.size     = storage.size() - offset;
        result.writable = false;
        result.rom      = true;
        result.flags    = cpu::kBusFlagBurst;
        return result;
    };

    // Ordered roughly by access frequency: work RAM and program ROM dominate.
    if (address >= kRomMainCpu && address < kRomMainCpu + 0x200000) {
        return rom_window(m_rom_maincpu, address - kRomMainCpu);
    }
    if (address >= kWorkRam && address < kWorkRam + 0x100000) {
        return window(m_work_ram, address - kWorkRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kScratchRam && address < kScratchRam + 0x20000) {
        return window(m_scratch_ram, address - kScratchRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kRomMirror && address < kRomMirror + 0x20000) {
        // MAME's model2o_mem maps this as a second view of the program ROM at
        // offset 0x20000, immediately above the 128 KB of scratch RAM. On Model 2A
        // the whole 256 KB is RAM instead.
        return rom_window(m_rom_maincpu, (address - kRomMirror) + 0x20000);
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
    if (address >= kCpuControl && address < kCpuControl + 0x38) {
        return window(m_cpu_control, address - kCpuControl, true, cpu::kBusFlagNone);
    }
    if (address >= kFramebufferA && address < kFramebufferA + 0x80000) {
        return window(m_framebuffer_a, address - kFramebufferA, true, cpu::kBusFlagBurst);
    }
    if (address >= kFramebufferB && address < kFramebufferB + 0x80000) {
        return window(m_framebuffer_b, address - kFramebufferB, true, cpu::kBusFlagBurst);
    }
    if (in_mirrored(address, kTextureRam0, 0x200000, 0x200000)) {
        // Reads come straight from the backing store; writes go through the
        // packing transform in register_write, so this window is read-only.
        return window(m_texture_ram0, address & 0x1fffff, false, cpu::kBusFlagBurst);
    }
    if (in_mirrored(address, kTextureRam1, 0x200000, 0x200000)) {
        return window(m_texture_ram1, address & 0x1fffff, false, cpu::kBusFlagBurst);
    }

    return {};
}

u16 Model2Original::register_flags(u32 address)
{
    // Bulk upload paths, which a program fills with multi-word stores and which
    // therefore have to advance the address. MAME flags exactly these.
    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        return cpu::kBusFlagBurst;  // Geometrizer microcode upload
    }
    if (address >= kCoproFunction && address < kCoproFunction + 0x4000) {
        return cpu::kBusFlagBurst;  // coprocessor function port
    }
    if (in_mirrored(address, 0x01020000, 4, 0x100000)) {
        return cpu::kBusFlagBurst;  // ABSEL, discarded but still burst-capable
    }
    if (address >= kLumaRam && address < kLumaRam + 0x20000) {
        return cpu::kBusFlagBurst;  // polygon luminance RAM
    }
    return cpu::kBusFlagNone;
}

// ---------------------------------------------------------------------------
// Register reads
// ---------------------------------------------------------------------------

u32 Model2Original::register_read(u32 address, u32 width)
{
    // Geometrizer function ports. Only the two address registers read back.
    if (address >= kGeoPort && address < kGeoPort + 0x4000) {
        const u32 offset = address - kGeoPort;
        if (offset == 0x2008) {
            return m_geo_write_start_address;
        }
        if (offset == 0x3008) {
            return m_geo_read_start_address;
        }
        SM2_TRACE("model2o: read from geometry port %04x", offset);
        return 0;
    }

    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        return 0xffffffff;  // not readable on real hardware
    }

    if (address >= kCoproFifo && address < kCoproFifo + 0x4000) {
        if (m_copro.output_empty()) {
            sync_copro();
        }
        return m_copro.host_fifo_read();
    }

    if (address >= kVideoRegs && address < kVideoRegs + 0x40) {
        const u32 offset = address - kVideoRegs;
        if (offset < 0x04) {
            return m_copro.control_read();
        }
        if (offset < 0x08) {
            // Coprocessor output FIFO status: non-zero means empty.
            if (m_copro.output_empty()) {
                sync_copro();
            }
            return m_copro.output_empty() ? 1 : 0;
        }
        if (offset >= 0x0c && offset < 0x10) {
            const u8 parity = m_render_mode ? static_cast<u8>((m_frames & 1) << 2)
                                            : static_cast<u8>((m_frames & 2) << 1);
            return parity | (m_videocontrol & 3);
        }
        if (offset >= 0x30 && offset < 0x40) {
            // Board identification string.
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


    // The dual-port RAM's right-hand port: eight bits wide on byte lanes 0 and 2
    // of each 32-bit word, which is what MAME's umask32(0x00ff00ff) says, so RAM
    // byte N lives at i960 byte offset N*2 and the 2K occupies 4K of address
    // space.
    if (address >= kDualPortRam && address < kDualPortRam + kDualPortWindow) {
        const u32 offset = address - kDualPortRam;
        if (width == 4) {
            const u32 low  = m_dpram.right_read(offset >> 1);
            const u32 high = m_dpram.right_read((offset >> 1) + 1);
            return low | (high << 16);
        }
        if ((offset & 1) != 0) {
            return 0xffffffff;  // an inactive byte lane
        }
        return m_dpram.right_read(offset >> 1);
    }

    if (address >= kUart && address < kUart + 0x04) {
        // Eight bits wide on byte lanes 0 and 2, as MAME's umask16(0x00ff) puts
        // it: register 0 is the data register and 1 the status register.
        if (width == 4) {
            const u32 data   = m_uart.read(0);
            const u32 status = m_uart.read(1);
            return data | (status << 16);
        }
        return m_uart.read((address - kUart) >> 1);
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
        const u32 index = (address - kLumaRam) / 4;
        return index < m_luma_ram.size() ? m_luma_ram[index] : 0;
    }

    note_unmapped_read(address, width);
    return 0;
}

// ---------------------------------------------------------------------------
// Register writes
// ---------------------------------------------------------------------------

void Model2Original::register_write(u32 address, u32 value, u32 width)
{
    // Texture RAM. Two 16-bit halves are packed into each 32-bit entry, so a
    // write's destination is half its window offset. MAME's tex0_w/tex1_w, which
    // model2_tgp_mem installs and this board inherits unchanged.
    if (in_mirrored(address, kTextureRam0, 0x200000, 0x200000)
        || in_mirrored(address, kTextureRam1, 0x200000, 0x200000)) {
        std::vector<u32>& sheet = in_mirrored(address, kTextureRam0, 0x200000, 0x200000)
                                      ? m_texture_ram0
                                      : m_texture_ram1;
        const u32 offset = (address & 0x1fffff) / 4;   // 32-bit window offset
        const u32 index  = offset >> 1;
        if (index < sheet.size()) {
            if ((offset & 1) == 0) {
                sheet[index] = (sheet[index] & 0xffff0000) | (value & 0xffff);
            } else {
                sheet[index] = (sheet[index] & 0x0000ffff) | ((value & 0xffff) << 16);
            }
            ++m_texture_generation;
        }
        return;
    }

    if (address >= kGeoPort && address < kGeoPort + 0x4000) {
        const u32 offset = address - kGeoPort;

        if (offset < 0x1000) {
            // The function number lives in the address, not the data, and is
            // folded into the word pushed onto the display list.
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
            } else {
                SM2_TRACE("model2o: display-list write past the buffer at %05x",
                          m_geo_write_start_address);
            }
            m_geo_write_start_address += 4;
            return;
        }
        if (offset == 0x1008) {
            m_geo_write_start_address = value & 0xfffff;
            return;
        }
        if (offset == 0x3008) {
            m_geo_read_start_address = value & 0xfffff;
            return;
        }
        SM2_TRACE("model2o: write to geometry port %04x = %08x", offset, value);
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
                    SM2_DEBUG("model2o: geometrizer upload started");
                } else {
                    SM2_DEBUG("model2o: geometrizer boot, %u words uploaded", m_geocnt);
                }
            }
            m_geoctl = value;
            return;
        }
        if (offset >= 0x0c && offset < 0x10) {
            m_videocontrol = value;
            return;
        }
        return;
    }

    if (address >= kIrqRegs && address < kIrqRegs + 0x08) {
        if ((address - kIrqRegs) < 4) {
            m_intreq &= value;
            irq_update();
        } else {
            // Delayed by 80 ns (2 CPU cycles at 25 MHz), as MAME does, because the
            // real hardware latches the mask through a register that settles a
            // clock or two after the write.
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

    // System 24 synchronisation registers.
    if (in_mirrored(address, 0x01020000, 4, 0x100000)) {
        return;  // ABSEL, always zero
    }
    if (in_mirrored(address, 0x01040000, 2, 0x100000)) {
        m_video.set_horizontal_sync(static_cast<u16>(value));
        return;
    }
    if (in_mirrored(address, 0x01060000, 2, 0x100000)) {
        m_video.set_vertical_sync(static_cast<u16>(value));
        return;
    }
    if (in_mirrored(address, 0x01070000, 4, 0x100000)) {
        return;  // video sync switch
    }

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

    if (address >= kDualPortRam && address < kDualPortRam + kDualPortWindow) {
        const u32 offset = address - kDualPortRam;
        if (width == 4) {
            m_dpram.right_write(offset >> 1, static_cast<u8>(value & 0xff));
            m_dpram.right_write((offset >> 1) + 1, static_cast<u8>((value >> 16) & 0xff));
            return;
        }
        if ((offset & 1) == 0) {
            m_dpram.right_write(offset >> 1, static_cast<u8>(value & 0xff));
        }
        return;
    }

    if (address >= kUart && address < kUart + 0x04) {
        m_uart.write((address - kUart) >> 1, static_cast<u8>(value));
        return;
    }

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
        const u32 index = (address - kLumaRam) / 4;
        if (index < m_luma_ram.size()) {
            m_luma_ram[index] = static_cast<u8>(value & 0xff);
            ++m_table_generation;
        }
        return;
    }

    note_unmapped_write(address, value, width);
}

// ---------------------------------------------------------------------------
// Bus interface
// ---------------------------------------------------------------------------

u8 Model2Original::read8(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr) {
        return *w.base;
    }
    return static_cast<u8>(register_read(address, 1) & 0xff);
}

u16 Model2Original::read16(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 2) {
        return load16(w.base);
    }
    return static_cast<u16>(register_read(address, 2) & 0xffff);
}

u32 Model2Original::read32(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 4) {
        return load32(w.base);
    }
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
std::pair<u8, u16> Model2Original::read8_flags(u32 address)
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

u16 Model2Original::write8_flags(u32 address, u8 value)
{
    const Window w = resolve(address);
    const u16    flags = w.base != nullptr ? w.flags : register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_writes[address >> 20];
    }
    write8(address, value);
    return flags;
}

std::pair<u32, u16> Model2Original::read32_flags(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 4) {
        if ((w.flags & cpu::kBusFlagBurst) == 0) {
            ++m_no_burst_reads[address >> 20];
        }
        return {load32(w.base), w.flags};
    }
    const u16 flags = register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_reads[address >> 20];
    }
    return {register_read(address, 4), flags};
}

void Model2Original::write8(u32 address, u8 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable) {
        *w.base = value;
        note_video_write(w, 1);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;  // writes to ROM are discarded
    }
    register_write(address, value, 1);
}

void Model2Original::write16(u32 address, u16 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 2) {
        store16(w.base, value);
        note_video_write(w, 2);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;
    }
    register_write(address, value, 2);
}

void Model2Original::write32(u32 address, u32 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 4) {
        store32(w.base, value);
        note_video_write(w, 4);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;
    }
    register_write(address, value, 4);
}

u16 Model2Original::write32_flags(u32 address, u32 value)
{
    const Window w     = resolve(address);
    const u16    flags = w.base != nullptr ? w.flags : register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_writes[address >> 20];
    }

    if (w.base != nullptr && w.writable && w.size >= 4) {
        store32(w.base, value);
        note_video_write(w, 4);
        return flags;
    }
    if (w.base != nullptr && w.rom) {
        return flags;
    }
    register_write(address, value, 4);
    return flags;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

void Model2Original::note_video_write(const Window& w, u32 width)
{
    switch (w.notify) {
        case Notify::None:
            return;
        case Notify::Palette:
            m_palette_dirty = true;
            ++m_table_generation;
            return;
        case Notify::TileRam:
            m_video.tiles().note_tile_write(w.offset, width);
            ++m_tile_generation;
            return;
        case Notify::CharRam:
            m_video.tiles().note_char_write(w.offset, width);
            ++m_char_generation;
            return;
    }
}

void Model2Original::compose_video()
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

void Model2Original::log_burst_summary() const
{
    bool any = false;
    for (u32 region = 0; region < kBurstRegions; ++region) {
        const u32 reads  = m_no_burst_reads[region];
        const u32 writes = m_no_burst_writes[region];
        if (reads == 0 && writes == 0) {
            continue;
        }
        if (!any) {
            SM2_INFO("multi-word accesses without burst capability, by 1 MB region:");
            any = true;
        }
        SM2_INFO("  %08x  %10u read(s)  %10u write(s)", region << 20, reads, writes);
    }
}

void Model2Original::note_unmapped_read(u32 address, u32 width)
{
    ++m_unmapped_reads[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2o: unmapped read%u at %08x (ip %08x)", width * 8, address,
                  m_cpu.pip());
    }
}

void Model2Original::note_unmapped_write(u32 address, u32 value, u32 width)
{
    ++m_unmapped_writes[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2o: unmapped write%u at %08x = %08x (ip %08x)", width * 8,
                  address, value, m_cpu.pip());
    }
}

void Model2Original::log_unmapped_summary() const
{
    if (m_unmapped_reads.empty() && m_unmapped_writes.empty()) {
        SM2_INFO("model2o: every access was mapped");
        return;
    }
    for (const auto& [region, count] : m_unmapped_reads) {
        SM2_WARN("model2o: %llu unmapped read(s) in %08x", (unsigned long long)count,
                 region);
    }
    for (const auto& [region, count] : m_unmapped_writes) {
        SM2_WARN("model2o: %llu unmapped write(s) in %08x", (unsigned long long)count,
                 region);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void Model2Original::set_nvram_directory(const std::string& directory)
{
    m_nvram_directory = directory;
}

void Model2Original::seed_eeprom_from_rom()
{
    // ROM_REGION16_LE, so the words are already in the order the chip
    // stores them and a straight copy is right on a little-endian host.
    (void)apply_default_image(m_roms.region("eeprom"), io_eeprom().bytes());
}

// ---------------------------------------------------------------------------
// The advanced I/O board
// ---------------------------------------------------------------------------

void Model2Original::wire_advanced_io_board()
{
    // Same shared RAM, reached the same way: over the serial side of the board's
    // 315-5338A into the dual-port RAM's left-hand port.
    m_ioboard2.set_dual_port(
        [this](u32 address) { return m_dpram.left_read(address); },
        [this](u32 address, u8 value) { m_dpram.left_write(address, value); });

    // MAME's model2o_state::vcop binds all three input ports here, unlike the
    // plain board's two: this cabinet has a second gun and a connector on IN2.
    m_ioboard2.set_input(0, [this] { return m_inputs.in0; });
    m_ioboard2.set_input(1, [this] { return m_inputs.in1; });
    m_ioboard2.set_input(2, [this] { return m_inputs.in2; });

    m_ioboard2.set_output([this](u8 value) { lamp_output_w(value); });

    // The guns, in the order the FPGA reports them: P1 Y, P1 X, P2 Y, P2 X. The
    // ranges come from the title's own calibration rather than from the hardware,
    // and the board needs them because its off-screen test is defined relative to
    // each axis's travel.
    const rom::LightgunSpec& gun = m_game.lightgun;
    m_ioboard2.set_lightgun(0, [this] { return m_inputs.gun_p1y; });
    m_ioboard2.set_lightgun(1, [this] { return m_inputs.gun_p1x; });
    m_ioboard2.set_lightgun(2, [this] { return m_inputs.gun_p2y; });
    m_ioboard2.set_lightgun(3, [this] { return m_inputs.gun_p2x; });
    m_ioboard2.set_lightgun_range(0, gun.p1y.minimum, gun.p1y.maximum);
    m_ioboard2.set_lightgun_range(1, gun.p1x.minimum, gun.p1x.maximum);
    m_ioboard2.set_lightgun_range(2, gun.p2y.minimum, gun.p2y.maximum);
    m_ioboard2.set_lightgun_range(3, gun.p2x.minimum, gun.p2x.maximum);

    if (!gun.present) {
        SM2_WARN("model2o: %s fits the advanced I/O board but declares no lightgun "
                 "calibration; its guns will read as pointed off screen",
                 m_game.name.c_str());
    }
}

Eeprom93c46& Model2Original::io_eeprom()
{
    return m_uses_advanced_io ? m_ioboard2.eeprom() : m_ioboard.eeprom();
}

const Eeprom93c46& Model2Original::io_eeprom() const
{
    return m_uses_advanced_io ? m_ioboard2.eeprom() : m_ioboard.eeprom();
}

Model2Original::IoBoardReport Model2Original::io_board_report() const
{
    IoBoardReport report;
    if (m_uses_advanced_io) {
        const Model1io2::Counters& io = m_ioboard2.counters();
        report.kind             = "Model 1 I/O (advanced)";
        report.present          = m_ioboard2.present();
        report.pc               = m_ioboard2.cpu().pc();
        report.sp               = m_ioboard2.cpu().sp();
        report.halted           = m_ioboard2.cpu().halted();
        report.instructions     = m_ioboard2.cpu().instructions();
        report.cycles           = m_ioboard2.cpu().cycles();
        report.dual_port_reads  = io.dual_port_reads;
        report.dual_port_writes = io.dual_port_writes;
        report.analog_samples   = io.analog_samples;
        report.output_writes    = io.output_writes;
        report.unmapped_reads   = io.unmapped_reads;
        report.unmapped_writes  = io.unmapped_writes;
        report.fpga_words       = io.fpga_words;
        report.lightgun_reads   = io.lightgun_reads;
        report.interrupts       = io.interrupts;
        return report;
    }

    const Model1io::Counters& io = m_ioboard.counters();
    report.present          = m_ioboard.present();
    report.pc               = m_ioboard.cpu().pc();
    report.sp               = m_ioboard.cpu().sp();
    report.halted           = m_ioboard.cpu().halted();
    report.instructions     = m_ioboard.cpu().instructions();
    report.cycles           = m_ioboard.cpu().cycles();
    report.dual_port_reads  = io.dual_port_reads;
    report.dual_port_writes = io.dual_port_writes;
    report.analog_samples   = io.analog_samples;
    report.output_writes    = io.output_writes;
    report.unmapped_reads   = io.unmapped_reads;
    report.unmapped_writes  = io.unmapped_writes;
    report.io_port_accesses = io.io_port_reads + io.io_port_writes;
    return report;
}

void Model2Original::load_nvram()
{
    if (m_nvram_directory.empty() || m_game.name.empty()) {
        return;
    }
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

    // The settings EEPROM lives on the I/O board here rather than on the CPU
    // board, but it holds the same thing and is saved beside the NVRAM under the
    // same name, so a set that later gains a second board variant keeps its
    // settings.
    (void)io_eeprom().load((base / (m_game.name + ".eeprom")).string());
}

void Model2Original::save_nvram() const
{
    if (m_nvram_directory.empty() || m_game.name.empty()) {
        return;
    }

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

    (void)io_eeprom().save((base / (m_game.name + ".eeprom")).string());
}

}  // namespace sm2::hw
