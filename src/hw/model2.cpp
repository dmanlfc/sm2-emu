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
// Sega Model 2A-CRX.
//
// The address decode, interrupt latch and timer behaviour follow MAME's
// src/mame/sega/model2.cpp (BSD-3-Clause), whose model2a_crx_mem this mirrors
// region for region so the two can be compared directly.

#include "hw/model2.h"

#include "core/log.h"

#include <algorithm>
#include <cstdio>
#include <bit>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Little-endian access helpers
// ---------------------------------------------------------------------------
// The i960 is little-endian and so is every host we target, so a memcpy is both
// correct and what the compiler turns into a single load.

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

/// View a byte region as 32-bit words.
///
/// The coprocessor addresses its ROMs a word at a time and never a byte, so this
/// is the shape it wants. Both platforms we build for are little-endian, and the
/// ROM assembly already produced little-endian words, so no swapping is needed;
/// an assertion would be the only thing a big-endian host required.
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
constexpr u32 kScratchRam      = 0x00200000;  // 256 KB
constexpr u32 kWorkRam         = 0x00500000;  // 1 MB
constexpr u32 kGeoPort         = 0x00800000;  // 16 KB, Geometrizer function ports
constexpr u32 kGeoProgram      = 0x00804000;  // 16 KB, Geometrizer upload
constexpr u32 kCoproFunction   = 0x00880000;  // 16 KB, coprocessor function port
constexpr u32 kCoproFifo       = 0x00884000;  // 16 KB, coprocessor FIFO
constexpr u32 kBufferRam       = 0x00900000;  // 128 KB, mirror 0x60000
constexpr u32 kVideoRegs       = 0x00980000;  // copro control, fifo status, videoctl, tgpid
constexpr u32 kCpuControl      = 0x00e00000;  // 0x38 bytes of wait-state registers
constexpr u32 kIrqRegs         = 0x00e80000;
constexpr u32 kTimerRegs       = 0x00f00000;
constexpr u32 kTileRam         = 0x01000000;  // 64 KB, mirror 0x110000
constexpr u32 kCharRam         = 0x01080000;  // 512 KB, mirror 0x100000
constexpr u32 kPaletteRam      = 0x01800000;  // 16 KB
constexpr u32 kColorXlat       = 0x01810000;  // 48 KB
constexpr u32 kZClip           = 0x0181c000;
constexpr u32 kCommRam         = 0x01a00000;  // 16 KB, mirror 0x10000
constexpr u32 kIoController    = 0x01c00000;  // 16 registers on byte lanes 0 and 2
constexpr u32 kUart            = 0x01c80000;
constexpr u32 kNvram           = 0x01d00000;  // 16 KB
constexpr u32 kDoaRam          = 0x01d80000;  // 315-5838 protection RAM window
constexpr u32 kDoaSrcAddr      = 0x01d87ff0;  // 315-5838 sequential read offset
constexpr u32 kDoaDataW        = 0x01d87ff4;  // 315-5838 protection input, unused in hack mode
constexpr u32 kDoaProtR        = 0x01d87ff8;  // 315-5838 decompressed data
constexpr u32 kDoaUnk          = 0x01d8400c;  // toggling busy-flag stub
constexpr u32 kCryptRam        = 0x01d80000;  // 64 KB, the 315-5881's staging buffer
constexpr u32 kCryptReady      = 0x01d90000;  // 315-5881 busy flag
constexpr u32 kCryptAddrLo     = 0x01d90008;  // source word address, low half
constexpr u32 kCryptAddrHi     = 0x01d9000a;  // high half, always written as zero
constexpr u32 kCryptSubkey     = 0x01d9000c;  // per-stream sequence key
constexpr u32 kCryptData       = 0x01d9000e;  // decrypted stream
constexpr u32 kRomMainData     = 0x02000000;  // 32 MB window
constexpr u32 kRomMainDataHigh = 0x06000000;  // 16 MB window at region offset 0x1000000
constexpr u32 kRenderMode      = 0x10000000;
constexpr u32 kPolygonCount    = 0x10400000;
constexpr u32 kFramebufferA    = 0x11600000;  // 512 KB
constexpr u32 kFramebufferB    = 0x11680000;  // 512 KB
constexpr u32 kTextureRam0     = 0x12000000;  // 2 MB window, mirror 0x200000
constexpr u32 kTextureRam1     = 0x12400000;  // 2 MB window, mirror 0x200000
constexpr u32 kLumaRam         = 0x12800000;  // byte-wide on lane 0, 0x8000 entries

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

