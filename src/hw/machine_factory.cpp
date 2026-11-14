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
#include "hw/machine_factory.h"

#include "core/log.h"
#include "hw/model1.h"
#include "hw/model2.h"
#include "hw/model2_original.h"
#include "hw/model2b.h"
#include "hw/model2c.h"
#include "hw/model3/model3.h"

namespace sm2::hw {

std::unique_ptr<Machine> create_machine(const rom::GameSpec& game, rom::RomSet roms)
{
    // Gated on rom::board_implemented, as RomLoader::load is. No default, so a
    // new enumerator without a case is a -Wswitch error.
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

        case rom::Board::Model1: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<Model1>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }

        case rom::Board::Model3: {
            if (!rom::board_implemented(game.board)) {
                break;
            }
            auto machine = std::make_unique<model3::Model3>();
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
