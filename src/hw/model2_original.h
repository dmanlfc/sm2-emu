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
#include "hw/copro_tgp.h"
#include "hw/geometrizer.h"
#include "hw/i8251.h"
#include "hw/m2comm.h"
#include "hw/mb8421.h"
#include "hw/model1io.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

// ===========================================================================
// Model2Original — the original Sega Model 2, the board before the CRX family.
//
// Derived from MAME's model2o_state in src/mame/sega/model2.cpp (BSD-3-Clause,
// copyright-holders R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
// Matthew Daniels). The address decode mirrors model2o_mem -- which is
// model2_tgp_mem plus four overrides -- region for region so the two can be
// compared directly.
//
// The same i960 main CPU, MB86234 TGP coprocessor, geometrizer, System 24 tilemap
// chip and video timing as Model 2A: MAME's model2o_state derives from
// model2_tgp_state exactly as model2a_state does, and shares its coprocessor
// wiring verbatim, so hw::CoproTgp is reused here unchanged.
//
// Three things differ, and all three are on the periphery:
//
//   * Scratch RAM is 128 KB, not 256 KB, and the second 128 KB of that window is
//     a mirror of the program ROM at offset 0x20000.
//   * There is no 315-5649 on the i960's bus. Inputs arrive through a 2K
//     dual-port RAM (hw::Mb8421) filled in by a whole separate computer, the
//     Model 1 I/O board (hw::Model1io), which runs its own Z80 program.
//   * The sound board is the Model 1 one -- 68000 + YM3438 + two MultiPCMs --
//     rather than the 68000/SCSP board the CRX family uses. It is not emulated;
//     see the note on m_uart below.
// ===========================================================================

class Model2Original final : public cpu::Bus, public Model2MachineBase {
public:
    // -- video timing (identical to Model 2A) -------------------------------
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

    // -- the serial link to the sound board ---------------------------------
    // MAME clocks model2o's UART from 16 MHz / 2 / 16, which is the same 31.25 kHz
    // Sega/MIDI rate the CRX boards use from their own 500 kHz clock.
    static constexpr u32 kUartBitRate     = 31'250;
    static constexpr u32 kUartBitsPerByte = 10;

    Model2Original();
    ~Model2Original() override;

    Model2Original(const Model2Original&)            = delete;
    Model2Original& operator=(const Model2Original&) = delete;

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

    [[nodiscard]] CoproTgp& copro() { return m_copro; }
    [[nodiscard]] const CoproTgp& copro() const { return m_copro; }

    [[nodiscard]] Geometrizer& geometry() { return m_geometry; }
    [[nodiscard]] const Geometrizer& geometry() const { return m_geometry; }

    /// The I/O board, for reporting whether its Z80 is executing sensibly.
    [[nodiscard]] Model1io& ioboard() { return m_ioboard; }
    [[nodiscard]] const Model1io& ioboard() const { return m_ioboard; }

    /// The dual-port RAM the I/O board and the i960 share.
    [[nodiscard]] const Mb8421& dual_port_ram() const { return m_dpram; }

    /// The serial link that would carry sound commands. See m_uart.
    [[nodiscard]] const I8251& uart() const { return m_uart; }

    [[nodiscard]] const RenderList& render_list() const override { return m_render_list; }

    [[nodiscard]] u64 cycles() const override { return m_cycles; }
    [[nodiscard]] u64 frames() const override { return m_frames; }

    [[nodiscard]] u32 intreq() const override { return m_intreq; }
    [[nodiscard]] u32 intena() const override { return m_intena; }

    void set_nvram_directory(const std::string& directory) override;
    void load_nvram() override;
    void save_nvram() const override;

    /// Copy the set's shipped EEPROM image over the chip, if it ships one.
    void seed_eeprom_from_rom();

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
    enum class Notify : u8 { None, Palette, TileRam, CharRam };

    struct Window {
        u8*    base     = nullptr;
        usize  size     = 0;
        bool   writable = false;
        bool   rom      = false;
        u16    flags    = cpu::kBusFlagNone;
        Notify notify   = Notify::None;
        u32    offset   = 0;
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

    void lamp_output_w(u8 value);
    void drive_board_write(u8 value);

    void note_unmapped_read(u32 address, u32 width);
    void note_unmapped_write(u32 address, u32 value, u32 width);

    // -- devices -----------------------------------------------------------

