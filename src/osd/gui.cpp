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
#include "core/net.h"
#include "osd/i18n.h"
#include "osd/input.h"
#include "osd/scraper.h"
#include "osd/ui_text.h"
#include "render/geometry.h"

#include "assets/pad_xbox_outline.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <stb_image.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace sm2::osd {

namespace {

using i18n::tr;
using i18n::tr_id;
using i18n::trf;

template <size_t N>
[[nodiscard]] std::array<const char*, N> tr_all(const std::array<const char*, N>& items)
{
    std::array<const char*, N> out{};
    for (size_t i = 0; i < N; ++i) {
        out[i] = tr(items[i]);
    }
    return out;
}

/// Lines of `text`, each pushed to the right edge of the widest; how right-to-
/// left text lines up.
void right_aligned_lines(std::string_view text)
{
    std::vector<std::string_view> lines;
    float                         width = 0.0f;
    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        lines.push_back(text.substr(pos, end - pos));
        width = std::max(width, ImGui::CalcTextSize(lines.back().data(),
                                                    lines.back().data() + lines.back().size()).x);
        pos = end + 1;
    }
    const float x0 = ImGui::GetCursorPosX();
    for (const std::string_view line : lines) {
        const float w = ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
        ImGui::SetCursorPosX(x0 + width - w);
        ImGui::TextUnformatted(line.data(), line.data() + line.size());
    }
}

/// A tooltip of already-translated text.
void tooltip(const char* text)
{
    if (!i18n::right_to_left()) {
        ImGui::SetTooltip("%s", text);
        return;
    }
    if (ImGui::BeginTooltip()) {
        right_aligned_lines(text);
        ImGui::EndTooltip();
    }
}

/// `msgid` translated and word-wrapped to the available width. Right-to-left
/// text is broken into lines before shaping, since ImGui wraps after it.
void text_wrapped(const char* msgid)
{
    if (!i18n::right_to_left()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tr(msgid));
        ImGui::PopTextWrapPos();
        return;
    }
    const float            width = ImGui::GetContentRegionAvail().x;
    const std::string_view text  = i18n::translate(msgid);
    std::string            lines;
    std::string            line;
    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find(' ', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view word = text.substr(pos, end - pos);
        std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && ImGui::CalcTextSize(i18n::shape(candidate).c_str()).x > width) {
            lines += i18n::shape(line) + "\n";
            candidate = std::string(word);
        }
        line = std::move(candidate);
        pos  = end + 1;
    }
    lines += i18n::shape(line);
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + std::max(0.0f, width - ImGui::CalcTextSize(lines.c_str()).x));
    right_aligned_lines(lines);
}

/// `text` as an ImGui slider format that shows it verbatim, for a value whose
/// text is already formatted (and translated).
std::string verbatim_format(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        out += c;
        if (c == '%') {
            out += '%';
        }
    }
    return out;
}

/// The x at which values start in rows labelled with `names` (translated), so
/// they line up after the widest label.
template <typename Rows, typename Name>
float value_column(const Rows& rows, Name name)
{
    float width = 0.0f;
    for (const auto& row : rows) {
        width = std::max(width, ImGui::CalcTextSize(tr(name(row))).x);
    }
    return ImGui::GetCursorPosX() + width + ImGui::GetStyle().ItemSpacing.x * 2.0f;
}

/// A language as the picker lists it: its own name when the overlay font can
/// draw it, with the English name alongside.
std::string language_label(const i18n::Language& language)
{
    // The language in use has its fonts and shaper loaded.
    const bool           current  = language.code == i18n::current();
    const std::string    shown    = current ? i18n::shape(language.name) : language.name;
    const std::u32string name     = i18n::to_utf32(shown);
    ImFont*              font     = ImGui::GetFont();
    const bool           drawable = (current || !i18n::needs_shaping(name))
                          && std::all_of(name.begin(), name.end(), [&](char32_t c) {
                                 return c <= IM_UNICODE_CODEPOINT_MAX
                                     && font->IsGlyphInFont(static_cast<ImWchar>(c));
                             });
    if (!drawable) {
        return language.english_name;
    }
    if (language.name == language.english_name) {
        return shown;
    }
    return shown + " (" + language.english_name + ")";
}

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

bool Gui::init(SDL_Window* window, const std::string& language)
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

    i18n::init();
    m_language = language;
    apply_language();

    m_window      = window;
    m_initialised = true;
    SM2_INFO("gui: initialised (ImGui %s)", IMGUI_VERSION);
    return true;
}

