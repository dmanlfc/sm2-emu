//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Sega Model 1: V60 host, MB86233 TGP, System 24 tilemaps, Model 1 sound board.
// Memory map, interrupts and timers from MAME's sega/model1.cpp (BSD-3-Clause,
// Olivier Galibert).
#pragma once

#include "core/types.h"
#include "cpu/bus.h"
#include "cpu/v60/v60.h"
#include "hw/geometrizer.h"
#include "hw/i8251.h"
#include "hw/m1audio.h"
#include "hw/m1comm.h"
#include "hw/mb8421.h"
#include "hw/model1_geometry.h"
#include "hw/model1_tgp.h"
#include "hw/model1io.h"
#include "hw/model1io2.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class Model1 final : public cpu::Bus, public Model2MachineBase {
public:
    static constexpr u32 kCpuClock        = 16'000'000;  ///< 32 MHz / 2, also the dot clock
    static constexpr u32 kHorizontalTotal = 656;
    static constexpr u32 kVerticalTotal   = 424;
    static constexpr u32 kVisibleHeight   = 384;
    static constexpr u32 kCyclesPerLine   = kHorizontalTotal;
    static constexpr u32 kCyclesPerFrame  = kCyclesPerLine * kVerticalTotal;

    /// The peripherals count time in 25 MHz cycles, the Model 2 host's clock.
    static constexpr u32 kPeripheralClock = 25'000'000;
    static constexpr u32 kUartBitRate     = 31'250;
    static constexpr u32 kUartBitsPerByte = 10;

    Model1();
    ~Model1() override;
    Model1(const Model1&)            = delete;
    Model1& operator=(const Model1&) = delete;

    [[nodiscard]] bool init(const rom::GameSpec& game, rom::RomSet roms) override;
    void               reset() override;
    void               run_frame() override;

    [[nodiscard]] Inputs&       inputs() override { return m_inputs; }
    [[nodiscard]] const Inputs& inputs() const override { return m_inputs; }
    [[nodiscard]] M1Comm&       comm() override { return m_comm; }
    [[nodiscard]] const M1Comm& comm() const override { return m_comm; }

    [[nodiscard]] CpuStatus main_cpu_status() const override;

    [[nodiscard]] cpu::v60::V60&       cpu() { return m_cpu; }
    [[nodiscard]] const cpu::v60::V60& cpu() const { return m_cpu; }
    [[nodiscard]] Model1Tgp&           tgp() { return m_tgp; }
    [[nodiscard]] const Model1Tgp&     tgp() const { return m_tgp; }
    [[nodiscard]] M1Audio&             sound() { return m_m1audio; }
    [[nodiscard]] const M1Audio&       sound() const { return m_m1audio; }
    [[nodiscard]] const I8251&         uart() const { return m_uart; }

    [[nodiscard]] const RenderList& render_list() const override { return m_render_list; }
    void                            set_widescreen(u32 columns) override { m_widescreen_columns = columns; }
    [[nodiscard]] bool              hud_spread() const override { return m_geometry.hud_spread(); }
    [[nodiscard]] u64               geometry_stage_nanoseconds() const override { return m_geometry_ns; }

    [[nodiscard]] u64 cycles() const override { return m_cycles; }
    [[nodiscard]] u64 frames() const override { return m_frames; }
    [[nodiscard]] u32 intreq() const override { return m_irq_status; }
    [[nodiscard]] u32 intena() const override { return static_cast<u8>(~m_irq_mask); }

    void set_nvram_directory(const std::string& directory) override { m_nvram_directory = directory; }
    void load_nvram() override;
    void save_nvram() const override;

    [[nodiscard]] bool save_state_data(std::vector<u8>& out) const override;
    [[nodiscard]] bool load_state_data(std::span<const u8> data) override;

    void set_log_unmapped(bool enable) override { m_log_unmapped = enable; }
    void log_burst_summary() const override {}
    void log_unmapped_summary() const override;

    [[nodiscard]] std::span<const u8>  tile_ram() const override { return m_video.tile_ram_shown(); }
    [[nodiscard]] std::span<const u8>  char_ram() const override { return m_char_ram; }
    [[nodiscard]] std::span<const u16> palette_ram() const override { return m_palette_ram; }
    [[nodiscard]] std::span<const u16> colour_translate() const override { return m_colorxlat; }
    [[nodiscard]] std::span<const u8>  luma_ram() const override { return {}; }
    [[nodiscard]] std::span<const u32> texture_ram(int) const override { return {}; }
    [[nodiscard]] std::span<const u32> buffer_ram() const override { return {}; }
    [[nodiscard]] std::span<const u8>  work_ram() const override { return m_work_ram; }
    [[nodiscard]] u32                  geometry_read_start_address() const override { return 0; }
    [[nodiscard]] bool                 render_test_mode() const override { return false; }
    [[nodiscard]] std::span<const u16> framebuffer(int) const override { return {}; }

    [[nodiscard]] bool palette_dirty() const override { return m_palette_dirty; }
    void               clear_palette_dirty() override { m_palette_dirty = false; }
    [[nodiscard]] u64  texture_generation() const override { return 1; }
    [[nodiscard]] u64  table_generation() const override { return m_table_generation; }
    [[nodiscard]] u64  tile_generation() const override { return m_tile_generation; }
    [[nodiscard]] u64  char_generation() const override { return m_char_generation; }

    [[nodiscard]] Model2Video&       video() override { return m_video; }
    [[nodiscard]] const Model2Video& video() const override { return m_video; }
    [[nodiscard]] u64 tilemap_compose_nanoseconds() const override { return m_video.last_compose_nanoseconds(); }
    void              compose_video() override;
    [[nodiscard]] bool needs_cpu_tilemap() const override { return m_video.has_overlay(); }
    void set_music_volume(u32 percent) override { m_m1audio.set_music_volume(percent); }

    // -- cpu::Bus, the V60's program space ------------------------------------

    u8   read8(u32 address) override;
    u16  read16(u32 address) override;
    u32  read32(u32 address) override;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;