    cpu::i960::I960 m_cpu;
    Model2Video     m_video;
    CoproTgp        m_copro;
    Geometrizer     m_geometry;

    /// The I/O board and the dual-port RAM between it and the i960. Together
    /// these replace the 315-5649 the CRX boards put on the i960's own bus: the
    /// program never touches a switch directly, it reads whatever the board's
    /// Z80 last left in the shared RAM.
    Model1io m_ioboard;
    Mb8421   m_dpram;

    /// The link board. Present on every Model 2 machine configuration in MAME,
    /// and a linked title will not leave its network check without one.
    M2Comm   m_comm;

    /// The uPD71051 that carries sound commands off the board.
    ///
    /// The M1 audio board at the other end -- a 68000 with a YM3438 and two
    /// MultiPCMs, MAME's SEGAM1AUDIO -- is not emulated. It is a different board
    /// from the 68000/SCSP one the CRX family uses and shares nothing with it, so
    /// hw::Model2Sound does not apply here and there is no sound at all on this
    /// board yet.
    ///
    /// The UART itself is fully present, because the handshake is not optional:
    /// the program enables the transmitter, unmasks the sound interrupt and waits
    /// for TxRDY before it will proceed. With the transmitter enabled and nothing
    /// consuming the bytes, that handshake completes -- transmitted bytes are
    /// counted and dropped -- and nothing on the receive side ever asserts RxRDY,
    /// which is the same as a sound board that never talks back. If a title turns
    /// out to wait for a reply, that will show up as a stall here rather than as
    /// an invented protocol.
    I8251 m_uart;

    // -- memory ------------------------------------------------------------

    rom::RomSet   m_roms;
    rom::GameSpec m_game;

    std::span<const u8> m_rom_maincpu;
    std::span<const u8> m_rom_main_data;
    std::span<const u8> m_rom_copro_tables;
    std::span<const u8> m_rom_copro_data;
    std::span<const u8> m_rom_polygons;
    std::span<const u8> m_rom_textures;

    std::vector<u8>  m_work_ram;      ///< 1 MB at 0x00500000
    std::vector<u8>  m_scratch_ram;   ///< 128 KB at 0x00200000, half of Model 2A's
    std::vector<u32> m_buffer_ram;    ///< 128 KB at 0x00900000, the display list
    std::vector<u8>  m_tile_ram;
    std::vector<u8>  m_char_ram;
    std::vector<u16> m_palette_ram;
    std::vector<u16> m_colorxlat;
    std::vector<u8>  m_luma_ram;
    std::vector<u32> m_texture_ram0;
    std::vector<u32> m_texture_ram1;
    std::vector<u16> m_framebuffer_a;
    std::vector<u16> m_framebuffer_b;
    std::vector<u8>  m_nvram;         ///< 16 KB battery-backed SRAM at 0x01d00000
    std::vector<u8>  m_cpu_control;
    std::vector<u8>  m_comm_ram;      ///< 16 KB link board shared RAM

    // -- interrupt latch ---------------------------------------------------
    u32 m_intreq = 0;
    u32 m_intena = 0;

    // -- timers ------------------------------------------------------------
    struct Timer {
        u32  value       = 0xfffff;
        u32  original    = 0;
        u64  start_cycle = 0;
        bool running     = false;
    };
    std::array<Timer, 4> m_timers{};

    // -- video and coprocessor registers -----------------------------------
    u32  m_videocontrol = 0;
    bool m_render_mode  = false;
    bool m_render_test  = false;
    bool m_render_unk   = false;
    u32  m_geoctl       = 0;
    u32  m_geocnt       = 0;
    u32  m_geo_write_start_address = 0;
    u32  m_geo_read_start_address  = 0;

    /// Last byte the I/O board latched to a force-feedback drive board. Only
    /// Daytona has one; nothing consumes it.
    u8   m_drive_board_latch = 0;
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

    Inputs     m_inputs;
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
    // Same MB86234 at the same 50 MHz as Model 2A: three coprocessor clocks per
    // instruction against the host's 25 MHz means two instructions per three host
    // cycles, and the debt is kept in thirds so the ratio is exact.
    u32 m_copro_debt = 0;
    static constexpr s32 kCoproSyncLimit  = 4096;
    bool m_in_copro_sync = false;
    static constexpr u32 kCoproInterleave = 128;
};

}  // namespace sm2::hw