void Gui::shutdown()
{
    release_pad_art();
    if (!m_initialised) return;

    ImGui_ImplSDL3_Shutdown();
    ui_text::shutdown();
    ImGui::DestroyContext();

    m_initialised = false;
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void Gui::apply_language()
{
    const std::string code = i18n::set_language(m_language);
    if (!ui_text::build_font()) {
        SM2_WARN("gui: showing the overlay in English instead of %s", code.c_str());
        i18n::set_language("en");
        ui_text::build_font();
    }
    m_language_pending = false;
}

void Gui::new_frame()
{
    if (!m_initialised) return;
    // Fonts may only change between frames.
    if (m_language_pending) {
        apply_language();
    }
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
    //
    // The floor is the font's native size in output pixels: the default font
    // is drawn for 13 px and turns to mush below that, and on short modes such
    // as a 640x240 15 kHz CRT the height ratio alone would shrink it to 6.5 px.
    // A short window gets a scrolling overlay instead of unreadable text.
    const ImGuiIO& io = ImGui::GetIO();
    const float    sx = io.DisplaySize.x > 0.0f ? io.DisplaySize.x / 992.0f : 1.0f;
    const float    sy = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 768.0f : 1.0f;
    const float    fb = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    float          scale = std::min(sx, sy);
    scale                = std::clamp(scale, std::clamp(1.0f / fb, 0.5f, 1.0f), 4.0f);

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
    if (config.language != m_language) {
        m_language         = config.language;
        m_language_pending = true;
    }

    // Cache this frame's present placement so the crosshair and Sinden-border
    // helpers frame the same rectangle the backend presents into.
    m_present_aspect = config.aspect_mode;
    m_present_method = config.scaling_method;

    // Shown regardless of F10 when enabled, so the counter is visible whether or
    // not the settings overlay is open.
    if (config.show_fps) {
        draw_fps_overlay(measured_hz, renderer_label);
    }

    // A notification (state saved/loaded), top-centered, over gameplay.
    if (m_notify_seconds_left > 0.0f) {
        m_notify_seconds_left -= ImGui::GetIO().DeltaTime;
        if (config.show_notifications) {
            draw_notification();
        }
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
        m_cursor_hidden = hide_cursor;
    }
    // Confine the pointer while playing fullscreen, so a light gun that presents
    // as a mouse cannot drag the cursor onto another monitor. In a window it is
    // left free, so the desktop pointer comes back once it leaves the window.
    const bool grab = hide_cursor && m_window != nullptr
                   && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (grab != m_mouse_grabbed) {
        SDL_SetWindowMouseGrab(m_window, grab);
        m_mouse_grabbed = grab;
    }
    if (config.lightgun && !m_picker_enabled) {
        draw_sinden_border(config);
        if (config.lightgun_crosshair) {
            draw_crosshairs(input);
        }
        draw_calibration_markers(input);
    }

    // Full-screen picker when active and Settings is closed; F10 opens Settings
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
        if (ImGui::BeginMenu(tr_id("File"))) {
            if (ImGui::MenuItem(tr_id("Hide overlay"), "F10")) {
                m_visible = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr_id("Quit"), "F9")) {
                SDL_Event quit_event{};
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr_id("Settings"))) {
            ImGui::MenuItem(tr_id("Vsync"), nullptr, &config.vsync);
            ImGui::MenuItem(tr_id("Fullscreen"), nullptr, &config.fullscreen);
            ImGui::MenuItem(tr_id("FPS counter"), "F8", &config.show_fps);
            ImGui::MenuItem(tr_id("On-screen notifications"), nullptr, &config.show_notifications);
            ImGui::MenuItem(tr_id("Light-gun mode"), nullptr, &config.lightgun);
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
    // 88% of the space between the menu bar and the status bar, centred,
    // re-applied every frame so it tracks a resize. On a short display the
    // bars are a large share of the height, so the window must not cover them.
    const ImGuiViewport* viewport   = ImGui::GetMainViewport();
    const float          status_bar = ImGui::GetFrameHeight() + 4.0f;
    const ImVec2 area(viewport->WorkSize.x, std::max(viewport->WorkSize.y - status_bar, 0.0f));
    const ImVec2 win_size(area.x * 0.88f, area.y * 0.88f);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + area.x * 0.5f, viewport->WorkPos.y + area.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                 | ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoSavedSettings;
    // Zero WindowMinSize so apply_scale()'s scaled minimum cannot override the
    // explicit size above.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(tr_id("Settings"), &m_visible, flags);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // -- Video tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Video"))) {
            ImGui::Checkbox(tr_id("Vsync"), &config.vsync);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                tooltip(tr("Wait for vertical blank before presenting.\n"
                           "Prevents tearing but adds up to one frame of latency."));
            }

            ImGui::Checkbox(tr_id("Fullscreen"), &config.fullscreen);
            ImGui::Checkbox(tr_id("FPS counter"), &config.show_fps);
            ImGui::SameLine();
            ImGui::Checkbox(tr_id("Notifications"), &config.show_notifications);

            // Overlay language; switches live.
            {
                const auto& languages = i18n::available();
                std::string current   = tr("Automatic (system)");
                if (config.language == "en") {
                    current = "English";
                }
                for (const i18n::Language& language : languages) {
                    if (language.code == config.language) {
                        current = language_label(language);
                    }
                }
                ImGui::SetNextItemWidth(220);
                if (ImGui::BeginCombo(tr_id("Language"), current.c_str())) {
                    if (ImGui::Selectable(tr("Automatic (system)"), config.language == "auto")) {
                        config.language = "auto";
                    }
                    if (ImGui::Selectable("English", config.language == "en")) {
                        config.language = "en";
                    }
                    for (const i18n::Language& language : languages) {
                        ImGui::PushID(language.code.c_str());
                        if (ImGui::Selectable(language_label(language).c_str(),
                                              config.language == language.code)) {
                            config.language = language.code;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }

            if (!gpu_names.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(tr("GPU"));

                int current = 0;  // 0 is "Auto"
                for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                    if (gpu_names[i] == config.gpu) {
                        current = i + 1;  // 0 is "Auto"
                        break;
                    }
                }

                if (ImGui::BeginCombo("##gpu", current == 0 ? tr("Auto (best)") : gpu_names[current - 1].c_str())) {
                    if (ImGui::Selectable(tr("Auto (best)"), current == 0)) {
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
                ImGui::TextUnformatted(tr("Renderer"));

                const auto label_of = [](const std::string& name) -> const char* {
                    if (name == "software") return tr("Software");
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
                    tooltip(tr("Which renderer draws the game. Applies on the\n"
                               "next launch - save settings and relaunch."));
                }
            }

            // Window size (only meaningful in windowed mode).
            if (!config.fullscreen) {
                ImGui::Separator();
                ImGui::TextUnformatted(tr("Window size"));
                int w = static_cast<int>(config.window_width);
                int h = static_cast<int>(config.window_height);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt(tr_id("Width"), &w, 16, 64);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt(tr_id("Height"), &h, 16, 64);
                config.window_width  = static_cast<u32>(std::max(496, w));
                config.window_height = static_cast<u32>(std::max(384, h));
            }

            // 3D render scale. Live runtime reallocation is out of scope for
            // v1, so this only edits the config; it applies on the next launch.
            ImGui::Separator();
            {
                static constexpr std::array<const char*, 8> kScaleLabels = {
                    N_("1x (native)"), "2x", "3x", "4x", "5x", "6x", "7x", N_("8x (4K)")};
                int scale_index =
                    std::clamp(static_cast<int>(config.render_scale),
                               1, static_cast<int>(kScaleLabels.size())) - 1;
                ImGui::SetNextItemWidth(140);
                if (ImGui::Combo(tr_id("3D render scale"), &scale_index, tr_all(kScaleLabels).data(),
                                 static_cast<int>(kScaleLabels.size()))) {
                    config.render_scale = static_cast<u32>(scale_index + 1);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("Internal 3D rendering resolution. Higher is crisper 3D\n"
                               "(2D/HUD stays sharp). GPU backends only; the software\n"
                               "renderer stays native. Applies on the next launch - save\n"
                               "settings and relaunch."));
                }
            }

            // Present-stage scaling: how the finished frame is put on screen.
            // Unlike the 3D render scale these are present-only, so they apply
            // live (main.cpp pushes them to the backend every frame).
            ImGui::Separator();
            {
                static constexpr std::array<const char*, 4> kMethodLabels = {
                    N_("Nearest"), N_("Bilinear"), N_("Sharp-bilinear"), N_("Integer")};
                int method_index =
                    std::clamp(static_cast<int>(config.scaling_method), 0, 3);
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo(tr_id("2D scaling"), &method_index, tr_all(kMethodLabels).data(),
                                 static_cast<int>(kMethodLabels.size()))) {
                    config.scaling_method = static_cast<ScalingMethod>(method_index);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("How the frame is magnified to the window.\n"
                               "Sharp-bilinear keeps 2D text crisp without the shimmer\n"
                               "nearest shows at non-integer window sizes. Integer snaps\n"
                               "to a whole multiple (pixel-perfect, with black bars).\n"
                               "Applies live."));
                }

                static constexpr std::array<const char*, 3> kAspectLabels = {
                    N_("4:3 (arcade)"), N_("Square pixels"), N_("Stretch")};
                int aspect_index =
                    std::clamp(static_cast<int>(config.aspect_mode), 0, 2);
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo(tr_id("Aspect"), &aspect_index, tr_all(kAspectLabels).data(),
                                 static_cast<int>(kAspectLabels.size()))) {
                    config.aspect_mode = static_cast<AspectMode>(aspect_index);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("4:3 is the arcade monitor's shape (the default).\n"
                               "Square pixels shows the raw 496x384 (slightly narrow).\n"
                               "Stretch fills the whole window. Applies live."));
                }
            }

            // CRT cosmetic filter. Off by default; all present-stage, live.
            ImGui::Separator();
            {
                ImGui::Checkbox(tr_id("CRT filter"), &config.crt_enabled);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("A cosmetic arcade-monitor look over the finished frame:\n"
                               "scanlines, an RGB shadow mask, glow and optional screen\n"
                               "curvature. Purely visual; applies live."));
                }
                if (config.crt_enabled) {
                    const auto pct_slider = [](const char* label, u32& value) {
                        int v = static_cast<int>(value);
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::SliderInt(tr_id(label), &v, 0, 100)) {
                            value = static_cast<u32>(std::clamp(v, 0, 100));
                        }
                    };
                    pct_slider(N_("Scanlines"), config.crt_scanline_strength);
                    pct_slider(N_("Mask"), config.crt_mask_strength);
                    pct_slider(N_("Glow"), config.crt_glow_strength);
                    pct_slider(N_("Curvature"), config.crt_curvature);
                }
            }

            // Enhancement: opt-in quality beyond the original hardware, GPU-gated
            // and off by default. Applies live (main.cpp pushes it each frame).
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Enhancement (cosmetic, beyond the original hardware)"));
            {
                // 3D texture filter. Anisotropic needs GPU support; grey it out
                // and force Faithful where the device reports none.
                static constexpr std::array<const char*, 2> kTexLabels = {
                    N_("Faithful"), N_("Anisotropic")};
                int tex_index = std::clamp(static_cast<int>(config.texture_filter), 0, 1);
                if (!m_caps_anisotropy) {
                    config.texture_filter = TextureFilter::Faithful;
                    tex_index             = 0;
                    ImGui::BeginDisabled();
                }
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo(tr_id("3D texture filter"), &tex_index, tr_all(kTexLabels).data(),
                                 static_cast<int>(kTexLabels.size()))) {
                    config.texture_filter = static_cast<TextureFilter>(tex_index);
                }
                if (!m_caps_anisotropy) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", tr("(unsupported)"));
                    if (ImGui::IsItemHovered()) {
                        tooltip(tr("This GPU does not report anisotropic filtering."));
                    }
                }

                // Anisotropy quality, only when Anisotropic and supported. The
                // 2/4/8/16 list is trimmed to the GPU's reported maximum.
                if (m_caps_anisotropy
                    && config.texture_filter == TextureFilter::Anisotropic) {
                    static constexpr std::array<int, 4>         kLevels = {2, 4, 8, 16};
                    static constexpr std::array<const char*, 4> kLevelLabels = {
                        "2x", "4x", "8x", "16x"};
                    const int cap = static_cast<int>(m_caps_max_anisotropy);
                    int count = 0;
                    for (int level : kLevels) {
                        if (level <= cap) {
                            ++count;
                        }
                    }
                    count = std::max(count, 1);
                    int sel = 0;
                    for (int i = 0; i < count; ++i) {
                        if (kLevels[static_cast<usize>(i)]
                            <= static_cast<int>(config.anisotropy)) {
                            sel = i;
                        }
                    }
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::Combo(tr_id("Anisotropy"), &sel, tr_all(kLevelLabels).data(), count)) {
                        config.anisotropy =
                            static_cast<u32>(kLevels[static_cast<usize>(sel)]);
                    }
                }

                // 2D upscale. xBR/ScaleFX need the GPU compute path the tilemap
                // composite already relies on, so they are always available here.
                static constexpr std::array<const char*, 3> kUpscaleLabels = {
                    N_("Faithful"), "xBR", "ScaleFX"};
                int up_index = std::clamp(static_cast<int>(config.upscale_2d), 0, 2);
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo(tr_id("2D upscale"), &up_index, tr_all(kUpscaleLabels).data(),
                                 static_cast<int>(kUpscaleLabels.size()))) {
                    config.upscale_2d = static_cast<Upscale2D>(up_index);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("Edge-smooth the 2D layers (HUD, text, menus). xBR and\n"
                               "ScaleFX round diagonals; Faithful keeps crisp pixels.\n"
                               "Applies live."));
                }

                // Translucency. Blended needs a depth-capable fill mask; grey it
                // out and force Stipple where the renderer has none.
                static constexpr std::array<const char*, 2> kTranslucencyLabels = {
                    N_("Stipple"), N_("Blended")};
                int translucency = std::clamp(static_cast<int>(config.translucency), 0, 1);
                if (!m_caps_blended) {
                    config.translucency = Translucency::Stipple;
                    translucency        = 0;
                    ImGui::BeginDisabled();
                }
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo(tr_id("Translucency"), &translucency, tr_all(kTranslucencyLabels).data(),
                                 static_cast<int>(kTranslucencyLabels.size()))) {
                    config.translucency = static_cast<Translucency>(translucency);
                }
                if (!m_caps_blended) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("The hardware draws see-through surfaces (lights, glass,\n"
                               "shadows) as a checkerboard. Blended draws the 50%\n"
                               "see-through surface it's trying to achieve. Applies live."));
                }

                ImGui::Checkbox(tr_id("Custom textures"), &config.custom_textures);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("Replace the game's 3D textures with images saved in\n"
                               "saves/textures/<game>/load, named as dumped. GPU\n"
                               "renderers only; Reload picks up edited files."));
                }
                if (config.custom_textures) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", trf("%zu loaded", m_custom_texture_count).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr_id("Reload"))) {
                        m_texture_reload_requested = true;
                    }
                }

                ImGui::Checkbox(tr_id("Dump textures"), &config.dump_textures);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("Collect every 3D texture the game draws and write them,\n"
                               "with an index.html to browse, to saves/textures/<game>/dump\n"
                               "when the game exits or this is turned off."));
                }
            }

            ImGui::EndTabItem();
        }

        // -- Audio tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Audio"))) {
            draw_audio_tab(config);
            ImGui::EndTabItem();
        }

        // -- Paths tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Paths"))) {
            const auto dir_field = [this](const char* label, const char* id,
                                         std::string& value, DirPickerTarget target,
                                         const char* help) {
                ImGui::TextUnformatted(tr(label));
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                if (ImGui::InputText(id, buf, sizeof(buf))) {
                    value = buf;
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string(tr("Browse...")) + "###browse" + id).c_str())) {
                    open_dir_picker(target, value);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr(help));
                }
            };

            // Read-only: the ini's own location is set by --config, not stored
            // inside it.
            {
                const std::string ini_path =
                    m_config_path.empty() ? default_config_path() : m_config_path;
                const std::string dir =
                    std::filesystem::path(ini_path).parent_path().string();
                ImGui::TextUnformatted(tr("Config directory"));
                ImGui::TextDisabled("%s", dir.empty() ? "." : dir.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    tooltip(tr("Where sm2-emu.ini is read from and saved to.\n"
                               "Set with --config <dir>; cannot be changed here."));
                }
                ImGui::Spacing();
            }

            dir_field(N_("ROM directory"), "##romdir", config.rom_dir,
                      DirPickerTarget::RomDir,
                      N_("Where the ROM archives live. A game launched by name is\n"
                         "loaded from here as <name>.zip or <name>.7z."));
            ImGui::Spacing();
            dir_field(N_("Saves directory"), "##saves", config.nvram_dir,
                      DirPickerTarget::NvramDir,
                      N_("Battery-backed saves: per-game NVRAM (.nv) and EEPROM\n"
                         "(.eeprom) images -- high scores and operator settings."));
            ImGui::Spacing();
            dir_field(N_("Screenshots directory"), "##shots", config.screenshot_dir,
                      DirPickerTarget::ScreenshotDir,
                      N_("Where F12 screenshots are written."));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Checkbox(tr_id("Scrape artwork online"), &config.scrape_artwork);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                tooltip(tr("Let the game picker fetch box art and descriptions from\n"
                           "ArcadeDB over the network. Off keeps sm2-emu offline: the\n"
                           "picker still lists and launches every game, with placeholder\n"
                           "tiles and no descriptions."));
            }

            ImGui::EndTabItem();
        }

        // -- Gamepad tab ---------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Gamepad"))) {
            draw_gamepad_tab(config, input);
            ImGui::EndTabItem();
        }

        // -- Wheel tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Wheel"))) {
            draw_wheel_tab(config, input);
            ImGui::EndTabItem();
        }

        // -- Light Gun tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Light Gun"))) {
            draw_lightgun_tab(config, input);
            ImGui::EndTabItem();
        }

        // -- Network tab -----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("Network"))) {
            draw_network_tab(config);
            ImGui::EndTabItem();
        }

        // -- States tab ----------------------------------------------------
        if (ImGui::BeginTabItem(tr_id("States"))) {
            draw_states_tab();
            ImGui::EndTabItem();
        }

        // -- About tab -----------------------------------------------------
        bool on_about_tab = false;
        if (ImGui::BeginTabItem(tr_id("About"))) {
            on_about_tab = true;
            ImGui::Text(" ____  __  __  ____         _____ __  __ _   _");
            ImGui::Text("/ ___||  \\/  ||___ \\       | ____|  \\/  | | | |");
            ImGui::Text("\\___ \\| |\\/| |  __) |_____ |  _| | |\\/| | | | |");
            ImGui::Text(" ___) | |  | | / __/|_____|| |___| |  | | |_| |");
            ImGui::Text("|____/|_|  |_||_____|      |_____|_|  |_|\\___/");
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("A Sega Model 2 arcade emulator"));
            ImGui::Spacing();
            ImGui::Text("Copyright (c) 2025+ Daniel Martin (dmanlfc)");
            ImGui::TextUnformatted(tr("BSD 3-Clause licence. See LICENSE."));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("sm2-emu exists because of open source shared with the community."));
            ImGui::TextUnformatted(tr("With thanks to the projects whose code makes it possible:"));
            ImGui::Spacing();
            ImGui::BulletText("%s", tr("The MAME project - Sega Model 2 emulation guidance"));
            ImGui::BulletText("%s", tr("Musashi - Motorola 68000 CPU core (Karl Stenerud)"));
            ImGui::BulletText("%s", tr("ymfm - Yamaha FM sound cores (Aaron Giles)"));
            ImGui::BulletText("%s", tr("SDL - windowing, input and audio"));
            ImGui::BulletText("%s", tr("Dear ImGui - this user interface (Omar Cornut)"));
            ImGui::BulletText("%s", tr("pugixml - games.xml parsing"));
            ImGui::BulletText("%s", tr("miniz - ZIP archive decompression"));
            ImGui::BulletText("%s", tr("LZMA SDK - 7-Zip archive decompression (Igor Pavlov)"));
            ImGui::BulletText("%s", tr("VulkanMemoryAllocator - GPU memory (AMD / GPUOpen)"));
            ImGui::BulletText("%s", tr("shaderc / glslang - shader compilation"));
            ImGui::BulletText("%s", tr("stb_image - box-art image decoding (Sean Barrett)"));
            ImGui::BulletText("%s", tr("Gamepad picture - Xelu's Free Controller Prompts (Nicolae Berbece), CC0"));
            ImGui::BulletText("%s", tr("libcurl - artwork scraping (optional)"));
            ImGui::BulletText("%s", tr("HarfBuzz - text shaping for translations"));
            ImGui::BulletText("%s", tr("SheenBidi - right-to-left text layout (Muhammad Tayyab Akram)"));
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("Game artwork and descriptions from ArcadeDB"));
            ImGui::TextUnformatted(tr("(adb.arcadeitalia.net, by Motoschifo)."));
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("See NOTICE for full per-component attribution."));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("If you'd like to support development:"));
            ImGui::Spacing();
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::Text("PayPal:");
            ImGui::SameLine();
            ImGui::TextLinkOpenURL("paypal.me/dmanlfc", "https://www.paypal.com/paypalme/dmanlfc");
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::Text("Ko-fi:");
            ImGui::SameLine();
            ImGui::TextLinkOpenURL("ko-fi.com/dmanlfc", "https://ko-fi.com/dmanlfc");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();

        // Drawn here so it keeps showing even after a tab switch.
        draw_dir_picker_popup(config);

        // Save button, on the tabs that carry settings but not on About.
        if (!on_about_tab) {
            ImGui::Separator();
            if (ImGui::Button(tr_id("Save settings"))) {
                const std::string path =
                    m_config_path.empty() ? default_config_path() : m_config_path;
                if (save_config(path, config)) {
                    SM2_INFO("gui: settings saved to %s", path.c_str());
                } else {
                    SM2_ERROR("gui: could not save settings to %s", path.c_str());
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", trf("Saved to %s", m_config_path.empty()
                                                     ? default_config_path().c_str()
                                                     : m_config_path.c_str()).c_str());
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
        ImGui::TextDisabled("%s", tr("Wheel connected."));
    } else {
        ImGui::TextDisabled("%s", tr("No wheel connected. Settings still apply once one is."));
    }
    ImGui::Spacing();

    // -- feel ---------------------------------------------------------------
    ImGui::Checkbox(tr_id("Force feedback"), &config.wheel_ffb);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Wheel force on driving games: centring and cornering\n"
                   "forces decoded from the game's own drive-board commands,\n"
                   "plus an impact jolt when you hit something."));
    }

    ImGui::BeginDisabled(!config.wheel_ffb);
    int resistance = static_cast<int>(config.wheel_ffb_strength);
    if (ImGui::SliderInt(tr_id("Resistance"), &resistance, 0, 100, "%d%%")) {
        resistance = ((resistance + 5) / 10) * 10;  // snap to 10 % steps
        config.wheel_ffb_strength = static_cast<u32>(std::clamp(resistance, 0, 100));
    }

    ImGui::Checkbox(tr_id("Invert force feedback"), &config.wheel_ffb_invert);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("For a wheel whose driver pushes the wrong way: it pulls\n"
                   "away from the centre instead of bringing it back, and\n"
                   "the game's forces go the opposite way."));
    }
    ImGui::EndDisabled();

    ImGui::Checkbox(tr_id("Rumble"), &config.wheel_rumble);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Vibrates on the game's impacts, for wheels without force\n"
                   "feedback. It never runs alongside force feedback."));
    }
    ImGui::BeginDisabled(!config.wheel_rumble);
    int rumble = static_cast<int>(config.wheel_rumble_strength);
    if (ImGui::SliderInt(tr_id("Rumble strength"), &rumble, 0, 100, "%d%%")) {
        rumble = ((rumble + 5) / 10) * 10;
        config.wheel_rumble_strength = static_cast<u32>(std::clamp(rumble, 0, 100));
    }
    ImGui::Checkbox(tr_id("Engine vibration"), &config.wheel_rumble_engine);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("An added vibration that grows with the throttle. It is not\n"
                   "from the game. Turn it off to feel only the game's force feedback."));
    }
    ImGui::EndDisabled();

    // Common wheel rotation ranges rather than a free slider: a wheel is set to
    // one of these, and 270 matches the Model 2 cabinet.
    static constexpr u32 kSteerRanges[] = {200, 240, 270, 360, 400, 540, 720, 900, 1080};
    const std::string current_range = trf("%u deg", config.wheel_steer_degrees);
    if (ImGui::BeginCombo(tr_id("Steering range"), current_range.c_str())) {
        for (const u32 range : kSteerRanges) {
            const std::string label    = trf("%u deg", range);
            const bool        selected = config.wheel_steer_degrees == range;
            if (ImGui::Selectable(label.c_str(), selected)) {
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
        tooltip(tr("Set this to your wheel's own rotation range. The lock\n"
                   "angle below is mapped onto it, so matching your wheel\n"
                   "gives arcade-like response."));
    }

    int lock = static_cast<int>(config.wheel_lock_degrees);
    if (ImGui::SliderInt(tr_id("Lock angle"), &lock, 180, 270,
                         verbatim_format(trf("%d deg", lock)).c_str())) {
        config.wheel_lock_degrees = static_cast<u32>(std::clamp(lock, 180, 270));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("How far to physically turn for full game lock (total,\n"
                   "so half each side of centre). Lower is more sensitive."));
    }

    // -- axis calibration ---------------------------------------------------
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Axes"));
    ImGui::TextDisabled("%s", tr("Auto-detected. Recalibrate if steering or a pedal is wrong."));

    struct AxisRow { const char* name; s32* axis; bool* invert; };
    const AxisRow axis_rows[] = {
        {N_("Steering"), &config.wheel_steer_axis, nullptr},
        {N_("Accelerator"), &config.wheel_accel_axis, &config.wheel_accel_invert},
        {N_("Brake"), &config.wheel_brake_axis, &config.wheel_brake_invert},
    };
    const float axis_column = value_column(axis_rows, [](const AxisRow& r) { return r.name; });

    ImGui::BeginDisabled(!connected);
    for (int row = 0; row < 3; ++row) {
        const AxisRow& r = axis_rows[row];
        ImGui::PushID(row);
        ImGui::TextUnformatted(tr(r.name));
        ImGui::SameLine(axis_column);
        if (*r.axis < 0) {
            ImGui::TextUnformatted(tr("auto"));
        } else if (r.invert != nullptr && *r.invert) {
            ImGui::TextUnformatted(trf("axis %d (inverted)", *r.axis).c_str());
        } else {
            ImGui::TextUnformatted(trf("axis %d", *r.axis).c_str());
        }
        ImGui::SameLine();
        const bool capturing = m_capture == Capture::Axis && m_capture_axis == row;
        if (capturing) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", tr("operate it..."));
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
            if (ImGui::SmallButton(tr_id("cancel"))) {
                m_capture = Capture::None;
            }
        } else if (ImGui::SmallButton(tr_id("Calibrate"))) {
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
        if (ImGui::SmallButton(tr_id("auto"))) {
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
    ImGui::TextUnformatted(tr("Buttons"));
    ImGui::TextDisabled("%s", tr("Press Bind, then press the wheel button for that control."));

    struct ButtonRow { const char* name; Config::WheelRole role; };
    const ButtonRow button_rows[] = {
        {N_("Start"),      Config::WheelRole::Start},
        {N_("Coin"),       Config::WheelRole::Coin},
        {N_("Button 1"),   Config::WheelRole::Button1},
        {N_("Button 2"),   Config::WheelRole::Button2},
        {N_("Button 3"),   Config::WheelRole::Button3},
        {N_("Button 4"),   Config::WheelRole::Button4},
        {N_("Shift up"),   Config::WheelRole::GearUp},
        {N_("Shift down"), Config::WheelRole::GearDown},
        {N_("Test"),       Config::WheelRole::Test},
        {N_("Service"),    Config::WheelRole::Service},
        {N_("Menu (F10)"), Config::WheelRole::Menu},
    };
    const float button_column =
        value_column(button_rows, [](const ButtonRow& r) { return r.name; });

    ImGui::BeginDisabled(!connected);
    for (const ButtonRow& r : button_rows) {
        const u32 role_index = static_cast<u32>(r.role);
        ImGui::PushID(static_cast<int>(role_index));
        const s32 bound = config.wheel_buttons[role_index];
        ImGui::TextUnformatted(tr(r.name));
        ImGui::SameLine(button_column);
        if (bound < 0) {
            ImGui::TextUnformatted(tr("unbound"));
        } else {
            ImGui::TextUnformatted(trf("button %d", bound).c_str());
        }
        ImGui::SameLine();
        const bool capturing = m_capture == Capture::Button && m_capture_role == role_index;
        if (capturing) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", tr("press a button..."));
            if (input != nullptr) {
                const s32 got = input->pressed_wheel_button();
                if (got >= 0) {
                    config.wheel_buttons[role_index] = got;
                    m_capture = Capture::None;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr_id("cancel"))) {
                m_capture = Capture::None;
            }
        } else if (ImGui::SmallButton(tr_id("Bind"))) {
            m_capture      = Capture::Button;
            m_capture_role = role_index;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr_id("clear"))) {
            config.wheel_buttons[role_index] = -1;
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Gamepad tab
// ---------------------------------------------------------------------------

namespace {

/// Where each physical control sits on the pad picture, as fractions of its size.
struct PadArtLayout {
    ImVec2 south, east, west, north, back, guide, start, lstick, rstick, lb, rb;
    ImVec2 up, down, left, right, lt, rt;
};

constexpr PadArtLayout kPadArt = {
    {0.752f, 0.534f}, {0.818f, 0.438f}, {0.685f, 0.438f}, {0.752f, 0.337f},
    {0.435f, 0.441f}, {0.503f, 0.286f}, {0.573f, 0.441f},
    {0.258f, 0.441f}, {0.630f, 0.650f}, {0.260f, 0.155f}, {0.740f, 0.155f},
    {0.378f, 0.588f}, {0.378f, 0.715f}, {0.335f, 0.650f}, {0.420f, 0.650f},
    {0.245f, 0.070f}, {0.755f, 0.070f},
};

constexpr ImVec2 kOffArt{-1.0f, -1.0f};

ImVec2 art_button_point(const PadArtLayout& a, s32 button)
{
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return a.south;
        case SDL_GAMEPAD_BUTTON_EAST:           return a.east;
        case SDL_GAMEPAD_BUTTON_WEST:           return a.west;
        case SDL_GAMEPAD_BUTTON_NORTH:          return a.north;
        case SDL_GAMEPAD_BUTTON_BACK:           return a.back;
        case SDL_GAMEPAD_BUTTON_GUIDE:          return a.guide;
        case SDL_GAMEPAD_BUTTON_START:          return a.start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return a.lstick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return a.rstick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return a.lb;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return a.rb;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return a.up;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return a.down;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return a.left;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return a.right;
        default:                                return kOffArt;
    }
}

ImVec2 art_axis_point(const PadArtLayout& a, s32 axis);

/// Where a binding sits on the picture.
ImVec2 art_binding_point(const PadArtLayout& a, s32 binding)
{
    if (binding >= Config::kPadAxisMinus) {
        return art_axis_point(a, binding - Config::kPadAxisMinus);
    }
    if (binding >= Config::kPadAxisPlus) {
        return art_axis_point(a, binding - Config::kPadAxisPlus);
    }
    return art_button_point(a, binding);
}

ImVec2 art_axis_point(const PadArtLayout& a, s32 axis)
{
    switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
        case SDL_GAMEPAD_AXIS_LEFTY:         return a.lstick;
        case SDL_GAMEPAD_AXIS_RIGHTX:
        case SDL_GAMEPAD_AXIS_RIGHTY:        return a.rstick;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return a.lt;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return a.rt;
        default:                             return kOffArt;
    }
}

/// Face buttons in their usual colours; everything else one neutral blue.
ImU32 button_colour(s32 button)
{
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return IM_COL32(96, 200, 96, 255);
        case SDL_GAMEPAD_BUTTON_EAST:  return IM_COL32(224, 84, 84, 255);
        case SDL_GAMEPAD_BUTTON_WEST:  return IM_COL32(96, 136, 236, 255);
        case SDL_GAMEPAD_BUTTON_NORTH: return IM_COL32(232, 200, 64, 255);
        default:                       return IM_COL32(150, 185, 235, 255);
    }
}

s32 default_pad_axis(Config::PadAxisRole role)
{
    switch (role) {
        case Config::PadAxisRole::AimX:  return SDL_GAMEPAD_AXIS_LEFTX;
        case Config::PadAxisRole::AimY:  return SDL_GAMEPAD_AXIS_LEFTY;
        case Config::PadAxisRole::Accel:  return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        case Config::PadAxisRole::Brake:  return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        case Config::PadAxisRole::LeverX: return SDL_GAMEPAD_AXIS_RIGHTX;
        case Config::PadAxisRole::LeverY: return SDL_GAMEPAD_AXIS_RIGHTY;
        default:                          return -1;
    }
}

}  // namespace

