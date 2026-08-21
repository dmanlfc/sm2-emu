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
// See i8251.h.

#include "hw/i8251.h"

#include "core/log.h"

namespace sm2::hw {
namespace {

/// Command register bits.
enum : u8 {
    kCommandTxEnable      = 0x01,
    kCommandDtr           = 0x02,
    kCommandRxEnable      = 0x04,
    kCommandSendBreak     = 0x08,
    kCommandErrorReset    = 0x10,
    kCommandRts           = 0x20,
    kCommandInternalReset = 0x40,
    kCommandHunt          = 0x80,
};

/// Mode register: the low two bits select the baud rate factor, and zero means
/// synchronous mode.
constexpr u8 kModeBaudFactorMask = 0x03;

}  // namespace

void I8251::configure(u32 host_clock, u32 bit_rate, u32 bits_per_byte)
{
    // How long a byte occupies the wire, in host cycles. Rounded to the nearest
    // cycle; the error is under a part in a thousand and this only sets how often
    // the transmitter can accept the next byte.
    if (bit_rate == 0 || bits_per_byte == 0) {
        m_byte_cycles = 1;
        return;
    }
    const u64 cycles = (static_cast<u64>(host_clock) * bits_per_byte + bit_rate / 2)
                     / bit_rate;
    m_byte_cycles = cycles > 0 ? static_cast<u32>(cycles) : 1;
}

void I8251::reset()
{
    m_tx_holding_empty = true;
    m_tx_shift_empty   = true;
    m_rx_full          = false;
    m_errors           = 0;
    m_rx_data          = 0;
    m_tx_data          = 0;
    m_tx_remaining     = 0;
    m_expect_mode      = true;
    m_mode             = 0;
    m_command          = 0;
    m_counters         = Counters{};
}

bool I8251::tx_enabled() const
{
    return (m_command & kCommandTxEnable) != 0;
}

bool I8251::rx_enabled() const
{
    return (m_command & kCommandRxEnable) != 0;
}

u8 I8251::read(u32 reg)
{
    if (reg == 0) {
        // Reading the data register acknowledges the byte.
        m_rx_full = false;
        update_ready();
        return m_rx_data;
    }

    // The status register's ready bits are not gated by the command register's
    // enables; only the pins are.
    //
    // DSR reads asserted unless something drives the pin, which nothing on a
    // Model 2 board does: MAME's i8251 constructs with `m_dsr(1)` and its
    // `write_dsr` is never bound in any Model 2 machine configuration. A driver
    // that checks the far end is present before sending its next byte therefore
    // sees this bit set on hardware, and Zero Gunner and Pilot Kids both do --
    // with the bit clear they sent one byte and then polled this register
    // thirty-eight million times waiting for it.
    u8 status = m_errors;
    if (m_dsr) {
        status |= kStatusDsr;
    }
    if (m_tx_holding_empty) {
        status |= kStatusTxRdy;
    }
    if (m_tx_shift_empty) {
        status |= kStatusTxEmpty;
    }
    if (m_rx_full) {
        status |= kStatusRxRdy;
    }
    ++m_counters.status_reads;
    return status;
}

void I8251::write(u32 reg, u8 value)
{
    if (reg == 0) {
        if (!tx_enabled()) {
            if (!m_warned_tx_disabled) {
                m_warned_tx_disabled = true;
                SM2_WARN("i8251: a byte was written with the transmitter disabled");
            }
            return;
        }

        // Reported here rather than when the mode word arrives. The conventional
        // way to initialise this part is three dummy writes to the control
        // register before the internal reset, to put its mode-then-command state
        // machine somewhere known whatever it was doing before; those dummies are
        // usually zero, which looks exactly like a request for synchronous mode.
        // Only an actual transmission proves the mode was meant.
        if ((m_mode & kModeBaudFactorMask) == 0 && !m_warned_sync_mode) {
            m_warned_sync_mode = true;
            SM2_WARN("i8251: a byte was transmitted in synchronous mode, "
                     "which is not modelled");
        }

        // The real 8251 has a holding register behind the shift register. When
        // the shift register is empty, writing moves the byte straight to the
        // shift register and the holding register is free again immediately (TxRDY
        // goes back high). When the shift register is busy, the byte stays in the
        // holding register (TxRDY stays low) until the shift register finishes.
        ++m_counters.data_writes;
        m_tx_holding_empty = false;
        m_tx_shift_empty   = false;

        // If the shift register was idle, immediately move this byte there —
        // the holding register is free again.
        if (m_tx_remaining == 0) {
            m_tx_shift_data    = value;
            m_tx_remaining     = m_byte_cycles;
            m_tx_holding_empty = true;  // Holding register is free for next byte
        } else {
            // Shift register busy — park byte in holding register
            m_tx_data = value;
        }
        update_ready();
        return;
    }

    if (m_expect_mode) {
        m_mode        = value;
        m_expect_mode = false;
        return;
    }

    m_command = value;

    if ((value & kCommandInternalReset) != 0) {
        // Back to expecting a mode word. The transmitter and receiver go idle,
        // but the mode itself is not cleared, which matches the part.
        m_tx_holding_empty = true;
        m_tx_shift_empty   = true;
        m_rx_full          = false;
        m_tx_remaining     = 0;
        m_expect_mode      = true;
        update_ready();
        return;
    }

    if ((value & kCommandErrorReset) != 0) {
        m_errors = 0;
    }

    // Enabling the transmitter makes it ready without anything else having
    // changed, and that is how the host gets its first sound interrupt.
    update_ready();
}

void I8251::run(u32 host_cycles)
{
    if (m_tx_remaining == 0) {
        return;
    }

    if (host_cycles < m_tx_remaining) {
        m_tx_remaining -= host_cycles;
        return;
    }

    m_tx_remaining = 0;
    ++m_counters.bytes_sent;
    if (m_tx_handler) {
        m_tx_handler(m_tx_shift_data);
    }

    // If the holding register has another byte waiting (was written while the
    // shift register was busy), move it to the shift register now.
    if (!m_tx_holding_empty) {
        m_tx_shift_data    = m_tx_data;
        m_tx_remaining     = m_byte_cycles;
        m_tx_holding_empty = true;  // Holding register is now free
    } else {
        m_tx_shift_empty = true;
    }
    update_ready();
}

void I8251::write_rxd(u8 value)
{
    if (!rx_enabled()) {
        // A disabled receiver drops the byte. Not warned about, because a program
        // legitimately leaves the receiver off while it is only sending.
        return;
    }

    if (m_rx_full) {
        // The previous byte was never read, so it is lost.
        ++m_counters.overruns;
        m_errors |= kStatusOverrun;
    }

    m_rx_data = value;
    m_rx_full = true;
    ++m_counters.bytes_received;
    update_ready();
}

void I8251::update_ready()
{
    if (m_ready_handler) {
        m_ready_handler();
    }
}

}  // namespace sm2::hw
