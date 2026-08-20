// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315-5838_317-0229_comp.cpp/.h
// (BSD-3-Clause, copyright-holders David Haywood, Samuel Neves, Peter
// Wilhelmsen, Morten Shearman Kirkegaard).
#pragma once

#include "core/types.h"

namespace sm2::hw {

/// Sega 315-5838/317-0229 compression and encryption chip, DOA hack mode only.
///
/// MAME never implemented the real compression/decryption algorithm for Dead
/// or Alive; its own device hardcodes the one 50-byte string the boot check
/// reads and returns 0x00 past it (`HACK_MODE_DOA`). This device replicates
/// exactly that, and nothing else the real chip does for other titles
/// (Decathlete's cipher, the ST-V compression tree upload, the no-key
/// pseudo-random mode).
class Sega3155838Comp {
public:
    void reset();

    /// Two sequential bytes of the fixed string, packed high/low, matching
    /// MAME's `data_r()`. Reading past the string's end yields 0x00 forever.
    [[nodiscard]] u16 data_r();

    /// Resets the sequential read offset to `value`, matching `srcaddr_w`.
    /// MAME's device also resets its internal compression bit-decoder state
    /// here, which has no equivalent in hack mode.
    void srcaddr_w(u32 value) { m_srcoffset = value & 0x007fffff; }

    /// Protection input data. Hack mode never reads it back; kept as a named
    /// no-op, matching `data_w_doa`, so the address decode still names every
    /// register the real chip has.
    void data_w_doa(u32 /*value*/) {}

private:
    [[nodiscard]] u8 next_byte();

    u32 m_srcoffset = 0;
};

}  // namespace sm2::hw
