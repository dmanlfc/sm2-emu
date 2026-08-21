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
#include "hw/eeprom_93c46.h"
#include "hw/i8251.h"
#include "hw/io_315_5649.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_sound.h"
#include "hw/model2_video.h"
#include "hw/sega_315_5838_comp.h"
#include "hw/sega_315_5881_crypt.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

/// Sega Model 2A-CRX.
///
/// Owns the memory map, the interrupt latch, the four count-down timers and the
/// I/O devices, and acts as the i960's bus. The address decode follows MAME's
/// model2a_crx_mem region for region so the two can be compared directly.
///
/// Not yet present: the geometry coprocessor and its FIFOs (phase 3), the
/// tilemap chip's rendering (phase 2) and the sound board (phase 5). Their
/// address ranges exist and behave as memory or as inert registers, which is
/// enough for the program to boot.
///
/// This is the Model 2A implementation of hw::Model2MachineBase (see
/// hw/model2_machine_base.h for the board-agnostic interface and why the CPU
/// status accessors it needs are exposed through a type-erased CpuStatus
/// rather than a virtual CPU base class). Its own additional accessors --
/// cpu(), copro(), sound(), uart(), geometry() and the rest -- return
/// board-specific hardware types that a Model 2B or 2C machine would not share,
/// so they stay outside the shared interface; code that needs them (the
/// boot-test report, the debug dumps) is written against the concrete Model2
/// class rather than the base interface.
class Model2 final : public cpu::Bus, public Model2MachineBase {
public:
    // -- video timing ------------------------------------------------------
    // From MAME's screen configuration: a 16 MHz dot clock, 656x424 total,
    // 496x384 visible. At the i960's 25 MHz that is exactly 1025 cycles per
    // scanline and 434600 per frame, giving 57.5245 Hz.
    static constexpr u32 kDotClock      = 16'000'000;
    static constexpr u32 kCpuClock      = 25'000'000;
    static constexpr u32 kHorizontalTotal = 656;
    static constexpr u32 kVerticalTotal   = 424;
    static constexpr u32 kVisibleWidth    = 496;
    static constexpr u32 kVisibleHeight   = 384;
    // Computed at 64 bits: 656 * 25000000 overflows a u32, which silently
    // produced a frame a fifth of its real length.
    static constexpr u32 kCyclesPerLine =
        static_cast<u32>(static_cast<u64>(kHorizontalTotal) * kCpuClock / kDotClock);
    static constexpr u32 kCyclesPerFrame = kCyclesPerLine * kVerticalTotal;
    static_assert(kCyclesPerLine == 1025, "scanline length changed unexpectedly");
    static_assert(kCyclesPerFrame == 434600, "frame length changed unexpectedly");

    /// How long one video frame lasts, in nanoseconds.
    ///
    /// The number the frame pacer needs, kept here rather than restated there so
    /// there is one definition of how fast this machine runs. 434600 cycles at
    /// 25 MHz is 17.384 ms exactly, which is 57.5245 Hz: near enough to a monitor's
    /// 60 Hz to look right and far enough from it that ignoring the difference
    /// makes the game run four percent fast.
    static constexpr u64 kFrameNanoseconds =
        static_cast<u64>(kCyclesPerFrame) * 1'000'000'000ULL / kCpuClock;
    static_assert(kFrameNanoseconds == 17'384'000, "frame duration changed unexpectedly");

    // -- the serial link to the sound board ---------------------------------
    // A 500 kHz clock on a divide-by-sixteen, which is sixteen times the 31.25 kHz
    // rate Sega uses for sound commands on every board that has this link, and
    // also the MIDI rate. Eight data bits between a start and a stop bit.
    static constexpr u32 kUartBitRate     = 31'250;
    static constexpr u32 kUartBitsPerByte = 10;

    Model2();
    ~Model2() override;

    Model2(const Model2&)            = delete;
    Model2& operator=(const Model2&) = delete;

    /// Wire up the ROMs and bring the machine to its reset state.
    [[nodiscard]] bool init(const rom::GameSpec& game, rom::RomSet roms) override;

