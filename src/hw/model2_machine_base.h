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
// The board-agnostic call surface every Model 2 machine variant presents to
// main.cpp, the debug dump functions (hw/model2_debug.cpp) and the renderer
// (render/vk/poly3d_pass.cpp).
//
// This interface is deliberately narrower than any one board's own class: it
// covers exactly what those three consumers call on a machine instance today,
// which is init/reset/run_frame, the input latch, the produced render list,
// NVRAM persistence, and the handful of video and diagnostic accessors the
// debug dumps and the 3D pass read. It does not cover the geometry coprocessor,
// the sound board or the sound link (Model2::copro()/sound()/uart()): those
// return board-specific hardware types (CoproTgp, Model2Sound, I8251) that do
// not generalise across boards -- Model 2B's coprocessor is a SHARC wrapped in
// a CoproSharc, Model 2C's is an MB86235 wrapped in a CoproTgpx4, and the
// original Model 2's sound board is YM3438 + MultiPCM, not 68000/SCSP. Code
// that needs those stays against the concrete board class.
//
// -- CPU state reporting across differing core types ------------------------
//
// main.cpp's boot-test report reads the main CPU's state through
// Model2::cpu(): faulted(), fault_message(), state_string(), halted() and
// instructions(). Every board this project will add runs a different core
// there (i960 on Model 2 and 2A, SHARC on Model 2B, MB86235 on Model 2C), so
// this interface needs some way to ask for that without knowing which.
//
// Two shapes were considered:
//
//   1. A type-erased CpuStatus snapshot struct (below), built by each board's
//      main_cpu_status() from whichever concrete core it owns.
//   2. A virtual base class every CPU core implements, so the interface could
//      just return a reference to it.
//
// (1) was chosen. cpu::i960::I960 and cpu::mb86233::MB86233 -- and every core
// this project ports the same way in later waves (SHARC, MB86235) -- are kept
// textually close to their MAME originals on purpose, so that upstream fixes
// stay diffable (see sm2_warnings_ported's rationale in src/CMakeLists.txt).
// Retrofitting a virtual interface onto them would touch code whose entire
// value is reading like upstream, for a feature (polymorphic status reporting)
// that only the boot-test's summary print actually needs. A type-erased struct
// costs one small allocation-bearing copy per report, which happens at most
// once a frame, and touches none of the ported cores.
#pragma once

#include "core/types.h"
#include "hw/geometrizer.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace sm2::hw {

class Model2Video;

/// Copy a set's shipped power-on image over battery-backed storage.
///
/// MAME's nvram_device and eeprom_base_device take their contents from a ROM
/// region named after the device when the set provides one, and the comment in
/// nvram_default says the region "always wins" over the all-ones default. Three
/// Model 2 sets depend on it. Manx TT ships an EEPROM image that selects DX or
/// Twin mode -- revision D picks DX and revision C picks Twin -- and Hanguk Pro
/// Yagu 98 ships both an EEPROM and a 16 KB backup-RAM image.
///
/// Ignoring it is not cosmetic: with a blank EEPROM, revision C comes up in DX
/// mode and sits on the motion-base self-test screen forever, which is exactly
/// where it was stuck while MAME went straight into a race.
///
/// A saved image from a previous run takes precedence over both, matching the
/// order MAME reads them in: nvram_default first, then the .nv file on top.
inline bool apply_default_image(std::span<const u8> shipped, std::span<u8> storage)
{
    if (shipped.size() != storage.size()) return false;
    std::copy(shipped.begin(), shipped.end(), storage.begin());
    return true;
}

/// State of every operator and player control, in hardware polarity.
///
/// All Model 2 digital inputs are active low, so an idle panel is all ones.
/// Until the input layer fills this from SDL it stays idle and the game sees a
/// machine nobody is touching.
struct Inputs {
    /// Coins, service, test and start. Bit 0 coin 1, bit 1 coin 2, bit 2 test,
    /// bit 3 service, bit 4 start 1, bit 5 start 2.
    u8 in0 = 0xff;

    /// Player 1: bits 0-3 buttons, bit 4 down, bit 5 up, bit 6 right, bit 7 left.
    u8 in1 = 0xff;

    /// Player 2, same layout.
    u8 in2 = 0xff;

    /// The CPU board's eight-position SW3.
    u8 dipswitches = 0xff;

    /// Eight analogue channels behind the I/O controller's mux. Unused by games
    /// with digital controls only.
    ///
    /// These are filled from the per-title channel assignment in
    /// rom::GameSpec::analog rather than by a fixed convention, because the
    /// channel a control sits on differs between titles. A machine with no
    /// analogue controls leaves them at their rest values.
    std::array<u8, 8> analog{};

    /// Gear selector, one bit per shifter position, active high. Only read by a
    /// title whose GameSpec sets `gearbox`.
    u8 gears = 0;

    /// Lightgun position, 10-bit, in the coordinate space the title calibrates
    /// for in rom::GameSpec::lightgun. These do not go through the analogue mux:
    /// the gun interface board is a serial device.
    u16 gun_p1x = 0;
    u16 gun_p1y = 0;
    u16 gun_p2x = 0;
    u16 gun_p2y = 0;
};

