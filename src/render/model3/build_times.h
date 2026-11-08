//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// CPU time of one Model 3 frame build and its parts, in milliseconds.
#pragma once

namespace sm2::render::model3 {

struct BuildTimes {
    double total    = 0.0;
    double textures = 0.0;
    double tilegen  = 0.0;
    double scene    = 0.0;
    double plan     = 0.0;
};

}  // namespace sm2::render::model3