const Gui::PadArt* Gui::pad_art()
{
    if (m_backend == nullptr) {
        return nullptr;
    }
    PadArt& art = m_pad_art;
    if (!art.tried) {
        art.tried = true;
        int            w = 0, h = 0, channels = 0;
        unsigned char* rgba = stbi_load_from_memory(
            assets::pad_xbox_outline, static_cast<int>(assets::pad_xbox_outline_size), &w, &h,
            &channels, 4);
        if (rgba != nullptr) {
            art.handle = m_backend->create_texture(static_cast<u32>(w), static_cast<u32>(h), rgba);
            art.w      = static_cast<float>(w);
            art.h      = static_cast<float>(h);
            stbi_image_free(rgba);
        }
    }
    return art.handle != 0 ? &art : nullptr;
}

void Gui::release_pad_art()
{
    if (m_pad_art.handle != 0 && m_backend != nullptr) {
        m_backend->destroy_texture(m_pad_art.handle);
    }
    m_pad_art = PadArt{};
}

void Gui::draw_gamepad_tab(Config& config, Input* input)
{
    const int pads = input != nullptr ? static_cast<int>(input->pad_count()) : 0;
    if (pads > 0) {
        ImGui::TextDisabled("%s", i18n::trnf("%d gamepad connected.", "%d gamepads connected.",
                                             static_cast<unsigned long>(pads), pads).c_str());
    } else {
        ImGui::TextDisabled("%s", tr("No gamepad connected. Settings still apply once one is."));
    }

    int&       edit = m_pad_edit_player;
    const auto p    = static_cast<usize>(edit);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Player"));
    ImGui::SameLine();
    if (ImGui::RadioButton("1", edit == 0)) { edit = 0; m_pad_capture_player = -1; m_pad_axis_capture_player = -1; }
    ImGui::SameLine();
    if (ImGui::RadioButton("2", edit == 1)) { edit = 1; m_pad_capture_player = -1; m_pad_axis_capture_player = -1; }
    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::Button(tr_id("Reset to SDL defaults"))) {
        config.pad_bindings[p]    = Config{}.pad_bindings[p];
        config.pad_axes[p]         = Config{}.pad_axes[p];
        config.pad_axis_invert[p]  = Config{}.pad_axis_invert[p];
        config.pad_axis_buttons[p] = Config{}.pad_axis_buttons[p];
        m_pad_capture_player       = -1;
        m_pad_axis_capture_player  = -1;
    }

    // -- the picture, with a slot for every control around it ---------------
    // A slot per cabinet control; a stick slot binds both axes of its role.
    enum class Kind { Button, Pedal, Stick };
    struct Slot {
        const char* name;
        Kind        kind;
        u32         role;
        int         column;  // 0 left of the picture, 1 right, 2 under the left stick, 3 under the right
    };
    static const Slot kSlots[] = {
        {N_("Brake"),       Kind::Pedal,  static_cast<u32>(Config::PadAxisRole::Brake), 0},
        {N_("Gear Down"),   Kind::Button, static_cast<u32>(Config::PadRole::GearDown),  0},
        {N_("Move Up"),     Kind::Button, static_cast<u32>(Config::PadRole::Up),        0},
        {N_("Move Left"),   Kind::Button, static_cast<u32>(Config::PadRole::Left),      0},
        {N_("Move Right"),  Kind::Button, static_cast<u32>(Config::PadRole::Right),     0},
        {N_("Move Down"),   Kind::Button, static_cast<u32>(Config::PadRole::Down),      0},
        {N_("Coin"),        Kind::Button, static_cast<u32>(Config::PadRole::Coin),      0},
        {N_("Accelerator"), Kind::Pedal,  static_cast<u32>(Config::PadAxisRole::Accel), 1},
        {N_("Gear Up"),     Kind::Button, static_cast<u32>(Config::PadRole::GearUp),    1},
        {N_("Button 4"),    Kind::Button, static_cast<u32>(Config::PadRole::Button4),   1},
        {N_("Button 3"),    Kind::Button, static_cast<u32>(Config::PadRole::Button3),   1},
        {N_("Button 2"),    Kind::Button, static_cast<u32>(Config::PadRole::Button2),   1},
        {N_("Button 1"),    Kind::Button, static_cast<u32>(Config::PadRole::Button1),   1},
        {N_("Start"),       Kind::Button, static_cast<u32>(Config::PadRole::Start),     1},
        {N_("Steer / Aim"), Kind::Stick,  static_cast<u32>(Config::PadAxisRole::AimX),   2},
        {N_("Right Lever"), Kind::Stick,  static_cast<u32>(Config::PadAxisRole::LeverX), 3},
    };

    const float         s      = m_ui_scale > 0.0f ? m_ui_scale : 1.0f;
    const PadArtLayout& layout = kPadArt;
    const PadArt*       art    = pad_art();
    ImDrawList*         draw   = ImGui::GetWindowDrawList();
    const ImVec2        origin = ImGui::GetCursorScreenPos();
    const float         avail  = ImGui::GetContentRegionAvail().x;
    const float         slot_w = 196.0f * s;
    const float         slot_h = 28.0f * s;
    const float         gap    = 26.0f * s;
    const float         img_w  = std::clamp(avail - 2.0f * (slot_w + gap), 240.0f * s, 620.0f * s);
    const float         img_h  = img_w * (art != nullptr ? art->h / art->w : 0.65f);
    const ImVec2        img0(origin.x + (avail - img_w) * 0.5f, origin.y + 40.0f * s);
    const ImVec2        img1(img0.x + img_w, img0.y + img_h);
    if (art != nullptr) {
        draw->AddImage(reinterpret_cast<ImTextureID>(m_backend->texture_imgui_id(art->handle)),
                       img0, img1);
    } else {
        draw->AddRect(img0, img1, IM_COL32(90, 95, 105, 255), 12.0f * s);
    }
    const auto at = [&](ImVec2 f) { return ImVec2(img0.x + f.x * img_w, img0.y + f.y * img_h); };

    // Light every button that is down.
    if (input != nullptr) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            const ImVec2 f = art_button_point(layout, b);
            if (f.x >= 0.0f && input->pad_physical_down(static_cast<u32>(edit), b)) {
                draw->AddCircle(at(f), 10.0f * s, IM_COL32(255, 255, 255, 230), 0, 2.5f * s);
            }
        }
    }

    const float  font      = ImGui::GetFontSize();
    const float  pitch     = img_h / 6.0f;
    const float  left_x    = img0.x - gap - slot_w;
    const float  right_x   = img1.x + gap;
    const float  below_y   = img1.y + 20.0f * s;
    int          rows[4]   = {0, 0, 0, 0};
    for (const Slot& slot : kSlots) {
        const int row = rows[slot.column]++;
        ImVec2    pos;
        ImVec2    anchor;  // where the connector leaves the slot
        if (slot.column == 0) {
            pos    = ImVec2(left_x, img0.y + row * pitch - slot_h * 0.5f);
            anchor = ImVec2(pos.x + slot_w, pos.y + slot_h * 0.5f);
        } else if (slot.column == 1) {
            pos    = ImVec2(right_x, img0.y + row * pitch - slot_h * 0.5f);
            anchor = ImVec2(pos.x, pos.y + slot_h * 0.5f);
        } else {
            // Under its stick, so the connector is vertical.
            const float stick_x = at(slot.column == 2 ? layout.lstick : layout.rstick).x;
            const float inset   = 30.0f * s;
            pos    = ImVec2(slot.column == 2 ? stick_x + inset - slot_w : stick_x - inset, below_y);
            anchor = ImVec2(stick_x, pos.y);
        }

        const bool  is_axis = slot.kind != Kind::Button;
        std::string value;
        ImVec2      target = kOffArt;
        ImU32       colour = IM_COL32(150, 185, 235, 255);
        bool        capturing;
        if (!is_axis) {
            const s32 bound = config.pad_bindings[p][slot.role];
            value           = bound < 0 ? tr("unbound") : Input::pad_button_name(bound);
            target          = art_binding_point(layout, bound);
            colour          = button_colour(bound);
            capturing       = m_pad_capture_player == edit && m_pad_capture_role == slot.role;
        } else {
            const auto role  = static_cast<Config::PadAxisRole>(slot.role);
            const s32  bound = config.pad_axes[p][slot.role];
            const s32  shown = bound >= 0 ? bound : default_pad_axis(role);
            if (slot.kind == Kind::Stick) {
                value = shown == SDL_GAMEPAD_AXIS_RIGHTX || shown == SDL_GAMEPAD_AXIS_RIGHTY
                            ? tr("Right Stick")
                        : shown == SDL_GAMEPAD_AXIS_LEFTX || shown == SDL_GAMEPAD_AXIS_LEFTY
                            ? tr("Left Stick")
                                    : Input::pad_axis_name(shown);
            } else {
                value = Input::pad_axis_name(shown);
                if (config.pad_axis_invert[p][slot.role]) {
                    value = trf("%s (inverted)", value.c_str());
                }
            }
            target = art_axis_point(layout, shown);
            if (const s32 button = config.pad_axis_buttons[p][slot.role];
                slot.kind == Kind::Pedal && button >= 0) {
                value  = Input::pad_button_name(button);
                target = art_binding_point(layout, button);
                colour = button_colour(button);
            }
            capturing = m_pad_axis_capture_player == edit && m_pad_axis_capture_role == slot.role;
        }

        ImGui::PushID(slot.name);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("slot", ImVec2(slot_w, slot_h));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            tooltip(slot.kind == Kind::Stick ? tr("Click, then push a stick.\n"
                                                  "Right-click to go back to the default.")
                    : slot.kind == Kind::Pedal ? tr("Click, then pull the trigger, push a stick\n"
                                                    "or press a button. Right-click for the default.")
                                               : tr("Click, then press a button or pull a trigger.\n"
                                                    "Right-click to unbind."));
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (capturing) {
                m_pad_capture_player      = -1;
                m_pad_axis_capture_player = -1;
            } else if (is_axis) {
                m_pad_capture_player      = -1;
                m_pad_axis_capture_player = edit;
                m_pad_axis_capture_role   = slot.role;
                if (input != nullptr) {
                    input->pad_axis_baseline(static_cast<u32>(edit), m_pad_axis_baseline.data());
                }
            } else {
                m_pad_axis_capture_player = -1;
                m_pad_capture_player      = edit;
                m_pad_capture_role        = slot.role;
                if (input != nullptr) {
                    input->pad_axis_baseline(static_cast<u32>(edit), m_pad_axis_baseline.data());
                }
            }
            capturing = !capturing;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (slot.kind == Kind::Stick) {
                for (const u32 axis : {slot.role, slot.role + 1}) {
                    config.pad_axes[p][axis]        = -1;
                    config.pad_axis_invert[p][axis] = false;
                }
            } else if (is_axis) {
                config.pad_axes[p][slot.role]         = -1;
                config.pad_axis_invert[p][slot.role]  = false;
                config.pad_axis_buttons[p][slot.role] = -1;
            } else {
                config.pad_bindings[p][slot.role] = -1;
            }
        }

        const ImVec2 end(pos.x + slot_w, pos.y + slot_h);
        const ImU32  bg = capturing ? IM_COL32(44, 68, 108, 255)
                        : hovered   ? IM_COL32(54, 58, 68, 255)
                                    : IM_COL32(38, 41, 48, 255);
        const ImU32  border = capturing ? IM_COL32(110, 160, 240, 255) : IM_COL32(72, 77, 88, 255);
        draw->AddRectFilled(pos, end, bg, 6.0f * s);
        draw->AddRect(pos, end, border, 6.0f * s, 0, 1.5f);
        const float text_y = pos.y + (slot_h - font) * 0.5f;
        draw->PushClipRect(pos, end, true);
        draw->AddText(ImVec2(pos.x + 10.0f * s, text_y), IM_COL32(230, 232, 236, 255), tr(slot.name));
        if (capturing) {
            value = is_axis ? tr("operate it...") : tr("press a button...");
        }
        const ImU32 value_col = capturing ? IM_COL32(140, 190, 255, 255) : IM_COL32(170, 176, 186, 255);
        const float value_w   = ImGui::CalcTextSize(value.c_str()).x;
        draw->AddText(ImVec2(end.x - 10.0f * s - value_w, text_y), value_col, value.c_str());
        draw->PopClipRect();

        if (target.x >= 0.0f) {
            const ImVec2 dot  = at(target);
            const ImU32  line = (colour & 0x00ffffffu) | 0x80000000u;
            if (slot.column < 2) {
                const ImVec2 edge(slot.column == 0 ? img0.x - 8.0f * s : img1.x + 8.0f * s, anchor.y);
                draw->AddLine(anchor, edge, line, 1.5f * s);
                draw->AddLine(edge, dot, line, 1.5f * s);
            } else {
                draw->AddLine(anchor, dot, line, 1.5f * s);
            }
            draw->AddCircleFilled(dot, 4.5f * s, colour);
        }

        if (capturing && input != nullptr) {
            if (slot.kind == Kind::Stick) {
                // Either axis of a stick binds the whole stick.
                bool      positive = true;
                const s32 got      = input->captured_pad_axis(
                    static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                const bool right = got == SDL_GAMEPAD_AXIS_RIGHTX || got == SDL_GAMEPAD_AXIS_RIGHTY;
                const bool left  = got == SDL_GAMEPAD_AXIS_LEFTX || got == SDL_GAMEPAD_AXIS_LEFTY;
                if (left || right) {
                    const u32 x = slot.role;
                    const u32 y = slot.role + 1;
                    config.pad_axes[p][x]        = right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
                    config.pad_axes[p][y]        = right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
                    config.pad_axis_invert[p][x] = false;
                    config.pad_axis_invert[p][y] = false;
                    m_pad_axis_capture_player    = -1;
                }
            } else if (is_axis) {
                bool      positive = true;
                const s32 got      = input->captured_pad_axis(
                    static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                const s32 button = input->pressed_pad_button(static_cast<u32>(edit));
                if (got >= 0) {
                    config.pad_axes[p][slot.role]         = got;
                    config.pad_axis_invert[p][slot.role]  = !positive;
                    config.pad_axis_buttons[p][slot.role] = -1;
                    m_pad_axis_capture_player             = -1;
                } else if (button >= 0) {
                    config.pad_axis_buttons[p][slot.role] = button;
                    m_pad_axis_capture_player             = -1;
                }
            } else {
                s32 got = input->pressed_pad_button(static_cast<u32>(edit));
                if (got < 0) {
                    bool      positive = true;
                    const s32 axis     = input->captured_pad_axis(
                        static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                    if (axis >= 0) {
                        got = (positive ? Config::kPadAxisPlus : Config::kPadAxisMinus) + axis;
                    }
                }
                if (got >= 0) {
                    config.pad_bindings[p][slot.role] = got;
                    m_pad_capture_player              = -1;
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, below_y + slot_h + 10.0f * s));
    ImGui::TextDisabled("%s", tr("The left stick also moves; the shoulders also act as Button 3/4. Right Lever "
                        "is Virtual On's. Test and Service are Guide+Start and Guide+Back."));

    // -- sticks and rumble ---------------------------------------------------
    ImGui::Separator();
    int sensitivity = static_cast<int>(config.pad_stick_sensitivity);
    if (ImGui::SliderInt(tr_id("Stick sensitivity"), &sensitivity, 25, 300, "%d%%")) {
        sensitivity = ((sensitivity + 2) / 5) * 5;
        config.pad_stick_sensitivity = static_cast<u32>(std::clamp(sensitivity, 25, 300));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Response of a stick's travel past its deadzone. Full deflection\n"
                   "is always full lock; under 100% the middle of the travel does\n"
                   "less, over 100% it does more."));
    }

    ImGui::Checkbox(tr_id("Rumble"), &config.pad_rumble);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Vibrates on the game's impacts in driving games.\n"
                   "Other games stay quiet."));
    }

    ImGui::BeginDisabled(!config.pad_rumble);
    int strength = static_cast<int>(config.pad_rumble_strength);
    if (ImGui::SliderInt(tr_id("Rumble strength"), &strength, 0, 100, "%d%%")) {
        strength = ((strength + 5) / 10) * 10;  // snap to 10 % steps
        config.pad_rumble_strength = static_cast<u32>(std::clamp(strength, 0, 100));
    }
    ImGui::Checkbox(tr_id("Steering vibration"), &config.pad_rumble_cornering);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("An added vibration that grows as you steer. It is not\n"
                   "from the game. Turn it off to feel only the game's force feedback."));
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Audio tab
// ---------------------------------------------------------------------------