/// A type-erased snapshot of the machine's main CPU state.
///
/// See the file comment above for why this is a plain struct rather than a
/// shared virtual CPU base class.
struct CpuStatus {
    /// One-line register dump, as the concrete core's own state_string().
    std::string state_string;

    /// What went wrong, if faulted is true. Empty otherwise.
    std::string fault_message;

    /// Total instructions retired since reset.
    u64 instructions = 0;

    bool halted  = false;
    bool faulted = false;
};

/// The call surface a Model 2 board variant presents to the rest of the
/// program.
///
/// Every method here already exists on hw::Model2 (the Model 2A
/// implementation); this interface exists so that a machine factory keyed on
/// rom::Board can return one of several board variants and have main.cpp, the
/// debug dumps and the renderer work against all of them unchanged.
class Model2MachineBase {
public:
    virtual ~Model2MachineBase() = default;

    Model2MachineBase()                                     = default;
    Model2MachineBase(const Model2MachineBase&)            = delete;
    Model2MachineBase& operator=(const Model2MachineBase&) = delete;

    // -- lifecycle -----------------------------------------------------------

    /// Wire up the ROMs and bring the machine to its reset state.
    [[nodiscard]] virtual bool init(const rom::GameSpec& game, rom::RomSet roms) = 0;

    virtual void reset() = 0;

    /// Advance one video frame, including the vertical blank interrupt.
    virtual void run_frame() = 0;

    // -- input and output -----------------------------------------------------

    [[nodiscard]] virtual Inputs&       inputs()       = 0;
    [[nodiscard]] virtual const Inputs& inputs() const = 0;

    /// This frame's screen-space polygons, in drawing order.
    [[nodiscard]] virtual const RenderList& render_list() const = 0;

    // -- persistence -----------------------------------------------------------

    /// Directory for the NVRAM and EEPROM images. Loaded now, saved on request.
    virtual void set_nvram_directory(const std::string& directory) = 0;
    virtual void load_nvram()                                      = 0;
    virtual void save_nvram() const                                = 0;

    // -- main CPU status, for the boot-test report ------------------------

    [[nodiscard]] virtual CpuStatus main_cpu_status() const = 0;

    // -- video, for the renderer and the debug dumps ------------------------

    [[nodiscard]] virtual std::span<const u8>  tile_ram() const  = 0;
    [[nodiscard]] virtual std::span<const u8>  char_ram() const  = 0;
    [[nodiscard]] virtual std::span<const u16> palette_ram() const       = 0;
    [[nodiscard]] virtual std::span<const u16> colour_translate() const  = 0;
    [[nodiscard]] virtual std::span<const u8>  luma_ram() const          = 0;
    [[nodiscard]] virtual std::span<const u32> texture_ram(int sheet) const = 0;

    /// The display list buffer, which the coprocessor fills and the geometry
    /// engine reads.
    [[nodiscard]] virtual std::span<const u32> buffer_ram() const = 0;

    /// The host CPU's work RAM, for state comparison against MAME's own.
    [[nodiscard]] virtual std::span<const u8> work_ram() const = 0;

    /// Byte offset in the display list where the geometry engine starts each
    /// frame. Needed to walk the buffer the way the engine does.
    [[nodiscard]] virtual u32 geometry_read_start_address() const = 0;

    /// Render test mode, bit 0 of the render mode register at 0x10000000.
    ///
    /// The manual describes it as letting the host reach memories that are
    /// otherwise always being reloaded. In practice it stops the DSP drawing and
    /// clearing the framebuffer and shows one of the two framebuffer banks
    /// directly, which is how Last Bronx draws its title screen.
    [[nodiscard]] virtual bool render_test_mode() const = 0;

    /// The two framebuffer banks, xGGGGGRRRRRBBBBB at 512 pixels per line. Which
    /// one is shown alternates with the frame number.
    [[nodiscard]] virtual std::span<const u16> framebuffer(int bank) const = 0;

    /// Cleared by the renderer once it has taken a palette change into account.
    [[nodiscard]] virtual bool palette_dirty() const = 0;
    virtual void               clear_palette_dirty() = 0;

    /// Counters that advance whenever the 3D renderer's source data changes; see
    /// hw::Model2's own documentation of these for why they are counters rather
    /// than flags.
    [[nodiscard]] virtual u64 texture_generation() const = 0;
    [[nodiscard]] virtual u64 table_generation() const   = 0;

    [[nodiscard]] virtual Model2Video&       video()       = 0;
    [[nodiscard]] virtual const Model2Video& video() const = 0;

    /// Produce this frame's 2D output, refreshing the palette first if the
    /// program changed it.
    virtual void compose_video() = 0;

    // -- diagnostics -----------------------------------------------------------

    [[nodiscard]] virtual u64 cycles() const = 0;
    [[nodiscard]] virtual u64 frames() const = 0;

    /// The interrupt latch, for reporting which sources the program has armed.
    [[nodiscard]] virtual u32 intreq() const = 0;
    [[nodiscard]] virtual u32 intena() const = 0;

    /// Log every access that lands outside a mapped region. Off by default.
    virtual void set_log_unmapped(bool enable) = 0;

    virtual void log_unmapped_summary() const = 0;
    virtual void log_burst_summary() const    = 0;
};

}  // namespace sm2::hw