    void reset() override;

    /// Advance one video frame, including the vertical blank interrupt.
    void run_frame() override;

    [[nodiscard]] Inputs& inputs() override { return m_inputs; }
    [[nodiscard]] const Inputs& inputs() const override { return m_inputs; }

    [[nodiscard]] cpu::i960::I960& cpu() { return m_cpu; }
    [[nodiscard]] const cpu::i960::I960& cpu() const { return m_cpu; }

    /// A type-erased snapshot of the i960's state, for hw::Model2MachineBase.
    /// The concrete accessor above stays available for code (the boot-test
    /// report, the debug dumps) that already depends on the i960 specifically.
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

    [[nodiscard]] CoproTgp& copro() { return m_copro; }
    [[nodiscard]] const CoproTgp& copro() const { return m_copro; }

    [[nodiscard]] Geometrizer& geometry() { return m_geometry; }
    [[nodiscard]] const Geometrizer& geometry() const { return m_geometry; }

    [[nodiscard]] Model2Sound& sound() { return m_sound; }
    [[nodiscard]] const Model2Sound& sound() const { return m_sound; }

    /// The serial link to the sound board, for reporting how much traffic it
    /// carried.
    [[nodiscard]] const I8251& uart() const { return m_uart; }

    /// This frame's screen-space polygons, in drawing order.
    ///
    /// Rebuilt by the geometry engine at each vertical blank, or every other one
    /// when the program has selected 30 Hz geometry.
    [[nodiscard]] const RenderList& render_list() const override { return m_render_list; }

    [[nodiscard]] u64 cycles() const override { return m_cycles; }
    [[nodiscard]] u64 frames() const override { return m_frames; }

    /// The interrupt latch, for reporting which sources the program has armed.
    [[nodiscard]] u32 intreq() const override { return m_intreq; }
    [[nodiscard]] u32 intena() const override { return m_intena; }

    // -- persistence -------------------------------------------------------

    /// Directory for the NVRAM and EEPROM images. Loaded now, saved on request.
    void set_nvram_directory(const std::string& directory) override;
    void load_nvram() override;
    void save_nvram() const override;

    // -- diagnostics -------------------------------------------------------

    /// Log every access that lands outside a mapped region. Off by default
    /// because a boot produces hundreds of thousands of them.
    void set_log_unmapped(bool enable) override { m_log_unmapped = enable; }

    /// Counts of unmapped accesses, keyed by 1 MB region.
    [[nodiscard]] const std::map<u32, u64>& unmapped_reads() const { return m_unmapped_reads; }
    [[nodiscard]] const std::map<u32, u64>& unmapped_writes() const { return m_unmapped_writes; }

    /// Report where multi-word accesses landed on a region that does not report
    /// burst capability.
    ///
    /// Worth watching because the consequence is silent and confusing: a
    /// multi-word load or store only advances its address when the region says it
    /// may, so a region wrongly lacking the flag turns a four-word transfer into
    /// four accesses to one address. The result is plausible-looking repeated
    /// data rather than an error.
    void log_burst_summary() const override;

    void log_unmapped_summary() const override;

    // -- views for the renderer, used from phase 2 onwards ------------------

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

    /// The display list buffer, which the coprocessor fills and the geometry
    /// engine reads. Comes up holding a repeated pattern, so "still all pattern"
    /// means nothing has written to it.
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

    /// Cleared by the renderer once it has taken the change into account.
    [[nodiscard]] bool palette_dirty() const override { return m_palette_dirty; }
    void clear_palette_dirty() override { m_palette_dirty = false; }

    /// Counters that advance whenever the 3D renderer's source data changes.
    ///
    /// The renderer keeps a copy of each per frame in flight and re-uploads only
    /// when its copy is out of date, so a game that loads its textures once at
    /// startup pays for them once. They are counters rather than flags because
    /// several frames are in flight and each has to decide for itself; a flag one
    /// of them cleared would leave the others stale.
    ///
    /// Texture RAM is tracked separately from the small tables because it is two
    /// megabytes against a few tens of kilobytes, and the tables change far more
    /// often.
    [[nodiscard]] u64 texture_generation() const override { return m_texture_generation; }
    [[nodiscard]] u64 table_generation() const override { return m_table_generation; }

