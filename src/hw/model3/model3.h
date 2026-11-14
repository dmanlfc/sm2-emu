// SPDX-License-Identifier: GPL-3.0-or-later
//
// A complete Sega Model 3 (Steps 1.0, 1.5, 2.0 and 2.1): memory map, I/O,
// system registers, security board, PCI, and the frame schedule that drives
// the PowerPC, the Real3D and the tile generator.
//
// Derived from Supermodel's Src/Model3/Model3.cpp and Model3.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team (Model3.cpp) and Copyright (c) 2011-2021 Bart
// Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle, and Spindizzi
// (Model3.h).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Notes:
//   - Implements hw::Machine; publishes a VideoFrame per run_frame().
//   - Memory is held in guest (big-endian) order. RAM, CROM, the bank window,
//     backup and security RAM and tilegen VRAM are cpu::PageMap pages; the
//     rest goes through PpcBus.
//   - Banked CROM of 64 MB or less is mirrored by page mapping, not copied.
//   - Backup and security RAM accept 8- and 16-bit accesses.
//   - VROM of 32 MB or less is mirrored whole.
//   - The frame's loops are Scheduler events in absolute cycles.
//   - Virtual RTC; inputs come from hw::Inputs, with game remaps done in the
//     input layer.
//   - Sound, drive and net boards attach through ports.h.
//   - Keisho's 93C46 and 315-5881, with a short EEPROM busy delay.
//   - Archive save states; NVRAM in the .nv block layout plus an
//     "Keisho RTC" block.
#pragma once

#include "cpu/page_map.h"
#include "cpu/ppc/ppc.h"
#include "hw/comm_board.h"
#include "hw/eeprom_93c46.h"
#include "hw/machine.h"
#include "hw/model3/irq.h"
#include "hw/model3/jtag.h"
#include "hw/model3/mpc10x.h"
#include "hw/model3/ports.h"
#include "hw/model3/real3d.h"
#include "hw/model3/rtc72421.h"
#include "hw/model3/scheduler.h"
#include "hw/model3/scsi_53c810.h"
#include "hw/model3/tilegen.h"
#include "hw/model3/video_frame.h"
#include "hw/sega_315_5881_crypt.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace sm2::hw::model3 {

class Model3 final : public Machine, public cpu::ppc::PpcBus, public PciDevice {
public:
    static constexpr u32 kRamSize        = 0x800000;    ///< 8 MB
    static constexpr u32 kCromSize       = 0x800000;    ///< fixed CROM window
    static constexpr u32 kBankSize       = 0x800000;    ///< one CROMxx bank
    static constexpr u32 kBackupRamSize  = 0x20000;
    static constexpr u32 kSecurityRamSize = 0x20000;
    static constexpr u32 kNetRamSize     = 0x10000;

    /// Scheduler event ids.
    enum Event : u32 {
        kEvVblankStart = 0,
        kEvIrq2Poll,
        kEvMidiPoll,
        kEvVblankRest,
        kEvVblankEnd,
        kEvLine,
        kEvSoundSlice,
        kEvFrameEnd,
    };

    Model3();
    ~Model3() override;

    [[nodiscard]] MachineFamily family() const override { return MachineFamily::Model3; }
    [[nodiscard]] Model3*       as_model3() override { return this; }
    [[nodiscard]] const Model3* as_model3() const override { return this; }

    // -- hw::Machine ---------------------------------------------------------------

    [[nodiscard]] bool init(const rom::GameSpec& game, rom::RomSet roms) override;
    void reset() override;
    void run_frame() override;

    [[nodiscard]] Inputs&       inputs() override { return m_inputs; }
    [[nodiscard]] const Inputs& inputs() const override { return m_inputs; }
    [[nodiscard]] CommBoard&       comm() override { return m_net_comm ? *m_net_comm : m_comm; }
    [[nodiscard]] const CommBoard& comm() const override { if (m_net_comm) return *m_net_comm; return m_comm; }

    void set_nvram_directory(const std::string& directory) override { m_nvram_directory = directory; }
    void load_nvram() override;
    void save_nvram() const override;

    [[nodiscard]] bool save_state_data(std::vector<u8>& out) const override;
    [[nodiscard]] bool load_state_data(std::span<const u8> data) override;

    [[nodiscard]] CpuStatus main_cpu_status() const override;
    [[nodiscard]] u64 cycles() const override { return m_sched.now(); }
    [[nodiscard]] u64 frames() const override { return m_frames; }
    void set_log_unmapped(bool enable) override { m_log_unmapped = enable; }

    // -- Model 3 specifics ----------------------------------------------------------

    /// Valid until the end of the run_frame() after next (double-buffered).
    [[nodiscard]] const VideoFrame& video_frame() const { return m_frames_out[m_published]; }