bool Gui::draw_volume_slider(Config& config, const VolumeFamily& family)
{
    const auto found = config.game_volumes.find(family.key);
    const u32  current =
        found != config.game_volumes.end() ? found->second : Config::kDefaultGameVolume;

    ImGui::PushID(family.key.c_str());
    bool changed = false;
    int  percent = static_cast<int>(current);
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::SliderInt("##volume", &percent, 0,
                         static_cast<int>(Config::kMaxGameVolume), "%d%%")) {
        percent = ((percent + 2) / 5) * 5;  // snap to 5 % steps
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(current == Config::kDefaultGameVolume);
    if (ImGui::Button(tr_id("Reset"))) {
        percent = static_cast<int>(Config::kDefaultGameVolume);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    if (changed) {
        const u32 value = static_cast<u32>(
            std::clamp(percent, 0, static_cast<int>(Config::kMaxGameVolume)));
        // 100 is stored as no entry, so the ini lists only changed families.
        if (value == Config::kDefaultGameVolume) {
            config.game_volumes.erase(family.key);
        } else {
            config.game_volumes[family.key] = value;
        }
    }
    return changed;
}

void Gui::draw_audio_tab(Config& config)
{
    ImGui::Checkbox(tr_id("Per-game volume"), &config.game_volume);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Raise or lower a game's volume from its default level.\n"
                   "One slider covers every revision of a game. Off plays\n"
                   "every game at its default, whatever the sliders say.\n"
                   "\n"
                   "Warning: above 100% loud effects can clip (audible\n"
                   "distortion)."));
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(!config.game_volume);

    // The running game's slider first.
    const VolumeFamily* playing = nullptr;
    for (const VolumeFamily& family : m_volume_families) {
        if (family.key == m_current_volume_family) {
            playing = &family;
            break;
        }
    }
    if (playing != nullptr) {
        ImGui::TextUnformatted(trf("Now playing: %s", playing->title.c_str()).c_str());
        draw_volume_slider(config, *playing);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##volume_filter", tr("Filter games"), m_volume_filter,
                             sizeof(m_volume_filter));
    ImGui::SameLine();
    const bool any_changed = !config.game_volumes.empty();
    ImGui::BeginDisabled(!any_changed);
    if (ImGui::Button(tr_id("Reset all"))) {
        config.game_volumes.clear();
    }
    ImGui::EndDisabled();

    std::string filter = m_volume_filter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Leave room for the Save row below the tabs.
    const ImGuiStyle& style  = ImGui::GetStyle();
    const float       footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f;
    const float       height = std::max(ImGui::GetContentRegionAvail().y - footer,
                                        ImGui::GetFrameHeightWithSpacing() * 4.0f);

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_BordersInnerH
                                     | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("volumes", 2, kFlags, ImVec2(0.0f, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Game"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(tr("Volume"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableHeadersRow();

        for (const VolumeFamily& family : m_volume_families) {
            if (!filter.empty()) {
                std::string haystack = family.title + " " + family.key;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (haystack.find(filter) == std::string::npos) {
                    continue;
                }
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(family.title.c_str());
            ImGui::TableNextColumn();
            draw_volume_slider(config, family);
        }
        ImGui::EndTable();
    }

    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Light Gun tab
// ---------------------------------------------------------------------------

void Gui::draw_lightgun_tab(Config& config, Input* input)
{
    ImGui::Checkbox(tr_id("Light-gun mode"), &config.lightgun);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Show the aiming crosshair and hide the mouse cursor,\n"
                   "for the light-gun titles (Virtua Cop 2, House of the\n"
                   "Dead, Gunblade NY, Rail Chase 2, Behind Enemy Lines)."));
    }

    ImGui::BeginDisabled(!config.lightgun);
    ImGui::Checkbox(tr_id("Show crosshair"), &config.lightgun_crosshair);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Draw sm2-emu's own crosshair. Turn off if the gun\n"
                   "has its own sight (e.g. a Sinden). Positional-gun\n"
                   "titles always draw their own regardless."));
    }
    ImGui::EndDisabled();

    ImGui::Checkbox(tr_id("Hide shot flash"), &config.lightgun_hide_flash);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Hide the white flash Virtua Cop, Virtua Cop 2 and\n"
                   "House of the Dead show on each shot."));
    }

    // Dedicated light guns (recoil motors, per-device buttons) need the evdev
    // or Raw Input backend. Without one, the mouse is the gun.
