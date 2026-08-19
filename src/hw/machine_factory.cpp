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

namespace sm2::hw {

std::unique_ptr<Model2MachineBase> create_machine(const rom::GameSpec& game, rom::RomSet roms)
{
    switch (game.board) {
        case rom::Board::Model2A: {
            auto machine = std::make_unique<Model2>();
            if (!machine->init(game, std::move(roms))) {
                return nullptr;
            }
            return machine;
        }

        // Not implemented yet. Rejected here rather than left to crash or to
        // produce a silently-empty machine, matching RomLoader::load's own
        // rejection message style. `roms` is never touched and no device
        // state is allocated on this path.
        case rom::Board::Model2:
        case rom::Board::Model2B:
        case rom::Board::Model2C:
            SM2_ERROR("'%s' is a %s board, which is not implemented yet. Only "
                      "Model 2A-CRX is supported.", game.name.c_str(),
                      rom::board_name(game.board));
            return nullptr;
    }

    // Unreachable: the switch above is exhaustive over every rom::Board value
    // (no default: label, so a new enumerator without a case here is a
    // compile-time -Wswitch error). Kept only because some compilers still
    // want a return on every path out of a non-void function.
    return nullptr;
}

}  // namespace sm2::hw
