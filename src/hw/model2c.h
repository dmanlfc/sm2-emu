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
#pragma once

#include "core/types.h"
#include "cpu/bus.h"
#include "cpu/i960/i960.h"
#include "cpu/mb86235/mb86235.h"
#include "hw/copro_fifo.h"
#include "hw/geometrizer.h"
#include "hw/eeprom_93c46.h"
#include "hw/i8251.h"
#include "hw/io_315_5649.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_sound.h"
#include "hw/model2_video.h"
#include "hw/sega_315_5881_crypt.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

// ===========================================================================
// CoproTgpx4 — the MB86235 coprocessor subsystem for Model 2C.
//
// Derived from MAME's model2c_state in src/mame/sega/model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels): copro_tgpx4_map, copro_tgpx4_data_map, copro_fifo_w,
// copro_halt, copro_boot and copro_function_port_w.
//
// The MB86235 "TGPx4" replaces Model 2B's SHARC. Unlike the SHARC, whose
// microcode lives in on-chip SRAM, the TGPx4's program memory is board RAM
// (MAME's copro_tgpx4_map maps 0x00000000-0x00000fff as RAM shared as
// "copro_tgpx4_program"), so this class owns it. The host uploads it through
// the same FIFO port the commands later travel over.
// ===========================================================================

class CoproTgpx4 final : public cpu::mb86235::Bus {
public:
    /// Words of board-supplied microcode RAM, 64 bits each.
    static constexpr u32 kProgramWords = cpu::mb86235::MB86235::kProgramWords;

    /// Depth of each host FIFO. MAME's model2c_state::machine_start uses 8,
    /// half of Model 2B's 16.
    static constexpr usize kFifoDepth = 8;

    /// Words of display list buffer the coprocessor can reach through its
    /// external bus.
    static constexpr u32 kBufferWords = 0x8000;

    CoproTgpx4();

    CoproTgpx4(const CoproTgpx4&)            = delete;
    CoproTgpx4& operator=(const CoproTgpx4&) = delete;

    /// Wire up external resources.
    ///
    /// `data_rom` is the copro_data ROM the TGPx4 reads through the upper half
    /// of its external bus. `buffer_ram` is the shared display list buffer,
    /// which it both reads and writes; it must outlive this object.
    void attach(std::span<const u32> data_rom, std::span<u32> buffer_ram);

    void reset();

    [[nodiscard]] cpu::mb86235::MB86235& cpu() { return m_cpu; }
    [[nodiscard]] const cpu::mb86235::MB86235& cpu() const { return m_cpu; }

    [[nodiscard]] CoproFifo& fifo_in() { return m_fifo_in; }
    [[nodiscard]] CoproFifo& fifo_out() { return m_fifo_out; }
    [[nodiscard]] const CoproFifo& fifo_in() const { return m_fifo_in; }
    [[nodiscard]] const CoproFifo& fifo_out() const { return m_fifo_out; }

    /// Advance the coprocessor by up to this many of its own cycles.
    [[nodiscard]] s32 run(s32 cycles);

    // -- host side ---------------------------------------------------------

    /// Write to the control register at 0x00980000.
    ///
    /// Bit 31 rising: start a microcode upload and reset the word counter.
    /// Bit 31 falling: boot, which only releases the halt line.
    void control_write(u32 value);
    [[nodiscard]] u32 control_read() const { return m_control; }

    /// Write to the FIFO port at 0x00884000.
    ///
    /// During an upload each pair of host writes assembles one 64-bit program
    /// word, low half first. Otherwise the value is pushed to the input FIFO.
    void host_fifo_write(u32 value);

    /// Read a result from the output FIFO.
    [[nodiscard]] u32 host_fifo_read();

    /// Write to a function port at 0x00880000. Encodes the function address
    /// from the write offset into the command word.
    void function_port_write(u32 byte_offset, u32 value);

    /// True when no result is waiting.
    [[nodiscard]] bool output_empty() const { return m_fifo_out.empty(); }

    /// Host writes accepted since the last upload began. Two of these make one
    /// 64-bit program word.
    [[nodiscard]] u32 uploaded_words() const { return m_upload_count; }

    /// Copro status register at 0x00980014 (MAME's copro_status_r).
    [[nodiscard]] u32 status() const { return m_upload_count == 0 ? 0xffffffffu : 0u; }