#ifndef SM2_HAVE_LIGHTGUNS
    (void)input;
    ImGui::Separator();
    text_wrapped("Player 1 aims with the mouse; the left button fires and the right "
                 "button reloads (shoot off screen).");
#else
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Recoil"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Kick the gun's rumble motor on each shot, for guns\n"
                   "that have one (Sinden and similar). No effect on a\n"
                   "gun without a motor."));
    }
    ImGui::Checkbox(tr_id("Enable recoil"), &config.lightgun_recoil);
    ImGui::BeginDisabled(!config.lightgun_recoil);
    int strength = static_cast<int>(config.lightgun_recoil_strength);
    if (ImGui::SliderInt(tr_id("Strength"), &strength, 0, 100)) {
        config.lightgun_recoil_strength = static_cast<u32>(std::clamp(strength, 0, 100));
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted(tr("Devices"));
    if (input != nullptr && input->gun_count() > 0) {
        for (usize i = 0; i < input->gun_count(); ++i) {
            ImGui::BulletText("%s", trf("Gun %zu (player %zu): %s", i + 1, i + 1,
                                        input->gun_name(i).c_str()).c_str());
        }
    } else {
#ifdef _WIN32
        text_wrapped("No dedicated light guns detected. Player 1 aims with the mouse; the "
                     "left button fires and the right button reloads (shoot off screen). "
                     "Plug in guns in absolute mouse mode (GUN4IR, Sinden, AimTrak) before "
                     "launching for independent per-player aiming.");
#else
        text_wrapped("No dedicated light guns detected. Player 1 aims with the mouse; the "
                     "left button fires and the right button reloads (shoot off screen). "
                     "Plug in guns tagged ID_INPUT_GUN for independent per-player aiming.");
#endif
    }

    ImGui::Separator();
    ImGui::TextUnformatted(tr("Gun buttons"));
    ImGui::TextDisabled("%s", tr("Press Bind, then press the button on that player's gun."));
    {
        static constexpr std::array<const char*, 8> kRoleNames = {
            N_("Trigger"), N_("Reload/Missile"), N_("Coin"), N_("Start"),
            N_("Hat up"), N_("Hat down"), N_("Hat left"), N_("Hat right"),
        };
        const usize guns = input != nullptr ? input->gun_count() : 0;
        const float indent = ImGui::GetFontSize();
        ImGui::Indent(indent);
        const float column = value_column(kRoleNames, [](const char* name) { return name; });
        ImGui::Unindent(indent);
        for (int p = 0; p < 2; ++p) {
            ImGui::TextUnformatted(trf("Player %d", p + 1).c_str());
            ImGui::Indent(indent);
            for (u32 r = 0; r < Config::kGunRoleCount; ++r) {
                ImGui::PushID(p * 100 + static_cast<int>(r));
                const u32 code = config.gun_buttons[static_cast<usize>(p)][r];
                ImGui::TextUnformatted(tr(kRoleNames[r]));
                ImGui::SameLine(column);
                if (code == 0) {
                    ImGui::TextUnformatted(tr("unbound"));
                } else {
                    ImGui::TextUnformatted(evdev_button_name(code).c_str());
                }
                ImGui::SameLine();
                const bool capturing = m_gun_capture_player == p
                                       && m_gun_capture_role == r;
                if (capturing) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", tr("press a button..."));
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
                    if (ImGui::SmallButton(tr_id("cancel"))) m_gun_capture_player = -1;
                } else if (ImGui::SmallButton(tr_id("Bind"))) {
                    m_gun_capture_player = p;
                    m_gun_capture_role   = r;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr_id("clear"))) {
                    config.gun_buttons[static_cast<usize>(p)][r] = 0;
                }
                ImGui::PopID();
            }
            ImGui::Unindent(indent);
        }
    }