Model2::Model2() : m_cpu(*this) {}

Model2::~Model2() = default;

bool Model2::init(const rom::GameSpec& game, rom::RomSet roms)
{
    m_game = game;
    m_roms = std::move(roms);

    m_rom_maincpu   = m_roms.region("maincpu");
    m_rom_main_data = m_roms.region("main_data");

    if (m_rom_maincpu.empty()) {
        SM2_ERROR("model2: the 'maincpu' region is missing");
        return false;
    }

    m_work_ram.assign(0x100000, 0);
    m_scratch_ram.assign(0x40000, 0);
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
    m_doa_ram.assign(0x8000, 0);
    m_crypt_ram.assign(0x10000, 0);

    // MAME's crypt_read_callback: a word read of the staging RAM, byte-swapped,
    // which for a little-endian bus is just a big-endian fetch.
    if (game.protection == rom::Protection::Sega315_5881 && game.protection_key == 0) {
        SM2_WARN("model2: %s needs a 315-5881 key and the database has none",
                 game.name.c_str());
    }
    m_crypt.set_key(game.protection_key);
    m_crypt.set_read_callback([this](u32 word_address) {
        const u32 offset = (word_address * 2) & 0xffffu;
        return static_cast<u16>((m_crypt_ram[offset] << 8) | m_crypt_ram[offset + 1]);
    });

    // The video stage keeps decoded copies of this memory, so it has to be
    // attached after the allocation above and before anything can write to it.
    m_video.attach(m_tile_ram, m_char_ram, m_palette_ram, m_colorxlat);

    // The coprocessor reads the CPU board's mathematical tables and the game's own
    // data ROM, and writes its results into the display list buffer.
    //
    // Whether the table ROM is present is the ROM loader's business, since it
    // knows what the set is supposed to contain. Its absence is reported by
    // CoproTgp and degrades trigonometry to zero rather than refusing to start,
    // which keeps a synthetic machine usable in tests.
    m_rom_copro_tables = m_roms.region("copro_tgp_tables");
    m_rom_copro_data   = m_roms.region("copro_data");
    m_copro.attach(as_words(m_rom_copro_tables), as_words(m_rom_copro_data),
                   m_buffer_ram);

    // The geometry engine walks the model ROM and reads texture headers out of the
    // texture ROM. Both are indexed as words, not bytes.
    m_rom_polygons = m_roms.region("polygons");
    m_rom_textures = m_roms.region("textures");
    m_geometry.attach(as_words(m_rom_polygons), as_halfwords(m_rom_textures),
                      m_buffer_ram);

    // The sound board. Nothing on the CPU board reads or writes it directly; the
    // two only meet over the serial link, so this is the whole connection: the
    // UART's transmitter feeds the SCSP's MIDI port and the SCSP's MIDI output
    // comes back into the UART's receiver.
    m_sound.attach(m_roms.region("audiocpu"), m_roms.region("samples"));
    m_uart.set_tx_handler([this](u8 value) { m_sound.midi_in(value); });
    m_sound.set_midi_out_handler([this](u8 value) { m_uart.write_rxd(value); });
    m_uart.set_ready_handler([this] { sound_ready_w(); });

    // Host side of the FIFO flow control. Only the machine can halt the host CPU,
    // which is why this is not in CoproTgp.
    //
    // A read of an empty output FIFO re-executes the instruction and then halts
    // the host until the coprocessor produces something. A write to a full input
    // FIFO halts the host until the coprocessor drains it. Both are released by
    // the opposite side making progress, and run_frame keeps stepping the
    // coprocessor while the host is halted, so neither can deadlock.
    m_copro.fifo_out().set_on_empty_retry([this] { m_cpu.stall(); });
    m_copro.fifo_out().set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_copro.fifo_out().set_on_unempty([this] { m_cpu.set_halted(false); });
    m_copro.fifo_in().set_on_full([this] { m_cpu.set_halted(true); });
    m_copro.fifo_in().set_on_unfull([this] { m_cpu.set_halted(false); });

    SM2_INFO("model2: %s board, %s", rom::board_name(game.board), game.title.c_str());
    reset();
    return true;
}

