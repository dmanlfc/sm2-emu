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
#include "osd/ui/kit.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace sm2::osd::ui {

namespace {

// Sizes in points; px() scales them.
constexpr float kRowPadX     = 12.0f;  ///< inside a group frame, left and right
constexpr float kRowPadY     = 8.0f;   ///< above and below a row's content
constexpr float kRowGap      = 14.0f;  ///< between the texts and the control
constexpr float kGroupAbove  = 16.0f;  ///< above a group title
constexpr float kGroupBelow  = 6.0f;   ///< between the title and the frame
constexpr float kFrameRound  = 6.0f;
constexpr float kCtrlRound   = 4.0f;
constexpr float kCtrlH       = 24.0f;  ///< a control's height
constexpr float kSegPadX     = 11.0f;  ///< inside a segment, each side
constexpr float kSegMinW     = 40.0f;
constexpr float kSwitchW     = 38.0f;
constexpr float kSwitchH     = 20.0f;
constexpr float kTrackW      = 150.0f;
constexpr float kTrackH      = 4.0f;
constexpr float kKnobR       = 7.0f;
constexpr float kButtonPadX  = 12.0f;
constexpr float kDropW       = 190.0f;
constexpr float kFieldW      = 230.0f;
constexpr float kNumberW     = 72.0f;
constexpr float kTabPadX     = 14.0f;
constexpr float kTabH        = 34.0f;
constexpr float kIconSide    = 14.0f;
constexpr float kBesideShare = 0.52f;  ///< a control wider than this share of the row goes under the texts
constexpr float kSmallShare  = 0.86f;  ///< the small text's size against the font's

bool nav_visible()
{
    return GImGui->NavCursorVisible;
}

const char* repeat_id(const char* given, const char* fallback)
{
    return given != nullptr && given[0] != '\0' ? given : fallback;
}

}  // namespace

Kit::Kit(const Palette& palette, float scale, NavState* nav) : m_p(palette), m_scale(scale), m_nav(nav) {}

// ---------------------------------------------------------------------------
// Navigation rows
// ---------------------------------------------------------------------------

void Kit::nav_row(bool free)
{
    if (m_nav == nullptr) {
        return;
    }
    m_nav->rows.push_back(free);
    m_focus_asked = m_nav->target_row == static_cast<int>(m_nav->rows.size()) - 1;
}

void Kit::nav_item()
{
    if (m_nav != nullptr && ImGui::IsItemFocused() && !m_nav->rows.empty()) {
        m_nav->focused_now = static_cast<int>(m_nav->rows.size()) - 1;
    }
}

void Kit::focus_if_asked()
{
    if (!m_focus_asked) {
        return;
    }
    m_focus_asked = false;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    // Neither ImGui's re-seeding of a lost focus nor a move of its own may
    // override this at the end of the frame.
    GImGui->NavInitRequest = false;
    ImGui::NavMoveRequestCancel();
    if (m_nav != nullptr) {
        m_nav->focused_now = static_cast<int>(m_nav->rows.size()) - 1;
        m_nav->target_row  = -1;
    }
}

void Kit::divider(bool free)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float padx  = px(kRowPadX);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top  = ImGui::GetCursorScreenPos();
    if (m_in_group && m_group_rows > 0) {
        list->AddLine(ImVec2(top.x + padx, top.y), ImVec2(top.x + width - padx, top.y), m_p.line, stroke());
    }
    ++m_group_rows;
    ImGui::Dummy(ImVec2(0.0f, px(kRowPadY)));
    nav_row(free);
}

float Kit::small() const
{
    return std::round(ImGui::GetFontSize() * kSmallShare);
}

