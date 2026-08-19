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
#include "hw/eeprom_93c46.h"

#include "core/log.h"

#include <cstdio>
#include <cstring>

namespace sm2::hw {
namespace {

// Opcodes, in the two bits following the start bit.
constexpr u32 kOpSpecial = 0;  ///< Sub-selected by the top two address bits.
constexpr u32 kOpWrite   = 1;
constexpr u32 kOpRead    = 2;
constexpr u32 kOpErase   = 3;

// Sub-opcodes for kOpSpecial, in address bits 5:4.
constexpr u32 kSubEwds = 0;  ///< Erase/write disable.
constexpr u32 kSubWral = 1;  ///< Write all.
constexpr u32 kSubEral = 2;  ///< Erase all.
constexpr u32 kSubEwen = 3;  ///< Erase/write enable.

/// Start bit, two opcode bits and six address bits.
constexpr u32 kCommandBits = 8;  // opcode and address; the start bit is consumed separately

}  // namespace

Eeprom93c46::Eeprom93c46()
{
    reset();
}

void Eeprom93c46::reset()
{
    m_memory.fill(0xffff);
    m_state         = State::Idle;
    m_cs            = false;
    m_clk           = false;
    m_di            = false;
    m_do            = true;
    m_shift         = 0;
    m_bits          = 0;
    m_address       = 0;
    m_read_data     = 0;
    m_read_bit      = 0;
    m_write_enabled = false;
    m_dirty         = true;
}

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------

void Eeprom93c46::set_cs(bool level)
{
    if (level == m_cs) {
        return;
    }
    m_cs = level;

    if (level) {
        begin_command();
    } else {
        // Dropping chip select abandons whatever was in progress and leaves the
        // data line high, which the device uses to signal "ready".
        m_state = State::Idle;
        m_shift = 0;
        m_bits  = 0;
        m_do    = true;
    }
}

void Eeprom93c46::set_di(bool level)
{
    m_di = level;
}

void Eeprom93c46::set_clk(bool level)
{
    const bool rising = level && !m_clk;
    m_clk = level;

    if (!rising || !m_cs) {
        return;
    }

    switch (m_state) {
        case State::Command:
            // Leading zeros before the start bit are ignored, which is what lets
            // a driver clock the line idle before beginning.
            if (m_bits == 0 && !m_di) {
                return;
            }
            if (m_bits == 0) {
                // This rising edge carried the start bit; the opcode follows.
                m_bits = 1;
                return;
            }

            m_shift = (m_shift << 1) | (m_di ? 1u : 0u);
            if (++m_bits > kCommandBits) {
                decode_command();
            }
            break;

        case State::Reading:
            if (m_read_bit == 0) {
                // The device emits a dummy zero before the data word.
                m_do       = false;
                m_read_bit = 1;
                break;
            }
            if (m_read_bit > 16) {
                // Sequential read: once a word is exhausted the address
                // auto-increments and the next one streams out without a new
                // command. This has to happen at the start of the clock that
                // needs the new data, not at the end of the previous one, or the
                // word's last bit is overwritten before it is sampled.
                m_address   = (m_address + 1) % kWordCount;
                m_read_data = m_memory[m_address];
                m_read_bit  = 1;
            }
            m_do = ((m_read_data >> (16 - m_read_bit)) & 1) != 0;
            ++m_read_bit;
            break;

        case State::Writing:
            m_shift = (m_shift << 1) | (m_di ? 1u : 0u);
            // The counter starts at zero here, unlike the command phase where the
            // start bit has already advanced it, so the sixteenth bit is the one
            // that completes the word.
            if (++m_bits == 16) {
                if (m_write_enabled) {
                    m_memory[m_address] = static_cast<u16>(m_shift & 0xffff);
                    m_dirty             = true;
                } else {
                    SM2_DEBUG("93c46: write to %02x ignored, writes are disabled",
                              m_address);
                }
                // No write delay is modelled, so report ready immediately.
                m_do    = true;
                m_state = State::Complete;
            }
            break;

        case State::Idle:
        case State::Complete:
            break;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Eeprom93c46::begin_command()
{
    m_state     = State::Command;
    m_shift     = 0;
    m_bits      = 0;
    m_read_bit  = 0;
    m_do        = true;  // ready
}

void Eeprom93c46::decode_command()
{
    const u32 opcode  = (m_shift >> 6) & 3;
    const u32 address = m_shift & 0x3f;

    m_shift = 0;
    m_bits  = 0;

    switch (opcode) {
        case kOpRead:
            m_address   = address;
            m_read_data = m_memory[m_address];
            m_read_bit  = 0;
            m_state     = State::Reading;
            break;

        case kOpWrite:
            m_address = address;
            m_state   = State::Writing;
            break;

        case kOpErase:
            if (m_write_enabled) {
                m_memory[address] = 0xffff;
                m_dirty           = true;
            }
            m_do    = true;
            m_state = State::Complete;
            break;

        case kOpSpecial:
            switch ((address >> 4) & 3) {
                case kSubEwen:
                    m_write_enabled = true;
                    break;
                case kSubEwds:
                    m_write_enabled = false;
                    break;
                case kSubEral:
                    if (m_write_enabled) {
                        m_memory.fill(0xffff);
                        m_dirty = true;
                    }
                    break;
                case kSubWral:
                    // Write-all needs a data word, so it behaves like a write
                    // whose result lands in every location. Not used by any
                    // Model 2 game; treated as a no-op with the data consumed.
                    SM2_DEBUG("93c46: write-all is not implemented");
                    break;
                default:
                    break;
            }
            m_do    = true;
            m_state = State::Complete;
            break;

        default:
            m_state = State::Complete;
            break;
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

std::span<const u8> Eeprom93c46::bytes() const
{
    return {reinterpret_cast<const u8*>(m_memory.data()), kByteCount};
}

std::span<u8> Eeprom93c46::bytes()
{
    return {reinterpret_cast<u8*>(m_memory.data()), kByteCount};
}

bool Eeprom93c46::load(const std::string& path)
{
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return false;
    }
    const usize read = std::fread(m_memory.data(), 1, kByteCount, handle);
    std::fclose(handle);

    if (read != kByteCount) {
        SM2_WARN("eeprom '%s' is %zu bytes, expected %u; ignoring it",
                 path.c_str(), read, kByteCount);
        m_memory.fill(0xffff);
        return false;
    }
    m_dirty = false;
    SM2_INFO("loaded eeprom from %s", path.c_str());
    return true;
}

bool Eeprom93c46::save(const std::string& path) const
{
    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (handle == nullptr) {
        SM2_WARN("could not write eeprom to '%s'", path.c_str());
        return false;
    }
    const usize written = std::fwrite(m_memory.data(), 1, kByteCount, handle);
    std::fclose(handle);
    return written == kByteCount;
}

}  // namespace sm2::hw
