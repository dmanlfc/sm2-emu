//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
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
#include "hw/model2_original.h"
#include "hw/model2b.h"
#include "hw/model2c.h"

namespace sm2::hw {

std::unique_ptr<Model2MachineBase> create_machine(const rom::GameSpec& game, rom::RomSet roms)
{
    // Each case gates on rom::board_implemented, the same function RomLoader::load
    // gates on, so the two stay in sync rather than being separately maintained.
    // No default: label, so a new enumerator without a case is a -Wswitch error.
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

        case rom::Board::Model2C: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<Model2C>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }

        case rom::Board::Model2: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<Model2Original>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }
    }

    SM2_ERROR("'%s' is a %s board, which is not implemented yet. The original "
              "Model 2 and the CRX boards (2A, 2B and 2C) are supported.",
              game.name.c_str(), rom::board_name(game.board));
    return nullptr;
}

}  // namespace sm2::hw
