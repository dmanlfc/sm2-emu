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
#include "osd/gui.h"
#include "core/log.h"
#include "osd/input.h"
#include "render/geometry.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace sm2::osd {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Gui::~Gui()
{
    if (m_initialised) {
        shutdown();
    }
}

bool Gui::init(SDL_Window* window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;  // No imgui.ini — settings live in sm2-emu.ini.

    // Dark style with some tweaks.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 2.0f;
    style.GrabRounding     = 2.0f;
    style.WindowBorderSize = 0.0f;

    // The platform backend only, chosen for input/clipboard/cursor handling.
    // Which GPU API actually draws the result is a render backend's own
    // ImGui renderer backend, initialised separately.
    if (!ImGui_ImplSDL3_InitForOther(window)) {
        SM2_ERROR("gui: ImGui_ImplSDL3_InitForOther failed");
        ImGui::DestroyContext();
        return false;
    }

    m_window      = window;
    m_initialised = true;
    SM2_INFO("gui: initialised (ImGui %s)", IMGUI_VERSION);
    return true;
}

void Gui::shutdown()
{
    if (!m_initialised) return;

    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    m_initialised = false;
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void Gui::new_frame()
{
    if (!m_initialised) return;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Gui::apply_scale()
{
    // Scale the whole overlay automatically with the render window. io.DisplaySize
    // is what ImGui actually draws into and the SDL3 backend tracks it every frame,
    // so it follows a resize or fullscreen switch even where SDL hides the pixel
    // size (Wayland). The scale is the window height against the base 768, clamped
    // so a small window keeps the base size and a huge one does not run away.
    const ImGuiIO& io    = ImGui::GetIO();
    float          scale = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 768.0f : 1.0f;
    scale                = std::clamp(scale, 1.0f, 4.0f);

    // Widget metrics are rescaled from a one-time base-style snapshot
    // (ScaleAllSizes is cumulative), and FontScaleMain drives ImGui 1.92's
    // dynamic font sizing so text re-rasterises crisply rather than stretching.
    static const ImGuiStyle base_style = ImGui::GetStyle();
    if (scale != m_ui_scale) {
        m_ui_scale          = scale;
        ImGuiStyle& style   = ImGui::GetStyle();
        style               = base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
    }
}

bool Gui::draw(Config& config, const std::vector<std::string>& gpu_names,
               float measured_hz, const char* renderer_label, Input* input)
{
    apply_scale();

    // Shown regardless of F1 when enabled, so the counter is visible whether or
    // not the settings overlay is open.
    if (config.show_fps) {
        draw_fps_overlay(measured_hz, renderer_label);
    }

    // In light-gun mode draw the aiming crosshair(s) and hide the OS cursor, so
    // only the crosshair is visible. gun_aims() is inactive for non-gun titles,
    // so a non-gun game shows nothing even with the mode on.
    //
    // The cursor is only hidden while the settings overlay is CLOSED: with the
    // overlay open the real pointer is needed to click its buttons and the close
    // box, and the crosshair is clamped to the game area so it cannot reach them.
    //
    // Toggle only on a state change. ImGui's SDL3 backend drives the OS cursor
    // itself every frame, so a bare SDL_HideCursor() is fought back the next
    // frame (which is what flickered). NoMouseCursorChange makes ImGui stop
    // touching the cursor, and then SDL_HideCursor() sticks.
    const bool hide_cursor = config.lightgun && !m_visible;
    if (hide_cursor != m_cursor_hidden) {
        ImGuiIO& io = ImGui::GetIO();
        if (hide_cursor) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            SDL_HideCursor();
        } else {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor();
        }
        // Confine the pointer to the window while playing, so a light gun that
        // presents as a mouse (e.g. a Sinden) cannot drag the desktop cursor off
        // onto another monitor, and its motion stays with sm2-emu. Released when
        // the settings overlay opens, so its buttons stay clickable.
        if (m_window != nullptr) {
            SDL_SetWindowMouseGrab(m_window, hide_cursor);
        }
        m_cursor_hidden = hide_cursor;
    }
    if (config.lightgun) {
        draw_sinden_border(config);
        if (config.lightgun_crosshair) {
            draw_crosshairs(input);
        }
    }

    if (m_visible) {
        draw_menu_bar(config);
        draw_settings(config, gpu_names, input);
        draw_status_bar(measured_hz);
    }

    return true;
}

void Gui::end_frame()
{
    if (!m_initialised) return;
    // Must be called every frame after new_frame(), regardless of whether
    // anything was drawn -- a render backend reads ImGui::GetDrawData() after
    // this to submit it through whatever GPU API it owns.
    ImGui::Render();
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void Gui::draw_menu_bar(Config& config)
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Hide overlay", "F1")) {
                m_visible = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                SDL_Event quit_event{};
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            ImGui::MenuItem("Vsync", nullptr, &config.vsync);
            ImGui::MenuItem("Fullscreen", nullptr, &config.fullscreen);
            ImGui::MenuItem("FPS counter", nullptr, &config.show_fps);
            ImGui::MenuItem("Light-gun mode", nullptr, &config.lightgun);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------

void Gui::draw_settings(Config& config, const std::vector<std::string>& gpu_names,
                        Input* input)
{
    // Size the window as a fraction of the render window and centre it, tracked
    // every frame so it always mirrors the window (a little smaller) through any
    // resize or fullscreen switch. No manual sizing: the layout is automatic.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 win_size(display.x * 0.60f, display.y * 0.72f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);

    if (!ImGui::Begin("Settings", &m_visible)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // -- Video tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Video")) {
            ImGui::Checkbox("Vsync", &config.vsync);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Wait for vertical blank before presenting.\n"
                                  "Prevents tearing but adds up to one frame of latency.");
            }

            ImGui::Checkbox("Fullscreen", &config.fullscreen);
            ImGui::Checkbox("FPS counter", &config.show_fps);

            // GPU selection.
            if (!gpu_names.empty()) {
                ImGui::Separator();
                ImGui::Text("GPU");

                // Find current selection.
                int current = 0;
                for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                    if (gpu_names[i] == config.gpu) {
                        current = i + 1;  // 0 is "Auto"
                        break;
                    }
                }

                // Build combo items.
                if (ImGui::BeginCombo("##gpu", current == 0 ? "Auto (best)" : gpu_names[current - 1].c_str())) {
                    if (ImGui::Selectable("Auto (best)", current == 0)) {
                        config.gpu.clear();
                    }
                    for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                        const bool selected = (current == i + 1);
                        if (ImGui::Selectable(gpu_names[i].c_str(), selected)) {
                            config.gpu = gpu_names[i];
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            // Window size (only meaningful in windowed mode).
            if (!config.fullscreen) {
                ImGui::Separator();
                ImGui::Text("Window size");
                int w = static_cast<int>(config.window_width);
                int h = static_cast<int>(config.window_height);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Width", &w, 16, 64);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Height", &h, 16, 64);
                config.window_width  = static_cast<u32>(std::max(496, w));
                config.window_height = static_cast<u32>(std::max(384, h));
            }

            ImGui::EndTabItem();
        }

        // -- Paths tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Paths")) {
            ImGui::Text("NVRAM directory");
            char nvram_buf[256];
            std::snprintf(nvram_buf, sizeof(nvram_buf), "%s", config.nvram_dir.c_str());
            if (ImGui::InputText("##nvram", nvram_buf, sizeof(nvram_buf))) {
                config.nvram_dir = nvram_buf;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Where EEPROM and battery-backed RAM are stored.\n"
                                  "Relative to the working directory.");
            }

            ImGui::Spacing();
            ImGui::Text("ROM database");
            char xml_buf[256];
            std::snprintf(xml_buf, sizeof(xml_buf), "%s", config.games_xml.c_str());
            if (ImGui::InputText("##gamesxml", xml_buf, sizeof(xml_buf))) {
                config.games_xml = xml_buf;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Path to games.xml. Leave empty to search\n"
                                  "beside the executable and in the system data directory.");
            }

            ImGui::EndTabItem();
        }

        // -- Wheel tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Wheel")) {
            draw_wheel_tab(config, input);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Light Gun")) {
            draw_lightgun_tab(config, input);
            ImGui::EndTabItem();
        }

        // -- About tab -----------------------------------------------------
        // The Save button below is suppressed on this tab: it carries no
        // settings, so a save control there is meaningless.
        bool on_about_tab = false;
        if (ImGui::BeginTabItem("About")) {
            on_about_tab = true;
            ImGui::Text("sm2-emu — A Sega Model 2 arcade emulator");
            ImGui::Spacing();
            ImGui::Text("Copyright (c) 2025+ Daniel Martin (dmanlfc)");
            ImGui::Text("BSD 3-Clause licence. See LICENSE.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Emulation derived from the MAME project.");
            ImGui::Text("See NOTICE for per-component attribution.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();

        // Save button, on the tabs that carry settings but not on About.
        if (!on_about_tab) {
            ImGui::Separator();
            if (ImGui::Button("Save settings")) {
                const std::string path = default_config_path();
                if (save_config(path, config)) {
                    SM2_INFO("gui: settings saved to %s", path.c_str());
                } else {
                    SM2_ERROR("gui: could not save settings to %s", path.c_str());
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Saved to sm2-emu.ini");
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Wheel tab
// ---------------------------------------------------------------------------

void Gui::draw_wheel_tab(Config& config, Input* input)
{
    const bool connected = input != nullptr && input->wheel_connected();
    if (connected) {
        ImGui::TextDisabled("Wheel connected.");
    } else {
        ImGui::TextDisabled("No wheel connected. Settings still apply once one is.");
    }
    ImGui::Spacing();

    // -- feel ---------------------------------------------------------------
    ImGui::Checkbox("Force feedback", &config.wheel_ffb);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Wheel force on driving games: centring and cornering\n"
                          "forces decoded from the game's own drive-board commands,\n"
                          "plus an impact jolt when you hit something.");
    }

    ImGui::BeginDisabled(!config.wheel_ffb);
    int resistance = static_cast<int>(config.wheel_ffb_strength);
    if (ImGui::SliderInt("Resistance", &resistance, 0, 100, "%d%%")) {
        resistance = ((resistance + 5) / 10) * 10;  // snap to 10 % steps
        config.wheel_ffb_strength = static_cast<u32>(std::clamp(resistance, 0, 100));
    }

    // Synthetic engine/road rumble, since the game streams no continuous buzz.
    ImGui::Checkbox("Rumble", &config.wheel_rumble);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A synthesised engine/road vibration that rises with\n"
                          "the throttle. Daytona sends no continuous rumble, so\n"
                          "this is a feel added on top, not game data.");
    }
    ImGui::BeginDisabled(!config.wheel_rumble);
    int rumble = static_cast<int>(config.wheel_rumble_strength);
    if (ImGui::SliderInt("Rumble strength", &rumble, 0, 100, "%d%%")) {
        rumble = ((rumble + 5) / 10) * 10;
        config.wheel_rumble_strength = static_cast<u32>(std::clamp(rumble, 0, 100));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // Common wheel rotation ranges rather than a free slider: a wheel is set to
    // one of these, and 270 matches the Model 2 cabinet.
    static constexpr u32 kSteerRanges[] = {200, 240, 270, 360, 400, 540, 720, 900, 1080};
    char current_range[16];
    std::snprintf(current_range, sizeof(current_range), "%u deg", config.wheel_steer_degrees);
    if (ImGui::BeginCombo("Steering range", current_range)) {
        for (const u32 range : kSteerRanges) {
            char label[16];
            std::snprintf(label, sizeof(label), "%u deg", range);
            const bool selected = config.wheel_steer_degrees == range;
            if (ImGui::Selectable(label, selected)) {
                config.wheel_steer_degrees = range;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Set this to your wheel's own rotation range. The lock\n"
                          "angle below is mapped onto it, so matching your wheel\n"
                          "gives arcade-like response.");
    }

    int lock = static_cast<int>(config.wheel_lock_degrees);
    if (ImGui::SliderInt("Lock angle", &lock, 180, 270, "%d deg")) {
        config.wheel_lock_degrees = static_cast<u32>(std::clamp(lock, 180, 270));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How far to physically turn for full game lock (total,\n"
                          "so half each side of centre). Lower is more sensitive.");
    }

    // -- axis calibration ---------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Axes");
    ImGui::TextDisabled("Auto-detected. Recalibrate if steering or a pedal is wrong.");

    struct AxisRow { const char* name; s32* axis; bool* invert; };
    const AxisRow axis_rows[] = {
        {"Steering", &config.wheel_steer_axis, nullptr},
        {"Accelerator", &config.wheel_accel_axis, &config.wheel_accel_invert},
        {"Brake", &config.wheel_brake_axis, &config.wheel_brake_invert},
    };

    ImGui::BeginDisabled(!connected);
    for (int row = 0; row < 3; ++row) {
        const AxisRow& r = axis_rows[row];
        ImGui::PushID(row);
        if (*r.axis < 0) {
            ImGui::Text("%-12s auto", r.name);
        } else {
            ImGui::Text("%-12s axis %d%s", r.name, *r.axis,
                        (r.invert != nullptr && *r.invert) ? " (inverted)" : "");
        }
        ImGui::SameLine();
        const bool capturing = m_capture == Capture::Axis && m_capture_axis == row;
        if (capturing) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "operate it...");
            if (input != nullptr) {
                bool positive = true;
                const s32 got = input->captured_axis(
                    m_axis_baseline.data(),
                    std::min<int>(input->wheel_axis_count(),
                                  static_cast<int>(m_axis_baseline.size())),
                    &positive);
                if (got >= 0) {
                    *r.axis = got;
                    // A pedal read as "released high, pressed low" is inverted; a
                    // downward move at capture time means exactly that.
                    if (r.invert != nullptr) {
                        *r.invert = !positive;
                    }
                    m_capture = Capture::None;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) {
                m_capture = Capture::None;
            }
        } else if (ImGui::SmallButton("Calibrate")) {
            m_capture      = Capture::Axis;
            m_capture_axis = row;
            if (input != nullptr) {
                input->wheel_axis_baseline(
                    m_axis_baseline.data(),
                    std::min<int>(input->wheel_axis_count(),
                                  static_cast<int>(m_axis_baseline.size())));
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("auto")) {
            *r.axis = -1;
            if (r.invert != nullptr) {
                *r.invert = false;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    // -- button binding -----------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Buttons");
    ImGui::TextDisabled("Press Bind, then press the wheel button for that control.");

    struct ButtonRow { const char* name; Config::WheelRole role; };
    const ButtonRow button_rows[] = {
        {"Start",      Config::WheelRole::Start},
        {"Coin",       Config::WheelRole::Coin},
        {"Button 1",   Config::WheelRole::Button1},
        {"Button 2",   Config::WheelRole::Button2},
        {"Button 3",   Config::WheelRole::Button3},
        {"Button 4",   Config::WheelRole::Button4},
        {"Shift up",   Config::WheelRole::GearUp},
        {"Shift down", Config::WheelRole::GearDown},
        {"Test",       Config::WheelRole::Test},
        {"Service",    Config::WheelRole::Service},
        {"Menu (F1)",  Config::WheelRole::Menu},
    };

    ImGui::BeginDisabled(!connected);
    for (const ButtonRow& r : button_rows) {
        const u32 role_index = static_cast<u32>(r.role);
        ImGui::PushID(static_cast<int>(role_index));
        const s32 bound = config.wheel_buttons[role_index];
        if (bound < 0) {
            ImGui::Text("%-11s unbound", r.name);
        } else {
            ImGui::Text("%-11s button %d", r.name, bound);
        }
        ImGui::SameLine();
        const bool capturing = m_capture == Capture::Button && m_capture_role == role_index;
        if (capturing) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a button...");
            if (input != nullptr) {
                const s32 got = input->pressed_wheel_button();
                if (got >= 0) {
                    config.wheel_buttons[role_index] = got;
                    m_capture = Capture::None;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) {
                m_capture = Capture::None;
            }
        } else if (ImGui::SmallButton("Bind")) {
            m_capture      = Capture::Button;
            m_capture_role = role_index;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            config.wheel_buttons[role_index] = -1;
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Light Gun tab
// ---------------------------------------------------------------------------

void Gui::draw_lightgun_tab(Config& config, Input* input)
{
    ImGui::Checkbox("Light-gun mode", &config.lightgun);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show the aiming crosshair and hide the mouse cursor,\n"
                          "for the light-gun titles (Virtua Cop 2, House of the\n"
                          "Dead, Gunblade NY, Rail Chase 2, Behind Enemy Lines).");
    }

    ImGui::BeginDisabled(!config.lightgun);
    ImGui::Checkbox("Show crosshair", &config.lightgun_crosshair);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw sm2-emu's own crosshair. Turn off if the gun\n"
                          "has its own sight (e.g. a Sinden). Positional-gun\n"
                          "titles always draw their own regardless.");
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Recoil");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Kick the gun's rumble motor on each shot, for guns\n"
                          "that have one (Sinden and similar). No effect on a\n"
                          "gun without a motor.");
    }
    ImGui::Checkbox("Enable recoil", &config.lightgun_recoil);
    ImGui::BeginDisabled(!config.lightgun_recoil);
    int strength = static_cast<int>(config.lightgun_recoil_strength);
    if (ImGui::SliderInt("Strength", &strength, 0, 100)) {
        config.lightgun_recoil_strength = static_cast<u32>(std::clamp(strength, 0, 100));
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Devices");
    if (input != nullptr && input->gun_count() > 0) {
        for (usize i = 0; i < input->gun_count(); ++i) {
            ImGui::BulletText("Gun %zu (player %zu): %s", i + 1, i + 1,
                              input->gun_name(i).c_str());
        }
    } else {
        ImGui::TextWrapped(
            "No dedicated light guns detected. Player 1 aims with the mouse; the "
            "left button fires and the right button reloads (shoot off screen). "
            "Plug in guns tagged ID_INPUT_GUN for independent per-player aiming.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Sinden border");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A bright frame around the game image that a Sinden\n"
                          "gun's camera tracks for aiming. Only shown in\n"
                          "light-gun mode.");
    }
    ImGui::Checkbox("Show border", &config.sinden_border);

    ImGui::BeginDisabled(!config.sinden_border);
    float rgb[3] = {
        static_cast<float>((config.sinden_border_colour >> 16) & 0xff) / 255.0f,
        static_cast<float>((config.sinden_border_colour >> 8) & 0xff) / 255.0f,
        static_cast<float>(config.sinden_border_colour & 0xff) / 255.0f,
    };
    if (ImGui::ColorEdit3("Colour", rgb, ImGuiColorEditFlags_NoInputs)) {
        const u32 r = static_cast<u32>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 g = static_cast<u32>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 b = static_cast<u32>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        config.sinden_border_colour = (r << 16) | (g << 8) | b;
    }
    int thickness = static_cast<int>(config.sinden_border_thickness);
    if (ImGui::SliderInt("Thickness", &thickness, 1, 64)) {
        config.sinden_border_thickness = static_cast<u32>(std::max(1, thickness));
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// FPS overlay (top right, always on)
// ---------------------------------------------------------------------------

void Gui::draw_fps_overlay(float measured_hz, const char* renderer_label)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2         margin{10.0F, 10.0F};

    // Sized to fit rather than fixed, so a longer renderer label never clips.
    char text[64];
    std::snprintf(text, sizeof(text), "%.1f FPS  [%s]", static_cast<double>(measured_hz),
                 renderer_label);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 padding{8.0F, 4.0F};
    const ImVec2 window_size{text_size.x + padding.x * 2.0F, text_size.y + padding.y * 2.0F};

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - window_size.x - margin.x,
               viewport->WorkPos.y + margin.y));
    ImGui::SetNextWindowSize(window_size);
    ImGui::SetNextWindowBgAlpha(0.55F);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    if (ImGui::Begin("##FpsOverlay", nullptr, kFlags)) {
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Light-gun crosshairs
// ---------------------------------------------------------------------------

void Gui::draw_crosshairs(const Input* input)
{
    if (input == nullptr) {
        return;
    }

    // Each aim is a 0..1 fraction of the game image. Map it back onto the same
    // letterboxed 4:3 rectangle the frame is presented into, so the crosshair
    // sits exactly where a shot lands. io.DisplaySize is that window.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) {
        return;
    }
    const render::Letterbox box = render::compute_letterbox(
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y));

    // Distinct per player: green for 1, cyan for 2.
    static const std::array<ImU32, 2> colours = {
        IM_COL32(0, 255, 0, 220),
        IM_COL32(0, 200, 255, 220),
    };

    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float radius = std::max(8.0f, box.height * 0.02f);
    const auto& aims = input->gun_aims();

    // The aim positions come from the input poll, which does not run while the
    // game is paused (settings overlay open). So player 1's crosshair would
    // freeze there. When player 1 is on the mouse fallback (no dedicated gun),
    // track the live ImGui pointer instead so the crosshair still follows the
    // mouse in the overlay. A player with a real gun still freezes, which is
    // correct -- its position only exists when polled.
    const bool p1_on_mouse = input->gun_count() < 1;

    for (usize player = 0; player < aims.size(); ++player) {
        const Input::GunAim& aim = aims[player];
        if (!aim.active) {
            continue;
        }
        float fx = aim.x;
        float fy = aim.y;
        if (player == 0 && p1_on_mouse && box.width > 0.0f && box.height > 0.0f) {
            // Read the pointer straight from SDL rather than io.MousePos: while
            // the settings window is focused ImGui reports a position relative to
            // that window, which would confine the crosshair to its width. SDL's
            // window-relative position spans the whole window. The overlay draws
            // in io.DisplaySize space, which the SDL3 backend keeps equal to the
            // window size, so this shares the letterbox already computed above.
            float mx = 0.0f;
            float my = 0.0f;
            SDL_GetMouseState(&mx, &my);
            fx = std::clamp((mx - box.x) / box.width, 0.0f, 1.0f);
            fy = std::clamp((my - box.y) / box.height, 0.0f, 1.0f);
        }
        const float cx = box.x + fx * box.width;
        const float cy = box.y + fy * box.height;
        const ImU32 colour = colours[player < colours.size() ? player : 0];
        list->AddCircle(ImVec2(cx, cy), radius, colour, 24, 2.0f);
        list->AddLine(ImVec2(cx - radius * 1.6f, cy), ImVec2(cx - radius * 0.4f, cy), colour, 2.0f);
        list->AddLine(ImVec2(cx + radius * 0.4f, cy), ImVec2(cx + radius * 1.6f, cy), colour, 2.0f);
        list->AddLine(ImVec2(cx, cy - radius * 1.6f), ImVec2(cx, cy - radius * 0.4f), colour, 2.0f);
        list->AddLine(ImVec2(cx, cy + radius * 0.4f), ImVec2(cx, cy + radius * 1.6f), colour, 2.0f);
    }
}

// ---------------------------------------------------------------------------
// Sinden light-gun border
// ---------------------------------------------------------------------------

void Gui::draw_sinden_border(const Config& config)
{
    if (!config.sinden_border) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) {
        return;
    }

    // Frame the letterboxed game image, not the raw window, so the border sits
    // on the picture the gun's camera actually sees.
    const render::Letterbox box = render::compute_letterbox(
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y));

    const u32   rgb = config.sinden_border_colour & 0xffffff;
    const ImU32 colour = IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);
    const float t = std::max(1.0f, static_cast<float>(config.sinden_border_thickness));

    const float x0 = box.x;
    const float y0 = box.y;
    const float x1 = box.x + box.width;
    const float y1 = box.y + box.height;

    // Four filled bands inset into the image edge. A solid band reads more
    // reliably to the gun's camera than a one-pixel outline.
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + t), colour);        // top
    list->AddRectFilled(ImVec2(x0, y1 - t), ImVec2(x1, y1), colour);        // bottom
    list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + t, y1), colour);        // left
    list->AddRectFilled(ImVec2(x1 - t, y0), ImVec2(x1, y1), colour);        // right
}

// ---------------------------------------------------------------------------
// Status bar (bottom of screen)
// ---------------------------------------------------------------------------

void Gui::draw_status_bar(float measured_hz)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float bar_height = ImGui::GetFrameHeight() + 4.0f;

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - bar_height));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x, bar_height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        ImGui::Text("%.1f Hz", static_cast<double>(measured_hz));
        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        ImGui::Text("F1: toggle overlay");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace sm2::osd
