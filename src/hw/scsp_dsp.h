// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// The SCSP's effects DSP.
//
// Ported from MAME's src/devices/sound/scspdsp.h (BSD-3-Clause, copyright-holders
// ElSemi, R. Belmont).
//
// Changes from upstream: MAME's address_space is replaced by ScspMemory, and the
// struct is named in our style. Everything else is kept as upstream wrote it so
// that fixes there stay diffable.

#pragma once

#include "core/types.h"

namespace sm2 {
class Archive;
}

namespace sm2::hw {

/// The 16-bit memory the SCSP addresses, shared with the sound 68000's work
/// RAM (so the DSP's delay line sits in program memory). Byte addresses in a
/// 20-bit space, as MAME's device_rom_interface<20, 1, 0, ENDIANNESS_BIG>.
class ScspMemory {
public:
    virtual ~ScspMemory() = default;

    [[nodiscard]] virtual u8  scsp_read_byte(u32 address)  = 0;
    [[nodiscard]] virtual u16 scsp_read_word(u32 address)  = 0;
    virtual void scsp_write_word(u32 address, u16 value)   = 0;
};

/// The DSP context.
struct ScspDsp {
    // Config
    ScspMemory* memory = nullptr;
    u32 RBP = 0;  // Ring buf pointer
    u32 RBL = 0;  // Delay ram (Ring buffer) size in words

    // context
    s16 COEF[64]{};      // 16 bit signed
    u16 MADRS[32]{};     // offsets (in words), 16 bit
    u16 MPRO[128 * 4]{}; // 128 steps 64 bit
    s32 TEMP[128]{};     // TEMP regs,24 bit signed
    s32 MEMS[32]{};      // MEMS regs,24 bit signed
    u32 DEC = 0;

    // input
    s32 MIXS[16]{};  // MIXS, 24 bit signed
    s16 EXTS[2]{};   // External inputs (CDDA)    16 bit signed

    // output
    s16 EFREG[16]{};  // EFREG, 16 bit signed

    bool Stopped = true;
    int  LastStep = 0;

    void Init();
    void SetSample(s32 sample, s32 SEL, s32 MXL);
    void Step();
    void Start();

    /// Save/restore the DSP's program/coefficient RAM and working registers.
    /// The `memory` back-pointer is excluded (re-bound by Init).
    void serialize(Archive& ar);
};

}  // namespace sm2::hw
