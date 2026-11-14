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
// See i8251.h.

#include "hw/i8251.h"

#include "core/archive.h"
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
    // How long a byte occupies the wire, in host cycles, rounded to the nearest.
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

void I8251::serialize(Archive& ar)
{
    ar.raw(m_byte_cycles);
    ar.raw(m_tx_holding_empty);
    ar.raw(m_tx_shift_empty);
    ar.raw(m_rx_full);
    ar.raw(m_dsr);
    ar.raw(m_errors);
    ar.raw(m_rx_data);
    ar.raw(m_tx_data);
    ar.raw(m_tx_shift_data);
    ar.raw(m_tx_remaining);
    ar.raw(m_expect_mode);
    ar.raw(m_mode);
    ar.raw(m_command);
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

    // The ready bits are not gated by the command register's enables. DSR reads
    // asserted, as nothing on a Model 2 board drives the pin; Zero Gunner and
    // Pilot Kids wait on it before each byte.
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

        // Reported here rather than at the mode write: the usual init sequence of
        // zero dummy writes looks like a request for synchronous mode.
        if ((m_mode & kModeBaudFactorMask) == 0 && !m_warned_sync_mode) {
            m_warned_sync_mode = true;
            SM2_WARN("i8251: a byte was transmitted in synchronous mode, "
                     "which is not modelled");
        }

        // A holding register sits behind the shift register. With the shifter
        // idle the byte moves straight through and TxRDY stays high; otherwise
        // it waits in the holding register with TxRDY low.
        ++m_counters.data_writes;
        m_tx_holding_empty = false;
        m_tx_shift_empty   = false;

        if (m_tx_remaining == 0) {
            m_tx_shift_data    = value;
            m_tx_remaining     = m_byte_cycles;
            m_tx_holding_empty = true;
        } else {
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

    // A byte parked in the holding register moves to the shift register now.
    if (!m_tx_holding_empty) {
        m_tx_shift_data    = m_tx_data;
        m_tx_remaining     = m_byte_cycles;
        m_tx_holding_empty = true;
    } else {
        m_tx_shift_empty = true;
    }
    update_ready();
}

void I8251::write_rxd(u8 value)
{
    if (!rx_enabled()) {
        // Not warned about: a program may leave the receiver off while sending.
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