float Kit::stroke() const
{
    return std::max(1.0f, std::round(m_scale));
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void Kit::icon(ImDrawList* list, Icon which, ImVec2 c, float side, ImU32 col, float w)
{
    const float h = side * 0.5f;
    const auto  P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    const auto  line = [&](float x0, float y0, float x1, float y1) {
        list->AddLine(P(x0, y0), P(x1, y1), col, w);
    };
    switch (which) {
        case Icon::None:
            break;
        case Icon::Display:
            list->AddRect(P(-1.0f, -0.8f), P(1.0f, 0.5f), col, side * 0.08f, 0, w);
            line(-0.45f, 0.9f, 0.45f, 0.9f);
            line(0.0f, 0.5f, 0.0f, 0.9f);
            break;
        case Icon::Audio: {
            const ImVec2 horn[] = {P(-1.0f, -0.35f), P(-0.45f, -0.35f), P(0.15f, -0.9f), P(0.15f, 0.9f),
                                   P(-0.45f, 0.35f), P(-1.0f, 0.35f)};
            list->AddPolyline(horn, 6, col, ImDrawFlags_Closed, w);
            list->AddBezierQuadratic(P(0.5f, -0.5f), P(0.95f, 0.0f), P(0.5f, 0.5f), col, w);
            break;
        }
        case Icon::Input: {
            // A pad: a rounded body with a stick dot and two buttons.
            list->AddRect(P(-1.0f, -0.5f), P(1.0f, 0.55f), col, side * 0.22f, 0, w);
            list->AddCircleFilled(P(-0.5f, 0.0f), side * 0.09f, col);
            list->AddCircleFilled(P(0.45f, -0.15f), side * 0.07f, col);
            list->AddCircleFilled(P(0.7f, 0.15f), side * 0.07f, col);
            break;
        }
        case Icon::Keyboard:
            list->AddRect(P(-1.0f, -0.6f), P(1.0f, 0.6f), col, side * 0.08f, 0, w);
            for (int i = 0; i < 3; ++i) {
                list->AddRectFilled(P(-0.7f + static_cast<float>(i) * 0.55f, -0.3f),
                                    P(-0.4f + static_cast<float>(i) * 0.55f, -0.05f), col);
            }
            line(-0.55f, 0.3f, 0.55f, 0.3f);
            break;
        case Icon::Link: {
            list->AddCircle(P(-0.55f, 0.0f), side * 0.22f, col, 0, w);
            list->AddCircle(P(0.55f, 0.0f), side * 0.22f, col, 0, w);
            line(-0.25f, 0.0f, 0.25f, 0.0f);
            break;
        }
        case Icon::Folder: {
            const ImVec2 tab[] = {P(-1.0f, 0.75f), P(-1.0f, -0.6f), P(-0.3f, -0.6f), P(-0.05f, -0.3f),
                                  P(1.0f, -0.3f), P(1.0f, 0.75f)};
            list->AddPolyline(tab, 6, col, ImDrawFlags_Closed, w);
            break;
        }
        case Icon::Disk:
            list->AddRect(P(-0.9f, -0.9f), P(0.9f, 0.9f), col, side * 0.08f, 0, w);
            list->AddRect(P(-0.5f, -0.9f), P(0.45f, -0.3f), col, 0.0f, 0, w);
            list->AddRectFilled(P(-0.5f, 0.2f), P(0.5f, 0.9f), col);
            break;
        case Icon::Gear: {
            list->AddCircle(P(0.0f, 0.0f), side * 0.28f, col, 0, w);
            for (int i = 0; i < 8; ++i) {
                const float a  = static_cast<float>(i) * 3.14159265f / 4.0f;
                const float ca = std::cos(a), sa = std::sin(a);
                line(ca * 0.55f, sa * 0.55f, ca * 0.95f, sa * 0.95f);
            }
            break;
        }
        case Icon::Info:
            list->AddCircle(P(0.0f, 0.0f), side * 0.46f, col, 0, w);
            line(0.0f, -0.1f, 0.0f, 0.55f);
            list->AddCircleFilled(P(0.0f, -0.5f), w * 0.9f, col);
            break;
        case Icon::Chevron:
            line(-0.5f, -0.3f, 0.0f, 0.3f);
            line(0.0f, 0.3f, 0.5f, -0.3f);
            break;
        case Icon::Reload: {
            list->PathArcTo(c, side * 0.4f, 0.6f, 5.6f);
            list->PathStroke(col, 0, w);
            line(0.75f, -0.55f, 0.95f, -0.05f);
            line(0.75f, -0.55f, 0.3f, -0.45f);
            break;
        }
        case Icon::Check:
            line(-0.7f, 0.05f, -0.2f, 0.55f);
            line(-0.2f, 0.55f, 0.75f, -0.5f);
            break;
        case Icon::Cross:
            line(-0.6f, -0.6f, 0.6f, 0.6f);
            line(-0.6f, 0.6f, 0.6f, -0.6f);
            break;
        case Icon::Question:
            list->AddCircle(P(0.0f, 0.0f), side * 0.48f, col, 0, w);
            list->AddBezierQuadratic(P(-0.3f, -0.3f), P(0.3f, -0.55f), P(0.0f, 0.05f), col, w);
            list->AddCircleFilled(P(0.0f, 0.5f), w * 0.9f, col);
            break;
    }
}

float Kit::heading_width(const char* text) const
{
    ImFont*     font    = ImGui::GetFont();
    const float size    = small();
    const float spacing = px(1.2f);
    float       width   = 0.0f;
    const char* s       = text;
    const char* end     = text + std::strlen(text);
    while (s < end) {
        unsigned int c    = 0;
        const int    used = ImTextCharFromUtf8(&c, s, end);
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        char buf[8] = {};
        ImTextCharToUtf8(buf, c);
        const int n = static_cast<int>(std::strlen(buf));
        width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf, buf + n).x + spacing;
        s += used > 0 ? used : 1;
    }
    return std::max(0.0f, width - spacing);
}

void Kit::heading(ImDrawList* list, ImVec2 at, ImU32 colour, const char* text) const
{
    ImFont*     font    = ImGui::GetFont();
    const float size    = small();
    const float spacing = px(1.2f);
    const char* s       = text;
    const char* end     = text + std::strlen(text);
    float       x       = at.x;
    while (s < end) {
        unsigned int c    = 0;
        const int    used = ImTextCharFromUtf8(&c, s, end);
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        char buf[8] = {};
        ImTextCharToUtf8(buf, c);
        const int n = static_cast<int>(std::strlen(buf));
        list->AddText(font, size, ImVec2(x, at.y), colour, buf, buf + n);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf, buf + n).x + spacing;
        s += used > 0 ? used : 1;
    }
}

void Kit::focus_ring(ImDrawList* list, ImVec2 a, ImVec2 b, float rounding) const
{
    const float out = px(2.0f);
    list->AddRect(ImVec2(a.x - out, a.y - out), ImVec2(b.x + out, b.y + out), m_p.accent,
                  rounding + out, 0, std::max(stroke(), px(1.5f)));
}

std::string Kit::fitted(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    std::string out;
    for (size_t i = 0; i < text.size();) {
        unsigned int c    = 0;
        const int    used = ImTextCharFromUtf8(&c, text.data() + i, text.data() + text.size());
        const size_t next = i + static_cast<size_t>(used > 0 ? used : 1);
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + next).x + dots > width) {
            break;
        }
        i = next;
        out.assign(text, 0, i);
    }
    return out + "...";
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

int Kit::steer(ImGuiID id)
{
    // Owned for the frame, so ImGui's navigation does not also move to the
    // item at that side.
    for (const ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft,
                               ImGuiKey_GamepadDpadRight}) {
        ImGui::SetKeyOwner(key, id);
    }
    int dir = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, id)
        || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, id)) {
        dir = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, id)
        || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, id)) {
        dir = 1;
    }
    return dir;
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

