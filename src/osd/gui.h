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

#include "core/config.h"
#include "core/types.h"

#include <array>
#include <string>
#include <vector>

struct SDL_Window;

namespace sm2::osd {

/// ImGui overlay drawn on top of the emulator's output.
///
/// The GUI is an overlay that appears when the user presses F1 (or launches
/// without a ROM). It provides access to settings that persist to sm2-emu.ini
/// and a game browser for selecting ROMs.
///
/// This class owns ImGui's context and its SDL3 platform backend only. Which
/// GPU API draws the widgets it builds is a render backend's concern, not
/// this one's -- see render::Backend::draw_overlay(), which calls new_frame()
/// and draw() here, then renders ImGui's resulting draw data through whatever
/// renderer backend (Vulkan today) it owns.
class Gui {
public:
    Gui() = default;
    ~Gui();

    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    /// Initialise ImGui and its SDL3 platform backend.
    ///
    /// Call once the window exists. The render backend initialises its own
    /// ImGui renderer backend separately, after this.
    [[nodiscard]] bool init(SDL_Window* window);

    void shutdown();

    /// Begin a new ImGui frame. Call once per frame before draw(), after the
    /// render backend's own new-frame call for its ImGui renderer backend.
    void new_frame();

    /// Draw the GUI windows: the always-on FPS counter, plus the F1 settings
    /// overlay when visible. Always returns true, since the FPS counter draws
    /// every frame regardless of `visible()` — the caller should always open a
    /// rendering scope for the backend's draw_overlay().
    [[nodiscard]] bool draw(Config& config,
                            const std::vector<std::string>& gpu_names,
                            float measured_hz,
                            const char* renderer_label,
                            class Input* input);

    /// Finish this frame's ImGui build. Call once, after draw(), before the
    /// render backend submits ImGui's draw data.
    void end_frame();

    /// Toggle the overlay on/off.
    void toggle() { m_visible = !m_visible; }

    /// Whether the overlay is currently shown. When visible, the emulator
    /// should still run but input is captured by ImGui.
    [[nodiscard]] bool visible() const { return m_visible; }

    /// Force the overlay visible (e.g. when launched with no ROM).
    void show() { m_visible = true; }

private:
    void draw_menu_bar(Config& config);
    void draw_settings(Config& config, const std::vector<std::string>& gpu_names,
                       class Input* input);
    void draw_wheel_tab(Config& config, class Input* input);
    void draw_status_bar(float measured_hz);
    void draw_fps_overlay(float measured_hz, const char* renderer_label);

    SDL_Window* m_window      = nullptr;
    bool        m_visible     = false;
    bool        m_initialised = false;
    float       m_ui_scale       = 0.0f;  ///< applied overlay scale; 0 forces first-frame apply.
    float       m_settings_scale = -1.0f; ///< scale the Settings window was last snapped to.

    // -- wheel calibration capture state -----------------------------------
    // Which control (if any) is currently waiting for the user to operate it,
    // and the axis baseline captured when an axis calibration began.
    enum class Capture { None, Button, Axis };
    Capture m_capture       = Capture::None;
    u32     m_capture_role  = 0;   ///< Config::WheelRole being bound, when Button.
    int     m_capture_axis  = 0;   ///< which analogue control, when Axis (0=steer,1=accel,2=brake).
    std::array<s16, 16> m_axis_baseline{};
};

}  // namespace sm2::osd