    // -- cpu::mb86235::Bus -------------------------------------------------

    [[nodiscard]] u64 program_read(u32 address) override;
    void program_write(u32 address, u64 data) override;
    [[nodiscard]] u32 external_read(u32 address) override;
    void external_write(u32 address, u32 data) override;

    [[nodiscard]] bool fifo_in_empty() const override { return m_fifo_in.empty(); }
    [[nodiscard]] bool fifo_in_full() const override { return m_fifo_in.full(); }
    [[nodiscard]] bool fifo_out_empty() const override { return m_fifo_out.empty(); }
    [[nodiscard]] bool fifo_out_full() const override { return m_fifo_out.full(); }

    [[nodiscard]] u32 fifo_in_pop() override;
    void fifo_out_push(u32 value) override;
    void fifo_in_clear() override { m_fifo_in.clear(); }
    void fifo_out_clear() override { m_fifo_out.clear(); }

private:
    cpu::mb86235::MB86235 m_cpu;

    CoproFifo m_fifo_in;
    CoproFifo m_fifo_out;

    /// The board's microcode RAM. Model 2B deliberately has no equivalent
    /// member because the SHARC keeps its program on-chip; the TGPx4's program
    /// space is external, so it lives here and is what program_read fetches
    /// from.
    std::vector<u64> m_program;

    std::span<const u32> m_data_rom;
    std::span<u32>       m_buffer_ram;

    u32 m_control      = 0;
    u32 m_upload_count = 0;
};

// ===========================================================================
// Model2C — Sega Model 2C-CRX.
//
// Derived from MAME's model2c_state in src/mame/sega/model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels).
//
// The same i960 main CPU, I/O controller, tilemap chip, geometrizer and
// 68000/SCSP sound board as Model 2B. What differs is the coprocessor (MB86235
// TGPx4 instead of an ADSP-21062 SHARC), the UART's address, and the texture
// RAM layout.
// ===========================================================================

class Model2C final : public cpu::Bus, public Model2MachineBase {
public:
    // -- video timing (same as Model 2A/2B) --------------------------------
    static constexpr u32 kDotClock        = 16'000'000;
    static constexpr u32 kCpuClock        = 25'000'000;
    static constexpr u32 kCoproClock      = 20'000'000;
    static constexpr u32 kHorizontalTotal = 656;
    static constexpr u32 kVerticalTotal   = 424;
    static constexpr u32 kVisibleWidth    = 496;
    static constexpr u32 kVisibleHeight   = 384;
    static constexpr u32 kCyclesPerLine =
        static_cast<u32>(static_cast<u64>(kHorizontalTotal) * kCpuClock / kDotClock);
    static constexpr u32 kCyclesPerFrame = kCyclesPerLine * kVerticalTotal;
    static constexpr u64 kFrameNanoseconds =
        static_cast<u64>(kCyclesPerFrame) * 1'000'000'000ULL / kCpuClock;

    static constexpr u32 kUartBitRate     = 31'250;
    static constexpr u32 kUartBitsPerByte = 10;

    Model2C();
    ~Model2C() override;

    Model2C(const Model2C&)            = delete;
    Model2C& operator=(const Model2C&) = delete;

    [[nodiscard]] bool init(const rom::GameSpec& game, rom::RomSet roms) override;
    void reset() override;
    void run_frame() override;

    [[nodiscard]] Inputs& inputs() override { return m_inputs; }
    [[nodiscard]] const Inputs& inputs() const override { return m_inputs; }

    [[nodiscard]] CpuStatus main_cpu_status() const override
    {
        CpuStatus status;
        status.state_string  = m_cpu.state_string();
        status.fault_message = m_cpu.fault_message();
        status.instructions  = m_cpu.instructions();
        status.halted        = m_cpu.halted();
        status.faulted       = m_cpu.faulted();
        return status;
    }

    [[nodiscard]] cpu::i960::I960& cpu() { return m_cpu; }
    [[nodiscard]] const cpu::i960::I960& cpu() const { return m_cpu; }

    [[nodiscard]] CoproTgpx4& copro() { return m_copro; }
    [[nodiscard]] const CoproTgpx4& copro() const { return m_copro; }

    [[nodiscard]] Geometrizer& geometry() { return m_geometry; }
    [[nodiscard]] const Geometrizer& geometry() const { return m_geometry; }