bool Kit::tabs(std::span<const Tab> tabs, int& current, bool compact)
{
    ImDrawList* list    = ImGui::GetWindowDrawList();
    const float h       = px(kTabH);
    const float side    = px(kIconSide);
    const float pad     = px(kTabPadX);
    const float gap     = px(6.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float avail   = ImGui::GetContentRegionAvail().x;

    // Widths: icon + label, or icon alone when compact and not current.
    float total = 0.0f;
    std::vector<float> widths(tabs.size());
    for (size_t i = 0; i < tabs.size(); ++i) {
        const bool  with_label = !compact || static_cast<int>(i) == current;
        const float label_w    = with_label ? ImGui::CalcTextSize(tabs[i].label).x : 0.0f;
        widths[i] = pad * 2.0f + side + (with_label ? gap + label_w : 0.0f);
        total += widths[i];
    }
    float x = origin.x + std::max(0.0f, std::floor((avail - total) * 0.5f));
    bool  changed = false;

    nav_row(false);
    ImGuiID focused_id = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const bool   on = static_cast<int>(i) == current;
        const ImVec2 a(x, origin.y);
        const ImVec2 b(x + widths[i], origin.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i));
        const bool clicked = ImGui::InvisibleButton("tab", ImVec2(widths[i], h), ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        if (on) focus_if_asked();
        nav_item();
        if (ImGui::IsItemFocused()) focused_id = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsItemFocused() && nav_visible();
        if (clicked && !on) {
            current = static_cast<int>(i);
            changed = true;
        }
        if (hovered && !on) {
            list->AddRectFilled(a, b, m_p.hover, px(kCtrlRound));
        }
        const ImU32 ink = on ? m_p.text : (hovered ? m_p.text : m_p.muted);
        const bool  with_label = !compact || on;
        float cx = a.x + pad + side * 0.5f;
        icon(list, tabs[i].icon, ImVec2(cx, (a.y + b.y) * 0.5f), side, on ? m_p.accent : ink, stroke());
        if (with_label) {
            const float ty = a.y + (h - ImGui::GetFontSize()) * 0.5f;
            list->AddText(ImVec2(a.x + pad + side + gap, ty), ink, tabs[i].label);
        }
        if (on) {
            const float uy = b.y - px(2.0f);
            list->AddRectFilled(ImVec2(a.x + pad * 0.5f, uy), ImVec2(b.x - pad * 0.5f, b.y), m_p.accent,
                                px(1.0f));
        }
        if (focused) {
            focus_ring(list, a, ImVec2(b.x, b.y - px(3.0f)), px(kCtrlRound));
        }
        x += widths[i];
    }
    // With focus on the strip, Left and Right change the page and keep the
    // focus on the page's tab.
    if (focused_id != 0) {
        const int dir = steer(focused_id);
        if (dir != 0) {
            const int next = std::clamp(current + dir, 0, static_cast<int>(tabs.size()) - 1);
            if (next != current) {
                current = next;
                changed = true;
            }
        }
        if (m_nav != nullptr && changed) m_nav->target_next = 0;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h));
    ImGui::Dummy(ImVec2(avail, 0.0f));
    return changed;
}

// ---------------------------------------------------------------------------
// Groups and rows
// ---------------------------------------------------------------------------

void Kit::begin_group(const char* title)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, px(kGroupAbove)));
    if (title != nullptr && title[0] != '\0') {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        heading(list, ImVec2(at.x + px(2.0f), at.y), m_p.faint, title);
        ImGui::Dummy(ImVec2(0.0f, small() + px(kGroupBelow)));
    }
    m_group_top  = ImGui::GetCursorScreenPos();
    m_group_rows = 0;
    m_in_group   = true;
    // Channel 1 takes the rows; channel 0 the frame behind them, drawn at the end.
    list->ChannelsSplit(2);
    list->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    // The frame's inner padding at the top.
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
}

void Kit::end_group()
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    ImGui::EndGroup();
    const float  width = ImGui::GetContentRegionAvail().x;
    const ImVec2 a     = m_group_top;
    const ImVec2 b(a.x + width, ImGui::GetItemRectMax().y);
    list->ChannelsSetCurrent(0);
    list->AddRectFilled(a, b, m_p.panel, px(kFrameRound));
    list->AddRect(a, b, m_p.edge, px(kFrameRound), 0, stroke());
    list->ChannelsMerge();
    m_in_group = false;
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
}

float Kit::texts_height(const char* name, const char* about, float width) const
{
    ImFont*     font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    float       h    = font->CalcTextSizeA(size, FLT_MAX, width, name).y;
    if (about != nullptr && about[0] != '\0') {
        h += px(2.0f) + font->CalcTextSizeA(small(), FLT_MAX, width, about).y;
    }
    return h;
}

void Kit::draw_texts(ImDrawList* list, ImVec2 at, float width, const char* name,
                     const char* about, const Opts& opts) const
{
    ImFont*     font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImU32 ink  = opts.disabled ? m_p.faint : m_p.text;
    list->AddText(font, size, at, ink, name, nullptr, width);
    const ImVec2 name_size = font->CalcTextSizeA(size, FLT_MAX, width, name);
    float        after     = at.x + name_size.x;
    if (opts.badge != nullptr && opts.badge[0] != '\0') {
        // A small pill after the name.
        const float  bw = font->CalcTextSizeA(small(), FLT_MAX, 0.0f, opts.badge).x + px(10.0f);
        const float  bh = small() + px(3.0f);
        const ImVec2 ba(after + px(8.0f), at.y + (size - bh) * 0.5f);
        const ImVec2 bb(ba.x + bw, ba.y + bh);
        list->AddRectFilled(ba, bb, with_alpha(m_p.accent, 0.18f), bh * 0.5f);
        list->AddText(font, small(), ImVec2(ba.x + px(5.0f), ba.y + px(1.5f)), m_p.accent, opts.badge);
        after = bb.x;
    }
    const_cast<Kit*>(this)->m_last_name_end = ImVec2(after, at.y + size * 0.5f);
    if (about != nullptr && about[0] != '\0') {
        list->AddText(font, small(), ImVec2(at.x, at.y + name_size.y + px(2.0f)),
                      opts.disabled ? m_p.faint : m_p.muted, about, nullptr, width);
    }
}

