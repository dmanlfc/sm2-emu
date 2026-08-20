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

#include "hw/model2_machine_base.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <memory>

namespace sm2::hw {

/// Construct and initialise whichever machine variant `game.board` calls for.
///
/// Dispatches on `game.board` with an exhaustive switch over every rom::Board
/// value: a missing case is a compile error (no `default:` label, so
/// -Wswitch fires) rather than a silently-empty machine at runtime. This is
/// what satisfies "Property 5: Machine dispatch is exhaustive and
/// side-effect-free on rejection" in the model2-fleet-compatibility design.
///
/// All four boards are implemented: Model2A constructs hw::Model2, Model2B
/// hw::Model2B, Model2C hw::Model2C and Model2 hw::Model2Original. Each case
/// calls the machine's init() with `roms` and returns it as the shared
/// interface, or nullptr if that init() fails.
///
/// A board whose rom::board_implemented() is false -- there are none today --
/// returns nullptr and logs a clear rejection, in the same style as
/// RomLoader::load's own board rejection, without touching `roms` or
/// allocating any device state, so a later wave's real implementation is never
/// fighting stale partial construction left behind by an earlier failed
/// attempt.
[[nodiscard]] std::unique_ptr<Model2MachineBase> create_machine(const rom::GameSpec& game,
                                                                 rom::RomSet roms);

}  // namespace sm2::hw
