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

#pragma once

#include "core/types.h"

#include <string>

namespace sm2::render {

/// Write tightly-packed 8-bit RGB pixels (row 0 = top) to `path` as a PNG.
/// Used by both frame-capture backends so screenshots are one format.
[[nodiscard]] bool write_png_rgb(const std::string& path, u32 width, u32 height,
                                 const u8* rgb);

}  // namespace sm2::render
