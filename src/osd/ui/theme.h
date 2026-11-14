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

#include <imgui.h>

#include <cstddef>
#include <string_view>

namespace sm2::osd::ui {

/// The colours of one overlay theme. Every surface of the settings, the game
/// list and the in-game notices is drawn from these, so a theme swaps the lot.
///
/// Surfaces: `backdrop` fills the window behind the settings (and the game
/// list), `panel` fills a group frame, `card` a raised control (a button, a
/// segment, a field). `line` separates rows, `edge` outlines panels and
/// controls, `hover` tints a control under the pointer.
///
/// Ink: `text` for names and values, `muted` for the explanation under a name,
/// `faint` for hints and group titles.
///
/// Emphasis: `accent` marks what has focus or is selected, with `on_accent` for
/// text drawn over it; `active` fills a pressed button or an "on" segment, with
/// `on_active`. `good`, `warn` and `bad` colour status text.
struct Palette {
    ImU32 backdrop, panel, card, line, edge, hover;
    ImU32 text, muted, faint;
    ImU32 accent, on_accent, active, on_active;
    ImU32 good, warn, bad;
    /// Whether the theme is light (dark ink on a light page): a few
    /// treatments, like the dimming over the game, invert for it.
    bool light;
};

/// A theme as the user picks it: its ini key, its shown name (translated
/// where drawn), a short description and the palette.
struct Theme {
    const char*    key;
    const char*    name;
    const char*    about;
    const Palette* palette;
};

/// The built-in themes, Ink first (the default).
[[nodiscard]] const Theme* themes(size_t* count);

/// The theme with that ini key, or the default when none matches.
[[nodiscard]] const Theme& theme_named(std::string_view key);

/// Push the palette into ImGui's own style colours, so the stock widgets the
/// kit does not replace (text fields, tables, popups) match the theme.
void apply_to_imgui(const Palette& palette, ImGuiStyle& style);

/// `colour` with its alpha scaled by `share` (0..1).
[[nodiscard]] constexpr ImU32 with_alpha(ImU32 colour, float share)
{
    const ImU32 a = static_cast<ImU32>(static_cast<float>(colour >> IM_COL32_A_SHIFT) * share + 0.5f);
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

/// A blend of two colours, `t` of the second.
[[nodiscard]] ImU32 mix(ImU32 a, ImU32 b, float t);

}  // namespace sm2::osd::ui
