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
#pragma once

/// The overlay's font and complex-script text.
///
/// The font is ImGui's ProggyClean with the system's fonts merged in for
/// whatever the current language (i18n::current()) needs. Text in scripts that
/// must be shaped or reordered is run through HarfBuzz and SheenBidi; each
/// positioned glyph that comes out is given a private-use code point that a
/// custom ImGui font source draws, so shaped strings pass through ImGui's text
/// functions like any other.
namespace sm2::osd::ui_text {

/// Rebuild the overlay font for the current language. Call between frames.
/// Returns false when the system lacks fonts for the language; the font is
/// then ProggyClean alone and the caller should fall back to English.
bool build_font();

/// Drop the shaper and its font data. Call before ImGui's context goes.
void shutdown();

}  // namespace sm2::osd::ui_text