    [[nodiscard]] Model2Sound& sound() { return m_sound; }
    [[nodiscard]] const Model2Sound& sound() const { return m_sound; }

    [[nodiscard]] const I8251& uart() const { return m_uart; }

    [[nodiscard]] const RenderList& render_list() const override { return m_render_list; }

    [[nodiscard]] u64 cycles() const override { return m_cycles; }
    [[nodiscard]] u64 frames() const override { return m_frames; }

    [[nodiscard]] u32 intreq() const override { return m_intreq; }
    [[nodiscard]] u32 intena() const override { return m_intena; }

    void set_nvram_directory(const std::string& directory) override;
    void load_nvram() override;
    void save_nvram() const override;

    void set_log_unmapped(bool enable) override { m_log_unmapped = enable; }
    void log_burst_summary() const override;
    void log_unmapped_summary() const override;

    [[nodiscard]] std::span<const u8>  tile_ram() const override { return m_tile_ram; }
    [[nodiscard]] std::span<const u8>  char_ram() const override { return m_char_ram; }
    [[nodiscard]] std::span<const u16> palette_ram() const override { return m_palette_ram; }
    [[nodiscard]] std::span<const u16> colour_translate() const override { return m_colorxlat; }
    [[nodiscard]] std::span<const u8>  luma_ram() const override { return m_luma_ram; }
    [[nodiscard]] std::span<const u32> texture_ram(int sheet) const override
    {
        return sheet == 0 ? std::span<const u32>(m_texture_ram0)
                          : std::span<const u32>(m_texture_ram1);
    }
    [[nodiscard]] std::span<const u32> buffer_ram() const override { return m_buffer_ram; }
    [[nodiscard]] std::span<const u8> work_ram() const override { return m_work_ram; }

    [[nodiscard]] u32 geometry_read_start_address() const override
    {
        return m_geo_read_start_address;
    }
    [[nodiscard]] bool render_test_mode() const override { return m_render_test; }

    [[nodiscard]] std::span<const u16> framebuffer(int bank) const override
    {
        return bank == 0 ? std::span<const u16>(m_framebuffer_a)
                         : std::span<const u16>(m_framebuffer_b);
    }

    [[nodiscard]] bool palette_dirty() const override { return m_palette_dirty; }
    void clear_palette_dirty() override { m_palette_dirty = false; }

    [[nodiscard]] u64 texture_generation() const override { return m_texture_generation; }
    [[nodiscard]] u64 table_generation() const override { return m_table_generation; }

    [[nodiscard]] Model2Video& video() override { return m_video; }
    [[nodiscard]] const Model2Video& video() const override { return m_video; }

    void compose_video() override;

    // -- cpu::Bus ----------------------------------------------------------

    u8  read8(u32 address) override;
    u16 read16(u32 address) override;
    u32 read32(u32 address) override;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;
    std::pair<u8, u16>  read8_flags(u32 address) override;
    u16                 write8_flags(u32 address, u8 value) override;
    std::pair<u32, u16> read32_flags(u32 address) override;
    u16 write32_flags(u32 address, u32 value) override;

private:
    enum class Notify : u8 { None, Palette, TileRam, CharRam, TextureRam };

    struct Window {
        u8*   base     = nullptr;
        usize size     = 0;
        bool  writable = false;
        bool  rom      = false;
        u16   flags    = cpu::kBusFlagNone;
        Notify notify  = Notify::None;
        u32   offset   = 0;
    };

    void note_video_write(const Window& window, u32 width);
    [[nodiscard]] Window resolve(u32 address);
    [[nodiscard]] static u16 register_flags(u32 address);
    [[nodiscard]] u32 register_read(u32 address, u32 width);
    void register_write(u32 address, u32 value, u32 width);

    [[nodiscard]] u32 timers_r(u32 index);
    void timers_w(u32 index, u32 value);
    void service_timers();
    [[nodiscard]] u64 next_timer_deadline() const;

    void irq_update();
    void raise_interrupt(u32 line);
    void sound_ready_w();
    void on_vblank_start();

    void step_copro(u32 host_cycles);
    void sync_copro();

    [[nodiscard]] u8 io_port_b_read();
    [[nodiscard]] u8 io_port_c_read();
    void io_port_a_write(u8 value);
    void lamp_output_w(u8 value);
    void drive_board_write(u8 value);