Kit::Placed Kit::begin_row(const char* name, const char* about, float w, float h, const Opts& opts)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    m_row_tip        = m_next_tip;
    m_next_tip       = nullptr;

    const float  padx  = px(kRowPadX);
    const float  pady  = px(kRowPadY);
    const ImVec2 top   = ImGui::GetCursorScreenPos();
    const float  width = ImGui::GetContentRegionAvail().x;
    if (m_in_group && m_group_rows > 0) {
        list->AddLine(ImVec2(top.x + padx, top.y), ImVec2(top.x + width - padx, top.y), m_p.line, stroke());
    }
    ++m_group_rows;

    const float inner    = width - padx * 2.0f;
    const bool  beside   = w <= inner * kBesideShare;
    const float ask_room = m_row_tip != nullptr ? px(20.0f) : 0.0f;
    const float text_w   = beside ? std::max(px(60.0f), inner - w - px(kRowGap) - ask_room) : inner - ask_room;
    const float text_h   = texts_height(name, about, text_w);

    Placed placed{};
    float  row_h = 0.0f;
    const ImVec2 text_at(top.x + padx, top.y + pady);
    if (beside) {
        row_h          = std::max(text_h, h) + pady * 2.0f;
        placed.control = ImVec2(top.x + width - padx - w, top.y + std::floor((row_h - h) * 0.5f));
        draw_texts(list, ImVec2(text_at.x, top.y + std::floor((row_h - text_h) * 0.5f)), text_w, name, about, opts);
    } else {
        row_h          = text_h + px(6.0f) + h + pady * 2.0f;
        placed.control = ImVec2(top.x + width - padx - w, top.y + pady + text_h + px(6.0f));
        draw_texts(list, text_at, text_w, name, about, opts);
    }
    placed.bottom  = top.y + row_h;
    m_row_hover_y0 = top.y;

    // The (?) after the name, for the mouse.
    if (m_row_tip != nullptr) {
        const float  r = px(6.5f);
        const ImVec2 c(m_last_name_end.x + px(8.0f) + r, m_last_name_end.y);
        icon(list, Icon::Question, c, r * 2.0f, m_p.faint, stroke());
        if (ImGui::IsMouseHoveringRect(ImVec2(c.x - r - px(3.0f), c.y - r - px(3.0f)),
                                       ImVec2(c.x + r + px(3.0f), c.y + r + px(3.0f)))) {
            ImGui::SetTooltip("%s", m_row_tip);
        }
    }

    // Reserve the row; the control is placed by its caller.
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(ImVec2(width, row_h));
    ImGui::SetCursorScreenPos(ImVec2(top.x, placed.bottom));
    return placed;
}

void Kit::end_row(const Placed& placed, bool control_hot)
{
    if (m_row_tip == nullptr) {
        return;
    }
    const ImVec2 top(ImGui::GetCursorScreenPos().x, m_row_hover_y0);
    const bool   row_hovered = ImGui::IsWindowHovered()
                          && ImGui::IsMouseHoveringRect(top, ImVec2(top.x + ImGui::GetContentRegionAvail().x,
                                                                    placed.bottom));
    if (control_hot || row_hovered) {
        m_focused_tip = m_row_tip;
    }
}

void Kit::paragraph(const char* title, const char* text)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float padx  = m_in_group ? px(kRowPadX) : px(2.0f);
    const float width = ImGui::GetContentRegionAvail().x - padx * 2.0f;
    ImVec2      at    = ImGui::GetCursorScreenPos();
    at.x += padx;
    at.y += px(kRowPadY);
    float h = px(kRowPadY);
    if (title != nullptr && title[0] != '\0') {
        heading(list, at, m_p.faint, title);
        at.y += small() + px(4.0f);
        h += small() + px(4.0f);
    }
    if (text != nullptr && text[0] != '\0') {
        const float th = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, width, text).y;
        list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, m_p.muted, text, nullptr, width);
        h += th;
    }
    h += px(kRowPadY);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
    if (m_in_group) ++m_group_rows;
}

void Kit::status(const char* text, ImU32 colour)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float padx  = px(kRowPadX);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top  = ImGui::GetCursorScreenPos();
    if (m_in_group && m_group_rows > 0) {
        list->AddLine(ImVec2(top.x + padx, top.y), ImVec2(top.x + width - padx, top.y), m_p.line, stroke());
    }
    ++m_group_rows;
    const float r = px(4.0f);
    const float th = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, width - padx * 2.0f - r * 4.0f, text).y;
    const float h  = th + px(kRowPadY) * 2.0f;
    list->AddCircleFilled(ImVec2(top.x + padx + r, top.y + h * 0.5f), r, colour);
    list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(top.x + padx + r * 4.0f, top.y + px(kRowPadY)),
                  m_p.text, text, nullptr, width - padx * 2.0f - r * 4.0f);
    ImGui::Dummy(ImVec2(width, h));
}

// ---------------------------------------------------------------------------
// Buttons and segments
// ---------------------------------------------------------------------------

ImVec2 Kit::button_size(const char* label, Icon ic) const
{
    const float text_w = label != nullptr && label[0] != '\0' ? ImGui::CalcTextSize(label).x : 0.0f;
    const float icon_w = ic != Icon::None ? px(kIconSide) + (text_w > 0.0f ? px(6.0f) : 0.0f) : 0.0f;
    return ImVec2(text_w + icon_w + px(kButtonPadX) * 2.0f, px(kCtrlH));
}

bool Kit::press_button(const char* label, ImVec2 at, ImVec2 size, Tone tone, Icon ic, bool enabled, bool on)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(at);
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    if (!enabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const bool   hovered = enabled && ImGui::IsItemHovered();
    const bool   held    = enabled && ImGui::IsItemActive();
    const bool   focused = ImGui::IsItemFocused() && nav_visible();
    const ImVec2 b(at.x + size.x, at.y + size.y);
    const float  r = px(kCtrlRound);

    ImU32 fill = m_p.card, edge = m_p.edge, ink = m_p.text;
    if (tone == Tone::Primary || on) {
        fill = m_p.active;
        edge = with_alpha(m_p.active, 0.0f);
        ink  = m_p.on_active;
    } else if (tone == Tone::Danger) {
        edge = with_alpha(m_p.bad, 0.8f);
        ink  = m_p.bad;
    }
    if (!enabled) {
        fill = with_alpha(fill, 0.5f);
        edge = with_alpha(edge, 0.5f);
        ink  = m_p.faint;
    } else if (held) {
        fill = mix(fill, m_p.text, 0.18f);
    } else if (hovered) {
        fill = mix(fill, m_p.text, 0.08f);
    }
    list->AddRectFilled(at, b, fill, r);
    if ((edge >> IM_COL32_A_SHIFT) != 0) {
        list->AddRect(at, b, edge, r, 0, stroke());
    }
    const float text_w = label != nullptr && label[0] != '\0' ? ImGui::CalcTextSize(label).x : 0.0f;
    float       x      = at.x + px(kButtonPadX);
    if (ic != Icon::None) {
        icon(list, ic, ImVec2(x + px(kIconSide) * 0.5f, (at.y + b.y) * 0.5f), px(kIconSide), ink, stroke());
        x += px(kIconSide) + (text_w > 0.0f ? px(6.0f) : 0.0f);
    }
    if (text_w > 0.0f) {
        list->AddText(ImVec2(x, at.y + (size.y - ImGui::GetFontSize()) * 0.5f), ink, label);
    }
    if (focused) {
        focus_ring(list, at, b, r);
    }
    return clicked && enabled;
}