#endif  // SM2_HAVE_LIGHTGUNS

    ImGui::Separator();
    ImGui::TextUnformatted(tr("Sinden border"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("A bright frame around the game image that a Sinden\n"
                   "gun's camera tracks for aiming. Only shown in\n"
                   "light-gun mode."));
    }
    ImGui::Checkbox(tr_id("Show border"), &config.sinden_border);

    ImGui::BeginDisabled(!config.sinden_border);
    float rgb[3] = {
        static_cast<float>((config.sinden_border_colour >> 16) & 0xff) / 255.0f,
        static_cast<float>((config.sinden_border_colour >> 8) & 0xff) / 255.0f,
        static_cast<float>(config.sinden_border_colour & 0xff) / 255.0f,
    };
    if (ImGui::ColorEdit3(tr_id("Colour"), rgb, ImGuiColorEditFlags_NoInputs)) {
        const u32 r = static_cast<u32>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 g = static_cast<u32>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 b = static_cast<u32>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        config.sinden_border_colour = (r << 16) | (g << 8) | b;
    }
    int thickness = static_cast<int>(config.sinden_border_thickness);
    if (ImGui::SliderInt(tr_id("Thickness"), &thickness, 1, 64)) {
        config.sinden_border_thickness = static_cast<u32>(std::max(1, thickness));
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Network tab
// ---------------------------------------------------------------------------

void Gui::draw_network_tab(Config& config)
{
    const auto text_field = [](const char* label, std::string& value, float width) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText(tr_id(label), buf, sizeof(buf))) {
            value = buf;
        }
    };

    ImGui::Checkbox(tr_id("Link cabinets over the network"), &config.link_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("Race linked with other machines on the same LAN running the same\n"
                   "title (Sega Rally, Daytona, Super GT 24h, Indy 500, ...). Off keeps\n"
                   "this cabinet standalone. Takes effect on the next game launch."));
    }

    text_wrapped("The comms board is a ring: each cabinet listens on its own address and "
                 "sends to the next cabinet. For two machines, point each at the other. "
                 "The master/slave role is chosen in the game's own test menu, not here.");

    ImGui::BeginDisabled(!config.link_enabled);

    ImGui::SeparatorText(tr("This cabinet"));

    text_field(N_("Local IP"), config.link_local_ip, 180.0f);
    ImGui::SameLine();
    if (ImGui::Button(tr_id("Detect"))) {
        if (auto iface = net::primary_interface()) {
            config.link_local_ip    = iface->ipv4;
            config.link_subnet_mask = iface->subnet_mask;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("This machine's address on the cabinet LAN. Blank\n"
                   "listens on every interface. Detect fills it from the\n"
                   "primary network adapter."));
    }

    text_field(N_("Subnet mask"), config.link_subnet_mask, 180.0f);

    {
        int port = static_cast<int>(config.link_port);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt(tr_id("Listen port"), &port)) {
            config.link_port = static_cast<u32>(std::clamp(port, 1, 65535));
        }
    }

    {
        int idx = static_cast<int>(config.link_cabinet_index);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt(tr_id("Cabinet number"), &idx)) {
            config.link_cabinet_index = static_cast<u32>(std::max(0, idx));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            tooltip(tr("Where this cabinet sits in the ring (0-based).\n"
                       "For your own bookkeeping; the game negotiates the\n"
                       "actual link id from the ring."));
        }
    }

    ImGui::SeparatorText(tr("Next cabinet in the ring"));

    text_field(N_("Next IP"), config.link_next_ip, 180.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        tooltip(tr("The cabinet this one sends to. For two machines, the\n"
                   "other machine's IP. Blank means this cabinet does not\n"
                   "forward (tail of a chain)."));
    }

    {
        int port = static_cast<int>(config.link_next_port);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt(tr_id("Next port"), &port)) {
            config.link_next_port = static_cast<u32>(std::clamp(port, 1, 65535));
        }
    }

    ImGui::EndDisabled();

    // Live status from the running link board.
    ImGui::SeparatorText(tr("Status"));
    if (!m_link_status.active) {
        ImGui::TextDisabled("%s", config.link_enabled
                                      ? tr("Link configured; starts at the next game launch.")
                                      : tr("Standalone (no cabinet link)."));
    } else if (!m_link_status.enabled) {
        ImGui::TextDisabled("%s", tr("Waiting for the game to switch the link board on..."));
    } else if (m_link_status.alive) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                           trf("Linked: cabinet %u of %u.", m_link_status.node,
                               m_link_status.count).c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s",
                           tr("Establishing the ring..."));
    }

    // Available interfaces, so the operator sees what to type.
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("This machine's interfaces:"));
    for (const net::Interface& iface : net::interfaces()) {
        if (iface.loopback) continue;
        ImGui::BulletText("%s  %s / %s", iface.name.c_str(), iface.ipv4.c_str(),
                          iface.subnet_mask.c_str());
    }
}

