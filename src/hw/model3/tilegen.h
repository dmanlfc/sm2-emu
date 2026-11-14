// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 2D tile generator: the emulation side only (VRAM, palette RAM,
// registers and the interrupt acknowledge). Drawing is the renderer's, from
// the VideoFrame snapshot.
//
// Derived from Supermodel's Src/Model3/TileGen.cpp and TileGen.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2012 Bart Trzynadlowski, Nik Henson.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Registers are latched per line for the renderer in render/model3. VRAM is
// held in PowerPC byte order so it maps straight into the PageMap.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"

#include <array>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

class IrqController;

class TileGen {
public:
    static constexpr u32 kVramBytes = VideoFrame::kTileGenVramBytes;  ///< 0x120000
    static constexpr u32 kRegs      = VideoFrame::kTileGenRegs;       ///< 64

    TileGen();

    void attach_irq(IrqController* irq) { m_irq = irq; }

    void reset();

    /// Host memory backing 0xF1000000-0xF111FFFF, big-endian bytes.
    [[nodiscard]] u8* vram() { return m_vram.data(); }
    [[nodiscard]] const u8* vram() const { return m_vram.data(); }

    /// `data` is byte-reversed from the PowerPC's store.
    [[nodiscard]] u32 read_register(unsigned reg) const { return m_regs[(reg & 0xFF) / 4]; }
    void write_register(unsigned reg, u32 data);

    [[nodiscard]] const std::array<u32, kRegs>& registers() const { return m_regs; }

    /// At the start of each active line (0-383).
    void latch_line(u32 line);
    [[nodiscard]] const std::array<TileGenLine, VideoFrame::kActiveLines>& line_latch() const
    {
        return m_lines;
    }

    void serialize(Archive& ar);

private:
    IrqController*        m_irq = nullptr;
    std::vector<u8>       m_vram;
    std::array<u32, kRegs> m_regs{};
    std::array<TileGenLine, VideoFrame::kActiveLines> m_lines{};
};

}  // namespace sm2::hw::model3
