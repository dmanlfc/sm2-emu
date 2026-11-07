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

// What a Model 3 machine hands the renderer once per frame.
//
// This is the whole contract between emulation and drawing: the renderer reads
// nothing else of the machine. The machine publishes one of these at the end of
// run_frame() and keeps it unchanged until the next run_frame() has finished,
// so the renderer can draw frame N on its own thread while frame N+1 is being
// emulated into the other buffer.
//
// The Real3D memories are host-native 32-bit words; the machine converts the
// PowerPC's big-endian stores as they arrive.

namespace sm2::hw::model3 {

/// A rectangle of texture RAM written since the previous frame, in texels.
struct TextureRect {
    u32 x      = 0;
    u32 y      = 0;
    u32 width  = 0;
    u32 height = 0;
};

struct VideoFrame {
    static constexpr u32 kCullingLoWords = 0x400000 / 4;  ///< 4 MB at 0x8C000000.
    static constexpr u32 kCullingHiWords = 0x100000 / 4;  ///< 1 MB at 0x8E000000.
    static constexpr u32 kPolygonWords   = 0x400000 / 4;  ///< 4 MB at 0x98000000.
    static constexpr u32 kTextureSize    = 2048;          ///< Texels per side.

    static constexpr u32 kTileGenVramBytes = 0x120000;  ///< VRAM, then palette at 0x100000.
    static constexpr u32 kTileGenRegs      = 64;

    rom::Model3Step step = rom::Model3Step::Step10;

    /// Advances once per published frame.
    u64 frame_number = 0;

    // -- Real3D Pro-1000 --------------------------------------------------------

    std::span<const u32> culling_lo;
    std::span<const u32> culling_hi;
    std::span<const u32> polygon_ram;
    std::span<const u32> vrom;         ///< Constant after load; shared, not copied.
    std::span<const u16> texture_ram;  ///< kTextureSize * kTextureSize texels.

    /// Texture RAM rectangles changed since the previous published frame, so a
    /// renderer re-decodes only those. When `texture_reset` is set the whole of
    /// texture RAM must be treated as changed (after a load or a save-state).
    std::vector<TextureRect> texture_uploads;
    bool                     texture_reset = false;

    // -- Tile generator ---------------------------------------------------------

    std::span<const u8>             tilegen_vram;  ///< kTileGenVramBytes, as the PowerPC wrote it.
    std::array<u32, kTileGenRegs>   tilegen_regs{};
};

}  // namespace sm2::hw::model3
