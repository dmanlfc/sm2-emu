//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
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
#include "rom/rom_set.h"

#include <optional>
#include <string>
#include <vector>

namespace sm2::rom {

class GameDatabase;

struct LoadResult {
    GameSpec game;
    RomSet   roms;
};

/// Why a load failed, worded for the player rather than the log.
struct LoadError {
    std::string              game;     ///< the set it was taken for, when known
    std::string              message;
    std::vector<std::string> missing;  ///< required chips not found
};

/// Reads a zip archive and assembles the ROM regions for whichever game it
/// holds.
///
/// Games are identified by the CRC32 of their contents rather than by the
/// archive's filename. That makes merged sets work: an archive holding several
/// revisions resolves to whichever one it can satisfy completely, and a set
/// renamed by the user still loads. A file that fails to match is a precise
/// error naming the chip and the CRC that was expected.
class RomLoader {
public:
    /// Assemble the ROMs in `archive_path`.
    ///
    /// When `preferred_game` is non-empty only that set is considered, which is
    /// how a specific revision inside a merged archive is selected. Otherwise
    /// every definition is tried and the best match wins.
    ///
    /// Parent ROMs missing from a clone's archive are looked for in
    /// `<parent>.zip` beside it, matching the usual split-set layout.
    ///
    /// On failure `error`, when given, says why.
    [[nodiscard]] static std::optional<LoadResult> load(const GameDatabase& database,
                                                        const std::string&  archive_path,
                                                        const std::string&  preferred_game = {},
                                                        LoadError*          error = nullptr);

    /// Whether `archive_path` holds the chips clone `clone_name` does not share
    /// with its parent: true of a merged archive, which keeps the clones beside
    /// the parent, false of a split set's parent archive. Reads only the
    /// archive's index, not its contents.
    [[nodiscard]] static bool holds_clone(const GameDatabase& database,
                                          const std::string&  archive_path,
                                          const std::string&  clone_name);

    /// Assemble one region from already-extracted file contents.
    ///
    /// Exposed so the interleaving can be tested directly, without a zip: it is
    /// the part most likely to be subtly wrong, and getting it wrong produces
    /// plausible-looking but scrambled data.
    [[nodiscard]] static std::optional<std::vector<u8>> assemble_region(
        const RegionSpec&                         region,
        const std::vector<std::vector<u8>>&       file_contents);

    /// Size a region needs in order to hold every file it declares.
    [[nodiscard]] static usize computed_region_size(const RegionSpec&         region,
                                                   const std::vector<usize>& file_sizes);

    /// Write every assembled region to `directory` as a raw binary file.
    ///
    /// For cross-checking against MAME, whose debugger can dump the equivalent
    /// region. An interleaving mistake produces data of exactly the right size,
    /// so comparing bytes against a known-good reference is the only way to be
    /// certain the layout is right.
    [[nodiscard]] static bool dump_regions(const RomSet&      roms,
                                           const std::string& directory);
};

}  // namespace sm2::rom
