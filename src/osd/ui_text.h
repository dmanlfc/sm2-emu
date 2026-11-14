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