    // -- video output ------------------------------------------------------

    [[nodiscard]] Model2Video& video() override { return m_video; }
    [[nodiscard]] const Model2Video& video() const override { return m_video; }

    /// Produce this frame's 2D output, refreshing the palette first if the
    /// program changed it.
    ///
    /// Separate from run_frame() because composition costs about as much as a
    /// frame of emulation and only the display needs it: a headless run does it
    /// once at the end rather than 600 times.
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
    /// Memory the video stage keeps a decoded copy of, so writes there have to be
    /// reported rather than just stored.
    enum class Notify : u8 {
        None,
        Palette,  ///< Palette RAM or the colour translation table.
        TileRam,  ///< Name tables; the scroll and mask registers need nothing.
        CharRam,  ///< Character pattern data.
    };

    /// A directly addressable window: plain memory with no side effects.
    struct Window {
        u8*   base     = nullptr;
        usize size     = 0;
        bool  writable = false;
        /// Read-only because it is ROM, so writes are discarded rather than
        /// handed to a register handler. Distinct from a window that is merely
        /// not directly writable, such as texture RAM, whose writes go through a
        /// packing transform.
        bool  rom      = false;
        /// Burst capability. A multi-word load only advances its address when the
        /// region reports this, which is what makes a load from a port re-read
        /// the same address instead of walking through memory.
        u16   flags    = cpu::kBusFlagNone;
        /// What the video stage has to be told about a write here.
        Notify notify  = Notify::None;
        /// Byte offset within the region, so a notification can say which entry
        /// changed rather than invalidating everything.
        u32   offset   = 0;
    };

    /// Tell the video stage that a write landed in memory it caches.
    void note_video_write(const Window& window, u32 width);

    /// Resolve an address to a memory window, or return an empty one when the
    /// address belongs to a register or is unmapped.
    [[nodiscard]] Window resolve(u32 address);

    /// Burst capability of a register region.
    ///
    /// Most register regions have none, which is what makes a multi-word access
    /// to a port re-read one address instead of walking memory. A few do have it,
    /// because they are write-only bulk upload paths rather than ports, and there
    /// the address must advance.
    [[nodiscard]] static u16 register_flags(u32 address);

    // Register handlers, named after their MAME counterparts.
    [[nodiscard]] u32 register_read(u32 address, u32 width);
    void register_write(u32 address, u32 value, u32 width);

    [[nodiscard]] u32  timers_r(u32 index);
    void timers_w(u32 index, u32 value);
    void service_timers();
    [[nodiscard]] u64 next_timer_deadline() const;

    void irq_update();
    void raise_interrupt(u32 line);

    /// Latch the sound interrupt, which the UART raises on either of its ready
    /// lines. MAME's sound_ready_w.
    void sound_ready_w();
    void on_vblank_start();

    /// Run the coprocessor for its share of `host_cycles` of host time.
    ///
    /// The coprocessor's instruction rate is two thirds of the host's, so the
    /// remainder is carried between calls rather than rounded away; over a frame
    /// that difference is thousands of instructions.
    void step_copro(u32 host_cycles);

    /// Run the coprocessor now, because the host is waiting on a result.
    ///
    /// This is what MAME achieves with a scheduler synchronisation. It runs until
    /// a result appears or the coprocessor can make no further progress, and
    /// borrows the time from the coprocessor's future share so the two do not
    /// drift apart.
    void sync_copro();

    [[nodiscard]] u8 io_port_b_read();
    [[nodiscard]] u8 io_port_c_read();
    [[nodiscard]] u8 lightgun_mux_read();
    void             lightgun_mux_write(u8 value);
    [[nodiscard]] u8 lightgun_data_read(u8 offset) const;
    [[nodiscard]] u8 lightgun_offscreen_read(u8 offset) const;
    void             drive_board_write(u8 value);
    void io_port_a_write(u8 value);
    void lamp_output_w(u8 value);

