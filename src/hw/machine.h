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
// The call surface every emulated machine presents to main.cpp. Model 1/2
// specifics live in Model2MachineBase; Model 3 derives from this directly.
#pragma once

#include "core/log.h"
#include "core/types.h"
#include "hw/save_state_io.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class CommBoard;
class Model2MachineBase;

namespace model3 {
class Model3;
}

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
/// mode and sits on the motion-base self-test screen forever.
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

    /// An extra port some cabinets add on the I/O controller's port E. Only
    /// Power Sled uses it: bit 1 is its "Cancel Network Check" button.
    u8 in3 = 0xff;

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
/// A plain struct rather than a shared virtual CPU base class: the V60, i960
/// and PowerPC cores have nothing in common beyond what is copied out here.
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

/// Which family a machine belongs to, for callers that need its own interface.
enum class MachineFamily : u8 {
    Model2,  ///< Model 1 and every Model 2 board: see Model2MachineBase.
    Model3,  ///< Every Model 3 stepping: see hw::model3::Model3.
};

/// The board-agnostic machine interface. See the file comment.
class Machine {
public:
    virtual ~Machine() = default;

    Machine()                          = default;
    Machine(const Machine&)            = delete;
    Machine& operator=(const Machine&) = delete;

    [[nodiscard]] virtual MachineFamily family() const = 0;

    /// This machine as its family's own interface, or nullptr.
    [[nodiscard]] virtual Model2MachineBase*       as_model2() { return nullptr; }
    [[nodiscard]] virtual const Model2MachineBase* as_model2() const { return nullptr; }
    [[nodiscard]] virtual model3::Model3*          as_model3() { return nullptr; }
    [[nodiscard]] virtual const model3::Model3*    as_model3() const { return nullptr; }

    // -- lifecycle -----------------------------------------------------------

    /// Wire up the ROMs and bring the machine to its reset state.
    [[nodiscard]] virtual bool init(const rom::GameSpec& game, rom::RomSet roms) = 0;

    virtual void reset() = 0;

    /// Advance one video frame, including the vertical blank interrupt.
    virtual void run_frame() = 0;

    // -- input and output -----------------------------------------------------

    [[nodiscard]] virtual Inputs&       inputs()       = 0;
    [[nodiscard]] virtual const Inputs& inputs() const = 0;

    /// The communication (link) board. main.cpp injects a LAN transport here
    /// when linking is on; the OSD reads its link state.
    [[nodiscard]] virtual CommBoard&       comm()       = 0;
    [[nodiscard]] virtual const CommBoard& comm() const = 0;

    // -- per-core timing, for --profile --------------------------------------
    //
    // run_frame() interleaves the main CPU, any coprocessor and the sound
    // board, so main.cpp sees one call and cannot time them apart. Each board
    // accumulates its own nanoseconds below, gated on profiling being armed
    // (these run thousands of times a frame). "i960" means the main CPU.

    /// Arm or disarm the per-core accumulators. main.cpp turns it on under
    /// --profile only.
    void set_core_profiling(bool enable) { m_core_profile.enabled = enable; }

    /// Nanoseconds the last run_frame() spent in each core. Zero when profiling
    /// was not armed for that frame.
    [[nodiscard]] u64 i960_stage_nanoseconds() const { return m_core_profile.i960_ns; }
    [[nodiscard]] u64 copro_stage_nanoseconds() const { return m_core_profile.copro_ns; }
    [[nodiscard]] u64 sound_stage_nanoseconds() const { return m_core_profile.sound_ns; }

    // -- persistence -----------------------------------------------------------

    /// Directory for the NVRAM and EEPROM images. Loaded now, saved on request.
    virtual void set_nvram_directory(const std::string& directory) = 0;
    virtual void load_nvram()                                      = 0;
    virtual void save_nvram() const                                = 0;

    /// Snapshot the whole machine into `out` / restore it from `data`. Both
    /// must be called only between frames (see run_frame). Default:
    /// unsupported, so a board that has not implemented save-states yet returns
    /// false rather than failing to compile. Return false on any error, leaving
    /// a running machine untouched on a failed load.
    [[nodiscard]] virtual bool save_state_data(std::vector<u8>& out) const
    {
        (void)out;
        return false;
    }
    [[nodiscard]] virtual bool load_state_data(std::span<const u8> data)
    {
        (void)data;
        return false;
    }

    /// The same through a state file at `path`.
    [[nodiscard]] bool save_state(const std::string& path) const
    {
        std::vector<u8> data;
        return save_state_data(data) && write_state_file(path, data);
    }
    [[nodiscard]] bool load_state(const std::string& path)
    {
        std::vector<u8> data;
        if (!read_state_file(path, data) || !load_state_data(data)) {
            return false;
        }
        SM2_INFO("save-state: loaded '%s'", path.c_str());
        return true;
    }

