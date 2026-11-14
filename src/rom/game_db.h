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

    /// Locate the bundled games.xml, searching in order: the current directory,
    /// next to the executable, then the installed data directory. Returns
    /// nothing if no candidate exists.
    [[nodiscard]] static std::optional<std::string> locate();

private:
    /// What each set declared itself, where the parsed value cannot tell.
    struct Declarations {
        std::set<std::string> board_inherited;  ///< Clones with no board attribute.
        std::set<std::string> protection;       ///< Sets with a protection attribute,
                                                ///< "none" included.
        std::set<std::string> model3;           ///< Sets with a <model3> element.
    };

    /// Fold each clone's parent into it, so a child set need only list the
    /// chips that differ.
    [[nodiscard]] bool merge_clones(const Declarations& declared);

    std::vector<GameSpec> m_games;
};

}  // namespace sm2::rom
