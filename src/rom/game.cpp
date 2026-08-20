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
#include "rom/game.h"

#include <algorithm>

namespace sm2::rom {

const char* board_name(Board board)
{
    switch (board) {
        case Board::Model2:  return "Model 2";
        case Board::Model2A: return "Model 2A-CRX";
        case Board::Model2B: return "Model 2B-CRX";
        case Board::Model2C: return "Model 2C-CRX";
    }
    return "unknown";
}

bool board_implemented(Board board)
{
    switch (board) {
        case Board::Model2A: return true;
        case Board::Model2B: return true;
        case Board::Model2C: return true;

        case Board::Model2:
            return false;
    }
    return false;
}

const RegionSpec* GameSpec::region(const std::string& region_name) const
{
    const auto match = std::find_if(regions.begin(), regions.end(),
                                    [&region_name](const RegionSpec& candidate) {
                                        return candidate.name == region_name;
                                    });
    return match != regions.end() ? &*match : nullptr;
}

}  // namespace sm2::rom
