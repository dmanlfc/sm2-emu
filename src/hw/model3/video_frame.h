//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#pragma once

#include "core/types.h"
#include "rom/game.h"

#include <array>
#include <span>
#include <vector>

// What a Model 3 machine hands the renderer once per frame; the renderer reads
// nothing else. Double-buffered: a frame stays intact until the end of the
// run_frame() after next. Real3D memories are host-native words.
//
// The renderer must apply every frame's texture_uploads, and treat texture RAM
// as wholly changed if frame_number skips. Line-of-sight goes back through
// Model3::set_line_of_sight() between frames only.

namespace sm2::hw::model3 {

/// A rectangle of texture RAM written since the previous frame, in texels.
struct TextureRect {
    u32 x      = 0;
    u32 y      = 0;
    u32 width  = 0;
    u32 height = 0;
    /// Mipmap level (0 = base).
    u32 level  = 0;
};

/// Tile generator registers latched at the start of an active line.
struct TileGenLine {
    u32 reg20 = 0;                ///< Layer priority / colour depth (0x20).
    u32 reg40 = 0;                ///< Layer A/A' colour offset (0x40).
    u32 reg44 = 0;                ///< Layer B/B' colour offset (0x44).
    std::array<u32, 4> scroll{};  ///< Layer enable + scroll (0x60-0x6C).
};

struct VideoFrame {
    static constexpr u32 kCullingLoWords = 0x400000 / 4;  ///< 4 MB at 0x8C000000.
    static constexpr u32 kCullingHiWords = 0x100000 / 4;  ///< 1 MB at 0x8E000000.
    static constexpr u32 kPolygonWords   = 0x400000 / 4;  ///< 4 MB at 0x98000000.
    static constexpr u32 kTextureSize    = 2048;          ///< Texels per side.

    static constexpr u32 kTileGenVramBytes = 0x120000;  ///< VRAM, then palette at 0x100000.
    static constexpr u32 kTileGenRegs      = 64;
    static constexpr u32 kActiveLines      = 384;

    rom::Model3Step step = rom::Model3Step::Step10;

    /// Advances once per published frame.
    u64 frame_number = 0;

    // -- Real3D Pro-1000 --------------------------------------------------------

    std::span<const u32> culling_lo;
    std::span<const u32> culling_hi;
    std::span<const u32> polygon_ram;
    std::span<const u32> vrom;         ///< Constant after load; shared, not copied.
    std::span<const u16> texture_ram;  ///< kTextureSize * kTextureSize texels.

    /// Changed since the previous frame; `texture_reset` means all of it.
    std::vector<TextureRect> texture_uploads;
    bool                     texture_reset = false;

    /// False when Mars's modeword bit 0x40000 is set.
    bool sun_clamp = true;

    /// Mercury modeword bit 2, latched at trigger: draw no 3D.
    bool block_culling = false;

    /// Mercury, Venus, Earth, Mars, Jupiter.
    std::array<u32, 5> modewords{};

    // -- Tile generator ---------------------------------------------------------

    std::span<const u8>             tilegen_vram;  ///< kTileGenVramBytes, as the PowerPC wrote it.
    std::array<u32, kTileGenRegs>   tilegen_regs{};

    std::array<TileGenLine, kActiveLines> tilegen_lines{};
};

}  // namespace sm2::hw::model3
