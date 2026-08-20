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
#include "cpu/sharc/sharc.h"
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
// CoproSharc — the ADSP-21062 coprocessor subsystem for Model 2B.
//
// Derived from MAME's model2b_state copro handling (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese).
//
// The SHARC replaces the MB86234 TGP. The host uploads microcode via DMA
// (16-bit words packed into the SHARC's external port), then the SHARC
// processes FIFO commands and writes results to buffer RAM, feeding the
// existing Geometrizer.
// ===========================================================================

class CoproSharc final : public cpu::sharc::Bus {
public:
    /// Depth of each host FIFO (MAME uses 16 for model2b).
    static constexpr usize kFifoDepth = 16;

    CoproSharc();

    CoproSharc(const CoproSharc&)            = delete;
    CoproSharc& operator=(const CoproSharc&) = delete;

    /// Wire up external resources.
    ///
    /// `data_rom` is the copro_data ROM the SHARC reads through its DM space.
    /// `buffer_ram` is the shared display list buffer.
    void attach(std::span<const u32> data_rom, std::span<u32> buffer_ram);

    void reset();

    [[nodiscard]] cpu::sharc::SHARC& cpu() { return m_cpu; }
    [[nodiscard]] const cpu::sharc::SHARC& cpu() const { return m_cpu; }

    [[nodiscard]] CoproFifo& fifo_in() { return m_fifo_in; }
    [[nodiscard]] CoproFifo& fifo_out() { return m_fifo_out; }
    [[nodiscard]] const CoproFifo& fifo_in() const { return m_fifo_in; }
    [[nodiscard]] const CoproFifo& fifo_out() const { return m_fifo_out; }

    /// Advance the coprocessor by up to this many of its own cycles.
    [[nodiscard]] s32 run(s32 cycles);

    // -- host side ---------------------------------------------------------

    /// Write to the control register at 0x00980000.
    ///
    /// Bit 31 rising: start microcode upload, halt the SHARC, reset counter.
    /// Bit 31 falling: boot the SHARC, release halt.
    void control_write(u32 value);
    [[nodiscard]] u32 control_read() const { return m_control; }

    /// Write to the FIFO port at 0x00884000.
    ///
    /// During an upload: 16-bit words sent via external_dma_write to SHARC PM.
    /// Normal mode: push to the input FIFO.
    void host_fifo_write(u32 value);

    /// Read a result from the output FIFO.
    [[nodiscard]] u32 host_fifo_read();

    /// Write to a function port at 0x00880000. Encodes the function address
    /// from the write offset into the command word.
    void function_port_write(u32 byte_offset, u32 value);

    /// True when no result is waiting.
    [[nodiscard]] bool output_empty() const { return m_fifo_out.empty(); }

    /// Words of microcode uploaded since the last upload began.
    [[nodiscard]] u32 uploaded_words() const { return m_upload_count; }

    /// Write to SHARC IOP register from the host bus (at 0x008c0000).
    void iop_write(u32 offset, u32 data);

    /// Copro status register at 0x00980014.
    [[nodiscard]] u32 status() const { return m_upload_count == 0 ? 0xffffffffu : 0u; }

    // -- cpu::sharc::Bus ---------------------------------------------------

    [[nodiscard]] u64 pm_read48(u32 address) override;
    void pm_write48(u32 address, u64 data) override;
    [[nodiscard]] u32 pm_read32(u32 address) override;
    void pm_write32(u32 address, u32 data) override;
    [[nodiscard]] u32 dm_read32(u32 address) override;
    void dm_write32(u32 address, u32 data) override;
    void external_dma_write(u32 address, u64 data) override;

private:
    cpu::sharc::SHARC m_cpu;

    CoproFifo m_fifo_in;
    CoproFifo m_fifo_out;

    /// Internal block 0 program memory: 4K words of 48 bits, stored as u64.
    static constexpr u32 kProgramWords = 0x4000;
    std::vector<u64> m_program;

    std::span<const u32> m_data_rom;
    std::span<u32>       m_buffer_ram;

    u32 m_control      = 0;
    u32 m_upload_count = 0;
};

// ===========================================================================
// Model2B — Sega Model 2B-CRX.
//
// Derived from MAME's model2b_state (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).
//
// The i960 is still the main CPU (same core, same Bus pattern). The SHARC
// replaces the MB86233 TGP as the geometry coprocessor. The sound board is
// the same 68000/SCSP as Model 2A.
// ===========================================================================

class Model2B final : public cpu::Bus, public Model2MachineBase {
public:
    // -- video timing (same as Model 2A) -----------------------------------
    static constexpr u32 kDotClock        = 16'000'000;
    static constexpr u32 kCpuClock        = 25'000'000;
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

    Model2B();
    ~Model2B() override;

    Model2B(const Model2B&)            = delete;
    Model2B& operator=(const Model2B&) = delete;

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

    [[nodiscard]] CoproSharc& copro() { return m_copro; }
    [[nodiscard]] const CoproSharc& copro() const { return m_copro; }

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
    std::pair<u32, u16> read32_flags(u32 address) override;
    u16 write32_flags(u32 address, u32 value) override;

private:
    enum class Notify : u8 { None, Palette, TileRam, CharRam };

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

    void note_unmapped_read(u32 address, u32 width);
    void note_unmapped_write(u32 address, u32 value, u32 width);

    // -- devices -----------------------------------------------------------

    cpu::i960::I960 m_cpu;
    Io315_5649      m_io;
    Eeprom93c46     m_eeprom;
    Model2Video     m_video;
    CoproSharc      m_copro;
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