bool Kit::button(const char* label, Tone tone, Icon ic, bool enabled)
{
    const ImVec2 at   = ImGui::GetCursorScreenPos();
    const ImVec2 size = button_size(label, ic);
    nav_row(false);
    const bool   out  = press_button(label, at, size, tone, ic, enabled);
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(size);
    return out;
}

bool Kit::button_at(const char* label, ImVec2 at, Tone tone, Icon ic, bool enabled)
{
    return press_button(label, at, button_size(label, ic), tone, ic, enabled);
}

ImVec2 Kit::segments_size(std::span<const char* const> labels) const
{
    float w = 0.0f;
    for (const char* label : labels) {
        w += std::max(px(kSegMinW), ImGui::CalcTextSize(label).x + px(kSegPadX) * 2.0f);
    }
    return ImVec2(w, px(kCtrlH));
}

bool Kit::segments_at(const char* id, ImVec2 at, std::span<const char* const> labels, int& value, bool enabled)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 size = segments_size(labels);
    const ImVec2 b(at.x + size.x, at.y + size.y);
    const float  r = px(kCtrlRound);
    const int    n = static_cast<int>(labels.size());
    bool changed = false;

    // One item for the whole control: focus lands on it as a unit, and
    // Left/Right move the choice. Clicks pick the cell under the mouse.
    if (enabled) nav_row(false);
    ImGui::SetCursorScreenPos(at);
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    if (!enabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const ImGuiID item    = ImGui::GetItemID();
    const bool    hovered = enabled && ImGui::IsItemHovered();
    const bool    focused = ImGui::IsItemFocused();

    // Cells.
    std::vector<float> xs(static_cast<size_t>(n) + 1);
    xs[0] = at.x;
    for (int i = 0; i < n; ++i) {
        xs[static_cast<size_t>(i) + 1] = xs[static_cast<size_t>(i)]
            + std::max(px(kSegMinW), ImGui::CalcTextSize(labels[static_cast<size_t>(i)]).x + px(kSegPadX) * 2.0f);
    }
    int hovered_cell = -1;
    if (hovered) {
        const float mx = ImGui::GetIO().MousePos.x;
        for (int i = 0; i < n; ++i) {
            if (mx >= xs[static_cast<size_t>(i)] && mx < xs[static_cast<size_t>(i) + 1]) hovered_cell = i;
        }
    }
    if (clicked && hovered_cell >= 0 && hovered_cell != value) {
        value   = hovered_cell;
        changed = true;
    }
    // Enter/A on the control with no mouse: step to the next choice.
    if (clicked && hovered_cell < 0 && n > 0) {
        value   = (value + 1) % n;
        changed = true;
    }
    if (focused && enabled) {
        const int dir = steer(item);
        if (dir != 0) {
            const int next = std::clamp(value + dir, 0, n - 1);
            if (next != value) {
                value   = next;
                changed = true;
            }
        }
    }

    const float alpha = enabled ? 1.0f : 0.5f;
    list->AddRectFilled(at, b, with_alpha(m_p.card, alpha), r);
    for (int i = 0; i < n; ++i) {
        const ImVec2 ca(xs[static_cast<size_t>(i)], at.y);
        const ImVec2 cb(xs[static_cast<size_t>(i) + 1], b.y);
        const bool   on = i == value;
        if (on) {
            const ImDrawFlags corners = (i == 0 ? ImDrawFlags_RoundCornersLeft : 0)
                                      | (i == n - 1 ? ImDrawFlags_RoundCornersRight : 0);
            list->AddRectFilled(ca, cb, with_alpha(m_p.active, alpha), r, corners == 0 ? ImDrawFlags_RoundCornersNone : corners);
        } else if (i == hovered_cell) {
            list->AddRectFilled(ca, cb, m_p.hover, 0.0f);
        }
        if (i > 0 && !on && i - 1 != value) {
            list->AddLine(ImVec2(ca.x, ca.y + px(5.0f)), ImVec2(ca.x, cb.y - px(5.0f)), m_p.line, stroke());
        }
        const ImVec2 ts = ImGui::CalcTextSize(labels[static_cast<size_t>(i)]);
        const ImU32  ink = !enabled ? m_p.faint : on ? m_p.on_active : m_p.muted;
        list->AddText(ImVec2(std::floor(ca.x + (cb.x - ca.x - ts.x) * 0.5f), std::floor(at.y + (size.y - ts.y) * 0.5f)),
                      ink, labels[static_cast<size_t>(i)]);
    }
    list->AddRect(at, b, with_alpha(m_p.edge, alpha), r, 0, stroke());
    if (focused && nav_visible()) {
        focus_ring(list, at, b, r);
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

bool Kit::toggle(const char* name, const char* about, bool& value, const Opts& opts)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 size(px(kSwitchW), px(kSwitchH));
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    const ImVec2 at     = placed.control;
    const ImVec2 b(at.x + size.x, at.y + size.y);

    ImGui::PushID(repeat_id(opts.id, name));
    ImGui::SetCursorScreenPos(at);
    if (!opts.disabled) nav_row(false);
    if (opts.disabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton("switch", size, ImGuiButtonFlags_EnableNav);
    if (opts.disabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const ImGuiID item    = ImGui::GetItemID();
    const bool    hovered = !opts.disabled && ImGui::IsItemHovered();
    const bool    focused = ImGui::IsItemFocused();
    ImGui::PopID();

    bool changed = false;
    if (clicked) {
        value   = !value;
        changed = true;
    }
    if (focused && !opts.disabled) {
        const int dir = steer(item);
        if (dir < 0 && value) { value = false; changed = true; }
        if (dir > 0 && !value) { value = true; changed = true; }
    }

    const float alpha = opts.disabled ? 0.5f : 1.0f;
    const float r     = size.y * 0.5f;
    ImU32 track = value ? m_p.active : m_p.card;
    if (hovered) track = mix(track, m_p.text, 0.08f);
    list->AddRectFilled(at, b, with_alpha(track, alpha), r);
    if (!value) list->AddRect(at, b, with_alpha(m_p.edge, alpha), r, 0, stroke());
    const float kr = r - px(3.0f);
    const float kx = value ? b.x - r : at.x + r;
    list->AddCircleFilled(ImVec2(kx, at.y + r), kr, with_alpha(value ? m_p.on_active : m_p.muted, alpha));
    if (focused && nav_visible()) focus_ring(list, at, b, r);

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hovered || focused);
    return changed;
}

bool Kit::choice(const char* name, const char* about, std::span<const char* const> labels, int& value,
                 const Opts& opts)
{
    const ImVec2 size   = segments_size(labels);
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    ImGui::PushID(repeat_id(opts.id, name));
    const bool changed = segments_at("segments", placed.control, labels, value, !opts.disabled);
    const bool hot     = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hot);
    return changed;
}

bool Kit::slider(const char* name, const char* about, int& value, int lo, int hi, int step, const char* shown,
                 const Opts& opts)
{
    ImDrawList* list    = ImGui::GetWindowDrawList();
    const float value_w = std::max(ImGui::CalcTextSize(shown).x, ImGui::CalcTextSize("100%").x) + px(8.0f);
    const float track_w = px(kTrackW);
    const ImVec2 size(track_w + px(8.0f) + value_w, px(kCtrlH));
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    const ImVec2 at     = placed.control;

    ImGui::PushID(repeat_id(opts.id, name));
    ImGui::SetCursorScreenPos(at);
    if (!opts.disabled) nav_row(false);
    if (opts.disabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton("track", ImVec2(track_w, size.y), ImGuiButtonFlags_EnableNav);
    if (opts.disabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const ImGuiID item    = ImGui::GetItemID();
    const bool    hovered = !opts.disabled && ImGui::IsItemHovered();
    const bool    active  = !opts.disabled && ImGui::IsItemActive();
    const bool    focused = ImGui::IsItemFocused();
    ImGui::PopID();

    bool changed = false;
    const float x0 = at.x + px(kKnobR), x1 = at.x + track_w - px(kKnobR);
    if (active) {
        const float t   = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        int         raw = lo + static_cast<int>(std::lround(t * static_cast<float>(hi - lo)));
        if (step > 1) raw = lo + ((raw - lo + step / 2) / step) * step;
        raw = std::clamp(raw, lo, hi);
        if (raw != value) { value = raw; changed = true; }
    }
    if (focused && !opts.disabled) {
        const int dir = steer(item);
        if (dir != 0) {
            const int next = std::clamp(value + dir * step, lo, hi);
            if (next != value) { value = next; changed = true; }
        }
    }

    const float alpha = opts.disabled ? 0.5f : 1.0f;
    const float cy    = at.y + size.y * 0.5f;
    const float t     = hi > lo ? static_cast<float>(value - lo) / static_cast<float>(hi - lo) : 0.0f;
    const float kx    = x0 + (x1 - x0) * t;
    list->AddRectFilled(ImVec2(x0, cy - px(kTrackH) * 0.5f), ImVec2(x1, cy + px(kTrackH) * 0.5f),
                        with_alpha(m_p.edge, alpha), px(kTrackH));
    list->AddRectFilled(ImVec2(x0, cy - px(kTrackH) * 0.5f), ImVec2(kx, cy + px(kTrackH) * 0.5f),
                        with_alpha(m_p.accent, alpha), px(kTrackH));
    const float kr = px(kKnobR) * (active ? 1.15f : 1.0f);
    list->AddCircleFilled(ImVec2(kx, cy), kr, with_alpha(hovered || active ? m_p.text : m_p.accent, alpha));
    if (focused && nav_visible()) {
        list->AddCircle(ImVec2(kx, cy), kr + px(3.0f), m_p.accent, 0, std::max(stroke(), px(1.5f)));
    }
    list->AddText(ImVec2(at.x + track_w + px(8.0f), at.y + (size.y - ImGui::GetFontSize()) * 0.5f),
                  opts.disabled ? m_p.faint : m_p.text, shown);

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hovered || focused || active);
    return changed;
}

bool Kit::dropdown(const char* name, const char* about, std::span<const std::string> labels, int& value,
                   const Opts& opts)
{
    return dropdown_impl(name, about, labels, value, nullptr, 0, opts);
}

bool Kit::dropdown(const char* name, const char* about, std::span<const std::string> labels, int& value,
                   char* filter, size_t filter_size, const Opts& opts)
{
    return dropdown_impl(name, about, labels, value, filter, filter_size, opts);
}

bool Kit::dropdown_impl(const char* name, const char* about, std::span<const std::string> labels, int& value,
                        char* filter, size_t filter_size, const Opts& opts)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    float        w   = px(kDropW);
    for (const std::string& label : labels) {
        w = std::max(w, ImGui::CalcTextSize(label.c_str()).x + px(kButtonPadX) * 2.0f + px(kIconSide) + px(8.0f));
    }
    w = std::min(w, ImGui::GetContentRegionAvail().x - px(kRowPadX) * 2.0f);
    const ImVec2 size(w, px(kCtrlH));
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    const ImVec2 at     = placed.control;
    const ImVec2 b(at.x + size.x, at.y + size.y);
    const float  r = px(kCtrlRound);

    ImGui::PushID(repeat_id(opts.id, name));
    ImGui::SetCursorScreenPos(at);
    if (!opts.disabled) nav_row(false);
    if (opts.disabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton("drop", size, ImGuiButtonFlags_EnableNav);
    if (opts.disabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const ImGuiID item    = ImGui::GetItemID();
    const bool    hovered = !opts.disabled && ImGui::IsItemHovered();
    const bool    focused = ImGui::IsItemFocused();
    bool          changed = false;
    const int     n       = static_cast<int>(labels.size());

    if (clicked) {
        ImGui::OpenPopup("list");
    }
    if (focused && !opts.disabled && !ImGui::IsPopupOpen("list")) {
        const int dir = steer(item);
        if (dir != 0 && n > 0) {
            const int next = std::clamp(value + dir, 0, n - 1);
            if (next != value) { value = next; changed = true; }
        }
    }

    const float alpha = opts.disabled ? 0.5f : 1.0f;
    ImU32       fill  = m_p.card;
    if (hovered) fill = mix(fill, m_p.text, 0.08f);
    list->AddRectFilled(at, b, with_alpha(fill, alpha), r);
    list->AddRect(at, b, with_alpha(m_p.edge, alpha), r, 0, stroke());
    const std::string current = value >= 0 && value < n ? labels[static_cast<size_t>(value)] : std::string();
    const std::string shown   = fitted(current, size.x - px(kButtonPadX) * 2.0f - px(kIconSide) - px(6.0f));
    list->AddText(ImVec2(at.x + px(kButtonPadX), at.y + (size.y - ImGui::GetFontSize()) * 0.5f),
                  opts.disabled ? m_p.faint : m_p.text, shown.c_str());
    icon(list, Icon::Chevron, ImVec2(b.x - px(kButtonPadX) - px(kIconSide) * 0.5f + px(2.0f), (at.y + b.y) * 0.5f),
         px(kIconSide), opts.disabled ? m_p.faint : m_p.muted, stroke());
    if (focused && nav_visible()) focus_ring(list, at, b, r);

    ImGui::SetNextWindowPos(ImVec2(at.x, b.y + px(2.0f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(size.x, 0.0f), ImVec2(size.x * 1.6f, px(360.0f)));
    if (ImGui::BeginPopup("list")) {
        std::string needle;
        if (filter != nullptr && filter_size > 0) {
            if (ImGui::IsWindowAppearing()) {
                filter[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "...", filter, filter_size);
            needle = filter;
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            ImGui::Separator();
        }
        for (int i = 0; i < n; ++i) {
            const std::string& label = labels[static_cast<size_t>(i)];
            if (!needle.empty()) {
                std::string hay = label;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (hay.find(needle) == std::string::npos) continue;
            }
            const bool on = i == value;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), on)) {
                if (!on) { value = i; changed = true; }
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            if (on && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hovered || focused);
    return changed;
}

bool Kit::number(const char* name, const char* about, int& value, int lo, int hi, int step, const Opts& opts)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float  bw  = px(kCtrlH);
    const ImVec2 size(px(kNumberW) + bw * 2.0f + px(4.0f) * 2.0f, px(kCtrlH));
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    const ImVec2 at     = placed.control;
    bool changed = false;

    ImGui::PushID(repeat_id(opts.id, name));
    if (!opts.disabled) nav_row(false);
    if (opts.disabled) ImGui::BeginDisabled();
    // Minus, the typed field, plus.
    if (press_button("-", at, ImVec2(bw, bw), Tone::Plain, Icon::None, !opts.disabled)) {
        const int next = std::clamp(value - step, lo, hi);
        if (next != value) { value = next; changed = true; }
    }
    const ImVec2 fa(at.x + bw + px(4.0f), at.y);
    ImGui::SetCursorScreenPos(fa);
    ImGui::SetNextItemWidth(px(kNumberW));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(6.0f), (px(kCtrlH) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(kCtrlRound));
    int typed = value;
    if (ImGui::InputScalar("##n", ImGuiDataType_S32, &typed, nullptr, nullptr, "%d",
                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        const int next = std::clamp(typed, lo, hi);
        if (next != value) { value = next; changed = true; }
    }
    nav_item();
    const bool field_focused = ImGui::IsItemFocused();
    const bool field_hot     = ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    if (field_focused && nav_visible() && !ImGui::IsItemActive()) {
        focus_ring(list, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), px(kCtrlRound));
    }
    if (press_button("+", ImVec2(fa.x + px(kNumberW) + px(4.0f), at.y), ImVec2(bw, bw), Tone::Plain, Icon::None,
                     !opts.disabled)) {
        const int next = std::clamp(value + step, lo, hi);
        if (next != value) { value = next; changed = true; }
    }
    if (opts.disabled) ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, field_hot || field_focused);
    return changed;
}

bool Kit::field(const char* name, const char* about, std::string& text, const char* hint, const char* button_label,
                bool* clicked, const Opts& opts)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x - px(kRowPadX) * 2.0f;
    const ImVec2 bsize = button_label != nullptr ? button_size(button_label) : ImVec2(0.0f, 0.0f);
    float        fw    = std::max(px(kFieldW), std::floor(avail * kBesideShare) - bsize.x
                                                   - (button_label != nullptr ? px(6.0f) : 0.0f));
    // A field goes under its texts on a narrow row; then it takes the width.
    const bool under = fw + bsize.x > avail * kBesideShare;
    if (under) fw = avail - bsize.x - (button_label != nullptr ? px(6.0f) : 0.0f);
    const ImVec2 size(fw + (button_label != nullptr ? px(6.0f) + bsize.x : 0.0f), px(kCtrlH));
    const Placed placed = begin_row(name, about, size.x, size.y, opts);
    const ImVec2 at     = placed.control;
    bool changed = false;

    ImGui::PushID(repeat_id(opts.id, name));
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemWidth(fw);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8.0f), (px(kCtrlH) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(kCtrlRound));
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", text.c_str());
    if (!opts.disabled) nav_row(false);
    if (opts.disabled) ImGui::BeginDisabled();
    if (ImGui::InputTextWithHint("##f", hint != nullptr ? hint : "", buf, sizeof buf)) {
        text    = buf;
        changed = true;
    }
    if (opts.disabled) ImGui::EndDisabled();
    focus_if_asked();
    nav_item();
    const bool focused = ImGui::IsItemFocused();
    const bool hot     = ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    if (focused && nav_visible() && !ImGui::IsItemActive()) {
        focus_ring(list, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), px(kCtrlRound));
    }
    if (button_label != nullptr) {
        const bool pressed = press_button(button_label, ImVec2(at.x + fw + px(6.0f), at.y), bsize, Tone::Plain,
                                          Icon::None, !opts.disabled);
        if (clicked != nullptr) *clicked = pressed;
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hot || focused);
    return changed;
}

void Kit::info(const char* name, const char* about, const char* value, ImU32 colour, const Opts& opts)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x - px(kRowPadX) * 2.0f;
    const float vw    = std::min(ImGui::CalcTextSize(value).x, avail);
    const Placed placed = begin_row(name, about, vw, ImGui::GetFontSize(), opts);
    const std::string shown = fitted(value, vw);
    list->AddText(placed.control, colour != 0 ? colour : (opts.disabled ? m_p.faint : m_p.muted), shown.c_str());
    if (shown != value && ImGui::IsMouseHoveringRect(placed.control,
                                                     ImVec2(placed.control.x + vw, placed.control.y + ImGui::GetFontSize()))) {
        ImGui::SetTooltip("%s", value);
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, false);
}