// ---------------------------------------------------------------------------
// States tab (save / load per-game slots)
// ---------------------------------------------------------------------------

void Gui::draw_states_tab()
{
    if (!m_state_game_loaded) {
        ImGui::TextDisabled("%s", tr("Load a game to save and restore its state."));
        return;
    }

    text_wrapped("Save the whole machine to a slot and restore it later. Quick-save is "
                 "F6, quick-load is F7.");
    ImGui::Spacing();

    // A table so the three buttons always fit regardless of the overlay width,
    // rather than fixed SameLine() offsets that push them off the right edge.
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp;
    // Fixed columns sized to the translated text, which can run longer.
    const ImGuiStyle& style  = ImGui::GetStyle();
    float             slot_w = 70.0f;
    for (const StateSlot& s : m_state_slots) {
        slot_w = std::max(slot_w, ImGui::CalcTextSize(s.label.c_str()).x);
    }
    float actions_w = style.ItemSpacing.x * 2.0f;
    for (const char* action : {tr("Save"), tr("Load"), tr("Delete")}) {
        actions_w += ImGui::CalcTextSize(action).x + style.FramePadding.x * 2.0f;
    }
    if (ImGui::BeginTable("states", 3, kFlags)) {
        ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, slot_w);
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed,
                                std::max(190.0f, actions_w));

        for (const StateSlot& s : m_state_slots) {
            ImGui::PushID(s.slot.c_str());
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.label.c_str());

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (s.occupied) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                                   s.timestamp.c_str());
            } else {
                ImGui::TextDisabled("%s", tr("empty"));
            }

            ImGui::TableNextColumn();
            if (ImGui::Button(tr_id("Save"))) {
                m_pending_state_request = StateRequest{StateRequest::Action::Save, s.slot};
            }
            ImGui::SameLine();
            // Load and Delete only make sense on an occupied slot.
            ImGui::BeginDisabled(!s.occupied);
            if (ImGui::Button(tr_id("Load"))) {
                m_pending_state_request = StateRequest{StateRequest::Action::Load, s.slot};
            }
            ImGui::SameLine();
            if (ImGui::Button(tr_id("Delete"))) {
                m_pending_state_request = StateRequest{StateRequest::Action::Delete, s.slot};
            }
            ImGui::EndDisabled();

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// FPS overlay (top right)
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
// On-screen notification (transient toast, top-centered)
// ---------------------------------------------------------------------------

