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

#pragma once

#include "core/types.h"

#include <string>

namespace sm2::render {

/// Write tightly-packed 8-bit RGB pixels (row 0 = top) to `path` as a PNG.
/// Used by both frame-capture backends so screenshots are one format.
[[nodiscard]] bool write_png_rgb(const std::string& path, u32 width, u32 height,
                                 const u8* rgb);

/// The same for 1..4 interleaved 8-bit channels.
[[nodiscard]] bool write_png(const std::string& path, u32 width, u32 height, u32 channels,
                             const u8* pixels);

}  // namespace sm2::render