int Kit::actions(const char* name, const char* about, std::span<const char* const> labels,
                 std::span<const bool> enabled, Tone first, const Opts& opts)
{
    float w = 0.0f;
    for (size_t i = 0; i < labels.size(); ++i) {
        w += button_size(labels[i]).x + (i > 0 ? px(6.0f) : 0.0f);
    }
    const Placed placed = begin_row(name, about, w, px(kCtrlH), opts);
    float x       = placed.control.x;
    int   clicked = -1;
    bool  hot     = false;
    ImGui::PushID(repeat_id(opts.id, name));
    bool any = enabled.empty();
    for (const bool on : enabled) any = any || on;
    if (!opts.disabled && any) nav_row(false);
    for (size_t i = 0; i < labels.size(); ++i) {
        const bool   on   = enabled.empty() || (i < enabled.size() && enabled[i]);
        const ImVec2 size = button_size(labels[i]);
        ImGui::PushID(static_cast<int>(i));
        if (press_button(labels[i], ImVec2(x, placed.control.y), size, i == 0 ? first : Tone::Plain, Icon::None,
                         on && !opts.disabled)) {
            clicked = static_cast<int>(i);
        }
        hot = hot || ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImGui::PopID();
        x += size.x + px(6.0f);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hot);
    return clicked;
}

