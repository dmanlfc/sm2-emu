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
#include "osd/gui.h"
#include "core/log.h"
#include "osd/input.h"
#include "osd/scraper.h"
#include "render/geometry.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <stb_image.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

namespace sm2::osd {

namespace {

// Name an evdev key code for the gun-button UI; hex fallback for anything odd.
[[nodiscard]] std::string evdev_button_name(u32 code)
{
    switch (code) {
        case 0x110: return "BTN_LEFT";
        case 0x111: return "BTN_RIGHT";
        case 0x112: return "BTN_MIDDLE";
        case 0x113: return "BTN_SIDE";
        case 0x114: return "BTN_EXTRA";
        default: break;
    }
    if (code >= 0x100 && code <= 0x109) {  // BTN_0..BTN_9
        return "BTN_" + std::to_string(code - 0x100);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", code);
    return buf;
}

}  // namespace

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
    // DisplaySize stays the SDL logical size (mouse arrives in that space, so
    // layout must too); correct DisplayFramebufferScale to the backend's real
    // pixel extent instead. imgui_impl_vulkan renders at DisplaySize*scale, so
    // this fills the presented image even where SDL under-reports the surface
    // pixel size on Wayland.
    if (m_framebuffer_width != 0 && m_framebuffer_height != 0) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            io.DisplayFramebufferScale =
                ImVec2(static_cast<float>(m_framebuffer_width) / io.DisplaySize.x,
                       static_cast<float>(m_framebuffer_height) / io.DisplaySize.y);
        }
    }
    ImGui::NewFrame();
}

