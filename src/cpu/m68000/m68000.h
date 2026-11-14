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
// Motorola 68000 CPU core, as found on the Model 2 sound board.
//
// A thin wrapper over Musashi (Karl Stenerud, MIT), the one CPU core in Keisho
// not ported from MAME. The 68000 is fully documented, so nothing is lost by
// not tracking MAME's implementation.
//
// Musashi keeps its live registers in one global structure and reaches memory
// through global callbacks. To let more than one 68000 coexist -- the sound
// board's and the DSB2 music board's -- each instance owns a saved Musashi
// context and bus, and every entry point makes itself current (via
// m68k_get/set_context) before touching Musashi, saving whoever was current.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"

#include <string>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::cpu::m68000 {

/// The seven autovectored interrupt levels. The SCSP drives these directly.
enum {
    kIrqNone = 0,
    kIrqMin  = 1,
    kIrqMax  = 7,
};

class M68000 {
public:
    /// Binds the bus Musashi's global memory callbacks use while this instance
    /// is current.
    explicit M68000(Bus& bus);
    ~M68000();

    M68000(const M68000&)            = delete;
    M68000& operator=(const M68000&) = delete;

    /// Take the reset exception: load SSP from address 0 and PC from address 4.
    /// On the Model 2 sound board, call this after the machine seeds those bytes
    /// from the sound ROM.
    void reset();

    /// Execute until at least `cycles` have been consumed. Returns the number
    /// actually used, which can exceed the request because instructions are not
    /// interruptible.
    s32 run(s32 cycles);

    /// Charge `cycles` of bus wait state to the instruction being executed.
    /// Only meaningful from inside run(), i.e. from a Bus callback; ignored
    /// otherwise.
    void stall(s32 cycles);

    /// Assert or release one of the seven interrupt levels.
    ///
    /// Level-triggered: the state is held here and re-applied before every
    /// run(), because Musashi clears the level when it services the exception.
    void set_irq_line(int level, bool asserted);

    /// Set the raw IPL pins directly (0 = none), bypassing the set_irq_line mask.
    void set_irq_level(int level);

    /// Release every interrupt level at once. The SCSP does this when nothing is
    /// pending, without necessarily knowing which level it last asserted.
    void clear_irq_lines();

    // -- state, for logging and tests --------------------------------------
    // No stopped(): Musashi has no public accessor for its STOP state.

    [[nodiscard]] u32 pc() const;
    [[nodiscard]] u32 sr() const;
    [[nodiscard]] u32 sp() const;
    [[nodiscard]] u32 data_reg(int index) const;     ///< D0-D7
    [[nodiscard]] u32 address_reg(int index) const;  ///< A0-A7

    /// Total cycles consumed since construction.
    [[nodiscard]] u64 cycles() const { return m_total_cycles; }

    /// Begin tracing the next `count` instructions to `path`.
    void start_trace(int count, const char* path);

    /// One-line register dump.
    [[nodiscard]] std::string state_string() const;

    /// Save/restore the whole 68000 state, excluding the bus.
    void serialize(Archive& ar);

private:
    void apply_irq() const;

    /// Make this instance Musashi's current context, saving whoever held it.
    /// Every public entry point calls this first.
    void make_current() const;

    Bus*            m_bus = nullptr;
    mutable std::vector<u8> m_context;  ///< Saved Musashi state when not current.

    u8  m_irq_mask     = 0;  ///< Bit n set means level n is asserted.
    bool m_running     = false;
    s32  m_stalled     = 0;  ///< Wait-state cycles charged during this run().
    u64 m_total_cycles = 0;
};

}  // namespace sm2::cpu::m68000
