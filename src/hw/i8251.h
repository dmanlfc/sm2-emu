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
// Intel 8251A / NEC uPD71051 USART, as the Model 2 CPU board uses it: the serial
// link that carries sound commands to the sound board.
//
// Not ported from MAME. MAME's i8251 is built on device_serial_interface and
// works a bit at a time, and the SCSP at the other end is too; reproducing that
// would mean bringing in the bit-timing framework to emulate a wire whose
// individual bits nothing on either side can observe. Like the 68000, the 8251 is
// a fully documented commodity part, so there is no reverse-engineering insight to
// preserve, and this is written from its programming model instead: whole bytes,
// with the byte period kept because throughput is the part the sound driver's
// command queue actually depends on.
//
// What is deliberately not modelled: synchronous mode, parity, the modem control
// lines beyond the command register's own bits, and the framing the mode word
// configures. Model 2 sets 8-N-1 at 31250 baud and never changes it, and the SCSP
// is wired the same way by construction, so the framing has no observable effect.
// Anything trying to use the parts left out is reported.

#pragma once

#include "core/types.h"

#include <functional>

namespace sm2::hw {

class I8251 {
public:
    /// A whole byte leaving the transmitter, one byte time after the CPU wrote it.
    using TxHandler = std::function<void(u8 value)>;

    /// Called whenever the TxRDY or RxRDY pins are recomputed, which is what
    /// MAME's update_tx_ready and update_rx_ready do with their devcb lines.
    ///
    /// Deliberately not edge-triggered. The consumer is expected to look at
    /// txrdy() and rxrdy() itself, because the Model 2 CPU board's sound interrupt
    /// depends on the level of both, and because enabling the transmitter has to
    /// be able to raise the interrupt with nothing else having changed. Getting
    /// that wrong deadlocks the link: the sound program never gets a command
    /// because the host never gets the interrupt that would make it send one.
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

    /// A byte arriving from the other end of the link.
    void write_rxd(u8 value);

    // -- inspection ---------------------------------------------------------

    struct Counters {
        u64 bytes_sent     = 0;
        u64 bytes_received = 0;
        u64 overruns       = 0;
        u64 status_reads   = 0;
        u64 data_writes    = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

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

    /// Errors, which are sticky until the command register's error reset.
    u8 m_errors = 0;

    u8 m_rx_data = 0;

    /// The byte being shifted out, and how much of its time is left.
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