private:
    /// IN/OUT reach the coprocessor ports through this, so an empty FIFO can
    /// make the V60 retry the instruction.
    class IoSpace final : public cpu::Bus {
    public:
        explicit IoSpace(Model1& owner) : m_owner(owner) {}
        u8   read8(u32 address) override { return static_cast<u8>(read16(address & ~1u) >> ((address & 1) * 8)); }
        u16  read16(u32 address) override { return m_owner.io_read16(address); }
        u32  read32(u32 address) override { return read16(address) | (static_cast<u32>(read16(address + 2)) << 16); }
        void write8(u32 address, u8 value) override { m_owner.io_write16(address & ~1u, value); }
        void write16(u32 address, u16 value) override { m_owner.io_write16(address, value); }
        void write32(u32 address, u32 value) override
        {
            write16(address, static_cast<u16>(value));
            write16(address + 2, static_cast<u16>(value >> 16));
        }

    private:
        Model1& m_owner;
    };

    [[nodiscard]] u16 io_read16(u32 address);
    void              io_write16(u32 address, u16 value);

    [[nodiscard]] u8* ram_window(u32 address, bool write);
    [[nodiscard]] u16 register_read16(u32 address, bool from_io);
    void              register_write16(u32 address, u16 value, u16 mask);
    void              note_ram_write(u32 address);

    [[nodiscard]] u16 copro_fifo_read(u32 half, bool from_io);
    void              copro_fifo_write(u32 half, u16 value);
    void              sync_tgp();
    void              step_tgp(u32 cycles);
    void              step_peripherals(u32 cycles);
    void              flush_audio();

    void irq_raise(u32 level);
    void irq_update();
    void irq_control_w(u8 data);
    void sound_ready_w();
    [[nodiscard]] static u8 irq_cycle(void* context);

    void timer_period_w(u32 index, u16 value);
    [[nodiscard]] u16 timer_r(u32 index);
    void service_timers();

    void on_vblank();
    void set_current_render_list();
    [[nodiscard]] std::span<const u16> current_list() const;

    void lamp_output_w(u8 value);
    void r360_w(u8 value);
    void serialize(Archive& ar);

    cpu::v60::V60  m_cpu;
    IoSpace        m_io_space;
    Model1Tgp      m_tgp;
    Model1Geometry m_geometry;
    Model2Video    m_video;
    Model1io       m_ioboard;
    Model1io2      m_ioboard2;
    bool           m_uses_io2 = false;
    Mb8421         m_dpram;
    M1Comm         m_comm;
    bool           m_has_comm = false;  ///< the cabinet carries a link board
    I8251          m_uart;
    M1Audio        m_m1audio;

    rom::RomSet   m_roms;
    rom::GameSpec m_game;

    std::span<const u8> m_rom;  ///< MAME's 32 MB "maincpu" region
    u32                 m_bank_offset = 0x1000000;

    std::vector<u8>  m_nvram;     ///< RAMA, 64 KB at 0x400000
    std::vector<u8>  m_work_ram;  ///< RAMB, 256 KB at 0x500000
    std::vector<u16> m_list_words;  ///< two 64 KB display lists at 0x600000
    std::vector<u8>  m_tile_ram;
    std::vector<u8>  m_char_ram;
    std::vector<u16> m_palette_ram;
    std::vector<u16> m_colorxlat;

    u16 m_listctl[2]{};

    /// Wing War R360's motion base: its status byte, and the channel the
    /// throttle sits on, which reaches the game through the base instead.
    bool m_r360          = false;
    u8   m_r360_state    = 0xff;
    u32  m_r360_throttle = 2;
    bool m_netmerc       = false;  ///< Seeds the headset tracker's pose.

    u8 m_irq_status = 0;
    u8 m_irq_mask   = 0xff;
    u8 m_last_irq   = 0;

    u16 m_timer_mode = 0;
    struct Timer {
        u16 period   = 0;
        u16 value    = 0;
        u64 deadline = 0;
    };
    std::array<Timer, 2> m_timers{};

    u32 m_fifo_read_latch  = 0;
    u32 m_fifo_write_latch = 0;

    u64 m_cycles      = 0;
    u64 m_frame_start = 0;
    u64 m_frames      = 0;
    bool m_in_frame   = false;
    u64 m_tgp_debt    = 0;
    u64 m_peripheral_debt = 0;
    u32 m_audio_pending   = 0;  ///< sound board host cycles not yet run
    bool m_in_tgp_sync = false;

    bool m_palette_dirty    = true;
    u64  m_table_generation = 1;
    u64  m_tile_generation  = 1;
    u64  m_char_generation  = 1;
    u64  m_geometry_ns      = 0;
    u32  m_widescreen_columns = 0;

    Inputs     m_inputs;
    RenderList m_render_list;
    RenderList m_overlay_list;

    bool        m_log_unmapped = false;
    u64         m_unmapped     = 0;
    std::string m_nvram_directory;
};

}  // namespace sm2::hw
