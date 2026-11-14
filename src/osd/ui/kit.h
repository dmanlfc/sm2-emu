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

#include "osd/ui/theme.h"

#include <imgui.h>

#include <span>
#include <string>
#include <vector>

namespace sm2::osd::ui {

/// What else a row of the kit may be told.
struct RowOpts {
    bool        disabled = false;   ///< drawn faint, not interactive
    const char* id       = nullptr; ///< ImGui ID when the name repeats in a group
    const char* badge    = nullptr; ///< a short word after the name, e.g. "next launch"
};

/// Where keyboard and gamepad focus is among the kit's rows, kept by the owner
/// between frames so Up and Down walk the rows in the order they are drawn
/// rather than by ImGui's spatial search. A "free" row (the gamepad picture)
/// keeps ImGui's own movement inside it.
struct NavState {
    std::vector<bool> free_rows;      ///< the rows drawn last frame: free or not
    int               focused_row = -1;  ///< the row holding ImGui's focused item, last frame
    int               target_row  = -1;  ///< the row to move focus to this frame, or -1
    int               target_next = -1;  ///< a row asked for while drawing, for the next frame
    // Rebuilt while a frame draws:
    std::vector<bool> rows;
    int               focused_now = -1;
};

/// The settings widgets, drawn with ImGui's draw lists in the theme's colours.
///
/// A page is a column of groups; a group is a framed column of rows. A row has
/// a name, an optional explanation under it, and its control at the right (or
/// under the texts when the row is too narrow for both). Every string passed
/// in is already translated.
///
/// Controls are ImGui items, so the mouse, the keyboard and a gamepad all work
/// through ImGui's own navigation: Up/Down or the D-pad move between them,
/// Enter or A presses, and Left/Right or the D-pad change the focused control
/// in place (the kit takes those keys from navigation while a control has
/// focus). The kit draws its own focus ring; ImGui's is turned off by the
/// theme.
///
/// One Kit is made per frame.
class Kit {
public:
    enum class Tone { Plain, Primary, Danger };

    /// The small vector icons the kit draws itself, so they take the theme's
    /// colours and need no image.
    enum class Icon {
        None, Display, Audio, Input, Keyboard, Link, Folder, Disk, Gear, Info,
        Chevron, Reload, Check, Cross, Question
    };

    struct Tab {
        const char* label;
        Icon        icon;
    };

    using Opts = RowOpts;

    Kit(const Palette& palette, float scale, NavState* nav = nullptr);

    [[nodiscard]] const Palette& palette() const { return m_p; }
    /// Points to pixels at the overlay's scale.
    [[nodiscard]] float px(float points) const { return points * m_scale; }
    /// The small text size in pixels, for explanations and hints.
    [[nodiscard]] float small() const;
    /// One stroke, in whole pixels.
    [[nodiscard]] float stroke() const;

    // -- drawing helpers ---------------------------------------------------

    static void icon(ImDrawList* list, Icon icon, ImVec2 centre, float side, ImU32 colour,
                     float stroke);
    /// Text in capitals (ASCII letters) at the small size, letters a little
    /// apart: group titles and the small headings.
    void heading(ImDrawList* list, ImVec2 at, ImU32 colour, const char* text) const;
    [[nodiscard]] float heading_width(const char* text) const;
    /// A ring in the accent colour around a focused control.
    void focus_ring(ImDrawList* list, ImVec2 a, ImVec2 b, float rounding) const;
    /// Text cut to `width`, with an ellipsis when cut.
    [[nodiscard]] static std::string fitted(const std::string& text, float width);

    // -- page pieces -------------------------------------------------------

    /// A strip of tabs across the available width. `compact` shows only the
    /// current tab's label beside its icon. True when the choice changed.
    bool tabs(std::span<const Tab> tabs, int& current, bool compact);

    void begin_group(const char* title);
    void end_group();

    /// The help of the next row: shown in a tooltip from its (?) and in the
    /// page foot while the row has focus (see focused_tip()).
    void tip(const char* text) { m_next_tip = text; }

    /// A free paragraph inside or outside a group: a heading and wrapped text.
    void paragraph(const char* title, const char* text);
    /// A line of status text in a group, coloured.
    void status(const char* text, ImU32 colour);

    // -- rows (true when the value changed) --------------------------------