void Gui::notify(std::string message)
{
    m_notify_text         = std::move(message);
    m_notify_seconds_left = 2.0f;  // a couple of seconds, per the request
}

void Gui::draw_notification()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Same font and size as the FPS overlay (the default font, no scaling),
    // top-centered rather than top-right.
    const ImVec2 text_size = ImGui::CalcTextSize(m_notify_text.c_str());
    const ImVec2 padding{8.0F, 4.0F};
    const ImVec2 window_size{text_size.x + padding.x * 2.0F, text_size.y + padding.y * 2.0F};

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - window_size.x) * 0.5F,
               viewport->WorkPos.y + 10.0F));
    ImGui::SetNextWindowSize(window_size);

    // Fade out over the last half-second so it does not just blink off.
    float alpha = 0.55F;
    if (m_notify_seconds_left < 0.5F) {
        alpha *= m_notify_seconds_left / 0.5F;
    }
    ImGui::SetNextWindowBgAlpha(alpha);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha / 0.55F);
    if (ImGui::Begin("##Notification", nullptr, kFlags)) {
        ImGui::TextUnformatted(m_notify_text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
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
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y),
        m_present_aspect, m_present_method);

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
    // mouse in the overlay. A player on a gun or the pad cursor still freezes,
    // which is correct -- its position only exists when polled.

    for (usize player = 0; player < aims.size(); ++player) {
        const Input::GunAim& aim = aims[player];
        if (!aim.active || aim.calibrating) {
            continue;
        }
        float fx = aim.x;
        float fy = aim.y;
        if (player == 0 && aim.mouse && box.width > 0.0f && box.height > 0.0f) {
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

// A host calibration tool (Batocera's) steps the gun's reported position through
// its targets and expects the screen to show where each one is, so the player
// can shoot it. The marker is drawn on the window rather than the game image,
// because the tool works in screen space, and regardless of the crosshair
// setting, because these guns usually run without one.
void Gui::draw_calibration_markers(const Input* input)
{
    if (input == nullptr) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) {
        return;
    }

    ImDrawList* list   = ImGui::GetForegroundDrawList();
    const float radius = std::max(12.0f, io.DisplaySize.y * 0.03f);
    bool        any    = false;
    for (const Input::GunAim& aim : input->gun_aims()) {
        if (!aim.active || !aim.calibrating) {
            continue;
        }
        any = true;
        const float cx = std::clamp(aim.win_x, 0.0f, 1.0f) * io.DisplaySize.x;
        const float cy = std::clamp(aim.win_y, 0.0f, 1.0f) * io.DisplaySize.y;
        const ImU32 white = IM_COL32(255, 255, 255, 240);
        const ImU32 red   = IM_COL32(255, 40, 40, 240);
        list->AddCircle(ImVec2(cx, cy), radius, white, 32, 3.0f);
        list->AddCircleFilled(ImVec2(cx, cy), radius * 0.2f, red);
        list->AddLine(ImVec2(cx - radius * 1.8f, cy), ImVec2(cx + radius * 1.8f, cy), white, 2.0f);
        list->AddLine(ImVec2(cx, cy - radius * 1.8f), ImVec2(cx, cy + radius * 1.8f), white, 2.0f);
    }
    if (any) {
        const char* text = tr("CALIBRATING GUN - shoot each target");
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 pos((io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y * 0.08f);
        list->AddRectFilled(ImVec2(pos.x - 12.0f, pos.y - 6.0f),
                            ImVec2(pos.x + size.x + 12.0f, pos.y + size.y + 6.0f),
                            IM_COL32(0, 0, 0, 160), 4.0f);
        list->AddText(pos, IM_COL32(255, 255, 255, 255), text);
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
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y),
        m_present_aspect, m_present_method);

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
// Directory picker (Paths tab "Browse..." buttons)
// ---------------------------------------------------------------------------

void Gui::open_dir_picker(DirPickerTarget target, const std::string& initial)
{
    namespace fs = std::filesystem;

    m_dir_picker_target = target;

    std::error_code ec;
    fs::path        start(initial);
    if (initial.empty() || !fs::is_directory(start, ec)) {
        // Fall back to the home directory, then cwd.
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        start = (home != nullptr) ? fs::path(home) : fs::current_path(ec);
    }

    m_dir_picker_path = start.string();
    refresh_dir_picker_entries();

    // Deferred to draw_dir_picker_popup() -- see m_dir_picker_request_open.
    m_dir_picker_request_open = true;
}

void Gui::refresh_dir_picker_entries()
{
    namespace fs = std::filesystem;

    m_dir_picker_subdirs.clear();
    m_dir_picker_unreadable = false;

    try {
        std::error_code ec;
        fs::directory_iterator it(m_dir_picker_path, ec);
        if (ec) {
            m_dir_picker_unreadable = true;
            return;
        }
        for (const auto& entry : it) {
            std::error_code is_dir_ec;
            if (entry.is_directory(is_dir_ec) && !is_dir_ec) {
                m_dir_picker_subdirs.push_back(entry.path().filename().string());
            }
        }
    } catch (const fs::filesystem_error&) {
        m_dir_picker_unreadable = true;
        m_dir_picker_subdirs.clear();
    }

    std::sort(m_dir_picker_subdirs.begin(), m_dir_picker_subdirs.end(),
              [](const std::string& a, const std::string& b) {
                  return std::lexicographical_compare(
                      a.begin(), a.end(), b.begin(), b.end(),
                      [](unsigned char ca, unsigned char cb) {
                          return std::tolower(ca) < std::tolower(cb);
                      });
              });
}

void Gui::draw_dir_picker_popup(Config& config)
{
    namespace fs = std::filesystem;

    if (m_dir_picker_request_open) {
        m_dir_picker_request_open = false;
        ImGui::OpenPopup(tr_id("Choose Directory"));
    }

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(tr_id("Choose Directory"), nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", m_dir_picker_path.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##dirpath", buf, sizeof buf,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_dir_picker_path = buf;
        refresh_dir_picker_entries();
    }

    const fs::path current(m_dir_picker_path);
    const fs::path parent = current.parent_path();
    const bool     has_parent = !parent.empty() && parent != current;
    if (!has_parent) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr_id("Up"))) {
        m_dir_picker_path = parent.string();
        refresh_dir_picker_entries();
    }
    if (!has_parent) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr_id("Refresh"))) {
        refresh_dir_picker_entries();
    }

    ImGui::Separator();

    if (m_dir_picker_unreadable) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           tr("Cannot list this directory (permission denied, or it "
                              "doesn't exist)."));
    }

    if (ImGui::BeginChild("##dirlist",
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true)) {
        for (const auto& name : m_dir_picker_subdirs) {
            if (ImGui::Selectable(name.c_str())) {
                m_dir_picker_path = (current / name).string();
                refresh_dir_picker_entries();
            }
        }
    }
    ImGui::EndChild();

    std::error_code ec;
    const bool valid = fs::is_directory(m_dir_picker_path, ec) && !ec;
    if (!valid) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr_id("Select This Folder"))) {
        switch (m_dir_picker_target) {
            case DirPickerTarget::RomDir:        config.rom_dir        = m_dir_picker_path; break;
            case DirPickerTarget::NvramDir:      config.nvram_dir      = m_dir_picker_path; break;
            case DirPickerTarget::ScreenshotDir: config.screenshot_dir = m_dir_picker_path; break;
            case DirPickerTarget::None: break;
        }
        m_dir_picker_target = DirPickerTarget::None;
        ImGui::CloseCurrentPopup();
    }
    if (!valid) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr_id("Cancel"))) {
        m_dir_picker_target = DirPickerTarget::None;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
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
        // Right-align by measured width so the hint never clips off the edge.
        const char*  hint      = tr("F10: toggle overlay");
        const float  hint_w    = ImGui::CalcTextSize(hint).x;
        const float  right_pad = ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(ImGui::GetWindowWidth() - hint_w - right_pad);
        ImGui::TextUnformatted(hint);
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
    // Free the old art or every rescan leaks a full set of textures.
    release_picker_textures();

    m_picker_entries  = std::move(entries);
    m_picker_backend  = backend;
    m_picker_scraper  = scraper;
    m_picker_enabled  = true;
    m_picker_selected = 0;
    m_picker_scroll   = 0.0f;

    // Return to the last-played game rather than the top of the list.
    if (!m_picker_last_launched.empty()) {
        for (usize i = 0; i < m_picker_entries.size(); ++i) {
            if (m_picker_entries[i].name == m_picker_last_launched) {
                m_picker_selected      = static_cast<int>(i);
                m_picker_scroll_to_sel = true;
                break;
            }
        }
    }

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
    if (out.has_value()) {
        m_picker_last_launched = *out;
    }
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

    ImGui::TextUnformatted(tr("Select a game"));
    // Right-align by measured width so the hint never clips off the edge
    const char* hint      = tr("Enter: launch   F10: settings   F9: quit");
    const float hint_w    = ImGui::CalcTextSize(hint).x;
    const float right_pad = ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(ImGui::GetWindowWidth() - hint_w - right_pad);
    ImGui::TextDisabled("%s", hint);
    ImGui::Separator();

    if (m_picker_entries.empty()) {
        ImGui::Spacing();
        text_wrapped("No Model 2 ROM archives were found in the ROM directory. Set it in "
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
        ImGui::TextDisabled("%s", still_fetching ? tr("Fetching description...")
                                                 : tr("No description available."));
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
