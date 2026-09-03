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

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

#include <algorithm>
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

    ImGuiIO& io = ImGui::GetIO();

    // ImGui_ImplSDL3_NewFrame sizes the UI from SDL's logical window size, which
    // does not always follow a live fullscreen switch or a HiDPI drawable. Pin
    // DisplaySize to the actual framebuffer pixels so the overlay fills the
    // window instead of shrinking to a corner of it.
    if (m_window != nullptr) {
        int pixel_w = 0;
        int pixel_h = 0;
        SDL_GetWindowSizeInPixels(m_window, &pixel_w, &pixel_h);
        if (pixel_w > 0 && pixel_h > 0) {
            io.DisplaySize             = ImVec2(static_cast<float>(pixel_w),
                                                static_cast<float>(pixel_h));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
    }

    // Scale the whole overlay with the window so it grows when the window is
    // enlarged, maximised or made fullscreen. The reference is the default
    // window height (768); scale never drops below 1.0 so small windows keep
    // the base size. Widget metrics are rescaled from a one-time base-style
    // snapshot (ScaleAllSizes is cumulative), and FontScaleMain drives ImGui
    // 1.92's dynamic font sizing so text re-rasterises crisply at the new size.
    static ImGuiStyle base_style = ImGui::GetStyle();
    const float       reference  = 768.0f;
    float             scale      = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / reference : 1.0f;
    scale                        = std::clamp(scale, 1.0f, 3.0f);
    if (scale != m_ui_scale) {
        m_ui_scale          = scale;
        ImGuiStyle& style   = ImGui::GetStyle();
        style               = base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
    }

    ImGui::NewFrame();
}

bool Gui::draw(Config& config, const std::vector<std::string>& gpu_names,
               float measured_hz, const char* renderer_label, Input* input)
{
    // Shown regardless of F1 when enabled, so the counter is visible whether or
    // not the settings overlay is open.
    if (config.show_fps) {
        draw_fps_overlay(measured_hz, renderer_label);
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
    // Size and place the window relative to the current display, re-snapping it
    // whenever the overlay scale changes (a resize, maximise or fullscreen
    // toggle). Between such changes the user can still nudge it, so the forced
    // condition only applies on the frame the scale actually changed.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  menu_h  = ImGui::GetFrameHeight();
    const float  margin  = 20.0f * m_ui_scale;
    const ImVec2 base_size(420.0f * m_ui_scale, 340.0f * m_ui_scale);
    const ImVec2 win_size(std::min(base_size.x, display.x - margin * 2.0f),
                          std::min(base_size.y, display.y - menu_h - margin * 2.0f));
    const ImGuiCond cond = (m_ui_scale != m_settings_scale) ? ImGuiCond_Always
                                                            : ImGuiCond_FirstUseEver;
    m_settings_scale = m_ui_scale;
    ImGui::SetNextWindowPos(ImVec2(margin, menu_h + margin), cond);
    ImGui::SetNextWindowSize(win_size, cond);

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

        // -- About tab -----------------------------------------------------
        if (ImGui::BeginTabItem("About")) {
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
    }

    // Save button at the bottom.
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
        ImGui::SetTooltip("A synthesised centring spring on driving games.\n"
                          "The drive board is not emulated, so this is a feel,\n"
                          "not the arcade's real motor force.");
    }

    int strength = static_cast<int>(config.wheel_ffb_strength);
    ImGui::BeginDisabled(!config.wheel_ffb);
    if (ImGui::SliderInt("Strength", &strength, 0, 100, "%d%%")) {
        strength = ((strength + 5) / 10) * 10;  // snap to 10 % steps
        config.wheel_ffb_strength = static_cast<u32>(std::clamp(strength, 0, 100));
    }
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
        ImGui::SetTooltip("Your wheel's rotation range. Lower is more sensitive;\n"
                          "the Model 2 cabinet was about 270 degrees.");
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
