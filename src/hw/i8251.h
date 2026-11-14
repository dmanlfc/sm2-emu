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
// Intel 8251A / NEC uPD71051 USART, as the Model 2 CPU board uses it: the serial
// link that carries sound commands to the sound board.
//
// Not ported from MAME, whose i8251 works a bit at a time. This is written from
// the programming model instead: whole bytes, keeping the byte period because
// the sound driver's command queue depends on the link's throughput.
//
// Not modelled: synchronous mode, parity, modem lines beyond the command
// register's bits, and mode-word framing (Model 2 always uses 8-N-1 at 31250
// baud). Use of any of these is reported.

#pragma once

#include "core/types.h"

#include <functional>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class I8251 {
public:
    /// A whole byte leaving the transmitter, one byte time after the CPU wrote it.
    using TxHandler = std::function<void(u8 value)>;

    /// Called whenever the TxRDY or RxRDY pins are recomputed.
    ///
    /// Deliberately not edge-triggered: the consumer reads txrdy() and rxrdy()
    /// itself, because the Model 2 sound interrupt depends on the level of both
    /// and enabling the transmitter alone must be able to raise it. Getting that
    /// wrong deadlocks the link.
    using ReadyHandler = std::function<void()>;

    void set_tx_handler(TxHandler handler) { m_tx_handler = std::move(handler); }
    void set_ready_handler(ReadyHandler handler) { m_ready_handler = std::move(handler); }

    /// `bit_rate` in bits per second and `bits_per_byte` including the start and
    /// stop bits, used together to work out how long a byte occupies the wire.
    /// `host_clock` is the clock run() is counted in.
    void configure(u32 host_clock, u32 bit_rate, u32 bits_per_byte);

    void reset();

    /// Register 0 is the data register, register 1 the status/control register.
    [[nodiscard]] u8 read(u32 reg);
    void write(u32 reg, u8 value);

    /// Advance the transmitter by `host_cycles` of the host clock.
    void run(u32 host_cycles);

    /// The TxRDY and RxRDY pins, which unlike the corresponding status register
    /// bits are gated by the command register's enables.
    [[nodiscard]] bool txrdy() const { return m_tx_holding_empty && tx_enabled(); }
    [[nodiscard]] bool rxrdy() const { return m_rx_full && rx_enabled(); }

    /// A byte arriving now would be latched, not dropped. Lets a sender pace
    /// itself to the reader (the Model 3 sound board feeding the DSB).
    [[nodiscard]] bool can_receive() const { return rx_enabled() && !m_rx_full; }

    /// A byte arriving from the other end of the link.
    void write_rxd(u8 value);

    /// The DSR pin, inverted as in MAME's `write_dsr`. No Model 2 board drives
    /// it, so it stays asserted.
    void write_dsr(bool level) { m_dsr = !level; }

    // -- inspection ---------------------------------------------------------

    struct Counters {
        u64 bytes_sent     = 0;
        u64 bytes_received = 0;
        u64 overruns       = 0;
        u64 status_reads   = 0;
        u64 data_writes    = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

    /// Save/restore the register and transmitter state. The callbacks are
    /// excluded (re-bound by the owner).
    void serialize(Archive& ar);

private:
    /// Status register bits, as the programming model defines them.
    enum : u8 {
        kStatusTxRdy   = 0x01,
        kStatusRxRdy   = 0x02,
        kStatusTxEmpty = 0x04,
        kStatusParity  = 0x08,
        kStatusOverrun = 0x10,
        kStatusFraming = 0x20,
        kStatusSynDet  = 0x40,
        kStatusDsr     = 0x80,
    };

    [[nodiscard]] bool tx_enabled() const;
    [[nodiscard]] bool rx_enabled() const;

    /// Tell the consumer the ready pins may have moved.
    void update_ready();

    TxHandler    m_tx_handler;
    ReadyHandler m_ready_handler;

    /// Host cycles one byte occupies the wire.
    u32 m_byte_cycles = 1;

    /// The transmitter accepted the CPU's byte and has room for another. Drives
    /// the status register's TxRDY bit ungated, and the TxRDY pin through the
    /// command register's enable.
    bool m_tx_holding_empty = true;
    bool m_tx_shift_empty   = true;
    bool m_rx_full          = false;

    /// Data set ready, as the status register reports it. Comes up asserted and
    /// is not cleared by a reset, matching MAME's constructor initialiser.
    bool m_dsr              = true;

    /// Errors, which are sticky until the command register's error reset.
    u8 m_errors = 0;

    u8 m_rx_data = 0;

    u8  m_tx_data      = 0;  ///< Byte in the holding register (or shift register)
    u8  m_tx_shift_data = 0; ///< Byte currently being shifted out
    u32 m_tx_remaining = 0;

    /// The 8251 takes a mode word after a reset and command words after that.
    bool m_expect_mode = true;
    u8   m_mode        = 0;
    u8   m_command     = 0;

    bool m_warned_tx_disabled = false;
    bool m_warned_sync_mode   = false;

    Counters m_counters;
};

}  // namespace sm2::hw
