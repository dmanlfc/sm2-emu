//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// The points where the boards around the Model 3 CPU board plug into the
// machine: the main CPU, the sound board (with its optional DSB), the drive
// board and the network board. Each is a small interface so the machine builds
// and is testable without any of them; hw::model3::Model3 runs with whatever is
// attached and treats an absent board as a dummy one.
//
// None of these is ever called from another thread: the machine calls them
// from inside run_frame() (or reset / serialize), in a fixed order, so
// emulation stays deterministic.
#pragma once

#include "core/types.h"
#include "cpu/ppc/ppc.h"
#include "hw/machine.h"

#include <memory>

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
class SoundBoard;
}

namespace sm2::hw::model3 {

// -- main CPU ---------------------------------------------------------------------

/// The machine's view of its PowerPC: cpu::ppc::Ppc's public surface.
/// model3_cpu_ppc.cpp adapts the real core.
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

/// The real PowerPC, when the build has it (SM2_MODEL3_HAVE_PPC); nullptr
/// otherwise. Defined in model3_cpu_ppc.cpp.
[[nodiscard]] std::unique_ptr<MainCpu> create_ppc_cpu();

// -- sound board ------------------------------------------------------------------

/// The Model 3 sound board (68000, two SCSPs, optional DSB1/DSB2), as the CPU
/// board sees it, through these touch points:
///
///   - 0xF0080000 (byte write): MIDI data to the 68000 side. The machine
///     calls write_midi() and then deasserts IRQ 0x40 itself.
///   - 0xF0080004 (byte write): MIDI control. Bit 0x20 enables the periodic
///     MIDI interrupt the *machine* generates during vblank (IRQ 0x40); the
///     sound board is not involved. The machine keeps this register.
///   - Reads of 0xF0080000 / 0xF0080004 return 0x00 / 0x83 (Magical Truck's
///     region check). midi_read() may override that.
///   - Time: run() advances the board by a number of PowerPC cycles. The
///     machine calls it timeslices_per_frame() times per frame at evenly spaced
///     points of the frame's schedule; by default once, at the end of the
///     frame.
class SoundPort {
public:
    virtual ~SoundPort() = default;

    /// Called once from Model3::init with the PowerPC clock run() counts in.
    virtual void attach(u32 ppc_hz) { (void)ppc_hz; }
    virtual void reset() = 0;

    virtual void write_midi(u8 data) = 0;
    /// The control port's value, mirrored to the board (the machine keeps its
    /// own copy for the interrupt schedule).
    virtual void write_control(u8 data) { (void)data; }
    [[nodiscard]] virtual u8 midi_read(u32 offset) { return offset == 4 ? 0x83 : 0x00; }

    virtual void run(u64 ppc_cycles) = 0;
    [[nodiscard]] virtual u32 timeslices_per_frame() const { return 1; }

    virtual void serialize(Archive& ar) = 0;

    /// Audio output, for main.cpp. May be nullptr.
    [[nodiscard]] virtual SoundBoard* sound_board() = 0;
};

/// The sound board for `game`, when the build has one
/// (SM2_MODEL3_HAVE_SOUND); nullptr otherwise. Defined by the sound board's
/// glue, model3_sound_port.cpp.
[[nodiscard]] std::unique_ptr<SoundPort> create_sound_port(const rom::GameSpec& game,
                                                           const rom::RomSet& roms);

// -- drive board ------------------------------------------------------------------

/// Force-feedback / billboard / ski board on the I/O port at 0xF0040010
/// (write) and 0xF004000C (read, ANDed into the second game-input byte unless
/// the board is a billboard).
class DriveBoardPort {
public:
    virtual ~DriveBoardPort() = default;
    virtual void reset() = 0;
    virtual void write(u8 data) = 0;
    [[nodiscard]] virtual u8 read() = 0;
    /// Billboards have no inputs, so 0xF004000C does not read them.
    [[nodiscard]] virtual bool is_billboard() const { return false; }
    /// Called once per frame after the main board.
    virtual void run_frame() = 0;
    virtual void serialize(Archive& ar) = 0;
};

// -- network board -----------------------------------------------------------------

/// The Step 1.5+ network board at 0xC0000000 (shared RAM 0xC0000000-FFFF, I/O
/// registers 0xC0010000-01FF, program RAM 0xC0020000-3FFFF). Addresses arrive
/// already swizzled; the machine owns the 64 KB net RAM window itself.
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
};

}  // namespace sm2::hw::model3