void Gui::apply_scale()
{
    // Scale content with the window. Measured against the base 992x768 on both
    // axes with the smaller ratio winning, so a wide-but-short window does not
    // get oversized text; clamped so it stays legible small and sane huge.
    const ImGuiIO& io = ImGui::GetIO();
    const float    sx = io.DisplaySize.x > 0.0f ? io.DisplaySize.x / 992.0f : 1.0f;
    const float    sy = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 768.0f : 1.0f;
    float          scale = std::min(sx, sy);
    scale                = std::clamp(scale, 0.5f, 4.0f);

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
    // Not while the picker is up: grabbing the pointer there would confine it to
    // the window, making the title bar and resize edges unreachable.
    const bool hide_cursor = config.lightgun && !m_visible && !m_picker_enabled;
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
    if (config.lightgun && !m_picker_enabled) {
        draw_sinden_border(config);
        if (config.lightgun_crosshair) {
            draw_crosshairs(input);
        }
    }

    // Full-screen picker when active and Settings is closed; F1 opens Settings
    // on top.
    if (m_picker_enabled && !m_visible) {
        draw_picker(config);
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
    // 88% of the window, centred, re-applied every frame so it tracks a resize.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2         area     = viewport->Size;
    const ImVec2         win_size(area.x * 0.88f, area.y * 0.88f);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + area.x * 0.5f, viewport->Pos.y + area.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                 | ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoSavedSettings;
    // Zero WindowMinSize so apply_scale()'s scaled minimum cannot override the
    // explicit size above.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin("Settings", &m_visible, flags);
    ImGui::PopStyleVar();
    if (!open) {
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

            if (!gpu_names.empty()) {
                ImGui::Separator();
                ImGui::Text("GPU");

                int current = 0;  // 0 is "Auto"
                for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                    if (gpu_names[i] == config.gpu) {
                        current = i + 1;  // 0 is "Auto"
                        break;
                    }
                }

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

            // Renderer, from the backends this build offers; takes effect next launch.
            if (!m_available_renderers.empty()) {
                ImGui::Separator();
                ImGui::Text("Renderer");

                const auto label_of = [](const std::string& name) -> const char* {
                    if (name == "software") return "Software";
                    if (name == "vulkan")   return "Vulkan";
                    if (name == "opengl")   return "OpenGL";
                    return name.c_str();
                };
                const std::string current_choice =
                    config.graphics_backend.empty() ? m_available_renderers.front()
                                                     : config.graphics_backend;

                if (ImGui::BeginCombo("##renderer", label_of(current_choice))) {
                    for (const std::string& name : m_available_renderers) {
                        const bool selected = (name == current_choice);
                        if (ImGui::Selectable(label_of(name), selected)) {
                            config.graphics_backend = name;
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
                    ImGui::SetTooltip("Which renderer draws the game. Applies on the\n"
                                      "next launch - save settings and relaunch.");
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

            // 3D render scale. Live runtime reallocation is out of scope for
            // v1, so this only edits the config; it applies on the next launch.
            ImGui::Separator();
            {
                static constexpr std::array<const char*, 4> kScaleLabels = {
                    "1x (native)", "2x", "3x", "4x"};
                int scale_index =
                    std::clamp(static_cast<int>(config.render_scale), 1, 4) - 1;
                ImGui::SetNextItemWidth(140);
                if (ImGui::Combo("3D render scale", &scale_index, kScaleLabels.data(),
                                 static_cast<int>(kScaleLabels.size()))) {
                    config.render_scale = static_cast<u32>(scale_index + 1);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Internal 3D rendering resolution. Higher is crisper 3D\n"
                        "(2D/HUD stays sharp). GPU backends only; the software\n"
                        "renderer stays native. Applies on the next launch - save\n"
                        "settings and relaunch.");
                }
            }

            ImGui::EndTabItem();
        }

        // -- Paths tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Paths")) {
            const auto dir_field = [](const char* label, const char* id,
                                      std::string& value, const char* help) {
                ImGui::Text("%s", label);
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                if (ImGui::InputText(id, buf, sizeof(buf))) {
                    value = buf;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", help);
                }
            };

            // Read-only: the ini's own location is set by --config, not stored
            // inside it.
            {
                const std::string ini_path =
                    m_config_path.empty() ? default_config_path() : m_config_path;
                const std::string dir =
                    std::filesystem::path(ini_path).parent_path().string();
                ImGui::Text("Config directory");
                ImGui::TextDisabled("%s", dir.empty() ? "." : dir.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Where sm2-emu.ini is read from and saved to.\n"
                                      "Set with --config <dir>; cannot be changed here.");
                }
                ImGui::Spacing();
            }

            dir_field("ROM directory", "##romdir", config.rom_dir,
                      "Where the ROM archives live. A game launched by name is\n"
                      "loaded from here as <name>.zip or <name>.7z.");
            ImGui::Spacing();
            dir_field("Saves directory", "##saves", config.nvram_dir,
                      "Battery-backed saves: per-game NVRAM (.nv) and EEPROM\n"
                      "(.eeprom) images -- high scores and operator settings.");
            ImGui::Spacing();
            dir_field("Screenshots directory", "##shots", config.screenshot_dir,
                      "Where F12 screenshots are written.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Checkbox("Scrape artwork online", &config.scrape_artwork);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Let the game picker fetch box art and descriptions from\n"
                    "ArcadeDB over the network. Off keeps sm2-emu offline: the\n"
                    "picker still lists and launches every game, with placeholder\n"
                    "tiles and no descriptions.");
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
            ImGui::Text(" ____  __  __  ____         _____ __  __ _   _");
            ImGui::Text("/ ___||  \\/  ||___ \\       | ____|  \\/  | | | |");
            ImGui::Text("\\___ \\| |\\/| |  __) |_____ |  _| | |\\/| | | | |");
            ImGui::Text(" ___) | |  | | / __/|_____|| |___| |  | | |_| |");
            ImGui::Text("|____/|_|  |_||_____|      |_____|_|  |_|\\___/");
            ImGui::Spacing();
            ImGui::Text("A Sega Model 2 arcade emulator");
            ImGui::Spacing();
            ImGui::Text("Copyright (c) 2025+ Daniel Martin (dmanlfc)");
            ImGui::Text("BSD 3-Clause licence. See LICENSE.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("sm2-emu exists because of open source shared with the community.");
            ImGui::Text("With thanks to the projects whose code makes it possible:");
            ImGui::Spacing();
            ImGui::BulletText("The MAME project - Sega Model 2 emulation guidance");
            ImGui::BulletText("Musashi - Motorola 68000 CPU core (Karl Stenerud)");
            ImGui::BulletText("ymfm - Yamaha FM sound cores (Aaron Giles)");
            ImGui::BulletText("SDL - windowing, input and audio");
            ImGui::BulletText("Dear ImGui - this user interface (Omar Cornut)");
            ImGui::BulletText("pugixml - games.xml parsing");
            ImGui::BulletText("miniz - ZIP archive decompression");
            ImGui::BulletText("LZMA SDK - 7-Zip archive decompression (Igor Pavlov)");
            ImGui::BulletText("VulkanMemoryAllocator - GPU memory (AMD / GPUOpen)");
            ImGui::BulletText("shaderc / glslang - shader compilation");
            ImGui::BulletText("stb_image - box-art image decoding (Sean Barrett)");
            ImGui::BulletText("libcurl - artwork scraping (optional)");
            ImGui::Spacing();
            ImGui::Text("Game artwork and descriptions from ArcadeDB");
            ImGui::Text("(adb.arcadeitalia.net, by Motoschifo).");
            ImGui::Spacing();
            ImGui::Text("See NOTICE for full per-component attribution.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();

        // Save button, on the tabs that carry settings but not on About.
        if (!on_about_tab) {
            ImGui::Separator();
            if (ImGui::Button("Save settings")) {
                const std::string path =
                    m_config_path.empty() ? default_config_path() : m_config_path;
                if (save_config(path, config)) {
                    SM2_INFO("gui: settings saved to %s", path.c_str());
                } else {
                    SM2_ERROR("gui: could not save settings to %s", path.c_str());
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Saved to %s", m_config_path.empty()
                                    ? default_config_path().c_str()
                                    : m_config_path.c_str());
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
    ImGui::TextUnformatted("Gun buttons");
    ImGui::TextDisabled("Press Bind, then press the button on that player's gun.");
    {
        static const char* const kRoleNames[] = {
            "Trigger", "Reload/Missile", "Coin", "Start",
            "Hat up", "Hat down", "Hat left", "Hat right",
        };
        const usize guns = input != nullptr ? input->gun_count() : 0;
        for (int p = 0; p < 2; ++p) {
            ImGui::Text("Player %d", p + 1);
            for (u32 r = 0; r < Config::kGunRoleCount; ++r) {
                ImGui::PushID(p * 100 + static_cast<int>(r));
                const u32 code = config.gun_buttons[static_cast<usize>(p)][r];
                if (code == 0) {
                    ImGui::Text("  %-14s unbound", kRoleNames[r]);
                } else {
                    ImGui::Text("  %-14s %s", kRoleNames[r],
                                evdev_button_name(code).c_str());
                }
                ImGui::SameLine();
                const bool capturing = m_gun_capture_player == p
                                       && m_gun_capture_role == r;
                if (capturing) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a button...");
                    u32 got = 0;
                    if (p < static_cast<int>(guns)) {
                        got = input->gun_take_last_pressed(static_cast<usize>(p));
                    }
                    // Mouse fallback so a binding can be set with no gun present.
                    if (got == 0) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))   got = 0x110;
                        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))  got = 0x111;
                        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) got = 0x112;
                    }
                    if (got != 0) {
                        config.gun_buttons[static_cast<usize>(p)][r] = got;
                        m_gun_capture_player = -1;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel")) m_gun_capture_player = -1;
                } else if (ImGui::SmallButton("Bind")) {
                    m_gun_capture_player = p;
                    m_gun_capture_role   = r;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("clear")) {
                    config.gun_buttons[static_cast<usize>(p)][r] = 0;
                }
                ImGui::PopID();
            }
        }
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

// ---------------------------------------------------------------------------
// Game picker
// ---------------------------------------------------------------------------

void Gui::hide_picker()
{
    m_picker_enabled = false;
}

void Gui::enable_picker(std::vector<PickerEntry> entries, render::Backend* backend,
                        Scraper* scraper)
{
    m_picker_entries  = std::move(entries);
    m_picker_backend  = backend;
    m_picker_scraper  = scraper;
    m_picker_enabled  = true;
    m_picker_selected = 0;
    m_picker_scroll   = 0.0f;

    // Load anything already cached up front: the scraper skips a set whose cache
    // file exists and never pushes a Ready for it, so without this a cached set
    // would show "Fetching..." forever. Uncached sets fill in from Ready later.
    if (m_picker_scraper != nullptr) {
        for (PickerEntry& entry : m_picker_entries) {
            const Scraper::Metadata meta =
                m_picker_scraper->load_metadata(entry.name, entry.title);
            if (!meta.found && meta.description.empty() && meta.image_paths.empty()) {
                continue;
            }
            entry.title           = meta.title;
            entry.year            = meta.year;
            entry.manufacturer    = meta.manufacturer;
            entry.description     = meta.description;
            entry.metadata_loaded = true;
        }
    }
}

std::optional<std::string> Gui::take_pending_launch()
{
    std::optional<std::string> out = std::move(m_pending_launch);
    m_pending_launch.reset();
    return out;
}

void Gui::release_picker_textures()
{
    if (m_picker_backend == nullptr) {
        return;
    }
    for (PickerEntry& entry : m_picker_entries) {
        for (const PickerEntry::Art1& art : entry.art) {
            if (art.handle != 0) {
                m_picker_backend->destroy_texture(art.handle);
            }
        }
        entry.art.clear();
    }
}

/// Pull each newly-cached set's metadata into its entry, once per frame.
void Gui::poll_scraper_and_load_metadata()
{
    if (m_picker_scraper == nullptr) {
        return;
    }
    for (const std::string& name : m_picker_scraper->take_ready()) {
        for (PickerEntry& entry : m_picker_entries) {
            if (entry.name != name) {
                continue;
            }
            const Scraper::Metadata meta =
                m_picker_scraper->load_metadata(entry.name, entry.title);
            entry.title           = meta.title;
            entry.year            = meta.year;
            entry.manufacturer    = meta.manufacturer;
            entry.description     = meta.description;
            entry.metadata_loaded = true;
            if (!meta.image_paths.empty()) {
                entry.art_state = PickerEntry::Art::Unknown;  // let the grid upload it
            }
            break;
        }
    }
}

namespace {

/// Decode a cached image to RGBA8 with stb. False on any failure.
[[nodiscard]] bool decode_image_rgba(const std::string& path, int* w, int* h,
                                     std::vector<unsigned char>* pixels)
{
    int            channels = 0;
    unsigned char* data     = stbi_load(path.c_str(), w, h, &channels, 4);
    if (data == nullptr) {
        return false;
    }
    pixels->assign(data, data + static_cast<usize>(*w) * static_cast<usize>(*h) * 4);
    stbi_image_free(data);
    return true;
}

}  // namespace

void Gui::draw_picker(Config& config)
{
    static_cast<void>(config);
    poll_scraper_and_load_metadata();

    // Show the pointer in case a previous state (light-gun mode) hid it.
    if (!SDL_CursorVisible()) {
        SDL_ShowCursor();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
    if (!ImGui::Begin("##picker", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::TextUnformatted("Select a game");
    ImGui::SameLine(ImGui::GetWindowWidth() - 300.0f);
    ImGui::TextDisabled("Enter: launch   F1: settings   Esc: quit");
    ImGui::Separator();

    if (m_picker_entries.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "No Model 2 ROM archives were found in the ROM directory. Set it in "
            "Settings (F1) > Paths, then relaunch.");
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    m_picker_selected =
        std::clamp(m_picker_selected, 0, static_cast<int>(m_picker_entries.size()) - 1);

    const float pane_w = std::min(360.0f, ImGui::GetContentRegionAvail().x * 0.34f);
    const float grid_w = ImGui::GetContentRegionAvail().x - pane_w - 8.0f;

    const ImVec2 tile{132.0f, 180.0f};
    const int    columns = std::max(1, static_cast<int>(grid_w / (tile.x + 12.0f)));
    const int    count   = static_cast<int>(m_picker_entries.size());

    // The picker drives selection itself rather than via ImGui nav focus, so
    // the description pane and launch stay in step with the highlighted tile.
    const ImGuiIO& io      = ImGui::GetIO();
    const auto     pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    int            sel     = m_picker_selected;
    if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight)) ++sel;
    if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft)) --sel;
    if (pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown)) sel += columns;
    if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp)) sel -= columns;
    sel = std::clamp(sel, 0, count - 1);
    if (sel != m_picker_selected) {
        m_picker_selected      = sel;
        m_picker_scroll        = 0.0f;
        m_picker_scroll_to_sel = true;
        m_picker_art_timer     = 0.0f;
        m_picker_art_index     = 0;
    }

    constexpr float kArtDwellSeconds = 2.0f;
    m_picker_art_timer += io.DeltaTime;
    if (m_picker_art_timer >= kArtDwellSeconds) {
        m_picker_art_timer = 0.0f;
        ++m_picker_art_index;
    }

    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)
        || pressed(ImGuiKey_GamepadFaceDown)) {
        m_pending_launch = m_picker_entries[static_cast<usize>(m_picker_selected)].name;
    }

    ImGui::BeginChild("##grid", ImVec2(grid_w, 0.0f), false);
    for (int i = 0; i < count; ++i) {
        PickerEntry& entry = m_picker_entries[static_cast<usize>(i)];

        // Lazy: decode + upload a tile's art the first time it is drawn; a
        // failure marks it None so it is not retried each frame.
        if (entry.art_state == PickerEntry::Art::Unknown && m_picker_backend != nullptr
            && m_picker_scraper != nullptr) {
            const Scraper::Metadata meta =
                m_picker_scraper->load_metadata(entry.name, entry.title);
            for (const std::string& image_path : meta.image_paths) {
                int                        iw = 0;
                int                        ih = 0;
                std::vector<unsigned char> rgba;
                if (decode_image_rgba(image_path, &iw, &ih, &rgba) && iw > 0 && ih > 0) {
                    const render::Backend::TextureHandle handle =
                        m_picker_backend->create_texture(static_cast<u32>(iw),
                                                         static_cast<u32>(ih), rgba.data());
                    if (handle != 0) {
                        entry.art.push_back({handle, static_cast<float>(iw),
                                             static_cast<float>(ih)});
                    }
                }
            }
            entry.art_state = entry.art.empty() ? PickerEntry::Art::None
                                                : PickerEntry::Art::Loaded;
        }

        if (i % columns != 0) {
            ImGui::SameLine();
        }

        ImGui::PushID(i);
        const bool   is_sel = (i == m_picker_selected);
        const ImVec2 p0     = ImGui::GetCursorScreenPos();

        // A button under the art carries click-to-select / click-selected-to-launch.
        if (ImGui::Button("##tile", tile)) {
            if (m_picker_selected == i) {
                m_pending_launch = entry.name;
            } else {
                m_picker_selected = i;
                m_picker_scroll   = 0.0f;
            }
        }
        if (ImGui::IsItemHovered()
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_pending_launch = entry.name;
        }
        if (is_sel && m_picker_scroll_to_sel) {
            ImGui::SetScrollHereY(0.5f);  // follow keyboard/gamepad selection
            m_picker_scroll_to_sel = false;
        }

        ImDrawList*  draw = ImGui::GetWindowDrawList();
        const ImVec2 p1{p0.x + tile.x, p0.y + tile.y};
        if (entry.art_state == PickerEntry::Art::Loaded && !entry.art.empty()
            && m_picker_backend != nullptr) {
            // The highlighted game cycles its images; others show the first.
            int img = 0;
            if (is_sel && entry.art.size() > 1) {
                img = m_picker_art_index % static_cast<int>(entry.art.size());
            }
            const PickerEntry::Art1& art = entry.art[static_cast<usize>(img)];

            // Letterbox to the tile so art is not stretched.
            draw->AddRectFilled(p0, p1, IM_COL32(20, 22, 27, 255));
            ImVec2 draw0 = p0;
            ImVec2 draw1 = p1;
            if (art.w > 0.0f && art.h > 0.0f) {
                const float scale =
                    std::min(tile.x / art.w, tile.y / art.h);
                const float dw = art.w * scale;
                const float dh = art.h * scale;
                const float ox = (tile.x - dw) * 0.5f;
                const float oy = (tile.y - dh) * 0.5f;
                draw0 = ImVec2(p0.x + ox, p0.y + oy);
                draw1 = ImVec2(draw0.x + dw, draw0.y + dh);
            }
            draw->AddImage(reinterpret_cast<ImTextureID>(
                               m_picker_backend->texture_imgui_id(art.handle)),
                           draw0, draw1);
        } else {
            draw->AddRectFilled(p0, p1, IM_COL32(28, 30, 36, 255));
            draw->PushClipRect(p0, p1, true);
            draw->AddText(nullptr, 0.0f, ImVec2(p0.x + 6.0f, p0.y + 6.0f),
                          IM_COL32(200, 205, 215, 255), entry.title.c_str());
            draw->PopClipRect();
        }
        draw->AddRect(p0, p1, is_sel ? IM_COL32(120, 190, 255, 255)
                                     : IM_COL32(60, 64, 74, 255),
                      0.0f, 0, is_sel ? 3.0f : 1.0f);

        ImGui::PopID();
    }
    ImGui::EndChild();

    // -- description pane --------------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("##pane", ImVec2(pane_w, 0.0f), true);
    const PickerEntry& cur = m_picker_entries[static_cast<usize>(m_picker_selected)];

    ImGui::TextWrapped("%s", cur.title.c_str());
    if (!cur.year.empty() || !cur.manufacturer.empty()) {
        std::string sub = cur.manufacturer;
        if (!cur.year.empty()) {
            sub += sub.empty() ? cur.year : ("  " + cur.year);
        }
        ImGui::TextDisabled("%s", sub.c_str());
    }
    ImGui::TextDisabled("%s", cur.name.c_str());
    ImGui::Separator();

    // Auto-scroll: hold at the top, creep up until fully past, hold, loop.
    // m_picker_scroll is seconds since the pick (0 on selection change), so a
    // new pick always starts at the top. Text that fits never scrolls.
    ImGui::BeginChild("##desc", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (cur.description.empty()) {
        const bool still_fetching =
            !cur.metadata_loaded && m_picker_scraper != nullptr
            && m_picker_scraper->scraping_available();
        ImGui::TextDisabled(still_fetching ? "Fetching description..."
                                           : "No description available.");
    } else {
        const float  avail_h = ImGui::GetContentRegionAvail().y;
        const float  wrap_w  = ImGui::GetContentRegionAvail().x;
        const ImVec2 size =
            ImGui::CalcTextSize(cur.description.c_str(), nullptr, false, wrap_w);

        float offset = 0.0f;
        if (size.y > avail_h) {
            constexpr float kHoldSeconds  = 2.5f;
            constexpr float kPixelsPerSec = 22.0f;
            const float     scroll_px     = (size.y - avail_h) + 24.0f;
            const float     scroll_secs   = scroll_px / kPixelsPerSec;
            const float     cycle         = kHoldSeconds + scroll_secs + kHoldSeconds;

            m_picker_scroll += io.DeltaTime;
            float t = m_picker_scroll;
            if (t > cycle) {
                t = 0.0f;
                m_picker_scroll = 0.0f;  // loop back to the top
            }
            if (t <= kHoldSeconds) {
                offset = 0.0f;                                  // hold at top
            } else if (t <= kHoldSeconds + scroll_secs) {
                offset = (t - kHoldSeconds) * kPixelsPerSec;    // scrolling up
            } else {
                offset = scroll_px;                             // hold at bottom
            }
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offset);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(cur.description.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

}  // namespace sm2::osd