    // -- main CPU status, for the boot-test report ------------------------

    [[nodiscard]] virtual CpuStatus main_cpu_status() const = 0;

    // -- diagnostics -----------------------------------------------------------

    [[nodiscard]] virtual u64 cycles() const = 0;
    [[nodiscard]] virtual u64 frames() const = 0;

    /// Bytes the game wrote to its force-feedback drive board, oldest first.
    struct DriveBoardWrites {
        std::array<u8, 16> bytes{};
        usize              count = 0;

        [[nodiscard]] std::span<const u8> view() const { return {bytes.data(), count}; }
    };

    /// The drive-board writes since the last call, which empties the list. A
    /// frame can hold several (Indy 500 sends an effect then two parameters), so
    /// sampling only the last byte would miss the effect.
    [[nodiscard]] DriveBoardWrites take_drive_board_writes()
    {
        const DriveBoardWrites writes = m_drive_board_writes;
        m_drive_board_writes.count    = 0;
        return writes;
    }

    /// The last value the game wrote to its lamp/coin-counter latch.
    [[nodiscard]] u8 lamp_latch() const { return m_lamp_latch; }

    /// Outputs of an emulated drive board/billboard (Model 3's Z80 boards).
    /// Forces: `constant` is -1..1 (positive = right), the rest 0..1.
    struct BoardOutputs {
        bool  force_feedback = false;  ///< a board is driving the forces below
        float constant       = 0.0f;
        float constant_y     = 0.0f;  ///< joystick Y axis, positive up
        float self_center    = 0.0f;
        float friction       = 0.0f;
        float vibrate        = 0.0f;

        bool              billboard = false;
        std::array<u8, 5> bill{};  ///< P1 digits 1-2, P2 digits 1-2, lamps (0 = segment lit)
    };
    [[nodiscard]] const BoardOutputs& board_outputs() const { return m_board_outputs; }

    /// Log every access that lands outside a mapped region. Off by default.
    virtual void set_log_unmapped(bool enable) = 0;

protected:
    /// Per-core wall-clock accumulators, filled by each board's run_frame().
    /// See set_core_profiling() above for why this is gated rather than always
    /// measured.
    struct CoreProfile {
        bool enabled  = false;
        u64  i960_ns  = 0;
        u64  copro_ns = 0;
        u64  sound_ns = 0;
    };
    CoreProfile m_core_profile;

    /// Called by each board's drive_board_write. When full, the newest byte
    /// replaces the last slot.
    void record_drive_board_write(u8 value)
    {
        auto& writes = m_drive_board_writes;
        if (writes.count < writes.bytes.size()) {
            writes.bytes[writes.count++] = value;
        } else {
            writes.bytes.back() = value;
        }
    }

    DriveBoardWrites m_drive_board_writes;

    /// The steering pot as the game reads it, for a drive board's answer.
    [[nodiscard]] u8 steering_value(const rom::GameSpec& game) const
    {
        for (usize channel = 0; channel < game.analog.size(); ++channel) {
            if (game.analog[channel].control == rom::AnalogControl::Steer) {
                return inputs().analog[channel];
            }
        }
        return 0x80;
    }

    /// Called by each board's lamp_output_w.
    void record_lamp_latch(u8 value) { m_lamp_latch = value; }

    u8 m_lamp_latch = 0;

    BoardOutputs m_board_outputs;

    /// Zero the per-frame accumulators. Called at the top of run_frame().
    void reset_core_profile()
    {
        m_core_profile.i960_ns  = 0;
        m_core_profile.copro_ns = 0;
        m_core_profile.sound_ns = 0;
    }

    /// RAII accumulator: adds its lifetime in nanoseconds to `into` on exit,
    /// but only when profiling is armed -- otherwise the clock is never read.
    class CoreScope {
    public:
        CoreScope(const CoreProfile& profile, u64& into)
            : m_into(profile.enabled ? &into : nullptr)
        {
            if (m_into != nullptr) {
                m_start = std::chrono::steady_clock::now();
            }
        }
        ~CoreScope()
        {
            if (m_into != nullptr) {
                *m_into += static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - m_start)
                        .count());
            }
        }
        CoreScope(const CoreScope&)            = delete;
        CoreScope& operator=(const CoreScope&) = delete;

    private:
        u64*                                  m_into = nullptr;
        std::chrono::steady_clock::time_point m_start{};
    };
};

}  // namespace sm2::hw
