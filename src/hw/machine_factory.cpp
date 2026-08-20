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
#include "hw/machine_factory.h"

#include "core/log.h"
#include "hw/model2.h"
#include "hw/model2b.h"

namespace sm2::hw {

std::unique_ptr<Model2MachineBase> create_machine(const rom::GameSpec& game, rom::RomSet roms)
{
    // The switch below still enumerates every rom::Board value explicitly (no
    // default: label), so a new enumerator without a case here remains a
    // compile-time -Wswitch error. But each case's accept/reject decision now
    // starts from rom::board_implemented rather than being re-decided by a
    // second, separately-maintained list: RomLoader::load gates on that exact
    // same function, so the two are provably in sync rather than just
    // currently-matching by coincidence. A future wave that flips
    // board_implemented() to true for a board still needs its own case here
    // to actually construct that board's machine class -- this only keeps the
    // *rejection* in sync, not the construction itself.
    switch (game.board) {
        case rom::Board::Model2A: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<Model2>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }

        case rom::Board::Model2B: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<Model2B>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }

        // Not implemented yet.
        case rom::Board::Model2:
        case rom::Board::Model2C:
            break;
    }

    SM2_ERROR("'%s' is a %s board, which is not implemented yet. Only "
              "Model 2A-CRX is supported.", game.name.c_str(),
              rom::board_name(game.board));
    return nullptr;
}

}  // namespace sm2::hw