void Model2::reset()
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

    // The display-list buffer comes up holding a specific pattern. MAME notes
    // that the hardware probably derives this itself; without it, a game that
    // parses the buffer before writing it reads nonsense.
    std::fill(m_buffer_ram.begin(), m_buffer_ram.end(), 0x07800f0fu);

    // Bring the analogue controls up where the hardware has them with nobody at
    // the cabinet. MAME gets this from each PORT_BIT's default value; here it
    // comes from the same data, so a headless run -- which never polls a host
    // device -- sees a centred wheel and released pedals rather than full lock
    // and full throttle. Several titles will not leave their self-test
    // otherwise.
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
    m_doa_comp.reset();
    m_doa_unk_toggle = false;
    m_crypt.reset();

    // The serial link to the sound board. 500 kHz clock divided by 16 is the
    // 31.25 kHz standard Sega and MIDI rate, and the frame is eight data bits
    // between a start and a stop bit.
    m_uart.configure(kCpuClock, kUartBitRate, kUartBitsPerByte);
    m_uart.reset();

    // Port directions and callbacks, matching MAME's model2a machine config:
    // A drives the EEPROM lines, B reads the operator panel or the EEPROM's data
    // line, C and D read the two players' controls, F drives lamps and coin
    // counters, G reads the CPU board dipswitches.
    m_io.set_output(0, [this](u8 value) { io_port_a_write(value); });
    m_io.set_input(1, [this] { return io_port_b_read(); });
    m_io.set_input(2, [this] { return io_port_c_read(); });
    m_io.set_input(3, [this] { return m_inputs.in2; });
    m_io.set_output(5, [this](u8 value) { lamp_output_w(value); });
    m_io.set_input(6, [this] { return m_inputs.dipswitches; });
    for (u32 channel = 0; channel < Io315_5649::kAnalogCount; ++channel) {
        m_io.set_analog(channel, [this, channel] { return m_inputs.analog[channel]; });
    }

    // Port E latches force-feedback commands. MAME binds it for Sega Rally,
    // Daytona and the Indy 500 family; every other title leaves it unconnected.
    if (m_game.drive_board) {
        m_io.set_output(4, [this](u8 value) { drive_board_write(value); });
    }

    // The lightgun interface board hangs off RS-422 channel 2 rather than the
    // analogue mux: the program writes a byte-lane selector and reads the
    // selected byte back.
    if (m_game.lightgun.present) {
        m_io.set_serial2([this] { return lightgun_mux_read(); },
                         [this](u8 value) { lightgun_mux_write(value); });
    }

    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

void Model2::run_frame()
{
    for (u32 line = 0; line < kVerticalTotal; ++line) {
        const u64 line_end   = m_frame_start + static_cast<u64>(line + 1) * kCyclesPerLine;
        const u64 line_start = m_cycles;

        while (m_cycles < line_end) {
            // Stop at whichever comes first, the end of the scanline or a timer
            // expiry, so timer interrupts land on the right cycle rather than
            // being rounded to a scanline.
            u64 target = std::min(line_end, next_timer_deadline());
            if (target <= m_cycles) {
                target = m_cycles + 1;
            }
            // Also stop after a short interval so the coprocessor is interleaved
            // finely. The two are coupled only through eight-deep FIFOs, and
            // letting the host run a whole scanline ahead would overflow them
            // constantly. Nothing would be lost, but the host would spend the
            // time halted.
            target = std::min(target, m_cycles + kCoproInterleave);

            const s32 slice = static_cast<s32>(std::min<u64>(target - m_cycles, 1u << 20));
            const s32 used  = m_cpu.run(slice);
            m_cycles += static_cast<u64>(used);
            step_copro(static_cast<u32>(used > 0 ? used : 0));

            // The UART must be ticked alongside the CPU, not just once per
            // scanline, because TxRDY going high triggers the sound interrupt.
            // If the UART only advances at the end of the scanline, a byte
            // written early in the scanline doesn't finish until the next one,
            // and the program can never send more than one byte per scanline
            // (~3 bytes/frame instead of the ~100/frame VF2 needs for SFX).
            m_uart.run(static_cast<u32>(used > 0 ? used : 0));

            service_timers();

            // Delayed intena update: apply the mask 2 cycles after the write.
            if (m_pending_intena_valid && m_cycles >= m_pending_intena_cycle) {
                m_intena = m_pending_intena;
                m_pending_intena_valid = false;
                // Re-evaluate sound ready with the new mask, as MAME's delayed
                // callback does.
                sound_ready_w();
                irq_update();
            }
        }

        // The sound board runs on its own clock and shares nothing with the host
        // but a serial line, so it does not need the fine interleave the
        // coprocessor does. Once per scanline is fine enough for the SCSP's
        // timers, which are the only thing on the board with a deadline, and
        // coarse enough that the per-call cost of entering the 68000 core is
        // negligible.
        const u32 line_cycles = static_cast<u32>(m_cycles - line_start);
        m_sound.run(line_cycles);

        // TxRDY and RxRDY are levels, and the interrupt latch samples them rather
        // than seeing an edge. MAME gets this from the UART's 500 kHz transmit
        // clock, which re-evaluates the ready lines thousands of times a frame;
        // sampling once a scanline is the same behaviour at coarser grain.
        //
        // Sampling rather than edge-triggering is what makes the link start at
        // all: the program enables the transmitter first and unmasks this
        // interrupt afterwards, so an edge-triggered latch would never see the
        // transmitter become ready and the host would wait forever for the
        // interrupt that tells it to send the first command.
        sound_ready_w();

        if (line == kVisibleHeight) {
            on_vblank_start();
        }
    }

    m_frame_start += kCyclesPerFrame;
    ++m_frames;
}

