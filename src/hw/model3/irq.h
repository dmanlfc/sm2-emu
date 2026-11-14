// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 interrupt controller.
//
// Derived from Supermodel's Src/Model3/IRQ.cpp and IRQ.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c) 2011
// Bart Trzynadlowski, Nik Henson.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The PowerPC IRQ line is a callback the machine installs. Its level is
// tracked so it can be driven again after a state load.
#pragma once

#include "core/archive.h"
#include "core/types.h"

#include <functional>
#include <utility>

namespace sm2::hw::model3 {

/// Bits above 0xFF (53C810, Real3D DMA) ignore the enable register.
class IrqController {
public:
    static constexpr u32 kVblank   = 0x02;
    static constexpr u32 kMidi     = 0x40;
    static constexpr u32 kNmiDma   = 0x100;  ///< SCSI and Real3D DMA (not maskable).

    using LineFn = std::function<void(bool)>;

    void set_line_callback(LineFn fn) { m_line_fn = std::move(fn); }

    void assert_irq(u32 bits)
    {
        m_state |= bits;
        if ((m_state & m_enable) != 0 || (m_state & ~0xFFu) != 0) {
            drive(true);
        }
    }

    void deassert_irq(u32 bits)
    {
        m_state &= ~bits;
        if ((m_state & m_enable) == 0 && (m_state & ~0xFFu) == 0) {
            drive(false);
        }
    }

    /// The line is not re-evaluated here: an interrupt enabled while already
    /// pending waits for the next assert.
    void write_enable(u8 data) { m_enable = data; }

    [[nodiscard]] u8 read_enable() const { return static_cast<u8>(m_enable & 0xFF); }
    [[nodiscard]] u8 read_state() const { return static_cast<u8>(m_state & 0xFF); }
    [[nodiscard]] u32 raw_state() const { return m_state; }
    [[nodiscard]] bool line() const { return m_line; }

    void reset()
    {
        m_enable = 0;
        m_state  = 0;
        drive(false);
    }

    void serialize(Archive& ar)
    {
        ar.raw(m_enable);
        ar.raw(m_state);
        ar.raw(m_line);
        if (ar.loading() && m_line_fn) m_line_fn(m_line);
    }

private:
    void drive(bool level)
    {
        m_line = level;
        if (m_line_fn) m_line_fn(level);
    }

    u32    m_enable = 0;
    u32    m_state  = 0;
    bool   m_line   = false;
    LineFn m_line_fn;
};

}  // namespace sm2::hw::model3