    /// Renderer feedback: line-of-sight value for priority level 0-3. Call
    /// between frames only.
    void set_line_of_sight(unsigned index, float value) { m_real3d.set_line_of_sight(index, value); }

    /// Unix seconds; emulated time thereafter (saved in states and NVRAM).
    void set_rtc_time(s64 unix_seconds) { m_rtc.set_time(unix_seconds); }

    /// Bit 0 P1, bit 1 P2 off screen; read via gun register 8.
    void set_gun_offscreen(u8 bits) { m_gun_offscreen = bits; }

    void attach_cpu(std::unique_ptr<MainCpu> cpu);
    void attach_sound(std::unique_ptr<SoundPort> sound);
    void attach_drive_board(std::unique_ptr<DriveBoardPort> board);
    void attach_net_board(std::unique_ptr<NetBoardPort> board);

    [[nodiscard]] SoundPort*  sound() { return m_sound.get(); }
    [[nodiscard]] MainCpu*    cpu() { return m_cpu.get(); }
    void set_exec_mode(cpu::ppc::ExecMode mode);

    [[nodiscard]] const rom::GameSpec& game() const { return m_game; }
    [[nodiscard]] const rom::RomSet&   roms() const { return m_roms; }
    /// 0x10, 0x15, 0x20, 0x21.
    [[nodiscard]] int stepping() const { return m_stepping; }

    /// Frame timing in PowerPC cycles.
    [[nodiscard]] u32 frame_cycles() const { return m_frame_cycles; }
    [[nodiscard]] u32 line_cycles() const { return m_line_cycles; }
    [[nodiscard]] u32 cpu_hz() const { return m_cpu_hz; }

    // Devices and memory, for debugging.
    [[nodiscard]] cpu::PageMap&    page_map() { return m_pages; }
    [[nodiscard]] IrqController&   irq() { return m_irq; }
    [[nodiscard]] Real3D&          real3d() { return m_real3d; }
    [[nodiscard]] TileGen&         tilegen() { return m_tilegen; }
    [[nodiscard]] Scsi53c810&      scsi() { return m_scsi; }
    [[nodiscard]] Mpc10x&          pci_bridge() { return m_mpc; }
    [[nodiscard]] Rtc72421&        rtc() { return m_rtc; }
    [[nodiscard]] Eeprom93c46&     eeprom() { return m_eeprom; }
    [[nodiscard]] Scheduler&       scheduler() { return m_sched; }
    [[nodiscard]] std::span<u8>    ram() { return m_ram; }
    [[nodiscard]] std::span<u8>    backup_ram() { return m_backup_ram; }
    [[nodiscard]] std::span<u8>    security_ram() { return m_security_ram; }
    [[nodiscard]] std::span<const u32> vrom() const { return m_vrom; }
    [[nodiscard]] u32 crom_bank_register() const { return m_crom_bank_reg; }
    [[nodiscard]] u8  midi_control() const { return m_midi_ctrl; }

    /// NVRAM image in the .nv block layout.
    [[nodiscard]] std::vector<u8> nvram_image() const;
    [[nodiscard]] bool apply_nvram_image(std::span<const u8> image);

    // -- cpu::ppc::PpcBus (big-endian values) ----------------------------------------

    u8  read8(u32 addr) override;
    u16 read16(u32 addr) override;
    u32 read32(u32 addr) override;
    u64 read64(u32 addr) override;
    void write8(u32 addr, u8 data) override;
    void write16(u32 addr, u16 data) override;
    void write32(u32 addr, u32 data) override;
    void write64(u32 addr, u64 data) override;

    // -- PciDevice (device 16) ----------------------------------------------------------

    [[nodiscard]] u32 read_pci_config(unsigned device, unsigned reg, unsigned bits,
                                      unsigned offset) const override;
    void write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset,
                          u32 data) override;