    bool toggle(const char* name, const char* about, bool& value, const Opts& opts = {});
    bool choice(const char* name, const char* about, std::span<const char* const> labels,
                int& value, const Opts& opts = {});
    /// `shown` is the value's text, e.g. "40%"; `step` is what Left/Right move by.
    bool slider(const char* name, const char* about, int& value, int lo, int hi, int step,
                const char* shown, const Opts& opts = {});
    bool dropdown(const char* name, const char* about, std::span<const std::string> labels,
                  int& value, const Opts& opts = {});
    /// A typed number with a +/- stepper.
    bool number(const char* name, const char* about, int& value, int lo, int hi, int step,
                const Opts& opts = {});
    /// A text field, `hint` faint while empty; `button` adds one after it and
    /// `*clicked` says whether it was. True when the text changed.
    bool field(const char* name, const char* about, std::string& text, const char* hint,
               const char* button = nullptr, bool* clicked = nullptr, const Opts& opts = {});
    /// A read-only value.
    void info(const char* name, const char* about, const char* value, ImU32 colour = 0,
              const Opts& opts = {});
    /// Buttons at the right; the index clicked, or -1. `enabled` may be empty
    /// (all enabled) or one per button. The first button takes `first`.
    int actions(const char* name, const char* about, std::span<const char* const> labels,
                std::span<const bool> enabled = {}, Tone first = Tone::Plain,
                const Opts& opts = {});
    /// A binding: its value in a cap (a key, a button, an axis), which a click
    /// or press starts capturing (`capturing` shows it waiting), with buttons
    /// after it. 0 for the cap, 1.. for the buttons in order, -1 for none.
    int binding(const char* name, const char* about, const char* value, bool capturing,
                std::span<const char* const> buttons, const Opts& opts = {});
    /// A searchable dropdown: `filter` is the typed text, kept by the caller.
    bool dropdown(const char* name, const char* about, std::span<const std::string> labels,
                  int& value, char* filter, size_t filter_size, const Opts& opts = {});
    /// A row's separator line, before content a page draws itself inside a
    /// group (and the start of a navigation row for it; `free` keeps ImGui's
    /// own movement among its items).
    void divider(bool free = false);
    /// Right after the first focusable item of content drawn by hand: moves
    /// focus to it when Up/Down asked for this row.
    void focus_if_asked();
    /// After each item of content drawn by hand: notes where focus is.
    void nav_item();
    /// A row whose control the caller draws: its top-left corner, `width`
    /// wide and `height` high.
    ImVec2 custom(const char* name, const char* about, float width, float height,
                  const Opts& opts = {});

    // -- controls on their own -------------------------------------------

    [[nodiscard]] ImVec2 button_size(const char* label, Icon icon = Icon::None) const;
    bool button(const char* label, Tone tone = Tone::Plain, Icon icon = Icon::None,
                bool enabled = true);
    bool button_at(const char* label, ImVec2 at, Tone tone = Tone::Plain, Icon icon = Icon::None,
                   bool enabled = true);
    [[nodiscard]] ImVec2 segments_size(std::span<const char* const> labels) const;
    bool segments_at(const char* id, ImVec2 at, std::span<const char* const> labels, int& value,
                     bool enabled = true);

    /// The help of the row with focus, or under the mouse, this frame.
    [[nodiscard]] const std::string& focused_tip() const { return m_focused_tip; }

private:
    struct Placed {
        ImVec2 control;     ///< top-left of the control
        float  bottom;      ///< the row's bottom y
    };
    /// Opens a row: the separator, the texts, and where a control `w` by `h`
    /// goes. Leaves the cursor at the row's end.
    Placed begin_row(const char* name, const char* about, float w, float h, const Opts& opts);
    /// After the control: hover and focus handling shared by every row.
    void end_row(const Placed& placed, bool control_hot);
    /// The texts of a row, wrapped to `width`: their height.
    float texts_height(const char* name, const char* about, float width) const;
    void draw_texts(ImDrawList* list, ImVec2 at, float width, const char* name,
                    const char* about, const Opts& opts) const;
    /// Takes Left/Right (keyboard and D-pad) for the focused item `id` and
    /// says which way, -1, 0 or 1.
    static int steer(ImGuiID id);
    /// A new navigation row begins (NavState); whether focus is asked here.
    void nav_row(bool free);
    bool dropdown_impl(const char* name, const char* about, std::span<const std::string> labels,
                       int& value, char* filter, size_t filter_size, const Opts& opts);
    bool press_button(const char* label, ImVec2 at, ImVec2 size, Tone tone, Icon icon,
                      bool enabled, bool on = false);

    const Palette& m_p;
    float          m_scale;
    NavState*      m_nav;
    bool           m_focus_asked = false;  ///< the next item of this row takes focus
    // The group being drawn: where it began and the row count so far.
    ImVec2      m_group_top{};
    int         m_group_rows   = 0;
    bool        m_in_group     = false;
    float       m_row_hover_y0 = 0.0f;
    const char* m_next_tip     = nullptr;
    const char* m_row_tip      = nullptr;
    std::string m_focused_tip;
    ImVec2      m_last_name_end{};  ///< where the (?) of the current row sits
};

}  // namespace sm2::osd::ui
