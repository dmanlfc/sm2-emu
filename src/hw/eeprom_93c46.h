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
#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <string>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// 93C46 serial EEPROM, 64 words of 16 bits.
///
/// Model 2 keeps its operator settings here, bit-banged through the 315-5649's
/// port A with the data line read back on port B. Written from the device's
/// serial protocol rather than ported.
class Eeprom93c46 {
public:
    static constexpr u32 kWordCount = 64;
    static constexpr u32 kByteCount = kWordCount * 2;

    Eeprom93c46();

    /// Erase to all ones, matching a blank device.
    void reset();

    // -- pin interface -----------------------------------------------------

    void set_cs(bool level);
    void set_clk(bool level);
    void set_di(bool level);

    [[nodiscard]] bool data_out() const { return m_do; }
    [[nodiscard]] bool reading() const { return m_state == State::Reading; }

    // -- persistence -------------------------------------------------------

    /// Contents as bytes, little-endian within each word.
    [[nodiscard]] std::span<const u8> bytes() const;
    [[nodiscard]] std::span<u8>       bytes();

    [[nodiscard]] bool load(const std::string& path);
    [[nodiscard]] bool save(const std::string& path) const;

    /// True when a word has been written since the last save.
    [[nodiscard]] bool dirty() const { return m_dirty; }
    void clear_dirty() { m_dirty = false; }

    /// Save/restore the memory contents and the serial state machine.
    void serialize(Archive& ar);

private:
    enum class State {
        Idle,      ///< Chip deselected.
        Command,   ///< Shifting in start bit, opcode and address.
        Reading,   ///< Shifting out a word; the dummy zero is already on the line.
        Writing,   ///< Shifting in a word to store.
        Complete,  ///< Command finished, waiting for chip select to drop.
    };

    void begin_command();
    void decode_command();

    std::array<u16, kWordCount> m_memory{};

    State m_state = State::Idle;
    bool  m_cs    = false;
    bool  m_clk   = false;
    bool  m_di    = false;
    bool  m_do    = true;

    /// Bits shifted in so far, and how many. The start bit is not retained.
    u32 m_shift = 0;
    u32 m_bits  = 0;

    u32 m_address = 0;

    /// The outgoing word, held in the top of a 32-bit register so bit 31 is
    /// always the line level, as MAME does; this keeps the dummy bit and the
    /// trailing ones right.
    u32 m_read_data = 0;
    u32 m_read_bit  = 0;
    bool m_write_enabled = false;
    bool m_dirty         = false;
};

}  // namespace sm2::hw
