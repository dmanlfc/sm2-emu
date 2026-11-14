// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
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

/// Knobs for RomLoader::load that only tools and tests turn.
struct LoadOptions {
    /// Assemble the regions even when the set's board cannot run yet.
    bool allow_unimplemented_board = false;
};

/// Reads a zip archive and assembles the ROM regions for whichever game it
/// holds.
///
/// Games are identified by the CRC32s of their contents, not the archive's
/// name, so merged and renamed sets work.
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
                                                        LoadError*          error = nullptr,
                                                        const LoadOptions&  options = {});

    /// Whether `archive_path` holds the chips clone `clone_name` does not share
    /// with its parent: true of a merged archive, which keeps the clones beside
    /// the parent, false of a split set's parent archive. Reads only the
    /// archive's index, not its contents.
    [[nodiscard]] static bool holds_clone(const GameDatabase& database,
                                          const std::string&  archive_path,
                                          const std::string&  clone_name);

    /// Assemble one region from already-extracted file contents. Exposed so the
    /// interleaving can be tested without a zip.
    [[nodiscard]] static std::optional<std::vector<u8>> assemble_region(
        const RegionSpec&                         region,
        const std::vector<std::vector<u8>>&       file_contents);

    /// Size a region needs in order to hold every file it declares.
    [[nodiscard]] static usize computed_region_size(const RegionSpec&         region,
                                                   const std::vector<usize>& file_sizes);

    /// Write every assembled region to `directory` as a raw binary file, for
    /// comparison with a region dumped from MAME's debugger.
    [[nodiscard]] static bool dump_regions(const RomSet&      roms,
                                           const std::string& directory);
};

}  // namespace sm2::rom