    // -- lightgun interface board (837-12079) -------------------------------
    // Hangs off the I/O controller's RS-422 channel 2, exactly as on Model 2A.
    // MAME wires the same pair of callbacks in model2c_state::hotd.
    [[nodiscard]] u8 lightgun_mux_read();
    void             lightgun_mux_write(u8 value);
    [[nodiscard]] u8 lightgun_data_read(u8 offset) const;
    [[nodiscard]] u8 lightgun_offscreen_read(u8 offset) const;

    void note_unmapped_read(u32 address, u32 width);
    void note_unmapped_write(u32 address, u32 value, u32 width);

    // -- devices -----------------------------------------------------------

    cpu::i960::I960 m_cpu;
    Io315_5649      m_io;
    Eeprom93c46     m_eeprom;
    Model2Video     m_video;
    CoproTgpx4      m_copro;
    Geometrizer     m_geometry;
    Model2Sound     m_sound;
    I8251           m_uart;
    Sega3155881Crypt m_crypt;

    // -- memory ------------------------------------------------------------

    rom::RomSet   m_roms;
    rom::GameSpec m_game;

    std::span<const u8> m_rom_maincpu;
    std::span<const u8> m_rom_main_data;
    std::span<const u8> m_rom_copro_data;
    std::span<const u8> m_rom_polygons;
    std::span<const u8> m_rom_textures;

    std::vector<u8>  m_work_ram;
    std::vector<u8>  m_scratch_ram;
    std::vector<u32> m_buffer_ram;
    std::vector<u8>  m_tile_ram;
    std::vector<u8>  m_char_ram;
    std::vector<u16> m_palette_ram;
    std::vector<u16> m_colorxlat;
    std::vector<u8>  m_luma_ram;
    std::vector<u32> m_texture_ram0;
    std::vector<u32> m_texture_ram1;
    std::vector<u16> m_framebuffer_a;
    std::vector<u16> m_framebuffer_b;
    std::vector<u8>  m_nvram;
    std::vector<u8>  m_cpu_control;
    std::vector<u8>  m_comm_ram;
    std::vector<u8>  m_crypt_ram;

    // -- interrupt latch ---------------------------------------------------
    u32 m_intreq = 0;
    u32 m_intena = 0;

    // -- timers ------------------------------------------------------------
    struct Timer {
        u32  value        = 0xfffff;
        u32  original     = 0;
        u64  start_cycle  = 0;
        bool running      = false;
    };
    std::array<Timer, 4> m_timers{};

    // -- video and coprocessor registers -----------------------------------
    u32  m_videocontrol   = 0;
    bool m_render_mode    = false;
    bool m_render_test    = false;
    bool m_render_unk     = false;
    u32  m_geoctl         = 0;
    u32  m_geocnt         = 0;
    u32  m_geo_write_start_address = 0;
    u32  m_geo_read_start_address  = 0;
    bool m_ctrlmode       = false;

    u8  m_gear_selected      = 0;
    u8  m_drive_board_latch  = 0;
    u8  m_lightgun_mux       = 0;
    bool m_palette_dirty     = true;

    u64 m_texture_generation = 1;
    u64 m_table_generation   = 1;

    // -- scheduling --------------------------------------------------------
    u64 m_cycles      = 0;
    u64 m_frame_start = 0;
    u64 m_frames      = 0;

    u32  m_pending_intena       = 0;
    u64  m_pending_intena_cycle = 0;
    bool m_pending_intena_valid = false;

    Inputs m_inputs;
    RenderList m_render_list;

    // -- diagnostics -------------------------------------------------------
    bool m_log_unmapped = false;
    std::map<u32, u64> m_unmapped_reads;
    std::map<u32, u64> m_unmapped_writes;

    static constexpr u32 kBurstRegions = 4096;
    std::vector<u32> m_no_burst_reads  = std::vector<u32>(kBurstRegions, 0);
    std::vector<u32> m_no_burst_writes = std::vector<u32>(kBurstRegions, 0);

    std::string m_nvram_directory;

    // -- coprocessor scheduling --------------------------------------------
    u32  m_copro_debt    = 0;
    static constexpr s32 kCoproSyncLimit = 4096;
    bool m_in_copro_sync = false;
    static constexpr u32 kCoproInterleave = 128;
};

}  // namespace sm2::hw