private:
    class NullComm final : public CommBoard {
    public:
        void set_transport(std::unique_ptr<CommTransport> transport) override { m_transport = std::move(transport); }
        [[nodiscard]] bool link_alive() const override { return false; }
        [[nodiscard]] u8   link_id() const override { return 0; }
        [[nodiscard]] u8   link_count() const override { return 0; }
        [[nodiscard]] bool enabled() const override { return false; }

    private:
        std::unique_ptr<CommTransport> m_transport;
    };

    // memory map
    void build_memory(const rom::RomSet& roms);
    void map_pages();
    void set_crom_bank(u32 value);

    u8   mmio_read8(u32 addr);
    u16  mmio_read16(u32 addr);
    u32  mmio_read32(u32 addr);
    void mmio_write8(u32 addr, u8 data);
    void mmio_write16(u32 addr, u16 data);
    void mmio_write32(u32 addr, u32 data);
    void unmapped(const char* what, u32 addr, u32 data = 0);

    // I/O
    u8   read_inputs(unsigned reg);
    [[nodiscard]] u8 game_inputs_g2() const;
    void write_inputs(unsigned reg, u8 data);
    u32  read_security(unsigned reg);
    void write_security(unsigned reg, u32 data);
    u16  read_security_ram(u32 word);
    [[nodiscard]] u8 read_system_register(unsigned reg) const;
    void write_system_register(unsigned reg, u8 data);
    void write_midi(unsigned offset, u8 data);
    [[nodiscard]] bool scsi_at_c0() const;

    // netboard window (0xC0000000) when a board is attached and running
    u8   net_read8(u32 addr);
    u32  net_read32(u32 addr);
    void net_write8(u32 addr, u8 data);
    void net_write32(u32 addr, u32 data);

    // frame schedule
    void begin_frame();
    void dispatch(u32 event, u64 when);
    void start_active(u64 when);
    void schedule_sound_slices();
    void run_cpu_until_frame_end();
    void end_frame();
    void run_sound_to_now();
    void publish_frame();
    void track_tilegen(bool on);
    [[nodiscard]] bool tilegen_store(u32 addr, u32 size, u32 data);
    void cpu_irq_line(bool level);

    void serialize(Archive& ar);

    // -- configuration --
    rom::GameSpec m_game;
    rom::RomSet   m_roms;
    int  m_stepping = 0x10;
    u32  m_cpu_hz   = 66'000'000;
    u32  m_frame_cycles = 0;
    u32  m_line_cycles  = 0;
    u32  m_vblank_cycles = 0;
    bool m_initialised = false;
    bool m_log_unmapped = false;
    bool m_has_gun = false;
    std::string m_nvram_directory;

    // -- memory (guest byte order) --
    cpu::PageMap    m_pages;
    std::vector<u8> m_ram;
    std::vector<u8> m_crom_fixed;
    std::vector<u8> m_crom_banked;
    std::vector<u8> m_zero_bank;
    u32             m_banked_mirror = 0;  ///< 64 MB when the set is mirrored, else 128 MB
    std::vector<u8> m_backup_ram;
    std::vector<u8> m_security_ram;
    std::vector<u8> m_net_ram;
    std::vector<u32> m_vrom;
    u32 m_crom_bank_reg = 0xFF;

    // -- devices --
    std::unique_ptr<MainCpu>        m_cpu;
    std::unique_ptr<SoundPort>      m_sound;
    std::unique_ptr<DriveBoardPort> m_drive;
    std::unique_ptr<NetBoardPort>   m_net;
    bool m_run_net_board = false;
    CommBoard* m_net_comm = nullptr;
    IrqController     m_irq;
    Mpc10x            m_mpc;
    PciBus            m_pci;
    Scsi53c810        m_scsi;
    Rtc72421          m_rtc;
    Eeprom93c46       m_eeprom;
    TileGen           m_tilegen;
    Real3D            m_real3d;
    Jtag              m_jtag;
    Sega3155881Crypt  m_crypt;
    NullComm          m_comm;
    Inputs            m_inputs;

    // -- I/O state --
    u8   m_input_bank  = 0;
    u8   m_serial_fifo1 = 0;
    u8   m_serial_fifo2 = 0;
    u8   m_gun_reg     = 0;
    u8   m_gun_offscreen = 0;
    s32  m_adc_channel = 0;
    u8   m_midi_ctrl   = 0;
    std::array<u8, 2> m_output_reg{};
    bool m_security_first_read = true;
    u32  m_security_ptr = 0;
    u8   m_eeprom_busy  = 0;

    // -- schedule --
    Scheduler m_sched;
    bool m_gpus_ready = false;   ///< false for the first frame after reset
    bool m_frame_done = false;
    bool m_in_frame   = false;
    bool m_in_cpu     = false;
    u64  m_frame_start = 0;
    s64  m_vblank_left = 0;
    u32  m_midi_count  = 0;
    u32  m_line        = 0;
    u32  m_flip_line   = 0;
    u64  m_last_sound  = 0;
    u32  m_sound_slice = 0;
    u64  m_frames      = 0;
    u64  m_unmapped    = 0;

    // -- publication --
    std::array<VideoFrame, 2> m_frames_out{};
    std::array<std::vector<u8>, 2> m_tilegen_copy;
    std::vector<TileGenWrite> m_tilegen_writes;
    bool m_tilegen_tracking = false;  ///< VRAM stores take the slow path and are journalled.
    u64 m_published_count = 0;
    unsigned m_published = 0;
};

}  // namespace sm2::hw::model3
