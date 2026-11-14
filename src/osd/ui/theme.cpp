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
#include "osd/ui/theme.h"

#include "osd/i18n.h"

#include <array>

namespace sm2::osd::ui {

namespace {

constexpr ImU32 rgb(unsigned hex, unsigned alpha = 0xFF)
{
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha);
}

/// Sunset: the logo's colours. An indigo night behind, coral for what has
/// focus and the sunset's pink for what is on or pressed.
constexpr Palette kSunset = {
    .backdrop  = rgb(0x15132B),
    .panel     = rgb(0x1E1B3C),
    .card      = rgb(0x2A2650),
    .line      = rgb(0xF3EFF5, 0x22),
    .edge      = rgb(0xF3EFF5, 0x3A),
    .hover     = rgb(0xF3EFF5, 0x14),
    .text      = rgb(0xF3EFF5),
    .muted     = rgb(0xB4AEC8),
    .faint     = rgb(0x7D7599),
    .accent    = rgb(0xFF7B5C),
    .on_accent = rgb(0x1E0E1E),
    .active    = rgb(0xF2558F),
    .on_active = rgb(0xFFF6F9),
    .good      = rgb(0x62D9A2),
    .warn      = rgb(0xFFC857),
    .bad       = rgb(0xFF5D5D),
    .light     = false,
};

/// Ink, the default: neutral charcoal and warm grey, a cool blue for
/// emphasis, so the overlay recedes behind the game.
constexpr Palette kInk = {
    .backdrop  = rgb(0x151617),
    .panel     = rgb(0x1E2022),
    .card      = rgb(0x2A2D30),
    .line      = rgb(0xECEBE6, 0x1E),
    .edge      = rgb(0xECEBE6, 0x36),
    .hover     = rgb(0xECEBE6, 0x12),
    .text      = rgb(0xECEBE6),
    .muted     = rgb(0xA9A79F),
    .faint     = rgb(0x6F6E68),
    .accent    = rgb(0x5EADFF),
    .on_accent = rgb(0x0B1420),
    .active    = rgb(0x3F8FE6),
    .on_active = rgb(0xF4F9FF),
    .good      = rgb(0x6CCB8E),
    .warn      = rgb(0xE8B84A),
    .bad       = rgb(0xE86060),
    .light     = false,
};

/// Paper: a light theme, warm paper with dark ink, a vermilion seal for
/// focus and indigo for what is on.
constexpr Palette kPaper = {
    .backdrop  = rgb(0xF3EEE3),
    .panel     = rgb(0xFBF8F1),
    .card      = rgb(0xEDE6D8),
    .line      = rgb(0x26211D, 0x1C),
    .edge      = rgb(0x26211D, 0x36),
    .hover     = rgb(0x26211D, 0x0E),
    .text      = rgb(0x26211D),
    .muted     = rgb(0x5E5650),
    .faint     = rgb(0x8E857A),
    .accent    = rgb(0xC8402A),
    .on_accent = rgb(0xFFF8F0),
    .active    = rgb(0x3B5BA5),
    .on_active = rgb(0xFFFFFF),
    .good      = rgb(0x2E8B57),
    .warn      = rgb(0xB8860B),
    .bad       = rgb(0xC0392B),
    .light     = true,
};

/// Arcade: blue skies. Sega's deep blue with a sun-yellow focus and a sky
/// cyan for what is on.
constexpr Palette kArcade = {
    .backdrop  = rgb(0x0A1E49),
    .panel     = rgb(0x102D68),
    .card      = rgb(0x1A3E86),
    .line      = rgb(0xF4F8FF, 0x22),
    .edge      = rgb(0xF4F8FF, 0x3C),
    .hover     = rgb(0xF4F8FF, 0x14),
    .text      = rgb(0xF4F8FF),
    .muted     = rgb(0xB9C9E8),
    .faint     = rgb(0x7D91BC),
    .accent    = rgb(0xFFD12B),
    .on_accent = rgb(0x12214A),
    .active    = rgb(0x3BB8FF),
    .on_active = rgb(0x06163A),
    .good      = rgb(0x5FE0A5),
    .warn      = rgb(0xFFB347),
    .bad       = rgb(0xFF6B6B),
    .light     = false,
};

/// High contrast: black, white and yellow, with heavier lines, for a small
/// or distant screen.
constexpr Palette kContrast = {
    .backdrop  = rgb(0x000000),
    .panel     = rgb(0x0A0A0A),
    .card      = rgb(0x1A1A1A),
    .line      = rgb(0xFFFFFF, 0x55),
    .edge      = rgb(0xFFFFFF, 0x80),
    .hover     = rgb(0xFFFFFF, 0x1A),
    .text      = rgb(0xFFFFFF),
    .muted     = rgb(0xD6D6D6),
    .faint     = rgb(0xA8A8A8),
    .accent    = rgb(0xFFE600),
    .on_accent = rgb(0x000000),
    .active    = rgb(0x00E5FF),
    .on_active = rgb(0x000000),
    .good      = rgb(0x4CFF7A),
    .warn      = rgb(0xFFB000),
    .bad       = rgb(0xFF4D4D),
    .light     = false,
};

constexpr std::array<Theme, 5> kThemes = {{
    {"ink",      N_("Ink"),           N_("charcoal and warm grey, a cool blue"),        &kInk},
    {"sunset",   N_("Sunset"),        N_("indigo night, coral and pink from the logo"), &kSunset},
    {"paper",    N_("Paper"),         N_("light: warm paper, vermilion and indigo"),    &kPaper},
    {"arcade",   N_("Arcade"),        N_("Sega blue skies with a yellow sun"),          &kArcade},
    {"contrast", N_("High contrast"), N_("black, white and yellow; heavier lines"),     &kContrast},
}};

ImVec4 vec(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

}  // namespace

const Theme* themes(size_t* count)
{
    *count = kThemes.size();
    return kThemes.data();
}

const Theme& theme_named(std::string_view key)
{
    for (const Theme& theme : kThemes) {
        if (key == theme.key) {
            return theme;
        }
    }
    return kThemes.front();
}

ImU32 mix(ImU32 a, ImU32 b, float t)
{
    const ImVec4 va = vec(a);
    const ImVec4 vb = vec(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(va.x + (vb.x - va.x) * t, va.y + (vb.y - va.y) * t,
                                                 va.z + (vb.z - va.z) * t, va.w + (vb.w - va.w) * t));
}

void apply_to_imgui(const Palette& p, ImGuiStyle& style)
{
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = vec(p.text);
    c[ImGuiCol_TextDisabled]         = vec(p.faint);
    c[ImGuiCol_WindowBg]             = vec(p.panel);
    c[ImGuiCol_ChildBg]              = vec(IM_COL32_BLACK_TRANS);
    c[ImGuiCol_PopupBg]              = vec(p.panel);
    c[ImGuiCol_Border]               = vec(p.edge);
    c[ImGuiCol_BorderShadow]         = vec(IM_COL32_BLACK_TRANS);
    c[ImGuiCol_FrameBg]              = vec(p.card);
    c[ImGuiCol_FrameBgHovered]       = vec(mix(p.card, p.text, 0.08f));
    c[ImGuiCol_FrameBgActive]        = vec(mix(p.card, p.text, 0.14f));
    c[ImGuiCol_TitleBg]              = vec(p.panel);
    c[ImGuiCol_TitleBgActive]        = vec(p.panel);
    c[ImGuiCol_TitleBgCollapsed]     = vec(p.panel);
    c[ImGuiCol_MenuBarBg]            = vec(p.panel);
    c[ImGuiCol_ScrollbarBg]          = vec(IM_COL32_BLACK_TRANS);
    c[ImGuiCol_ScrollbarGrab]        = vec(with_alpha(p.text, 0.18f));
    c[ImGuiCol_ScrollbarGrabHovered] = vec(with_alpha(p.text, 0.30f));
    c[ImGuiCol_ScrollbarGrabActive]  = vec(with_alpha(p.text, 0.42f));
    c[ImGuiCol_CheckMark]            = vec(p.accent);
    c[ImGuiCol_SliderGrab]           = vec(p.accent);
    c[ImGuiCol_SliderGrabActive]     = vec(p.active);
    c[ImGuiCol_Button]               = vec(p.card);
    c[ImGuiCol_ButtonHovered]        = vec(mix(p.card, p.text, 0.10f));
    c[ImGuiCol_ButtonActive]         = vec(p.active);
    c[ImGuiCol_Header]               = vec(with_alpha(p.accent, 0.35f));
    c[ImGuiCol_HeaderHovered]        = vec(with_alpha(p.accent, 0.50f));
    c[ImGuiCol_HeaderActive]         = vec(with_alpha(p.accent, 0.65f));
    c[ImGuiCol_Separator]            = vec(p.line);
    c[ImGuiCol_SeparatorHovered]     = vec(p.edge);
    c[ImGuiCol_SeparatorActive]      = vec(p.accent);
    c[ImGuiCol_ResizeGrip]           = vec(IM_COL32_BLACK_TRANS);
    c[ImGuiCol_ResizeGripHovered]    = vec(with_alpha(p.accent, 0.5f));
    c[ImGuiCol_ResizeGripActive]     = vec(p.accent);
    c[ImGuiCol_Tab]                  = vec(p.card);
    c[ImGuiCol_TabHovered]           = vec(with_alpha(p.accent, 0.5f));
    c[ImGuiCol_TabSelected]          = vec(p.accent);
    c[ImGuiCol_TabSelectedOverline]  = vec(p.accent);
    c[ImGuiCol_TabDimmed]            = vec(p.card);
    c[ImGuiCol_TabDimmedSelected]    = vec(p.card);
    c[ImGuiCol_TableHeaderBg]        = vec(p.card);
    c[ImGuiCol_TableBorderStrong]    = vec(p.edge);
    c[ImGuiCol_TableBorderLight]     = vec(p.line);
    c[ImGuiCol_TableRowBg]           = vec(IM_COL32_BLACK_TRANS);
    c[ImGuiCol_TableRowBgAlt]        = vec(with_alpha(p.text, 0.03f));
    c[ImGuiCol_TextLink]             = vec(p.accent);
    c[ImGuiCol_TextSelectedBg]       = vec(with_alpha(p.accent, 0.35f));
    c[ImGuiCol_DragDropTarget]       = vec(p.accent);
    c[ImGuiCol_NavCursor]            = vec(IM_COL32_BLACK_TRANS);  // the kit draws its own ring
    c[ImGuiCol_NavWindowingHighlight] = vec(with_alpha(p.text, 0.7f));
    c[ImGuiCol_NavWindowingDimBg]    = vec(with_alpha(p.backdrop, 0.2f));
    c[ImGuiCol_ModalWindowDimBg]     = vec(with_alpha(p.backdrop, 0.6f));
}

}  // namespace sm2::osd::ui
