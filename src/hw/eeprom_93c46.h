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
#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <string>

namespace sm2::hw {

/// 93C46 serial EEPROM, 64 words of 16 bits.
///
/// Model 2 stores its operator settings here, bit-banged through four lines of
/// the 315-5649 I/O controller's port A with the data line read back on port B.
/// The self-test reads it, so a game whose EEPROM never responds will complain
/// about its settings on every boot.
///
/// Modelled from the device's serial protocol rather than ported: it is a small
/// well-documented state machine, and writing it out makes the timing of the
/// dummy bit and the write-enable latch explicit.
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

    // -- persistence -------------------------------------------------------

    /// Contents as bytes, little-endian within each word.
    [[nodiscard]] std::span<const u8> bytes() const;
    [[nodiscard]] std::span<u8>       bytes();

    [[nodiscard]] bool load(const std::string& path);
    [[nodiscard]] bool save(const std::string& path) const;

    /// True when a word has been written since the last save.
    [[nodiscard]] bool dirty() const { return m_dirty; }
    void clear_dirty() { m_dirty = false; }

private:
    enum class State {
        Idle,      ///< Chip deselected.
        Command,   ///< Shifting in start bit, opcode and address.
        Reading,   ///< Shifting out a word, preceded by a dummy zero.
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

    u32 m_address    = 0;
    u16 m_read_data  = 0;
    u32 m_read_bit   = 0;
    bool m_write_enabled = false;
    bool m_dirty         = false;
};

}  // namespace sm2::hw