void Model2::step_copro(u32 host_cycles)
{
    // Two coprocessor instructions per three host cycles. The remainder is kept
    // so the ratio is exact across slices.
    m_copro_debt += host_cycles * 2;
    const s32 cycles = static_cast<s32>(m_copro_debt / 3);
    m_copro_debt %= 3;

    if (cycles > 0) {
        static_cast<void>(m_copro.run(cycles));
    }
}

void Model2::sync_copro()
{
    if (m_in_copro_sync) {
        return;
    }
    m_in_copro_sync = true;

    // Run in small steps and stop as soon as a result appears, so a program that
    // polls for one value does not let the coprocessor race ahead of the host.
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

u64 Model2::next_timer_deadline() const
{
    u64 deadline = ~0ULL;
    for (const Timer& timer : m_timers) {
        if (timer.running) {
            deadline = std::min(deadline, timer.start_cycle + timer.original);
        }
    }
    return deadline;
}

void Model2::service_timers()
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

void Model2::on_vblank_start()
{
    // The geometry engine starts a new frame here. Bit 0 of the video control
    // register selects 30 Hz geometry, in which case it runs on even frames only
    // and the renderer shows the previous result again.
    //
    // This happens before the interrupt, as it does on hardware: the program's
    // vertical blank handler assumes the list it submitted has been consumed.
    if ((m_videocontrol & 1) == 0 || (m_frames & 1) == 0) {
        m_geometry.set_read_start_address(m_geo_read_start_address);
        m_geometry.set_crtc_offsets(m_video.crtc_x_offset(), m_video.crtc_y_offset());
        m_geometry.run(&m_render_list);
    }

    raise_interrupt(1u << 0);
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void Model2::sound_ready_w()
{
    // MAME's model2_state::sound_ready_w. The sound interrupt is asserted if
    // either RxRDY or TxRDY is active, and only latches while it is enabled, so a
    // program that masks it off misses it rather than taking it later.
    const u32 line = 1u << 10;
    if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

void Model2::raise_interrupt(u32 line)
{
    // A source only latches while it is enabled, which is what lets a game mask
    // an interrupt off and miss it entirely rather than taking it later.
    if ((m_intena & line) != 0) {
        m_intreq |= line;
        irq_update();
    }
}

void Model2::irq_update()
{
    // Twelve latched sources, four external lines.
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

u32 Model2::timers_r(u32 index)
{
    Timer& timer = m_timers[index];
    if (timer.running) {
        // The counter is not stored, it is derived: the master clock and the
        // timer clock are both 25 MHz, so one count is one master cycle.
        const u64 elapsed = m_cycles - timer.start_cycle;
        timer.value = elapsed >= timer.original
                          ? 0
                          : static_cast<u32>(timer.original - elapsed);
    }
    return timer.value;
}

void Model2::timers_w(u32 index, u32 value)
{
    Timer& timer      = m_timers[index];
    timer.value       = value;
    timer.original    = value;
    timer.start_cycle = m_cycles;
    timer.running     = true;
}

// ---------------------------------------------------------------------------
// I/O controller callbacks
// ---------------------------------------------------------------------------

void Model2::io_port_a_write(u8 value)
{
    // The settings EEPROM hangs off four bits of this port. Bit 0 additionally
    // switches port B between the operator panel and the EEPROM's data line.
    m_ctrlmode = bit(value, 0) != 0;
    m_eeprom.set_di(bit(value, 5) != 0);
    m_eeprom.set_cs(bit(value, 6) != 0);
    m_eeprom.set_clk(bit(value, 7) != 0);
}

u8 Model2::io_port_b_read()
{
    const u8 panel = m_inputs.in0;
    if (!m_ctrlmode) {
        return panel;
    }
    // In control mode the upper bits carry the EEPROM's data line and the low
    // nibble still reads the coin and test inputs.
    return static_cast<u8>(0xc0 | (m_eeprom.data_out() ? 0x20 : 0x00) | 0x10
                           | (panel & 0x0f));
}

u8 Model2::io_port_c_read()
{
    u8 data = m_inputs.in1;

    if (m_game.gearbox) {
        // MAME's daytona_gearbox_r. The three bits are not a gear number: the
        // shifter's five positions encode as 0, 2, 1, 6, 5, and releasing the
        // stick holds the last selection rather than returning to neutral, so a
        // program watching for a legal code never sees an illegal one. Without
        // this, IN1's idle 0xff presents the code 7, which no shifter produces.
        static constexpr u8 kGearValues[5] = {0, 2, 1, 6, 5};
        for (u32 gear = 0; gear < 5; ++gear) {
            if ((m_inputs.gears & (1u << gear)) != 0) {
                m_gear_selected = static_cast<u8>(gear);
                break;
            }
        }
        data = static_cast<u8>((data & ~0x70u) | (kGearValues[m_gear_selected] << 4));
    }

    return data;
}

// ---------------------------------------------------------------------------
// Lightgun interface board (837-12079)
// ---------------------------------------------------------------------------

u8 Model2::lightgun_data_read(u8 offset) const
{
    // Four 10-bit axes presented as eight byte lanes, in the board's own order:
    // P1 Y, P1 X, P2 Y, P2 X.
    const std::array<u16, 4> axes = {
        m_inputs.gun_p1y, m_inputs.gun_p1x, m_inputs.gun_p2y, m_inputs.gun_p2x,
    };
    const u16 value = axes[(offset >> 1) & 3];
    return (offset & 1) != 0 ? static_cast<u8>(value >> 8) : static_cast<u8>(value);
}

u8 Model2::lightgun_offscreen_read(u8 offset) const
{
    // Bit 0 is set while player 1 is aimed off the screen, bit 1 for player 2,
    // which is how a game distinguishes a reload from a miss. MAME derives the
    // border from each axis's own calibrated travel rather than from the raster,
    // because the gun's range and the visible area are not the same thing.
    constexpr float kBorderFraction = 0.05f;

    const auto offscreen = [](u16 value, const rom::LightgunAxis& axis) {
        const int border = static_cast<int>(
            static_cast<float>(axis.maximum - axis.minimum) * kBorderFraction);
        return value <= axis.minimum + border || value >= axis.maximum - border;
    };

    u16 data = 0xfffc;
    if (offscreen(m_inputs.gun_p1x, m_game.lightgun.p1x)
        || offscreen(m_inputs.gun_p1y, m_game.lightgun.p1y)) {
        data |= 1;
    }
    if (offscreen(m_inputs.gun_p2x, m_game.lightgun.p2x)
        || offscreen(m_inputs.gun_p2y, m_game.lightgun.p2y)) {
        data |= 2;
    }
    return static_cast<u8>((data >> ((offset & 1) * 8)) & 0xff);
}

u8 Model2::lightgun_mux_read()
{
    return m_lightgun_mux < 8 ? lightgun_data_read(m_lightgun_mux)
                              : lightgun_offscreen_read(0);
}

void Model2::lightgun_mux_write(u8 value)
{
    m_lightgun_mux = value;
}

void Model2::drive_board_write(u8 value)
{
    // MAME's drive_board_w latches the byte and pulses the drive CPU's IRQ. Only
    // Sega Rally and Daytona have a real Z80 behind it, and nothing on the host
    // side reads the latch back, so accepting the write is the whole
    // requirement for the games that reach gameplay without one.
    m_drive_board_latch = value;
}

void Model2::lamp_output_w(u8 value)
{
    // Coin counters and six cabinet lamps. Recorded for a future output layer;
    // nothing in the emulator consumes it yet.
    SM2_TRACE("model2: lamps = %02x", value);
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

Model2::Window Model2::resolve(u32 address)
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
        // ROM is exposed read-only; writes are discarded, matching MAME's
        // .rom().nopw().
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
    if (m_game.protection == rom::Protection::Sega315_5838_Doa
        && address >= kDoaRam && address < kDoaRam + 0x8000
        && !(address >= kDoaUnk && address < kDoaUnk + 4)
        && !(address >= kDoaSrcAddr && address < kDoaProtR + 4)) {
        return window(m_doa_ram, address - kDoaRam, true, cpu::kBusFlagNone);
    }
    if (m_game.protection == rom::Protection::Sega315_5881
        && address >= kCryptRam && address < kCryptRam + 0x10000) {
        return window(m_crypt_ram, address - kCryptRam, true, cpu::kBusFlagBurst);
    }
    if (address >= kCpuControl && address < kCpuControl + 0x38) {
        // Wait-state configuration. Plain memory, and notably the one memory
        // region MAME does not flag as burst-capable.
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

u16 Model2::register_flags(u32 address)
{
    // Bulk upload paths, which a program fills with multi-word stores and which
    // therefore have to advance the address. MAME flags exactly these.
    //
    // Getting this wrong is quiet and misleading rather than fatal: a four-word
    // store to a region lacking the flag writes all four words to the first
    // address, so the destination ends up holding the first word repeated. The
    // data looks structured, which makes it easy to mistake for the program's own
    // doing.
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

u32 Model2::register_read(u32 address, u32 width)
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
        SM2_TRACE("model2: read from geometry port %04x", offset);
        return 0;
    }

    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        // Not readable on real hardware.
        return 0xffffffff;
    }

    if (address >= kCoproFifo && address < kCoproFifo + 0x4000) {
        // Reading a result. If none is waiting, let the coprocessor run first:
        // MAME reaches the same point through a scheduler synchronisation, and
        // without it the host would halt for a whole slice on every poll.
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
            // Coprocessor output FIFO status: non-zero means empty. A game polls
            // this before reading a result, so the coprocessor has to have had a
            // chance to produce one.
            if (m_copro.output_empty()) {
                sync_copro();
            }
            return m_copro.output_empty() ? 1 : 0;
        }
        if (offset >= 0x0c && offset < 0x10) {
            // Frame parity in the upper bits, the two control bits below.
            const u8 parity = m_render_mode ? static_cast<u8>((m_frames & 1) << 2)
                                            : static_cast<u8>((m_frames & 2) << 1);
            return parity | (m_videocontrol & 3);
        }
        if (offset >= 0x30 && offset < 0x40) {
            // Board identification string, read and discarded by Top Skater.
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

    // I/O controller: eight bits wide, sitting on byte lanes 0 and 2 of each
    // 32-bit word, so register N lives at byte offset N*2.
    if (address >= kIoController && address < kIoController + 0x20) {
        const u32 offset = address - kIoController;
        if (width == 4) {
            const u32 reg = offset >> 1;
            return static_cast<u32>(m_io.read(reg))
                 | (static_cast<u32>(m_io.read(reg + 1)) << 16);
        }
        if ((offset & 1) != 0) {
            // An inactive byte lane.
            return 0xffffffff;
        }
        return m_io.read(offset >> 1);
    }

    if (address >= kUart && address < kUart + 0x04) {
        // The serial link to the sound board. Eight bits wide on byte lanes 0 and
        // 2, as MAME's umask16(0x00ff) puts it, so register N is at byte offset
        // N*2: 0 is the data register and 1 the status register.
        //
        // A 32-bit read at the base retrieves both registers simultaneously:
        // data in the low byte (offset 0) and status in the third byte (offset 2).
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
        // Number of polygons in the current list. Only Sky Target reads it.
        return m_geometry.polygon_count();
    }

    if (address >= 0x10800000 && address < 0x10800004) {
        return 0;
    }

    if (address >= kLumaRam && address < kLumaRam + 0x20000) {
        // Byte-wide on lane 0, so one entry per 32-bit word.
        const u32 index = (address - kLumaRam) / 4;
        return index < m_luma_ram.size() ? m_luma_ram[index] : 0;
    }

    if (m_game.protection == rom::Protection::Sega315_5838_Doa) {
        if (address >= kDoaProtR && address < kDoaProtR + 4) {
            // doa only reads 16 bits at a time; ST-V titles sharing this chip
            // read 32, so a 32-bit access packs two sequential 16-bit reads,
            // matching MAME's doa_prot_r.
            if (width == 4) {
                const u32 high = m_doa_comp.data_r();
                const u32 low  = m_doa_comp.data_r();
                return (high << 16) | low;
            }
            return m_doa_comp.data_r();
        }
        if (address >= kDoaUnk && address < kDoaUnk + 4) {
            m_doa_unk_toggle = !m_doa_unk_toggle;
            return m_doa_unk_toggle ? 0xffff : 0xfff0;
        }
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

void Model2::register_write(u32 address, u32 value, u32 width)
{
    // Texture RAM. Two 16-bit halves are packed into each 32-bit entry, so a
    // write's destination is half its window offset. Reads bypass this, which
    // means the two disagree; games only ever write, and the renderer reads the
    // packed form, so it does not matter. Carried over from MAME.
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
                    // Eye mode, used by Sega Rally's car select.
                    word |= ((offset >> 10) & 3) << 29;
                }
            } else {
                return;
            }

            const u32 index = m_geo_write_start_address / 4;
            if (index < m_buffer_ram.size()) {
                m_buffer_ram[index] = word;
            } else {
                SM2_TRACE("model2: display-list write past the buffer at %05x",
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
        SM2_TRACE("model2: write to geometry port %04x = %08x", offset, value);
        return;
    }

    if (address >= kGeoProgram && address < kGeoProgram + 0x4000) {
        if ((m_geoctl & 0x80000000) != 0) {
            // Program upload: only the word count matters, since the Geometrizer
            // is high-level emulated rather than executed.
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
            // Coprocessor control. Setting the top bit starts a microcode upload
            // and clearing it boots the coprocessor.
            m_copro.control_write(value);
            return;
        }
        if (offset >= 0x08 && offset < 0x0c) {
            // Geometrizer control, same upload and boot convention.
            if (((value ^ m_geoctl) & 0x80000000) != 0) {
                if ((value & 0x80000000) != 0) {
                    m_geocnt = 0;
                    SM2_DEBUG("model2: geometrizer upload started");
                } else {
                    SM2_DEBUG("model2: geometrizer boot, %u words uploaded", m_geocnt);
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
            // Acknowledge: the written value is a mask of bits to keep.
            m_intreq &= value;
            irq_update();
        } else {
            // MAME delays this by 80 ns (2 CPU cycles at 25 MHz) because the
            // real hardware latches the mask through a register that settles one
            // or two clocks after the write. Without the delay, an interrupt
            // enabled and then immediately disabled within the same handler is
            // never seen because the priority gate blocks it on the same cycle
            // the enable appears.
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

    // System 24 synchronisation registers. These shift the raster relative to
    // the monitor. The tilemap ignores them; the 3D projection does not.
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
        // 0xff disables clipping entirely.
        m_geometry.set_z_clip(static_cast<u8>(value));
        return;
    }

    if (address >= 0x01a04000 && address < 0x01a04004) {
        return;  // link board control, no board fitted
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

    if (address >= 0x01c00040 && address < 0x01c00044) {
        return;
    }

    // 0x01c00020 to 0x01c0003f is genuinely undecoded on Model 2A: MAME maps
    // only the sixteen I/O registers below it and the inert word above. The boot
    // code writes the string "SEGA" here anyway, which on the original Model 2
    // would have landed in the I/O board's dual-port RAM. Reported rather than
    // swallowed, because it is real behaviour and not a gap in this map.

    if (address >= kUart && address < kUart + 0x04) {
        if ((address - kUart) == 0) {
            SM2_TRACE("model2: uart data write %02x (ip=%08x)", value & 0xff, m_cpu.ip());
        }
        m_uart.write((address - kUart) >> 1, static_cast<u8>(value));
        return;
    }

    if (address >= kRenderMode && address < kRenderMode + 0x200000) {
        m_render_test = bit(value, 0) != 0;
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

    if (m_game.protection == rom::Protection::Sega315_5838_Doa) {
        if (address >= kDoaSrcAddr && address < kDoaSrcAddr + 4) {
            m_doa_comp.srcaddr_w(value);
            return;
        }
        if (address >= kDoaDataW && address < kDoaDataW + 4) {
            m_doa_comp.data_w_doa(value);
            return;
        }
    }

    if (m_game.protection == rom::Protection::Sega315_5881) {
        // Sixteen bits wide, and the games only ever write them that way. A
        // 32-bit store at the address register covers both halves, low first.
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

u8 Model2::read8(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr) {
        return *w.base;
    }
    return static_cast<u8>(register_read(address, 1) & 0xff);
}

u16 Model2::read16(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 2) {
        return load16(w.base);
    }
    return static_cast<u16>(register_read(address, 2) & 0xffff);
}

u32 Model2::read32(u32 address)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.size >= 4) {
        return load32(w.base);
    }
    return register_read(address, 4);
}

std::pair<u32, u16> Model2::read32_flags(u32 address)
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
    // Ports report no burst capability, which is what makes a multi-word load
    // from the coprocessor FIFO re-read the same address instead of walking
    // forward through it. The upload paths in register_flags are the exception.
    return {register_read(address, 4), flags};
}

void Model2::write8(u32 address, u8 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable) {
        if (address >= kPaletteRam && address < kPaletteRam + 0x4000) {
            SM2_TRACE("model2: palette8 write offset=%04x value=%02x", address - kPaletteRam, value);
        }
        *w.base = value;
        note_video_write(w, 1);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;  // writes to ROM are discarded
    }
    // Either unmapped, or a window whose writes need a transform: texture RAM
    // packs two halves per entry, so it is readable directly but not writable.
    register_write(address, value, 1);
}

void Model2::write16(u32 address, u16 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 2) {
        if (address >= kPaletteRam && address < kPaletteRam + 0x4000) {
            SM2_TRACE("model2: palette16 write offset=%04x value=%04x", address - kPaletteRam, value);
        }
        store16(w.base, value);
        note_video_write(w, 2);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;
    }
    register_write(address, value, 2);
}

void Model2::write32(u32 address, u32 value)
{
    const Window w = resolve(address);
    if (w.base != nullptr && w.writable && w.size >= 4) {
        if (address >= kPaletteRam && address < kPaletteRam + 0x4000) {
            SM2_TRACE("model2: palette32 write offset=%04x value=%08x", address - kPaletteRam, value);
        }
        store32(w.base, value);
        note_video_write(w, 4);
        return;
    }
    if (w.base != nullptr && w.rom) {
        return;
    }
    register_write(address, value, 4);
}

u16 Model2::write32_flags(u32 address, u32 value)
{
    const Window w = resolve(address);
    const u16    flags =
        w.base != nullptr ? w.flags : register_flags(address);
    if ((flags & cpu::kBusFlagBurst) == 0) {
        ++m_no_burst_writes[address >> 20];
    }

    if (w.base != nullptr && w.writable && w.size >= 4) {
        if (address >= kPaletteRam && address < kPaletteRam + 0x4000) {
            SM2_TRACE("model2: palette32f write offset=%04x value=%08x", address - kPaletteRam, value);
        }
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

void Model2::note_video_write(const Window& w, u32 width)
{
    switch (w.notify) {
        case Notify::None:
            return;
        case Notify::Palette:
            // MAME recomputes one entry on a palette write and the whole table
            // when the colour translation changes. Rebuilding all 4096 entries
            // costs microseconds and happens at most once per frame here, so both
            // cases take the same path.
            m_palette_dirty = true;
            ++m_table_generation;
            return;
        case Notify::TileRam:
            m_video.tiles().note_tile_write(w.offset, width);
            return;
        case Notify::CharRam:
            m_video.tiles().note_char_write(w.offset, width);
            return;
    }
}

void Model2::compose_video()
{
    if (m_palette_dirty) {
        m_video.refresh_pens();
        m_palette_dirty = false;
    }
    m_video.compose();
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void Model2::log_burst_summary() const
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

void Model2::note_unmapped_read(u32 address, u32 width)
{
    ++m_unmapped_reads[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2: unmapped read%u at %08x (ip %08x)", width * 8, address,
                  m_cpu.pip());
    }
}

void Model2::note_unmapped_write(u32 address, u32 value, u32 width)
{
    ++m_unmapped_writes[address & 0xfff00000u];
    if (m_log_unmapped) {
        SM2_DEBUG("model2: unmapped write%u at %08x = %08x (ip %08x)", width * 8,
                  address, value, m_cpu.pip());
    }
}

void Model2::log_unmapped_summary() const
{
    if (m_unmapped_reads.empty() && m_unmapped_writes.empty()) {
        SM2_INFO("model2: every access was mapped");
        return;
    }
    for (const auto& [region, count] : m_unmapped_reads) {
        SM2_WARN("model2: %llu unmapped read(s) in %08x", (unsigned long long)count,
                 region);
    }
    for (const auto& [region, count] : m_unmapped_writes) {
        SM2_WARN("model2: %llu unmapped write(s) in %08x", (unsigned long long)count,
                 region);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void Model2::set_nvram_directory(const std::string& directory)
{
    m_nvram_directory = directory;
}

void Model2::load_nvram()
{
    if (m_nvram_directory.empty() || m_game.name.empty()) {
        return;
    }
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

void Model2::save_nvram() const
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

    (void)m_eeprom.save((base / (m_game.name + ".eeprom")).string());
}

}  // namespace sm2::hw