int Kit::binding(const char* name, const char* about, const char* value, bool capturing,
                 std::span<const char* const> buttons, const Opts& opts)
{
    ImDrawList* list  = ImGui::GetWindowDrawList();
    const float cap_w = std::max(px(110.0f), ImGui::CalcTextSize(value).x + px(kButtonPadX) * 2.0f);
    float       w     = cap_w;
    for (const char* label : buttons) {
        w += px(6.0f) + button_size(label).x;
    }
    const Placed placed = begin_row(name, about, w, px(kCtrlH), opts);
    const ImVec2 at     = placed.control;
    int  out = -1;
    bool hot = false;

    ImGui::PushID(repeat_id(opts.id, name));
    if (!opts.disabled) nav_row(false);
    // The cap: a key with a thicker bottom edge; waiting, in the accent.
    {
        const ImVec2 size(cap_w, px(kCtrlH));
        ImGui::SetCursorScreenPos(at);
        if (opts.disabled) ImGui::BeginDisabled();
        const bool clicked = ImGui::InvisibleButton("cap", size, ImGuiButtonFlags_EnableNav);
        if (opts.disabled) ImGui::EndDisabled();
        focus_if_asked();
        nav_item();
        const bool hovered = !opts.disabled && ImGui::IsItemHovered();
        const bool focused = ImGui::IsItemFocused();
        hot = hovered || focused;
        if (clicked) out = 0;
        const ImVec2 b(at.x + size.x, at.y + size.y);
        const float  r = px(kCtrlRound);
        const float  alpha = opts.disabled ? 0.5f : 1.0f;
        ImU32 fill = capturing ? with_alpha(m_p.accent, 0.25f) : m_p.card;
        if (hovered && !capturing) fill = mix(fill, m_p.text, 0.08f);
        list->AddRectFilled(at, b, with_alpha(fill, alpha), r);
        list->AddRect(at, b, with_alpha(capturing ? m_p.accent : m_p.edge, alpha), r, 0, stroke());
        if (!capturing) {
            list->AddLine(ImVec2(at.x + r, b.y - px(1.0f)), ImVec2(b.x - r, b.y - px(1.0f)),
                          with_alpha(m_p.edge, alpha), px(2.0f));
        }
        const ImVec2 ts = ImGui::CalcTextSize(value);
        list->AddText(ImVec2(std::floor(at.x + (size.x - ts.x) * 0.5f), std::floor(at.y + (size.y - ts.y) * 0.5f)),
                      opts.disabled ? m_p.faint : capturing ? m_p.accent : m_p.text, value);
        if (focused && nav_visible()) focus_ring(list, at, b, r);
    }
    float x = at.x + cap_w;
    for (size_t i = 0; i < buttons.size(); ++i) {
        x += px(6.0f);
        const ImVec2 size = button_size(buttons[i]);
        ImGui::PushID(static_cast<int>(i));
        if (press_button(buttons[i], ImVec2(x, at.y), size, Tone::Plain, Icon::None, !opts.disabled)) {
            out = static_cast<int>(i) + 1;
        }
        ImGui::PopID();
        hot = hot || ImGui::IsItemHovered() || ImGui::IsItemFocused();
        x += size.x;
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, hot);
    return out;
}

ImVec2 Kit::custom(const char* name, const char* about, float width, float height, const Opts& opts)
{
    const Placed placed = begin_row(name, about, width, height, opts);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, placed.bottom));
    end_row(placed, false);
    return placed.control;
}

}  // namespace sm2::osd::ui