    void note_unmapped_read(u32 address, u32 width);
    void note_unmapped_write(u32 address, u32 value, u32 width);

    // -- devices -----------------------------------------------------------

    cpu::i960::I960 m_cpu;
    Io315_5649      m_io;
    Eeprom93c46     m_eeprom;
    Model2Video     m_video;
    CoproTgp        m_copro;
    Geometrizer     m_geometry;

    /// The sound board: its own 68000, RAM and ROM, on its own clock. Stepped
    /// from run_frame. The only link to the CPU board is a serial one, which is
    /// not wired up yet, so for now it runs its program and nobody listens.
    Model2Sound     m_sound;

    /// The uPD71051 that carries sound commands to the sound board. Its clock is
    /// 500 kHz on a divide-by-sixteen, which is the 31.25 kHz rate Sega uses for
    /// this link everywhere, and the frame is 8-N-1.
    I8251           m_uart;

    /// The DOA boot-check protection chip. Always constructed, but only wired
    /// into the memory map when `m_game.protection` asks for it, so titles
    /// that do not need it see no behaviour change.
    Sega3155838Comp m_doa_comp;

    /// The 315-5881 stream cipher on the security board. Same arrangement as
    /// above: always constructed, only decoded when `m_game.protection` asks.
    Sega3155881Crypt m_crypt;

    // -- memory ------------------------------------------------------------

    rom::RomSet m_roms;
    rom::GameSpec m_game;

    std::span<const u8> m_rom_maincpu;
    std::span<const u8> m_rom_main_data;
    std::span<const u8> m_rom_copro_tables;
    std::span<const u8> m_rom_copro_data;
    std::span<const u8> m_rom_polygons;
    std::span<const u8> m_rom_textures;

    std::vector<u8>  m_work_ram;      ///< 1 MB at 0x00500000
    std::vector<u8>  m_scratch_ram;   ///< 256 KB at 0x00200000
    std::vector<u32> m_buffer_ram;    ///< 128 KB at 0x00900000, the display list
    std::vector<u8>  m_tile_ram;      ///< 64 KB, System 24 name tables
    std::vector<u8>  m_char_ram;      ///< 512 KB, System 24 tile patterns
    std::vector<u16> m_palette_ram;   ///< 0x2000 entries, BGR555
    std::vector<u16> m_colorxlat;     ///< 0x6000 entries, the master colour LUT
    std::vector<u8>  m_luma_ram;      ///< 32 KB, per-polygon tone curves
    std::vector<u32> m_texture_ram0;  ///< 2 MB window; writes pack into the low half
    std::vector<u32> m_texture_ram1;
    std::vector<u16> m_framebuffer_a; ///< 512 KB, only reachable in render test mode
    std::vector<u16> m_framebuffer_b;
    std::vector<u8>  m_nvram;         ///< 16 KB battery-backed SRAM at 0x01d00000
    std::vector<u8>  m_cpu_control;   ///< 0x38 bytes of wait-state registers
    std::vector<u8>  m_comm_ram;      ///< 16 KB link board shared RAM

    /// DOA protection chip's RAM window at 0x01d80000, matching MAME's
    /// model2_0229_mem. Only reachable when `m_game.protection` asks for it.
    std::vector<u8>  m_doa_ram;

    /// The 315-5881's staging RAM at 0x01d80000, matching MAME's
    /// model2_5881_mem. The program writes the ciphertext here and the chip
    /// reads it back through its callback, so this is the whole data path.
    std::vector<u8>  m_crypt_ram;

    // -- interrupt latch ---------------------------------------------------
    // Twelve sources folded onto the i960's four external lines:
    //   bit 0        vertical blank            -> IRQ0
    //   bit 1        unused
    //   bits 2 to 5  timers 0 to 3             -> IRQ2
    //   bits 6 to 9  other IRQ2 sources, unused
    //   bit 10       sound board UART          -> IRQ3
    //   bit 11       second IRQ3 source, unused
    u32 m_intreq = 0;
    u32 m_intena = 0;

