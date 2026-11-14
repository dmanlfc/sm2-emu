// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// Interfaces for the boards around the Model 3 CPU board; any may be absent.
// All are called on the emulation thread only, in a fixed order.
#pragma once

#include "core/types.h"
#include "cpu/ppc/ppc.h"
#include "hw/machine.h"

#include <memory>
#include <span>

namespace sm2 {
class Archive;
}

namespace sm2::cpu {
class PageMap;
}

namespace sm2::rom {
struct GameSpec;
class RomSet;
}

namespace sm2::hw {
class CommBoard;
class SoundBoard;
}

namespace sm2::hw::model3 {

// -- main CPU ---------------------------------------------------------------------

class MainCpu {
public:
    virtual ~MainCpu() = default;

    virtual void init(const cpu::ppc::PpcConfig& config, cpu::PageMap& pages,
                      cpu::ppc::PpcBus& bus)                     = 0;
    virtual void reset()                                         = 0;
    /// Run until at least `cycles` have passed or end_timeslice() was called;
    /// returns the cycles actually run.
    virtual s64  run(s64 cycles)                                 = 0;
    virtual void end_timeslice()                                 = 0;
    virtual void set_irq(bool asserted)                          = 0;
    virtual void set_exec_mode(cpu::ppc::ExecMode mode)          = 0;
    [[nodiscard]] virtual u64       total_cycles() const         = 0;
    [[nodiscard]] virtual CpuStatus status() const               = 0;
    virtual void serialize(Archive& ar)                          = 0;
    virtual void invalidate(u32 address, u32 size)               = 0;
};

/// nullptr without SM2_MODEL3_HAVE_PPC.
[[nodiscard]] std::unique_ptr<MainCpu> create_ppc_cpu();

// -- sound board ------------------------------------------------------------------

/// The sound board as the CPU board sees it. The machine owns the MIDI
/// interrupt; run() is called timeslices_per_frame() times, evenly spaced.
class SoundPort {
public:
    virtual ~SoundPort() = default;

    /// Once, from Model3::init.
    virtual void attach(u32 ppc_hz) { (void)ppc_hz; }
    virtual void reset() = 0;

    virtual void write_midi(u8 data) = 0;
    virtual void write_control(u8 data) { (void)data; }
    [[nodiscard]] virtual u8 midi_read(u32 offset) { return offset == 4 ? 0x83 : 0x00; }

    virtual void run(u64 ppc_cycles) = 0;
    [[nodiscard]] virtual u32 timeslices_per_frame() const { return 1; }

    virtual void serialize(Archive& ar) = 0;

    /// May be nullptr.
    [[nodiscard]] virtual SoundBoard* sound_board() = 0;

    /// The MPEG music board's level in percent.
    virtual void set_music_volume(u32 percent) { (void)percent; }
};

/// nullptr without SM2_MODEL3_HAVE_SOUND.
[[nodiscard]] std::unique_ptr<SoundPort> create_sound_port(const rom::GameSpec& game,
                                                           const rom::RomSet& roms);

// -- drive board ------------------------------------------------------------------

/// Drive board: writes 0xF0040010, reads ANDed into 0xF004000C.
class DriveBoardPort {
public:
    virtual ~DriveBoardPort() = default;
    virtual void reset() = 0;
    virtual void write(u8 data) = 0;
    [[nodiscard]] virtual u8 read() = 0;
    /// Billboards have no inputs, so 0xF004000C does not read them.
    [[nodiscard]] virtual bool is_billboard() const { return false; }
    /// Once per frame, after the main board.
    virtual void run_frame() = 0;
    virtual void serialize(Archive& ar) = 0;
};

/// nullptr when the game has none, or a wheel/billboard lacks its program.
[[nodiscard]] std::unique_ptr<DriveBoardPort> create_drive_board(const rom::GameSpec& game,
                                                                 const rom::RomSet& roms,
                                                                 const Inputs& inputs,
                                                                 Machine::BoardOutputs& outputs);

// -- network board -----------------------------------------------------------------

/// Step 1.5+ network board. Addresses arrive already swizzled.
class NetBoardPort {
public:
    virtual ~NetBoardPort() = default;
    virtual void reset() = 0;
    [[nodiscard]] virtual u8  read_comm_ram8(u32 addr)  = 0;
    [[nodiscard]] virtual u16 read_comm_ram16(u32 addr) = 0;
    [[nodiscard]] virtual u32 read_comm_ram32(u32 addr) = 0;
    virtual void write_comm_ram8(u32 addr, u8 data)   = 0;
    virtual void write_comm_ram16(u32 addr, u16 data) = 0;
    virtual void write_comm_ram32(u32 addr, u32 data) = 0;
    [[nodiscard]] virtual u16 read_io_register(u32 reg) = 0;
    virtual void write_io_register(u32 reg, u16 data)   = 0;
    virtual void run_frame()                            = 0;
    virtual void serialize(Archive& ar)                 = 0;
    /// Backs Machine::comm(), or nullptr.
    [[nodiscard]] virtual CommBoard* comm() { return nullptr; }
};

/// `net_ram` is the machine's 64 KB window at 0xC0020000.
[[nodiscard]] std::unique_ptr<NetBoardPort> create_net_board(const rom::GameSpec& game,
                                                             std::span<u8> net_ram);

}  // namespace sm2::hw::model3
