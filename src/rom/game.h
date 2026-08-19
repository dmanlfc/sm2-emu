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

#include <string>
#include <vector>

// The ROM database data model.
//
// Games are described in data/games.xml rather than in code, so adding one is a
// data edit. The schema mirrors how the ROM chips are physically wired: a
// region is a flat byte array, and each file contributes `chunk` bytes every
// `stride` bytes starting at `offset`. That single mechanism expresses every
// interleaving Model 2 uses.

namespace sm2::rom {

/// Which Model 2 board variant, which decides the coprocessor and sound
/// hardware. Only Model2A is implemented; the rest are here so the database can
/// describe them and the loader can refuse them with a clear message.
enum class Board {
    Model2,   ///< Original. TGP + Model 1 sound board.
    Model2A,  ///< 2A-CRX. TGP + 68000/SCSP.
    Model2B,  ///< 2B-CRX. ADSP-21062 SHARC + 68000/SCSP.
    Model2C,  ///< 2C-CRX. MB86235 TGPx4 + 68000/SCSP.
};

[[nodiscard]] const char* board_name(Board board);

/// Logical input groups a game uses, so the input layer can bind only what is
/// relevant and the UI can hide the rest.
enum class InputFlags : u32 {
    None      = 0,
    Common    = 1U << 0,  ///< Coins, service, test, start.
    Joystick1 = 1U << 1,
    Joystick2 = 1U << 2,
    Buttons3  = 1U << 3,  ///< Three attack buttons per player (fighting games).
    Vehicle   = 1U << 4,  ///< Steering, pedals, gear shift.
    Gun1      = 1U << 5,
    Gun2      = 1U << 6,
};

[[nodiscard]] constexpr InputFlags operator|(InputFlags a, InputFlags b)
{
    return static_cast<InputFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has_input(InputFlags set, InputFlags wanted)
{
    return (static_cast<u32>(set) & static_cast<u32>(wanted)) != 0;
}

/// One ROM chip's contribution to a region.
struct FileSpec {
    std::string name;             ///< Filename inside the archive.
    u32         offset  = 0;      ///< Destination byte offset within the region.
    u32         crc32   = 0;      ///< Expected CRC32.
    bool        has_crc = false;  ///< False means identify by name alone.
};

/// A flat byte array assembled from one or more ROM chips.
struct RegionSpec {
    std::string name;

    /// Declared size. Zero means "derive it from the files", which is the usual
    /// case; an explicit size matters when the hardware sees a region larger
    /// than the chips populating it.
    u32 size = 0;

    /// Interleaving. `stride == chunk` is a plain contiguous copy; Model 2's
    /// 32-bit regions built from pairs of 16-bit chips use stride 4, chunk 2,
    /// with the two files at offsets 0 and 2.
    u32 stride = 1;
    u32 chunk  = 1;

    /// Swap every pair of bytes after assembly. This is what MAME's
    /// ROM_LOAD16_WORD_SWAP does, and it is how the big-endian 68000 sound
    /// program and its samples are stored.
    bool byte_swap = false;

    /// Value for bytes no chip covers. MAME's ROMREGION_ERASEFF regions, such
    /// as the texture ROMs, need 0xFF rather than zero.
    u8 fill = 0x00;

    /// A region that may legitimately be absent. Also covers regions that exist
    /// but hold no chips on this board, such as copro_data on Virtua Fighter 2.
    bool required = true;

    std::vector<FileSpec> files;
};

/// Everything the database knows about one game.
struct GameSpec {
    std::string name;    ///< Set identifier, e.g. "vf2".
    std::string parent;  ///< Set this is a clone of, empty if a parent.

    std::string title;
    std::string version;
    std::string manufacturer;
    u32         year = 0;

    Board      board  = Board::Model2A;
    InputFlags inputs = InputFlags::None;

    /// True when the set is known not to run yet, so the loader can warn.
    bool preliminary = false;

    std::vector<RegionSpec> regions;

    [[nodiscard]] const RegionSpec* region(const std::string& region_name) const;
};

}  // namespace sm2::rom
