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

#include "rom/game.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sm2::rom {

/// The parsed contents of games.xml.
class GameDatabase {
public:
    [[nodiscard]] bool load(const std::string& path);

    /// Parse from memory. Used by the tests so they need no data file.
    [[nodiscard]] bool load_from_string(std::string_view xml, const char* origin = "<memory>");

    [[nodiscard]] const GameSpec* find(std::string_view name) const;

    [[nodiscard]] const std::vector<GameSpec>& games() const { return m_games; }

    [[nodiscard]] bool empty() const { return m_games.empty(); }

    /// Locate games.xml, searching in order: an explicit override, the current
    /// directory, next to the executable, then the installed data directory.
    /// Returns nothing if no candidate exists.
    [[nodiscard]] static std::optional<std::string> locate(const std::string& override_path);

private:
    /// Fold each clone's regions over its parent's, so a child set need only
    /// list the chips that differ.
    /// Fold each clone's parent into it.
    ///
    /// `board_inherited` names the clones that declared no board of their own, so
    /// the merge can take the parent's. That is passed in rather than stored on
    /// GameSpec because it is a fact about the file, not about the hardware.
    [[nodiscard]] bool merge_clones(const std::set<std::string>& board_inherited);

    std::vector<GameSpec> m_games;
};

}  // namespace sm2::rom
