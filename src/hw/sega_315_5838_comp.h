// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315-5838_317-0229_comp.cpp/.h
// (BSD-3-Clause, copyright-holders David Haywood, Samuel Neves, Peter
// Wilhelmsen, Morten Shearman Kirkegaard).
#pragma once

#include "core/types.h"

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// Sega 315-5838/317-0229 compression and encryption chip, DOA hack mode only.
///
/// Like MAME's `HACK_MODE_DOA`, this returns the one 50-byte string the Dead or
/// Alive boot check reads, then 0x00. The real algorithm is not implemented.
class Sega3155838Comp {
public:
    void reset();

    /// Two sequential bytes of the fixed string, packed high/low, matching
    /// MAME's `data_r()`. Reading past the string's end yields 0x00 forever.
    [[nodiscard]] u16 data_r();

    /// Resets the sequential read offset to `value`, matching `srcaddr_w`.
    void srcaddr_w(u32 value) { m_srcoffset = value & 0x007fffff; }

    /// Protection input data; a no-op in hack mode, matching `data_w_doa`.
    void data_w_doa(u32 /*value*/) {}

    /// Save/restore the sequential read offset (the only durable state).
    void serialize(Archive& ar);

private:
    [[nodiscard]] u8 next_byte();

    u32 m_srcoffset = 0;
};

}  // namespace sm2::hw