    // -- timers ------------------------------------------------------------
    // Four independent down-counters clocked at 25 MHz, which is also the master
    // clock, so a count is exactly one master cycle.
    struct Timer {
        u32  value        = 0xfffff;
        u32  original     = 0;
        u64  start_cycle  = 0;
        bool running      = false;
    };
    std::array<Timer, 4> m_timers{};

    // -- video and coprocessor registers -----------------------------------

    u32  m_videocontrol   = 0;
    bool m_render_mode    = false;  ///< true: 60 Hz, false: 30 Hz
    bool m_render_test    = false;  ///< host access to the live framebuffer
    bool m_render_unk     = false;
    u32  m_geoctl         = 0;
    u32  m_geocnt         = 0;
    u32  m_geo_write_start_address = 0;
    u32  m_geo_read_start_address  = 0;
    bool m_ctrlmode       = false;  ///< port B returns EEPROM data instead of IN0

    /// Which of the lightgun interface board's byte lanes the program selected.
    u8 m_lightgun_mux = 0;

    /// Gear selector state for a cabinet with a shifter. The gate holds the
    /// last gear when nothing is pressed, so the program never reads neutral
    /// mid-shift.
    u8 m_gear_selected = 0;

    /// Last byte latched to the drive board. Nothing consumes it yet; the games
    /// that need one only require the write to be accepted.
    u8 m_drive_board_latch = 0;
    bool m_palette_dirty  = true;

    /// Toggled on every read of the DOA protection chip's busy-flag stub at
    /// 0x01d8400c, matching MAME's doa_unk_r.
    bool m_doa_unk_toggle = false;

    /// Both start at one so that a renderer whose copies start at zero uploads
    /// once before the first frame.
    u64 m_texture_generation = 1;
    u64 m_table_generation   = 1;

    // -- scheduling --------------------------------------------------------

    u64 m_cycles      = 0;  ///< master cycles since reset, at 25 MHz
    u64 m_frame_start = 0;
    u64 m_frames      = 0;

    /// Delayed intena update, matching MAME's 80 ns timer.
    u32  m_pending_intena       = 0;
    u64  m_pending_intena_cycle = 0;
    bool m_pending_intena_valid = false;

    Inputs m_inputs;

    RenderList m_render_list;

    // -- diagnostics -------------------------------------------------------

    bool m_log_unmapped = false;
    std::map<u32, u64> m_unmapped_reads;
    std::map<u32, u64> m_unmapped_writes;

    /// Multi-word accesses that found no burst capability, one counter per 1 MB of
    /// the 32-bit space. A flat array rather than a map so the count costs an
    /// increment on a path taken by every ldl and stl.
    static constexpr u32 kBurstRegions = 4096;
    std::vector<u32> m_no_burst_reads  = std::vector<u32>(kBurstRegions, 0);
    std::vector<u32> m_no_burst_writes = std::vector<u32>(kBurstRegions, 0);

    std::string m_nvram_directory;

    // -- coprocessor scheduling --------------------------------------------

    /// Unspent host cycles owed to the coprocessor, times two.
    ///
    /// The coprocessor takes three of its 50 MHz clocks per instruction, so it
    /// retires two instructions for every three host cycles. Keeping the remainder
    /// here makes the ratio exact over any number of slices.
    u32 m_copro_debt = 0;

    /// Longest run granted to a single synchronisation, in coprocessor cycles.
    ///
    /// A bound is needed because a program that waits on a result the coprocessor
    /// will never produce would otherwise spin here forever.
    static constexpr s32 kCoproSyncLimit = 4096;

    /// Guards against a synchronisation triggering another one.
    bool m_in_copro_sync = false;

    /// Longest the host runs before the coprocessor is given a turn, in host
    /// cycles. Small enough that the FIFOs rarely overflow, large enough that the
    /// interleave costs little.
    static constexpr u32 kCoproInterleave = 128;
};

}  // namespace sm2::hw
