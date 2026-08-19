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
// Motorola 68000 CPU core, as found on the Model 2 sound board.
//
// This is a thin wrapper over Musashi (Karl Stenerud, MIT), the one piece of
// CPU emulation in sm2-emu that is not ported from MAME. Musashi is a
// self-contained C library designed to be driven through memory callbacks,
// where MAME's 68000 is either generated from a table into two hundred thousand
// lines per variant or is itself a Musashi fork; both are woven into MAME's
// device and address-space framework. The 68000 is also a fully documented
// commodity part, so unlike the geometry engine or the SCSP there is no
// reverse-engineering insight in one implementation over another and nothing is
// lost by not tracking MAME's.
//
// Musashi reaches memory through global functions, so it supports exactly one
// instance per process. Model 2 has one sound 68000, so that is not a
// constraint here, but the constructor asserts it rather than letting a second
// instance silently steal the bus.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"

#include <string>

namespace sm2::cpu::m68000 {

/// The seven autovectored interrupt levels. The SCSP drives these directly.
enum {
    kIrqNone = 0,
    kIrqMin  = 1,
    kIrqMax  = 7,
};

class M68000 {
public:
    /// Binds the bus Musashi's global memory callbacks will forward to.
    ///
    /// Only one instance may exist at a time; constructing a second is a
    /// programming error and aborts.
    explicit M68000(Bus& bus);
    ~M68000();

    M68000(const M68000&)            = delete;
    M68000& operator=(const M68000&) = delete;

    /// Take the reset exception: load SSP from address 0 and PC from address 4.
    ///
    /// On the Model 2 sound board those first 16 bytes of RAM are seeded from
    /// the start of the sound ROM by the machine before this is called, so this
    /// must run after that copy.
    void reset();

    /// Execute until at least `cycles` have been consumed. Returns the number
    /// actually used, which can exceed the request because instructions are not
    /// interruptible.
    s32 run(s32 cycles);

    /// Assert or release one of the seven interrupt levels.
    ///
    /// Level-triggered: the state is held here and re-applied before every
    /// run(), because Musashi clears the level when it services the exception.
    /// The SCSP asserts and clears explicitly, so holding the level is what
    /// matches the hardware; without the re-apply an interrupt that stays
    /// asserted would only ever be taken once.
    void set_irq_line(int level, bool asserted);

    /// Release every interrupt level at once.
    ///
    /// The SCSP asks for this when nothing is pending: it stops driving the three
    /// IPL pins rather than clearing one level at a time, and it does not
    /// necessarily remember which level it last asserted.
    void clear_irq_lines();

    // -- state, for logging and the headless sound bring-up test ------------
    //
    // Musashi's STOP state has no public accessor and reaching into m68ki_cpu
    // for it would mean including its private header, so there is no stopped()
    // here. A sound program wedged in STOP shows up as a PC that does not move,
    // which the frame counters report anyway.

    [[nodiscard]] u32 pc() const;
    [[nodiscard]] u32 sr() const;
    [[nodiscard]] u32 sp() const;
    [[nodiscard]] u32 data_reg(int index) const;     ///< D0-D7
    [[nodiscard]] u32 address_reg(int index) const;  ///< A0-A7

    /// Total cycles consumed since construction.
    [[nodiscard]] u64 cycles() const { return m_total_cycles; }

    /// One-line register dump.
    [[nodiscard]] std::string state_string() const;

private:
    void apply_irq() const;

    // The bus itself is not held here: Musashi's memory callbacks are free
    // functions, so the binding lives at file scope in m68000.cpp and this class
    // only owns the interrupt state.
    u8  m_irq_mask     = 0;  ///< Bit n set means level n is asserted.
    u64 m_total_cycles = 0;
};

}  // namespace sm2::cpu::m68000
