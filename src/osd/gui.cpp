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
#include <imgui_internal.h>

#include <stb_image.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
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

/// The key bound to a shortcut, for display, or nullptr for none.
const char* hotkey_name(const Config& config, Config::Hotkey hotkey)
{
    const std::string& name = config.hotkey(hotkey);
    return name.empty() ? nullptr : name.c_str();
}

/// As hotkey_name(), with "none" for an unbound shortcut.
const char* hotkey_label(const Config& config, Config::Hotkey hotkey)
{
    const char* name = hotkey_name(config, hotkey);
    return name != nullptr ? name : tr("none");
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

/// A centring axis as a bar: the deadzone shaded about the middle and the raw
/// reading as a marker, so the zone can be set to just cover a resting stick.
void centre_bar(const char* id, s16 raw, u32 deadzone_percent)
{
    const ImGuiStyle& style  = ImGui::GetStyle();
    const float       width  = ImGui::CalcItemWidth();
    const float       height = ImGui::GetFrameHeight();
    const ImVec2      pos    = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float mid  = pos.x + width * 0.5f;
    const float dead = static_cast<float>(deadzone_percent) / 100.0f;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                        ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
    if (dead > 0.0f) {
        const float half = width * 0.5f * dead;
        draw->AddRectFilled(ImVec2(mid - half, pos.y), ImVec2(mid + half, pos.y + height),
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    }
    draw->AddLine(ImVec2(mid, pos.y), ImVec2(mid, pos.y + height),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled));

    // Whole pixels, so stick noise does not flicker the marker's edges.
    const float value  = std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
    const float x      = std::round(mid + width * 0.5f * value);
    const bool  inside = std::abs(value) <= dead;
    draw->AddRectFilled(ImVec2(x - 2.0f, pos.y), ImVec2(x + 2.0f, pos.y + height),
                        ImGui::GetColorU32(inside ? ImGuiCol_TextDisabled
                                                  : ImGuiCol_SliderGrabActive));
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

// What a Gamepad tab slot does in a game, named after the cabinet control:
// the slot's own name when it keeps its generic one, nullptr when the game
// does not read it.
[[nodiscard]] const char* typed_slot_name(const Gui::GameFamily& family, const char* slot)
{
    const auto is = [&](rom::InputFlags flag) {
        return rom::has_input(static_cast<rom::InputFlags>(family.inputs), flag);
    };
    const auto at = [&](const char* name) { return std::strcmp(slot, name) == 0; };
    using F = rom::InputFlags;
    const bool model3 = is(F::Fighting) || is(F::SpikeOut) || is(F::Soccer) || is(F::Vr4)
                     || is(F::ViewChange) || is(F::Handbrake) || is(F::Harley) || is(F::AnalogJoystick)
                     || is(F::Fishing) || is(F::TwinJoysticks) || is(F::MagTruck) || is(F::Ski)
                     || is(F::Shift4) || is(F::ShiftUpDown) || is(F::AnalogGun1);

    // Virtual On's twin sticks, both on one pad.
    if (is(F::TwinJoysticks) || family.key == "von") {
        struct Named { const char* slot; const char* name; };
        static const Named kVon[] = {
            {"Move Up", N_("Left Lever Up")},       {"Move Down", N_("Left Lever Down")},
            {"Move Left", N_("Left Lever Left")},   {"Move Right", N_("Left Lever Right")},
            {"Button 4", N_("Right Lever Up")},     {"Button 1", N_("Right Lever Down")},
            {"Button 3", N_("Right Lever Left")},   {"Button 2", N_("Right Lever Right")},
            {"Brake", N_("Left Shot")},             {"Accelerator", N_("Right Shot")},
            {"Gear Down", N_("Left Dash")},         {"Gear Up", N_("Right Dash")},
            {"Steer / Aim", N_("Left Lever")},      {"Right Lever", N_("Right Lever")},
        };
        for (const Named& named : kVon) {
            if (at(named.slot)) return named.name;
        }
        return slot;
    }
    if (is(F::Ski)) {
        if (at("Gear Down")) return N_("Left Pole");
        if (at("Gear Up")) return N_("Right Pole");
    }

    if (at("Brake")) {
        if (family.brake && is(F::Fishing)) return N_("Line Tension");
        return family.gun || family.brake ? slot : nullptr;
    }
    if (at("Accelerator")) return family.gun || family.accel ? slot : nullptr;
    if (at("Gear Up") || at("Gear Down")) return family.gears ? slot : nullptr;
    if (at("Right Lever")) return family.lever ? (is(F::Fishing) ? N_("Joystick") : slot) : nullptr;
    if (at("Steer / Aim")) {
        if (family.gun) return N_("Aim");
        if (!family.aim) return nullptr;
        return is(F::Vehicle) ? N_("Steer") : is(F::Fishing) ? N_("Rod") : slot;
    }
    if (at("Start") || at("Coin")) return slot;

    const int button = at("Button 1") ? 1 : at("Button 2") ? 2 : at("Button 3") ? 3 : at("Button 4") ? 4 : 0;
    if (button != 0) {
        if (family.gun) {
            if (button == 1) return N_("Trigger");
            if (button == 2) return family.missile ? N_("Missile") : N_("Gun Reload");
        }
        if (!model3) return slot;
        static const char* const kFighting[] = {N_("Punch"), N_("Kick"), N_("Guard"), N_("Escape")};
        static const char* const kSpikeOut[] = {N_("Beat"), N_("Charge"), N_("Shift"), N_("Jump")};
        static const char* const kSoccer[]   = {N_("Long Pass"), N_("Shoot"), N_("Short Pass"), nullptr};
        static const char* const kVr[]       = {N_("VR1 (red)"), N_("VR2 (blue)"), N_("VR3 (yellow)"), N_("VR4 (green)")};
        static const char* const kSkyStick[] = {N_("Event 1"), N_("Event 2"), nullptr, nullptr};
        static const char* const kFishing[]  = {N_("Cast"), N_("Select"), nullptr, nullptr};
        const usize i = static_cast<usize>(button - 1);
        if (is(F::Fighting)) return kFighting[i];
        if (is(F::SpikeOut)) return kSpikeOut[i];
        if (is(F::Soccer)) return kSoccer[i];
        if (is(F::Vr4)) return kVr[i];
        if (is(F::AnalogJoystick)) return kSkyStick[i];
        if (is(F::Fishing)) return kFishing[i];
        if (is(F::Harley)) return button == 1 ? N_("Music Select") : button == 2 ? N_("View") : nullptr;
        if (button == 1 && is(F::ViewChange)) return N_("View");
        if (button == 2 && is(F::Handbrake)) return N_("Handbrake");
        if (button == 1 && is(F::MagTruck)) return N_("Pedal");
        if (is(F::Ski)) return button == 3 ? N_("Select 1") : button == 4 ? N_("Select 2")
                             : button == 2 ? N_("Select 3") : nullptr;
        return nullptr;
    }

    // The directions.
    if (!model3 || is(F::Fighting) || is(F::SpikeOut) || is(F::Soccer)) {
        return slot;
    }
    if (is(F::AnalogJoystick)) {
        return at("Move Up") ? N_("Trigger 1") : at("Move Down") ? N_("Trigger 2") : nullptr;
    }
    return nullptr;
}

#ifdef SM2_HAVE_LIGHTGUNS
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
#endif

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
    io.IniFilename = nullptr;  // No imgui.ini — settings live in keisho.ini.

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 6.0f;
    style.PopupRounding    = 6.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.FramePadding     = ImVec2(8.0f, 4.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    m_base_style = style;

    // The platform backend only; each render backend initialises its own ImGui
    // renderer.
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
    release_bezel();
    release_crosshair_images();
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
    // DisplaySize stays the SDL logical size, which mouse input uses; correct
    // DisplayFramebufferScale to the backend's real pixel extent instead, since
    // SDL can under-report it on Wayland.
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
    // Scale content against a base of 992x768, the smaller axis ratio winning.
    // The floor is the font's native size in output pixels, since the default
    // font is unreadable below 13 px; a short window scrolls instead.
    const ImGuiIO& io = ImGui::GetIO();
    const float    sx = io.DisplaySize.x > 0.0f ? io.DisplaySize.x / 992.0f : 1.0f;
    const float    sy = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 768.0f : 1.0f;
    const float    fb = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    float          scale = std::min(sx, sy);
    scale                = std::clamp(scale, std::clamp(1.0f / fb, 0.5f, 1.0f), 4.0f);

    // Rescale from a base-style snapshot, since ScaleAllSizes is cumulative.
    if (scale != m_ui_scale) {
        m_ui_scale          = scale;
        ImGuiStyle& style   = ImGui::GetStyle();
        style               = m_base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
    }
}

bool Gui::draw(Config& config, const std::vector<std::string>& gpu_names,
               float measured_hz, const char* renderer_label, Input* input)
{
    apply_theme(config);
    apply_scale();
    if (config.language != m_language) {
        m_language         = config.language;
        m_language_pending = true;
    }

    // Cache this frame's present placement so the crosshair and Sinden-border
    // helpers frame the same rectangle the backend presents into.
    m_present_aspect = m_shown_aspect;
    m_present_method = config.scaling_method;

    if (m_palette == nullptr) {
        apply_theme(config);
    }
    // Shown whether or not the settings overlay is open.
    if (config.show_fps) {
        draw_fps_overlay(measured_hz, renderer_label);
    }

    // A notification (state saved/loaded), top-centred, over gameplay.
    if (m_notify_seconds_left > 0.0f) {
        m_notify_seconds_left -= ImGui::GetIO().DeltaTime;
        if (config.show_notifications) {
            draw_notification();
        }
    }

    // A gun game hides the OS cursor so only the crosshair shows, but not while
    // the overlay or picker is open, where the real pointer is needed.
    // NoMouseCursorChange stops ImGui's SDL3 backend re-showing the cursor every
    // frame.
    const GameFamily* playing = nullptr;
    for (const GameFamily& f : m_families) {
        if (f.key == m_current_family && !m_current_game.empty()) {
            playing = &f;
        }
    }
    using Mode          = Config::LightgunMode;
    const bool gun_game = playing != nullptr && playing->gun && config.lightgun != Mode::Off;
    const bool hide_cursor = (gun_game || config.lightgun == Mode::On) && !m_visible && !m_picker_enabled
                          && !m_load_error.has_value();
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
    // Confine the pointer in fullscreen, so a light gun that presents as a mouse
    // cannot drag it onto another monitor.
    const bool grab = hide_cursor && m_window != nullptr
                   && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (grab != m_mouse_grabbed) {
        SDL_SetWindowMouseGrab(m_window, grab);
        m_mouse_grabbed = grab;
    }
    m_present_area = {};
    const bool fullscreen = m_window != nullptr
                         && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (config.bezel && fullscreen && !m_current_game.empty() && !m_picker_enabled
        && m_present_aspect != AspectMode::Widescreen && m_present_aspect != AspectMode::Stretch) {
        draw_bezel(config);
    }

    if ((gun_game || config.lightgun == Mode::On) && !m_picker_enabled && !m_visible) {
        draw_sinden_border(config);
        if (config.lightgun_crosshair) {
            draw_crosshairs(config, input);
        }
        draw_calibration_markers(input);
    }

    // Full-screen picker when active and Settings is closed; F10 opens Settings
    // on top.
    if (m_picker_enabled && !m_visible) {
        draw_picker(config);
    }

    if (m_visible) {
        draw_menu(config, gpu_names, input, measured_hz);
    } else {
        m_popup_open = false;
    }
    autosave(config);

    if (m_load_error.has_value()) {
        draw_load_error();
    }

    return true;
}

void Gui::end_frame()
{
    if (!m_initialised) return;
    // Every frame, even if nothing was drawn: the render backend reads
    // ImGui::GetDrawData() afterwards.
    ImGui::Render();
}

// ---------------------------------------------------------------------------
// The settings menu: a framed page of grouped rows (osd/ui/kit.h), themed
// (osd/ui/theme.h), with tabs across the top and help in its foot.
// ---------------------------------------------------------------------------

namespace {

/// The pages, in tab order.
enum Page : int { kPageDisplay, kPageAudio, kPageInput, kPagePaths, kPageLink, kPageStates,
                  kPageHotkeys, kPageSystem, kPageAbout, kPageCount };

/// The ini's whole path for the foot and the Paths page.
std::string config_path_shown(const std::string& configured)
{
    return configured.empty() ? default_config_path() : configured;
}

/// A path with the home directory shortened to ~, or `none` when empty.
std::string path_label(const std::string& path, const char* none)
{
    if (path.empty()) {
        return none;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0' && path.rfind(home, 0) == 0) {
        return "~" + path.substr(std::strlen(home));
    }
    return path;
}

}  // namespace

void Gui::apply_theme(const Config& config)
{
    const ui::Theme& theme = ui::theme_named(config.theme);
    if (theme.palette == m_palette) {
        return;
    }
    m_palette = theme.palette;
    ui::apply_to_imgui(*m_palette, m_base_style);
    ui::apply_to_imgui(*m_palette, ImGui::GetStyle());
}

void Gui::autosave(Config& config)
{
    // The settings are written a moment after they stop changing, never
    // mid-drag or mid-typing. Values the command line set are held back by
    // the save filter, as before.
    const ImGuiIO& io = ImGui::GetIO();
    if (!m_saved_snapshot.has_value()) {
        m_saved_snapshot = config;
    }
    if (m_saved_flash > 0.0f) {
        m_saved_flash -= io.DeltaTime;
    }
    if (config == *m_saved_snapshot) {
        m_dirty_seconds = 0.0f;
        return;
    }
    m_dirty_seconds += io.DeltaTime;
    const bool busy = io.WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Left) || capturing_key();
    if (m_dirty_seconds < 0.6f || busy) {
        return;
    }
    const std::string path = config_path_shown(m_config_path);
    if (save_config(path, m_save_filter ? m_save_filter(config) : config)) {
        SM2_INFO("gui: settings saved to %s", path.c_str());
        m_saved_flash = 2.5f;
    } else {
        SM2_ERROR("gui: could not save settings to %s", path.c_str());
    }
    m_saved_snapshot = config;
    m_dirty_seconds  = 0.0f;
}

void Gui::escape()
{
    if (capturing_input()) {
        cancel_captures();
    } else if (!m_popup_open) {
        m_visible = false;
    }
}

void Gui::draw_menu(Config& config, const std::vector<std::string>& gpu_names, Input* input,
                    float measured_hz)
{
    const ui::Palette& p = *m_palette;
    ui::Kit            kit(p, m_ui_scale > 0.0f ? m_ui_scale : 1.0f, &m_nav);
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Behind the frame: the game dimmed, or the backdrop alone.
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const ImVec2 a = vp->WorkPos;
        const ImVec2 b(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
        const bool   game_behind = !m_current_game.empty();
        bg->AddRectFilled(a, b, game_behind ? ui::with_alpha(p.backdrop, p.light ? 0.80f : 0.74f) : p.backdrop);
    }

    // The frame: as wide as a comfortable page, as tall as the window allows.
    const float  margin = kit.px(14.0f);
    const ImVec2 size(std::min(vp->WorkSize.x - margin * 2.0f, kit.px(1000.0f)),
                      std::max(kit.px(200.0f), vp->WorkSize.y - margin * 2.0f));
    const ImVec2 a(std::floor(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f),
                   std::floor(vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f));
    const ImVec2 b(a.x + size.x, a.y + size.y);

    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                                      | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##menu", nullptr, kFlags);
    ImGui::PopStyleVar(2);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float round = kit.px(10.0f);
    list->AddRectFilled(a, b, p.backdrop, round);
    list->AddRect(a, b, p.edge, round, 0, kit.stroke());

    // Up and Down walk the rows in the order they are drawn, unless focus is
    // in a free row (the gamepad picture) or a list is open: there ImGui's
    // own movement applies. With nothing of ours focused, Down goes to the
    // first row of the page and Up to the tabs.
    const ImGuiID window_id = ImGui::GetCurrentWindow()->ID;
    {
        const int  row   = m_nav.focused_row;
        const int  rows  = static_cast<int>(m_nav.free_rows.size());
        const bool known = row >= 0 && row < rows;
        const bool free  = known && m_nav.free_rows[static_cast<size_t>(row)];
        m_nav.target_row  = m_nav.target_next;
        m_nav.target_next = -1;
        if (rows > 0 && !free && !m_popup_open) {
            for (const ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_GamepadDpadUp,
                                       ImGuiKey_GamepadDpadDown}) {
                ImGui::SetKeyOwner(key, window_id);
            }
            int target = row;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, window_id)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, ImGuiInputFlags_Repeat, window_id)) {
                target = known ? std::min(row + 1, rows - 1) : std::min(1, rows - 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, window_id)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, ImGuiInputFlags_Repeat, window_id)) {
                target = known ? std::max(row - 1, 0) : 0;
            }
            if (target != row && target >= 0) {
                m_nav.target_row = target;
            }
        }
        m_nav.rows.clear();
        m_nav.focused_now = -1;
    }

    // Pages: LB/RB on a pad, PageUp/PageDown on the keyboard (taken from
    // ImGui's own scrolling while the menu is up). Focus moves to the new
    // page's tab.
    ImGui::SetKeyOwner(ImGuiKey_PageUp, window_id);
    ImGui::SetKeyOwner(ImGuiKey_PageDown, window_id);
    int page_step = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, ImGuiInputFlags_None, ImGuiKeyOwner_Any)
        || ImGui::IsKeyPressed(ImGuiKey_PageUp, ImGuiInputFlags_None, window_id)) {
        page_step = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, ImGuiInputFlags_None, ImGuiKeyOwner_Any)
        || ImGui::IsKeyPressed(ImGuiKey_PageDown, ImGuiInputFlags_None, window_id)) {
        page_step = 1;
    }
    if (page_step != 0 && !m_popup_open) {
        m_page = (m_page + page_step + kPageCount) % kPageCount;
        m_nav.focused_row = -1;
        m_nav.target_row  = 0;
    }

    // -- tabs ----------------------------------------------------------------
    const std::array<ui::Kit::Tab, kPageCount> tabs = {{
        {tr("Display"), ui::Kit::Icon::Display},
        {tr("Audio"), ui::Kit::Icon::Audio},
        {tr("Input"), ui::Kit::Icon::Input},
        {tr("Paths"), ui::Kit::Icon::Folder},
        {tr("Link"), ui::Kit::Icon::Link},
        {tr("States"), ui::Kit::Icon::Disk},
        {tr("Hotkeys"), ui::Kit::Icon::Keyboard},
        {tr("System"), ui::Kit::Icon::Gear},
        {tr("About"), ui::Kit::Icon::Info},
    }};
    const float pad = kit.px(14.0f);
    // The strip's child starts a little above the tabs, so the focus ring,
    // which is drawn just outside a tab, is not clipped at its top.
    const float ring_room = kit.px(4.0f);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + kit.px(10.0f) - ring_room));
    ImGui::PushClipRect(a, b, true);
    {
        // Compact tabs (icons alone, bar the current) when the labels would
        // not all fit.
        float labelled = 0.0f;
        for (const ui::Kit::Tab& tab : tabs) {
            labelled += ImGui::CalcTextSize(tab.label).x + kit.px(14.0f) * 2.0f + kit.px(14.0f) + kit.px(6.0f);
        }
        const bool compact = labelled > size.x - pad * 2.0f;
        ImGui::BeginChild("tabs", ImVec2(size.x - pad * 2.0f, kit.px(34.0f) + ring_room), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ring_room);
        int page = m_page;
        if (kit.tabs(tabs, page, compact)) {
            m_page            = page;
            m_nav.focused_row = -1;
        }
        ImGui::EndChild();
    }
    // A line under the tabs that fades at its ends.
    {
        const float y  = a.y + kit.px(10.0f) + kit.px(34.0f) + kit.px(4.0f);
        const float x0 = a.x + pad, x1 = b.x - pad;
        const float fade = (x1 - x0) * 0.12f;
        list->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + fade, y + kit.stroke()),
                                      ui::with_alpha(p.accent, 0.0f), p.accent, p.accent, ui::with_alpha(p.accent, 0.0f));
        list->AddRectFilled(ImVec2(x0 + fade, y), ImVec2(x1 - fade, y + kit.stroke()), p.accent);
        list->AddRectFilledMultiColor(ImVec2(x1 - fade, y), ImVec2(x1, y + kit.stroke()),
                                      p.accent, ui::with_alpha(p.accent, 0.0f), ui::with_alpha(p.accent, 0.0f), p.accent);
    }

    // -- foot: the help of the focused row, the keys -------------------------
    const float foot_h   = kit.px(36.0f);
    const float foot_top = b.y - foot_h;
    const float page_top = a.y + kit.px(10.0f) + kit.px(34.0f) + kit.px(10.0f);

    // -- the page ------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, page_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit.px(4.0f), 0.0f));
    ImGui::PushID(m_page);
    ImGui::BeginChild("page", ImVec2(size.x - pad * 2.0f, foot_top - kit.px(6.0f) - page_top),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    switch (m_page) {
        case kPageDisplay: page_display(kit, config, gpu_names); break;
        case kPageAudio:   page_audio(kit, config); break;
        case kPageInput:   page_input(kit, config, input); break;
        case kPageHotkeys: page_hotkeys(kit, config); break;
        case kPageLink:    page_link(kit, config); break;
        case kPagePaths:   page_paths(kit, config); break;
        case kPageStates:  page_states(kit, config); break;
        case kPageSystem:  page_system(kit, config); break;
        default:           page_about(kit); break;
    }
    ImGui::Dummy(ImVec2(0.0f, kit.px(12.0f)));
    ImGui::EndChild();
    ImGui::PopID();

    // -- foot ----------------------------------------------------------------
    list->AddLine(ImVec2(a.x, foot_top), ImVec2(b.x, foot_top), p.line, kit.stroke());
    {
        std::string keys = trf("%s: back", tr("Esc"));
        if (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) {
            keys += "   ";
            keys += tr("LB/RB: pages");
        }
        if (m_saved_flash > 0.0f) {
            keys = std::string(tr("Saved")) + "   " + keys;
        }
        const float keys_w = ImGui::CalcTextSize(keys.c_str()).x;
        const float ty     = foot_top + (foot_h - ImGui::GetFontSize()) * 0.5f;
        if (m_saved_flash > 0.0f) {
            const float alpha = std::min(1.0f, m_saved_flash / 0.5f);
            list->AddText(ImVec2(b.x - pad - keys_w, ty), ui::with_alpha(p.good, alpha), tr("Saved"));
            const float saved_w = ImGui::CalcTextSize(tr("Saved")).x;
            list->AddText(ImVec2(b.x - pad - keys_w + saved_w, ty), p.faint, keys.c_str() + std::strlen(tr("Saved")));
        } else {
            list->AddText(ImVec2(b.x - pad - keys_w, ty), p.faint, keys.c_str());
        }
        // The help at the left: the row's tip, else the game and its speed.
        std::string help = kit.focused_tip();
        if (help.empty()) {
            if (!m_current_game.empty()) {
                help = trf("%s   %.1f Hz", m_current_game.c_str(), static_cast<double>(measured_hz));
            } else {
                help = tr("Changes are saved as you make them.");
            }
        }
        const float help_w = std::max(0.0f, size.x - pad * 3.0f - keys_w);
        ImFont*     font   = ImGui::GetFont();
        const float help_h = font->CalcTextSizeA(kit.small(), FLT_MAX, help_w, help.c_str()).y;
        list->AddText(font, kit.small(), ImVec2(a.x + pad, foot_top + (foot_h - help_h) * 0.5f), p.muted,
                      help.c_str(), nullptr, help_w);
    }
    ImGui::PopClipRect();

    // Popups of the page (the folder picker) open from the frame's level, so
    // they survive a page change.
    draw_dir_picker_popup(config);

    m_popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::End();
    m_nav.free_rows   = m_nav.rows;
    m_nav.focused_row = m_nav.focused_now;

    // B on a pad goes back to the game, as Escape does; not while a button is
    // being bound, where B is a button like any other.
    if (!m_popup_open && !capturing_input()
        && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, ImGuiInputFlags_None, ImGuiKeyOwner_Any)) {
        m_visible = false;
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Gui::page_display(ui::Kit& kit, Config& config, const std::vector<std::string>& gpu_names)
{
    const char* next_launch = tr("next launch");
    const char* model12     = tr("Model 1 & 2 only");

    kit.begin_group(tr("Renderer"));
    if (!m_available_renderers.empty()) {
        std::vector<const char*> labels;
        int                      current = 0;
        const std::string        chosen  = config.graphics_backend.empty() ? m_available_renderers.front()
                                                                           : config.graphics_backend;
        for (size_t i = 0; i < m_available_renderers.size(); ++i) {
            const std::string& name = m_available_renderers[i];
            labels.push_back(name == "software" ? tr("Software") : name == "vulkan" ? "Vulkan"
                                                 : name == "opengl" ? "OpenGL" : name.c_str());
            if (name == chosen) current = static_cast<int>(i);
        }
        kit.tip(tr("Which renderer draws the game. The software renderer needs no GPU and suits "
                   "small boards; Vulkan and OpenGL pull ahead on desktop graphics."));
        if (kit.choice(tr("Renderer"), tr("What draws the game."), labels, current, {.badge = next_launch})) {
            config.graphics_backend = m_available_renderers[static_cast<size_t>(current)];
        }
    }
    if (!gpu_names.empty()) {
        std::vector<std::string> labels = {tr("Automatic (best)")};
        int                      current = 0;
        for (size_t i = 0; i < gpu_names.size(); ++i) {
            labels.push_back(gpu_names[i]);
            if (gpu_names[i] == config.gpu) current = static_cast<int>(i) + 1;
        }
        if (kit.dropdown(tr("Graphics card"), tr("The device the GPU renderers draw on."), labels, current,
                         {.badge = next_launch})) {
            config.gpu = current == 0 ? std::string() : gpu_names[static_cast<size_t>(current) - 1];
        }
    }
    kit.tip(tr("Waits for the display before showing a frame. Stops tearing; adds up to a frame of delay."));
    kit.toggle(tr("Vsync"), tr("Wait for vertical blank before presenting."), config.vsync);
    kit.toggle(tr("Fullscreen"), nullptr, config.fullscreen);
    {
        int w = static_cast<int>(config.window_width);
        int h = static_cast<int>(config.window_height);
        if (kit.number(tr("Window width"), tr("In windowed mode."), w, 496, 7680, 16, {.disabled = config.fullscreen})) {
            config.window_width = static_cast<u32>(w);
        }
        if (kit.number(tr("Window height"), nullptr, h, 384, 4320, 16, {.disabled = config.fullscreen})) {
            config.window_height = static_cast<u32>(h);
        }
    }
    kit.end_group();

    kit.begin_group(tr("Picture"));
    {
        static constexpr std::array<const char*, 8> kScales = {"1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
        int scale = std::clamp(static_cast<int>(config.render_scale), 1, 8) - 1;
        kit.tip(tr("Internal 3D rendering resolution, times the native 496x384. Higher is crisper 3D; "
                   "the 2D stays sharp either way. GPU renderers only."));
        if (kit.choice(tr("3D render scale"), tr("1x is the arcade's own. 8x suits a 4K display."), kScales, scale,
                       {.badge = next_launch})) {
            config.render_scale = static_cast<u32>(scale + 1);
        }
    }
    {
        static constexpr std::array<const char*, 4> kMethods = {N_("Nearest"), N_("Bilinear"), N_("Sharp"), N_("Integer")};
        int method = std::clamp(static_cast<int>(config.scaling_method), 0, 3);
        kit.tip(tr("How the finished frame is magnified to the window. Sharp keeps 2D text crisp without the "
                   "shimmer Nearest shows at odd sizes. Integer snaps to a whole multiple, with black bars."));
        if (kit.choice(tr("Scaling"), tr("How the frame is magnified to the window."), tr_all(kMethods), method)) {
            config.scaling_method = static_cast<ScalingMethod>(method);
        }
    }
    {
        static constexpr std::array<const char*, 4> kAspects = {N_("4:3"), N_("Square pixels"), N_("16:9"), N_("Stretch")};
        int aspect = std::clamp(static_cast<int>(config.aspect_mode), 0, 3);
        kit.tip(tr("4:3 is the arcade monitor's shape. Square pixels shows the raw 496x384, slightly narrow. "
                   "16:9 draws the 3D wider with the 2D in the middle (GPU renderers). Stretch fills the window."));
        if (kit.choice(tr("Aspect"), tr("The shape the picture is shown at."), tr_all(kAspects), aspect)) {
            config.aspect_mode = static_cast<AspectMode>(aspect);
        }
    }
    {
        int  brightness = static_cast<int>(config.brightness);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", brightness);
        kit.tip(tr("Where the picture's black sits. 50% matches an arcade capture; lower is darker."));
        if (kit.slider(tr("Brightness"), nullptr, brightness, 0, 100, 5, shown)) {
            config.brightness = static_cast<u32>(brightness);
        }
    }
    kit.tip(tr("In fullscreen, frame the 4:3 picture with an image from the bezels folder: <game>.png, "
               "the parent set's, or default.png. The picture fills the image's transparent window."));
    kit.toggle(tr("Bezel"), tr("Cabinet artwork around the picture in fullscreen."), config.bezel);
    kit.end_group();

    kit.begin_group(tr("CRT filter"));
    kit.tip(tr("A cosmetic arcade-monitor look over the finished frame: scanlines, a shadow mask, glow "
               "and optional curvature. Purely visual."));
    kit.toggle(tr("CRT filter"), tr("Scanlines, mask, glow and curvature."), config.crt_enabled);
    if (config.crt_enabled) {
        const auto pct = [&](const char* name, u32& value) {
            int  v = static_cast<int>(value);
            char shown[16];
            std::snprintf(shown, sizeof shown, "%d%%", v);
            if (kit.slider(name, nullptr, v, 0, 100, 5, shown)) {
                value = static_cast<u32>(v);
            }
        };
        pct(tr("Scanlines"), config.crt_scanline_strength);
        pct(tr("Mask"), config.crt_mask_strength);
        pct(tr("Glow"), config.crt_glow_strength);
        pct(tr("Curvature"), config.crt_curvature);
    }
    kit.end_group();

    kit.begin_group(tr("Enhancements beyond the original hardware"));
    {
        static constexpr std::array<const char*, 2> kTex = {N_("Faithful"), N_("Anisotropic")};
        int tex = std::clamp(static_cast<int>(config.texture_filter), 0, 1);
        if (!m_caps_anisotropy) {
            config.texture_filter = TextureFilter::Faithful;
            tex                   = 0;
        }
        kit.tip(m_caps_anisotropy ? tr("Anisotropic filtering sharpens textures seen at a slant.")
                                  : tr("This GPU does not report anisotropic filtering."));
        if (kit.choice(tr("3D texture filter"), m_caps_anisotropy ? nullptr : tr("Not available on this GPU."),
                       tr_all(kTex), tex, {.disabled = !m_caps_anisotropy})) {
            config.texture_filter = static_cast<TextureFilter>(tex);
        }
        if (m_caps_anisotropy && config.texture_filter == TextureFilter::Anisotropic) {
            static constexpr std::array<int, 4>         kLevels = {2, 4, 8, 16};
            static constexpr std::array<const char*, 4> kLabels = {"2x", "4x", "8x", "16x"};
            const int cap = static_cast<int>(m_caps_max_anisotropy);
            int count = 1;
            for (int i = 1; i < 4; ++i) {
                if (kLevels[static_cast<usize>(i)] <= cap) count = i + 1;
            }
            int sel = 0;
            for (int i = 0; i < count; ++i) {
                if (kLevels[static_cast<usize>(i)] <= static_cast<int>(config.anisotropy)) sel = i;
            }
            if (kit.choice(tr("Anisotropy"), nullptr, std::span<const char* const>(kLabels.data(), static_cast<size_t>(count)), sel)) {
                config.anisotropy = static_cast<u32>(kLevels[static_cast<usize>(sel)]);
            }
        }
    }
    {
        static constexpr std::array<const char*, 3> kUp = {N_("Faithful"), "xBR", "ScaleFX"};
        int up = std::clamp(static_cast<int>(config.upscale_2d), 0, 2);
        kit.tip(tr("Edge-smooth the 2D layers (HUD, text, menus). xBR and ScaleFX round diagonals; "
                   "Faithful keeps crisp pixels."));
        if (kit.choice(tr("2D upscale"), nullptr, tr_all(kUp), up)) {
            config.upscale_2d = static_cast<Upscale2D>(up);
        }
    }
    {
        static constexpr std::array<const char*, 2> kTrans = {N_("Stipple"), N_("Blended")};
        int trans = std::clamp(static_cast<int>(config.translucency), 0, 1);
        if (!m_caps_blended) {
            config.translucency = Translucency::Stipple;
            trans               = 0;
        }
        kit.tip(tr("The hardware draws see-through surfaces (lights, glass, shadows) as a checkerboard. "
                   "Blended draws the 50% see-through surface it is trying to achieve."));
        if (kit.choice(tr("Translucency"), m_model3_running ? model12 : nullptr, tr_all(kTrans), trans,
                       {.disabled = !m_caps_blended || m_model3_running})) {
            config.translucency = static_cast<Translucency>(trans);
        }
    }
    kit.tip(tr("Draw the scenery to the edge of the course instead of the arcade's short range, so it no "
               "longer pops in. Daytona USA only."));
    kit.toggle(tr("Extended draw distance"), m_model3_running ? model12 : tr("Daytona USA."),
               config.extended_draw_distance, {.disabled = m_model3_running});
    kit.tip(tr("Replace the game's 3D textures with images saved in saves/textures/<game>/load, named as "
               "dumped. GPU renderers only."));
    kit.toggle(tr("Custom textures"), tr("Replacement textures from the saves folder."), config.custom_textures);
    if (config.custom_textures) {
        static constexpr std::array<const char*, 1> kReload = {N_("Reload")};
        const std::string loaded = i18n::trnf("%zu texture loaded", "%zu textures loaded",
                                              static_cast<unsigned long>(m_custom_texture_count),
                                              m_custom_texture_count);
        if (kit.actions(tr("Loaded textures"), loaded.c_str(), tr_all(kReload)) == 0) {
            m_texture_reload_requested = true;
        }
    }
    kit.tip(tr("Collect every 3D texture the game draws and write them, with an index.html to browse, to "
               "saves/textures/<game>/dump when the game exits or this is turned off."));
    kit.toggle(tr("Dump textures"), tr("Write every texture the game draws."), config.dump_textures);
    kit.end_group();

    kit.begin_group(tr("Performance"));
    kit.tip(tr("Hold the machine to its own 57.5 Hz. Off runs as fast as the host can, without sound sync."));
    kit.toggle(tr("Limit speed"), tr("Run at the arcade's 57.5 Hz."), config.throttle);
    kit.tip(tr("The software renderer draws each frame on a second thread, one frame behind the emulation."));
    kit.toggle(tr("Software renderer on a second thread"), tr("Faster; a frame of latency."), config.software_async);
    kit.tip(tr("On a big.LITTLE chip, the drawing runs on the little cores and leaves the big ones to the emulation."));
    kit.toggle(tr("Draw on the slow cores"), tr("big.LITTLE hosts, with the second thread."),
               config.software_slow_cores, {.disabled = !config.software_async});
    kit.tip(tr("Model 3 builds each frame on a second thread while the next one is emulated; shown a frame later."));
    kit.toggle(tr("Model 3 frame build on a second thread"), tr("Faster; a frame of latency."), config.model3_async);
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void Gui::page_audio(ui::Kit& kit, Config& config)
{
    kit.begin_group(tr("Volume"));
    {
        int  music = static_cast<int>(config.music_volume);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", music);
        kit.tip(tr("Only for games with a separate music board: Star Wars Arcade, Sega Touring Car "
                   "Championship, Top Skater and the Model 3 games with a DSB. Sound effects stay as they are."));
        if (kit.slider(tr("Music volume"), tr("The music board's level, where a game has one."), music, 0, 100, 5, shown)) {
            config.music_volume = static_cast<u32>(music);
        }
    }
    kit.end_group();

    kit.begin_group(tr("Per-game volume"));
    kit.tip(tr("Raise or lower a game's volume from its default level. One slider covers every revision "
               "of a game. Off plays every game at its default, whatever the sliders say. Above 100% loud "
               "effects can clip."));
    kit.toggle(tr("Per-game volume"), tr("Each game at its own level."), config.game_volume);
    {
        std::string filter = m_volume_filter;
        bool        reset  = false;
        const std::string changed = i18n::trnf("%zu game changed", "%zu games changed",
                                               static_cast<unsigned long>(config.game_volumes.size()),
                                               config.game_volumes.size());
        if (kit.field(tr("Find a game"), config.game_volumes.empty() ? nullptr : changed.c_str(), filter,
                      tr("Filter games"), tr("Reset all"), &reset, {.disabled = false})) {
            std::snprintf(m_volume_filter, sizeof m_volume_filter, "%s", filter.c_str());
        }
        if (reset) {
            config.game_volumes.clear();
        }
    }
    kit.end_group();

    // One slider per family, the running game's first.
    std::string filter = m_volume_filter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto volume_row = [&](const GameFamily& family, const char* badge) {
        const auto found = config.game_volumes.find(family.key);
        int        value = static_cast<int>(found != config.game_volumes.end() ? found->second
                                                                              : Config::kDefaultGameVolume);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", value);
        ui::Kit::Opts opts;
        opts.disabled = !config.game_volume;
        opts.id       = family.key.c_str();
        opts.badge    = badge;
        if (kit.slider(family.title.c_str(), nullptr, value, 0, static_cast<int>(Config::kMaxGameVolume), 5, shown, opts)) {
            if (value == static_cast<int>(Config::kDefaultGameVolume)) {
                config.game_volumes.erase(family.key);
            } else {
                config.game_volumes[family.key] = static_cast<u32>(value);
            }
        }
    };
    const GameFamily* playing = nullptr;
    for (const GameFamily& family : m_families) {
        if (family.key == m_current_family && !m_current_game.empty()) playing = &family;
    }
    kit.begin_group(tr("Games"));
    if (playing != nullptr) {
        volume_row(*playing, tr("playing"));
    }
    for (const GameFamily& family : m_families) {
        if (&family == playing) continue;
        if (!filter.empty()) {
            std::string haystack = family.title + " " + family.key;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(filter) == std::string::npos) continue;
        }
        volume_row(family, nullptr);
    }
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Input: the gamepad, wheel and light-gun pages, as they were, in a frame
// ---------------------------------------------------------------------------

void Gui::page_input(ui::Kit& kit, Config& config, Input* input)
{
    static constexpr std::array<const char*, 3> kDevices = {N_("Gamepad"), N_("Wheel"), N_("Light gun")};
    const std::array<const char*, 3> labels = tr_all(kDevices);
    ImGui::Dummy(ImVec2(0.0f, kit.px(14.0f)));
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 sz = kit.segments_size(labels);
        if (kit.segments_at("device", at, labels, m_input_page)) {
            cancel_captures();
        }
        ImGui::SetCursorScreenPos(at);
        ImGui::Dummy(sz);
    }
    switch (m_input_page) {
        case 1:  page_wheel(kit, config, input); break;
        case 2:  page_lightgun(kit, config, input); break;
        default: page_gamepad(kit, config, input); break;
    }
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------

void Gui::page_hotkeys(ui::Kit& kit, Config& config)
{
    struct HotkeyRow {
        const char*    name;
        const char*    about;
        Config::Hotkey hotkey;
    };
    struct HotkeyGroup {
        const char*              title;
        std::span<const HotkeyRow> rows;
    };
    // In the order they are reached for: what is done while playing, then
    // the window, then leaving.
    static constexpr HotkeyRow kPlay[] = {
        {N_("Pause"),         nullptr,                                            Config::Hotkey::Pause},
        {N_("Fast-forward"),  N_("While held."),                                  Config::Hotkey::FastForward},
        {N_("Rewind"),        N_("Held: runs the game backwards. Needs Rewind on, on the System page."), Config::Hotkey::Rewind},
        {N_("Quick-save"),    N_("Saves the whole machine to the quick slot."),   Config::Hotkey::QuickSave},
        {N_("Quick-load"),    N_("Restores the quick slot."),                     Config::Hotkey::QuickLoad},
    };
    static constexpr HotkeyRow kWindow[] = {
        {N_("FPS counter"),   nullptr,                                            Config::Hotkey::Fps},
        {N_("Settings"),      N_("This menu."),                                   Config::Hotkey::Overlay},
        {N_("Fullscreen"),    nullptr,                                            Config::Hotkey::Fullscreen},
        {N_("Screenshot"),    nullptr,                                            Config::Hotkey::Screenshot},
    };
    static constexpr HotkeyRow kLeave[] = {
        {N_("Back to games"), N_("Leaves the game for the game list, or quits."), Config::Hotkey::Exit},
        {N_("Quit"),          nullptr,                                            Config::Hotkey::Quit},
    };
    static const HotkeyGroup kGroups[] = {
        {N_("Playing"), kPlay},
        {N_("Window"),  kWindow},
        {N_("Leaving"), kLeave},
    };
    for (const HotkeyGroup& group : kGroups) {
        kit.begin_group(tr(group.title));
        for (const HotkeyRow& row : group.rows) {
            const int  index     = static_cast<int>(row.hotkey);
            const bool capturing = m_capture_hotkey == index;
            const std::string key = capturing ? std::string(tr("press a key...")) : hotkey_label(config, row.hotkey);
            ui::Kit::Opts opts;
            opts.id = row.name;
            static constexpr std::array<const char*, 2> kButtons = {N_("Default"), N_("Clear")};
            switch (kit.binding(tr(row.name), row.about != nullptr ? tr(row.about) : nullptr, key.c_str(), capturing,
                                tr_all(kButtons), opts)) {
                case 0: m_capture_hotkey = capturing ? -1 : index; break;
                case 1:
                    config.hotkeys[static_cast<usize>(index)] = Config::kDefaultHotkeys[static_cast<usize>(index)];
                    m_capture_hotkey = -1;
                    break;
                case 2:
                    config.hotkeys[static_cast<usize>(index)].clear();
                    m_capture_hotkey = -1;
                    break;
                default: break;
            }
        }
        if (&group == &kGroups[0]) {
            kit.paragraph(nullptr, tr("Click a key, then press the new one. Escape cancels. A key a game also uses does both."));
        }
        kit.end_group();
    }

    kit.begin_group(tr("Gamepad"));
    kit.tip(tr("Hold Guide and press a button for the menu, save, load, rewind and the rest; the Input page "
               "lists them. Off for a host that maps the hotkeys itself, such as Batocera."));
    kit.toggle(tr("Guide button shortcuts"), tr("Guide held with a button: menu, save, load, rewind..."),
               config.pad_shortcuts);
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Link: cabinets over the network, and the cabinet outputs
// ---------------------------------------------------------------------------

void Gui::page_link(ui::Kit& kit, Config& config)
{
    kit.begin_group(tr("Cabinet link"));
    kit.tip(tr("Race linked with other machines on the same LAN running the same title (Sega Rally, "
               "Daytona, Super GT 24h, Indy 500, ...). Off keeps this cabinet standalone."));
    kit.toggle(tr("Link cabinets over the network"), tr("Takes effect at the next game launch."), config.link_enabled);
    kit.paragraph(nullptr, tr("The comms board is a ring: each cabinet listens on its own address and sends to "
                              "the next. For two machines, point each at the other. The master/slave role is "
                              "chosen in the game's own test menu, not here."));
    kit.end_group();

    const ui::Kit::Opts off{.disabled = !config.link_enabled};
    kit.begin_group(tr("This cabinet"));
    {
        bool detect = false;
        kit.tip(tr("This machine's address on the cabinet LAN. Blank listens on every interface. Detect "
                   "fills it from the primary network adapter."));
        kit.field(tr("Local IP"), nullptr, config.link_local_ip, tr("every interface"), tr("Detect"), &detect, off);
        if (detect) {
            if (auto iface = net::primary_interface()) {
                config.link_local_ip    = iface->ipv4;
                config.link_subnet_mask = iface->subnet_mask;
            }
        }
        kit.field(tr("Subnet mask"), nullptr, config.link_subnet_mask, nullptr, nullptr, nullptr, off);
        int port = static_cast<int>(config.link_port);
        if (kit.number(tr("Listen port"), nullptr, port, 1, 65535, 1, off)) config.link_port = static_cast<u32>(port);
        int cabinet = static_cast<int>(config.link_cabinet_index);
        kit.tip(tr("Where this cabinet sits in the ring, from 0. For your own bookkeeping; the game "
                   "negotiates the real link id from the ring."));
        if (kit.number(tr("Cabinet number"), nullptr, cabinet, 0, 7, 1, off)) {
            config.link_cabinet_index = static_cast<u32>(cabinet);
        }
        int delay = static_cast<int>(config.link_delay);
        kit.tip(tr("Model 3 only. How many frames late the other cabinets' data is read. Raise it if a race "
                   "over Wi-Fi stutters; lower it on a wired LAN. Each frame shows the other cars 1/60 s later."));
        if (kit.number(tr("Link delay"), tr("In frames; Model 3."), delay, 1, 30, 1, off)) {
            config.link_delay = static_cast<u32>(delay);
        }
    }
    kit.end_group();

    kit.begin_group(tr("Next cabinet in the ring"));
    kit.tip(tr("The cabinet this one sends to. For two machines, the other machine's IP. Blank means this "
               "cabinet does not forward (the tail of a chain)."));
    kit.field(tr("Next IP"), nullptr, config.link_next_ip, tr("none"), nullptr, nullptr, off);
    {
        int port = static_cast<int>(config.link_next_port);
        if (kit.number(tr("Next port"), nullptr, port, 1, 65535, 1, off)) config.link_next_port = static_cast<u32>(port);
    }
    kit.end_group();

    kit.begin_group(tr("Status"));
    if (!m_link_status.active) {
        kit.status(config.link_enabled ? tr("Link configured; starts at the next game launch.")
                                       : tr("Standalone (no cabinet link)."),
                   kit.palette().faint);
    } else if (!m_link_status.enabled) {
        kit.status(tr("Waiting for the game to switch the link board on..."), kit.palette().warn);
    } else if (m_link_status.alive) {
        kit.status(trf("Linked: cabinet %u of %u.", m_link_status.node, m_link_status.count).c_str(),
                   kit.palette().good);
    } else {
        kit.status(tr("Establishing the ring..."), kit.palette().warn);
    }
    for (const net::Interface& iface : net::interfaces()) {
        if (iface.loopback) continue;
        const std::string value = iface.ipv4 + " / " + iface.subnet_mask;
        ui::Kit::Opts     opts;
        opts.id = iface.name.c_str();
        kit.info(iface.name.c_str(), nullptr, value.c_str(), 0, opts);
    }
    kit.end_group();

    kit.begin_group(tr("Cabinet outputs"));
    kit.tip(tr("Send lamps, gun recoil and drive-board output to MAMEHooker, Hook Of The Reaper and "
               "similar tools, using MAME's network output protocol."));
    kit.toggle(tr("Network outputs"), tr("Lamps and drive-board bytes, as MAME sends them."), config.outputs_network);
    {
        int port = static_cast<int>(config.outputs_network_port);
        if (kit.number(tr("Port"), tr("MAME uses 8000."), port, 1, 65535, 1, {.disabled = !config.outputs_network})) {
            config.outputs_network_port = static_cast<u32>(port);
        }
    }
#ifdef _WIN32
    kit.tip(tr("The same outputs over MAME's Windows message protocol, for tools that hook MAME that way."));
    kit.toggle(tr("Windows outputs"), nullptr, config.outputs_windows);
#endif
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

void Gui::page_paths(ui::Kit& kit, Config& config)
{
    const auto dir_row = [&](const char* name, const char* about, std::string& value, DirPickerTarget target,
                             const char* tip, const char* empty_means) {
        bool browse = false;
        if (tip != nullptr) kit.tip(tip);
        ui::Kit::Opts opts;
        opts.id = name;
        kit.field(name, about, value, empty_means, tr("Browse..."), &browse, opts);
        if (browse) {
            open_dir_picker(target, value);
        }
    };

    kit.begin_group(tr("Games"));
    kit.tip(tr("One zip or 7z per game, named as in MAME. A game launched by name is loaded as <name>.zip or "
               "<name>.7z from its system's folder; Model 1 and Model 3 fall back to the Model 2 folder."));
    dir_row(tr("Model 1 ROM folder"), nullptr, config.model1_rom_dir, DirPickerTarget::Model1RomDir,
            tr("Where the Model 1 ROM archives live. Empty means the Model 2 folder."), tr("the Model 2 folder"));
    dir_row(tr("Model 2 ROM folder"), tr("One zip or 7z per game, named as in MAME."), config.model2_rom_dir,
            DirPickerTarget::Model2RomDir, tr("Where the Model 2 ROM archives live, and any Model 1 or Model 3 archives "
                                              "kept with them."), tr("not set"));
    dir_row(tr("Model 3 ROM folder"), nullptr, config.model3_rom_dir, DirPickerTarget::Model3RomDir,
            tr("Where the Model 3 ROM archives live. Empty means the Model 2 folder."), tr("the Model 2 folder"));
    kit.end_group();

    kit.begin_group(tr("Saves and pictures"));
    dir_row(tr("Saves"), tr("NVRAM, EEPROM, save states and textures."), config.nvram_dir, DirPickerTarget::NvramDir,
            tr("Battery-backed saves: per-game NVRAM (.nv) and EEPROM (.eeprom) images, high scores and operator settings."),
            nullptr);
    dir_row(tr("Screenshots"), nullptr, config.screenshot_dir, DirPickerTarget::ScreenshotDir,
            tr("Where screenshots are written."), nullptr);
    dir_row(tr("Bezels"), nullptr, config.bezel_dir, DirPickerTarget::BezelDir,
            tr("Bezel images: <game>.png, the parent set's, or default.png. Empty means a bezels folder beside keisho.ini."),
            tr("beside keisho.ini"));
    dir_row(tr("Crosshairs"), nullptr, config.crosshair_dir, DirPickerTarget::CrosshairDir,
            tr("Crosshair images: <game>-p1.png (-p2 for player 2) or <game>.png for both, then the parent set's, "
               "then default. Without one the built-in crosshair is drawn. Empty means a crosshairs folder beside keisho.ini."),
            tr("beside keisho.ini"));
    kit.end_group();

    kit.begin_group(tr("Game list"));
    kit.tip(tr("Let the game list fetch box art and descriptions from ArcadeDB over the network. Off keeps "
               "Keisho offline: the list still launches every game, with plain tiles."));
    kit.toggle(tr("Fetch artwork online"), tr("Box art and descriptions from ArcadeDB."), config.scrape_artwork);
    kit.end_group();

    kit.begin_group(tr("Keisho files"));
    kit.tip(tr("Where keisho.ini is read from and saved to. Set with --config <dir>."));
    kit.info(tr("Settings file"), nullptr, path_label(config_path_shown(m_config_path), "").c_str());
    kit.info(tr("Artwork cache"), nullptr, path_label(config.artwork_dir, tr("none")).c_str());
    kit.end_group();
}

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------

void Gui::page_states(ui::Kit& kit, const Config& config)
{
    if (!m_state_game_loaded) {
        kit.paragraph(nullptr, tr("Load a game to save and restore its state."));
        return;
    }
    kit.begin_group(tr("Save states"));
    kit.paragraph(nullptr, trf("Save the whole machine to a slot and restore it later. Quick-save is %s, "
                               "quick-load is %s.",
                               hotkey_label(config, Config::Hotkey::QuickSave),
                               hotkey_label(config, Config::Hotkey::QuickLoad)).c_str());
    static constexpr std::array<const char*, 3> kActions = {N_("Save"), N_("Load"), N_("Delete")};
    const std::array<const char*, 3>            labels   = tr_all(kActions);
    for (const StateSlot& s : m_state_slots) {
        const std::array<bool, 3> enabled = {true, s.occupied, s.occupied};
        ui::Kit::Opts             opts;
        opts.id = s.slot.c_str();
        const int clicked = kit.actions(s.label.c_str(), s.occupied ? s.timestamp.c_str() : tr("empty"), labels,
                                        enabled, s.occupied ? ui::Kit::Tone::Plain : ui::Kit::Tone::Primary, opts);
        if (clicked == 0) m_pending_state_request = StateRequest{StateRequest::Action::Save, s.slot};
        if (clicked == 1) m_pending_state_request = StateRequest{StateRequest::Action::Load, s.slot};
        if (clicked == 2) m_pending_state_request = StateRequest{StateRequest::Action::Delete, s.slot};
    }
    kit.end_group();
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

void Gui::page_system(ui::Kit& kit, Config& config)
{
    kit.begin_group(tr("Interface"));
    {
        const auto&              languages = i18n::available();
        std::vector<std::string> labels    = {tr("Automatic (system)"), "English"};
        std::vector<std::string> codes     = {"auto", "en"};
        int                      current   = config.language == "en" ? 1 : 0;
        for (const i18n::Language& language : languages) {
            labels.push_back(language_label(language));
            codes.push_back(language.code);
            if (language.code == config.language) current = static_cast<int>(codes.size()) - 1;
        }
        if (kit.dropdown(tr("Language"), nullptr, labels, current)) {
            config.language = codes[static_cast<size_t>(current)];
        }
    }
    {
        size_t           count  = 0;
        const ui::Theme* themes = ui::themes(&count);
        std::vector<const char*> names;
        int                      current = 0;
        for (size_t i = 0; i < count; ++i) {
            names.push_back(tr(themes[i].name));
            if (config.theme == themes[i].key) current = static_cast<int>(i);
        }
        if (kit.choice(tr("Theme"), tr(themes[static_cast<size_t>(current)].about), names, current)) {
            config.theme = themes[static_cast<size_t>(current)].key;
        }
    }
    kit.toggle(tr("FPS counter"), tr("In the top-right corner."), config.show_fps);
    kit.toggle(tr("Notifications"), tr("Brief notes such as \"State saved\"."), config.show_notifications);
    kit.end_group();

    kit.begin_group(tr("Play"));
    kit.tip(tr("Wheels and gamepads keep working while the window is not in front. Keyboard and mouse "
               "still need the window in front."));
    kit.toggle(tr("Background controller input"), tr("Pads and wheels work while the window is behind."),
               config.input_background);
    // The tip's text must outlive the row that shows it.
    const char* rewind_key = hotkey_name(config, Config::Hotkey::Rewind);
    std::string rewind_tip = tr("Keeps the last stretch of play in memory. Hold the rewind key and the game runs "
                                "backwards through it at double speed; let go and play carries on from there. "
                                "Taking the snapshots costs a little speed.");
    rewind_tip += " ";
    rewind_tip += rewind_key != nullptr ? trf("The key is %s.", rewind_key)
                                        : tr("No key is set for it: see the Hotkeys page.");
    kit.tip(rewind_tip.c_str());
    kit.toggle(tr("Rewind"), tr("Keep recent play in memory to run back through."), config.rewind);
    {
        // Always shown, so the setting is found with rewind off; greyed until it is on.
        int  buffer = static_cast<int>(config.rewind_buffer_mb);
        char shown[24];
        std::snprintf(shown, sizeof shown, "%d MiB", buffer);
        kit.tip(tr("RAM set aside for the history, not disk. Snapshots are stored as what changed, so 128 MiB "
                   "holds about half a minute of most games."));
        if (kit.slider(tr("Rewind memory"), tr("More memory keeps more of the past."), buffer, 32, 1024, 32, shown,
                       {.disabled = !config.rewind})) {
            config.rewind_buffer_mb = static_cast<u32>(buffer);
        }
    }
    kit.end_group();
}

// ---------------------------------------------------------------------------
// About
// ---------------------------------------------------------------------------

void Gui::page_about(ui::Kit& kit)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, kit.px(18.0f)));
    {
        // The name, large, in the accent; the strap line under it.
        const ImVec2 at   = ImGui::GetCursorScreenPos();
        const float  big  = ImGui::GetFontSize() * 2.0f;
        list->AddText(ImGui::GetFont(), big, at, kit.palette().accent, "KEISHO");
        list->AddText(ImVec2(at.x + kit.px(2.0f), at.y + big + kit.px(4.0f)), kit.palette().muted,
                      tr("A Sega Model 1, 2 & 3 arcade emulator"));
        ImGui::Dummy(ImVec2(0.0f, big + ImGui::GetFontSize() + kit.px(8.0f)));
    }
    kit.begin_group(tr("Keisho"));
    kit.info(tr("Version"), nullptr, SM2_VERSION);
    kit.info(tr("Copyright"), nullptr, "2025+ Daniel Martin (dmanlfc)");
    kit.info(tr("Licence"), nullptr, tr("GPL-3.0-or-later with additional terms; see LICENSE and NOTICE."));
    kit.end_group();

    kit.begin_group(tr("Support development"));
    {
        const ImVec2 at = kit.custom("PayPal", nullptr, ImGui::CalcTextSize("paypal.me/dmanlfc").x, ImGui::GetFontSize());
        ImGui::SetCursorScreenPos(at);
        ImGui::TextLinkOpenURL("paypal.me/dmanlfc", "https://www.paypal.com/paypalme/dmanlfc");
    }
    {
        const ImVec2 at = kit.custom("Ko-fi", nullptr, ImGui::CalcTextSize("ko-fi.com/dmanlfc").x, ImGui::GetFontSize());
        ImGui::SetCursorScreenPos(at);
        ImGui::TextLinkOpenURL("ko-fi.com/dmanlfc", "https://ko-fi.com/dmanlfc");
    }
    kit.end_group();

    kit.begin_group(tr("With thanks to"));
    kit.paragraph(nullptr, tr("Keisho exists because of open source shared with the community."));
    kit.info("MAME", nullptr, tr("Sega Model 1, 2 and 3 emulation guidance"));
    kit.info("Supermodel", nullptr, tr("Model 3 reference (Bart Trzynadlowski and the Supermodel team)"));
    kit.info("Musashi", nullptr, tr("Motorola 68000 CPU core (Karl Stenerud)"));
    kit.info("ymfm", nullptr, tr("Yamaha FM sound cores (Aaron Giles)"));
    kit.info("SDL", nullptr, tr("windowing, input and audio"));
    kit.info("Dear ImGui", nullptr, tr("this user interface (Omar Cornut)"));
    kit.info("pugixml", nullptr, tr("games.xml parsing"));
    kit.info("miniz", nullptr, tr("ZIP archive decompression"));
    kit.info("LZMA SDK", nullptr, tr("7-Zip archive decompression (Igor Pavlov)"));
    kit.info("VulkanMemoryAllocator", nullptr, tr("GPU memory (AMD / GPUOpen)"));
    kit.info("shaderc / glslang", nullptr, tr("shader compilation"));
    kit.info("stb_image", nullptr, tr("image decoding (Sean Barrett)"));
    kit.info(tr("Gamepad picture"), nullptr, tr("Xelu's Free Controller Prompts (Nicolae Berbece), CC0"));
    kit.info("Noto Sans", nullptr, tr("the menu's font (The Noto Project Authors), OFL 1.1"));
    kit.info("libcurl", nullptr, tr("artwork fetching (optional)"));
    kit.info("HarfBuzz", nullptr, tr("text shaping for translations"));
    kit.info("SheenBidi", nullptr, tr("right-to-left text layout (Muhammad Tayyab Akram)"));
    kit.info("ArcadeDB", nullptr, tr("game artwork and descriptions (adb.arcadeitalia.net, by Motoschifo)"));
    kit.end_group();

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
    {0.435f, 0.441f}, {0.503f, 0.319f}, {0.573f, 0.441f},
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

namespace {

bool find_bezel_window(const u8* rgba, int w, int h, int* x, int* y, int* ww, int* hh)
{
    const auto clear = [&](int px, int py) {
        return rgba[(static_cast<usize>(py) * static_cast<usize>(w) + static_cast<usize>(px)) * 4 + 3]
               < 128;
    };
    const int cx = w / 2;
    const int cy = h / 2;
    if (!clear(cx, cy)) {
        return false;
    }
    int left = cx, right = cx, top = cy, bottom = cy;
    while (left > 0 && clear(left - 1, cy)) --left;
    while (right < w - 1 && clear(right + 1, cy)) ++right;
    while (top > 0 && clear(cx, top - 1)) --top;
    while (bottom < h - 1 && clear(cx, bottom + 1)) ++bottom;
    *x  = left;
    *y  = top;
    *ww = right - left + 1;
    *hh = bottom - top + 1;
    return true;
}

}  // namespace

void Gui::load_bezel(const std::string& path)
{
    release_bezel();
    m_bezel.path = path;
    if (path.empty() || m_backend == nullptr) {
        return;
    }
    int            w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (rgba == nullptr) {
        SM2_WARN("bezel: could not read '%s'", path.c_str());
        return;
    }
    int x = 0, y = 0, ww = 0, hh = 0;
    if (!find_bezel_window(rgba, w, h, &x, &y, &ww, &hh)) {
        SM2_WARN("bezel: '%s' has no transparent window in its centre", path.c_str());
    } else {
        m_bezel.handle   = m_backend->create_texture(static_cast<u32>(w), static_cast<u32>(h), rgba);
        m_bezel.w        = static_cast<float>(w);
        m_bezel.h        = static_cast<float>(h);
        m_bezel.window_x = static_cast<float>(x);
        m_bezel.window_y = static_cast<float>(y);
        m_bezel.window_w = static_cast<float>(ww);
        m_bezel.window_h = static_cast<float>(hh);
        SM2_INFO("bezel: %s (%dx%d, window %dx%d at %d,%d)", path.c_str(), w, h, ww, hh, x, y);
    }
    stbi_image_free(rgba);
}

void Gui::release_bezel()
{
    if (m_bezel.handle != 0 && m_backend != nullptr) {
        m_backend->destroy_texture(m_bezel.handle);
    }
    m_bezel = Bezel{};
}

void Gui::draw_bezel(const Config& config)
{
    namespace fs = std::filesystem;
    const fs::path dir = config.bezel_dir.empty()
                             ? fs::path(m_config_path.empty() ? default_config_path() : m_config_path)
                                       .parent_path()
                                   / "bezels"
                             : fs::path(config.bezel_dir);
    const std::string lookup = dir.string() + '\n' + m_current_game;
    if (lookup != m_bezel_lookup) {
        m_bezel_lookup = lookup;
        std::string found;
        for (const std::string& stem : {m_current_game, m_current_family, std::string("default")}) {
            std::error_code error;
            const fs::path  candidate = dir / (stem + ".png");
            if (!stem.empty() && fs::exists(candidate, error)) {
                found = candidate.string();
                break;
            }
        }
        if (found != m_bezel.path) {
            load_bezel(found);
        }
    }
    if (m_bezel.handle == 0) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) {
        return;
    }
    const float  scale = std::min(io.DisplaySize.x / m_bezel.w, io.DisplaySize.y / m_bezel.h);
    const ImVec2 p0((io.DisplaySize.x - m_bezel.w * scale) * 0.5f,
                    (io.DisplaySize.y - m_bezel.h * scale) * 0.5f);
    const ImVec2 p1(p0.x + m_bezel.w * scale, p0.y + m_bezel.h * scale);
    m_present_area.x      = (p0.x + m_bezel.window_x * scale) / io.DisplaySize.x;
    m_present_area.y      = (p0.y + m_bezel.window_y * scale) / io.DisplaySize.y;
    m_present_area.width  = m_bezel.window_w * scale / io.DisplaySize.x;
    m_present_area.height = m_bezel.window_h * scale / io.DisplaySize.y;
    ImGui::GetBackgroundDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(m_backend->texture_imgui_id(m_bezel.handle)), p0, p1);
}

void Gui::update_crosshair_images(const Config& config)
{
    namespace fs = std::filesystem;
    const fs::path dir = config.crosshair_dir.empty()
                             ? fs::path(m_config_path.empty() ? default_config_path() : m_config_path)
                                       .parent_path()
                                   / "crosshairs"
                             : fs::path(config.crosshair_dir);
    const std::string lookup = dir.string() + '\n' + m_current_game;
    if (lookup == m_crosshair_lookup) {
        return;
    }
    m_crosshair_lookup = lookup;
    for (usize player = 0; player < m_crosshair_images.size(); ++player) {
        const std::string suffix = "-p" + std::to_string(player + 1);
        std::string       found;
        for (const std::string& stem : {m_current_game, m_current_family, std::string("default")}) {
            if (stem.empty()) {
                continue;
            }
            for (const std::string& name : {stem + suffix, stem}) {
                std::error_code error;
                const fs::path  candidate = dir / (name + ".png");
                if (fs::exists(candidate, error)) {
                    found = candidate.string();
                    break;
                }
            }
            if (!found.empty()) {
                break;
            }
        }
        CrosshairImage& image = m_crosshair_images[player];
        if (found == image.path) {
            continue;
        }
        if (image.handle != 0 && m_backend != nullptr) {
            m_backend->destroy_texture(image.handle);
        }
        image      = CrosshairImage{};
        image.path = found;
        if (found.empty() || m_backend == nullptr) {
            continue;
        }
        int            w = 0, h = 0, channels = 0;
        unsigned char* rgba = stbi_load(found.c_str(), &w, &h, &channels, 4);
        if (rgba == nullptr) {
            SM2_WARN("crosshair: could not read '%s'", found.c_str());
            continue;
        }
        image.handle = m_backend->create_texture(static_cast<u32>(w), static_cast<u32>(h), rgba);
        image.w      = static_cast<float>(w);
        image.h      = static_cast<float>(h);
        stbi_image_free(rgba);
        SM2_INFO("crosshair: player %u %s (%dx%d)", static_cast<unsigned>(player + 1),
                 found.c_str(), w, h);
    }
}

void Gui::release_crosshair_images()
{
    for (CrosshairImage& image : m_crosshair_images) {
        if (image.handle != 0 && m_backend != nullptr) {
            m_backend->destroy_texture(image.handle);
        }
        image = CrosshairImage{};
    }
    m_crosshair_lookup.clear();
}

void Gui::release_pad_art()
{
    if (m_pad_art.handle != 0 && m_backend != nullptr) {
        m_backend->destroy_texture(m_pad_art.handle);
    }
    m_pad_art = PadArt{};
}

// ---------------------------------------------------------------------------
// Input: gamepad
// ---------------------------------------------------------------------------

namespace {

/// The pad slots, in the order the Gamepad page lays them out: a slot per
/// cabinet control a pad binding can drive. Buttons and directions are
/// Config::PadRole values; pedals and sticks are Config::PadAxisRole values.
enum class SlotKind { Button, Pedal, Stick, Axis };  // Axis: one centred axis on its own
struct Slot {
    const char* name;  ///< the generic name, which a game may rename
    SlotKind    kind;
    u32         role;
    int         column;  ///< 0 left of the picture, 1 right, 2 under the left stick, 3 under the right
};
constexpr Slot kSlots[] = {
    {N_("Brake"),       SlotKind::Pedal,  static_cast<u32>(Config::PadAxisRole::Brake), 0},
    {N_("Gear Down"),   SlotKind::Button, static_cast<u32>(Config::PadRole::GearDown),  0},
    {N_("Move Up"),     SlotKind::Button, static_cast<u32>(Config::PadRole::Up),        0},
    {N_("Move Left"),   SlotKind::Button, static_cast<u32>(Config::PadRole::Left),      0},
    {N_("Move Right"),  SlotKind::Button, static_cast<u32>(Config::PadRole::Right),     0},
    {N_("Move Down"),   SlotKind::Button, static_cast<u32>(Config::PadRole::Down),      0},
    {N_("Coin"),        SlotKind::Button, static_cast<u32>(Config::PadRole::Coin),      0},
    {N_("Accelerator"), SlotKind::Pedal,  static_cast<u32>(Config::PadAxisRole::Accel), 1},
    {N_("Gear Up"),     SlotKind::Button, static_cast<u32>(Config::PadRole::GearUp),    1},
    {N_("Button 4"),    SlotKind::Button, static_cast<u32>(Config::PadRole::Button4),   1},
    {N_("Button 3"),    SlotKind::Button, static_cast<u32>(Config::PadRole::Button3),   1},
    {N_("Button 2"),    SlotKind::Button, static_cast<u32>(Config::PadRole::Button2),   1},
    {N_("Button 1"),    SlotKind::Button, static_cast<u32>(Config::PadRole::Button1),   1},
    {N_("Start"),       SlotKind::Button, static_cast<u32>(Config::PadRole::Start),     1},
    {N_("Steer / Aim"), SlotKind::Stick,  static_cast<u32>(Config::PadAxisRole::AimX),   2},
    {N_("Right Lever"), SlotKind::Stick,  static_cast<u32>(Config::PadAxisRole::LeverX), 3},
    {N_("Elevation"),   SlotKind::Axis,   static_cast<u32>(Config::PadAxisRole::AimY),   3},
};

/// A control of a game's cabinet as the Cabinet picture shows it: what it is,
/// its name, and the slots that drive it. A joystick's four buttons are up,
/// down, left and right.
struct CabinetControl {
    enum class Kind { Button, Coin, Joystick, Wheel, Handlebar, Stick, Gun, Board, Rod, Bat, Pedal, Shift, Gate };
    Kind               kind;
    const char*        name;  ///< msgid
    std::array<s32, 4> buttons{-1, -1, -1, -1};  ///< PadRole values
    std::array<s32, 2> axes{-1, -1};             ///< PadAxisRole values: x and y, or a pedal's value
    bool               lit = false;              ///< pressed now (buttons, pedals past half, shifts)
    float              x   = 0.0f, y = 0.0f;     ///< -1..1 (sticks) or 0..1 (pedals), now
};

/// Everything the Gamepad page knows of a game's cabinet: each slot's name
/// there (nullptr: the cabinet has no such control), and the controls drawn.
struct CabinetPanel {
    std::array<const char*, std::size(kSlots)> slot_names{};
    std::vector<CabinetControl>                controls;
};

/// Per-title names for Model 1 and 2 slots, where the generic layout's names
/// are wrong or the control has another name on the cabinet. nullptr: unused.
struct NamedSlot {
    const char* family;
    const char* slot;
    const char* name;
};
constexpr NamedSlot kNamedSlots[] = {
    {"swa", "Brake", N_("Laser")},               {"swa", "Accelerator", N_("Torpedo")},
    {"swa", "Button 1", N_("Laser")},            {"swa", "Button 2", N_("Torpedo")},
    {"swa", "Button 3", N_("View")},             {"swa", "Button 4", nullptr},
    {"swa", "Move Up", N_("Throttle forward")},  {"swa", "Move Down", N_("Throttle back")},
    {"swa", "Move Left", nullptr},               {"swa", "Move Right", nullptr},
    {"swa", "Gear Up", nullptr},                 {"swa", "Gear Down", nullptr},
    {"swa", "Steer / Aim", N_("Yoke")},          {"swa", "Right Lever", N_("Throttle")},
    {"wingwar", "Button 1", N_("Machine Gun")},  {"wingwar", "Button 2", N_("Missile")},
    {"wingwar", "Button 3", N_("Smoke")},        {"wingwar", "Button 4", nullptr},
    {"wingwar", "Move Up", N_("View 1")},        {"wingwar", "Move Right", N_("View 2")},
    {"wingwar", "Move Down", N_("View 3")},      {"wingwar", "Move Left", N_("View 4")},
    {"wingwar", "Brake", N_("Throttle back")},   {"wingwar", "Accelerator", N_("Throttle forward")},
    {"wingwar", "Gear Up", nullptr},             {"wingwar", "Gear Down", nullptr},
    {"wingwar", "Steer / Aim", N_("Stick")},     {"wingwar", "Right Lever", N_("Throttle")},
    {"desert", "Gear Down", N_("Reverse")},      {"desert", "Gear Up", N_("Forward")},
    {"desert", "Shifter", N_("Gear Lever")},
    {"desert", "Button 1", N_("Machine Gun")},   {"desert", "Button 2", N_("Cannon")},
    {"desert", "Button 3", N_("View 1")},        {"desert", "Button 4", N_("View 2")},
    {"desert", "Move Up", N_("View 3")},         {"desert", "Move Down", nullptr},
    {"desert", "Move Left", nullptr},            {"desert", "Move Right", nullptr},
    {"desert", "Steer / Aim", N_("Steering")},   {"desert", "Accelerator", N_("Accelerator")},
    {"desert", "Elevation", N_("Turret")},
    {"segawski", "Gear Down", N_("Pitch Left")}, {"segawski", "Gear Up", N_("Pitch Right")},
    {"segawski", "Steer / Aim", N_("Handlebars")},
    {"powsled", "Gear Down", N_("Cancel Error")}, {"powsled", "Gear Up", N_("Cancel Network")},
    {"swtrilgy", "Steer / Aim", N_("Flight Stick")},
    {"skichamp", "Steer / Aim", N_("Skis")},
    {"magtruck", "Steer / Aim", N_("Handle")},
    {"harley", "Steer / Aim", N_("Handlebars")},
    {"skytargt", "Button 1", N_("Machine Gun")}, {"skytargt", "Button 2", N_("Missile")},
    {"skytargt", "Button 3", N_("View")},        {"skytargt", "Button 4", nullptr},
    {"skytargt", "Steer / Aim", N_("Flight Stick")},
    {"dynamcop", "Button 1", N_("Punch")},       {"dynamcop", "Button 2", N_("Kick")},
    {"dynamcop", "Button 3", N_("Jump")},
    {"zerogun", "Button 1", N_("Shot")},         {"zerogun", "Button 2", N_("Bomb")},
    {"zerogun", "Button 3", nullptr},
    {"zeroguna", "Button 1", N_("Shot")},        {"zeroguna", "Button 2", N_("Bomb")},
    {"zeroguna", "Button 3", nullptr},
    {"pltkids", "Button 1", N_("Shot")},         {"pltkids", "Button 2", N_("Bomb")},
    {"pltkids", "Button 3", nullptr},
    {"vstriker", "Button 1", N_("Long Pass")},   {"vstriker", "Button 2", N_("Shoot")},
    {"vstriker", "Button 3", N_("Short Pass")},
    {"manxtt", "Steer / Aim", N_("Handlebars")}, {"manxtt", "Accelerator", N_("Throttle")},
    {"motoraid", "Steer / Aim", N_("Handlebars")}, {"motoraid", "Accelerator", N_("Throttle")},
    {"motoraid", "Button 1", N_("Punch")},       {"motoraid", "Button 2", N_("Kick")},
    {"waverunr", "Steer / Aim", N_("Handlebars")}, {"waverunr", "Accelerator", N_("Throttle")},
    {"topskatr", "Steer / Aim", N_("Board")},
    {"skisuprg", "Steer / Aim", N_("Skis")},
    {"dynabb", "Button 1", N_("Swing")},         {"dynabb", "Button 2", N_("Bunt")},
    {"dynabb", "Button 3", N_("Run")},
    {"dynabb97", "Button 1", N_("Swing")},       {"dynabb97", "Button 2", N_("Bunt")},
    {"dynabb97", "Button 3", N_("Run")},
    {"hpyagu98", "Button 1", N_("Swing")},       {"hpyagu98", "Button 2", N_("Bunt")},
    {"hpyagu98", "Button 3", N_("Run")},
};

/// Fighting games of Model 1 and 2: punch, kick, guard on three buttons.
bool fighting_m12(const std::string& key)
{
    static const char* const kFighting[] = {"vf", "vf2", "fvipers", "doa", "lastbrnx", "schamp", "airwlkrs"};
    return std::any_of(std::begin(kFighting), std::end(kFighting), [&](const char* k) { return key == k; });
}

/// What a slot is called on this game's cabinet, or nullptr where the cabinet
/// has no such control. The per-title tables first, then the Model 3 rules
/// (typed_slot_name), then what Model 1 and 2's input flags say.
/// The entry of kNamedSlots for a family's slot (or pseudo-slot, such as
/// "Shifter"), or nullptr when the table has none.
const NamedSlot* named_slot(std::string_view family, const char* slot)
{
    for (const NamedSlot& named : kNamedSlots) {
        if (family == named.family && std::strcmp(slot, named.slot) == 0) return &named;
    }
    return nullptr;
}

const char* cabinet_slot_name(const Gui::GameFamily& family, const Slot& slot)
{
    if (const NamedSlot* named = named_slot(family.key, slot.name); named != nullptr) {
        return named->name;
    }
    // A second aim axis stands as its own control only where it is not
    // part of a stick: a wheel that also tilts.
    if (std::strcmp(slot.name, "Elevation") == 0) return family.tilt ? slot.name : nullptr;
    // A gun's trigger and reload sit on the pedals' triggers too.
    if (family.gun && std::strcmp(slot.name, "Accelerator") == 0) return N_("Trigger");
    if (family.gun && std::strcmp(slot.name, "Brake") == 0) return family.missile ? N_("Missile") : N_("Gun Reload");
    using F        = rom::InputFlags;
    const auto is  = [&](F flag) { return rom::has_input(static_cast<F>(family.inputs), flag); };
    const auto at  = [&](const char* name) { return std::strcmp(slot.name, name) == 0; };
    const bool m3  = is(F::Fighting) || is(F::SpikeOut) || is(F::Soccer) || is(F::Vr4) || is(F::ViewChange)
                  || is(F::Handbrake) || is(F::Harley) || is(F::AnalogJoystick) || is(F::Fishing)
                  || is(F::TwinJoysticks) || is(F::MagTruck) || is(F::Ski) || is(F::Shift4) || is(F::ShiftUpDown)
                  || is(F::AnalogGun1) || family.key == "von";
    if (m3) {
        return typed_slot_name(family, slot.name);
    }

    // Model 1 and 2, from the flags.
    const int button = at("Button 1") ? 1 : at("Button 2") ? 2 : at("Button 3") ? 3 : at("Button 4") ? 4 : 0;
    const bool direction = at("Move Up") || at("Move Down") || at("Move Left") || at("Move Right");
    if (at("Start") || at("Coin")) return slot.name;
    if (family.gun) {
        // A gun: aim, trigger and reload (or the missile), and nothing else.
        if (at("Steer / Aim")) return N_("Aim");
        if (at("Accelerator") || button == 1) return N_("Trigger");
        if (at("Brake") || button == 2) return family.missile ? N_("Missile") : N_("Gun Reload");
        return nullptr;
    }
    if (at("Steer / Aim")) {
        if (family.steer == Gui::GameFamily::Steer::Wheel) return N_("Steering Wheel");
        if (family.steer == Gui::GameFamily::Steer::Handlebar) return N_("Handlebars");
        if (family.steer == Gui::GameFamily::Steer::Stick) return N_("Stick");
        if (family.steer == Gui::GameFamily::Steer::Board) return N_("Board");
        return family.joystick ? N_("Joystick") : nullptr;
    }
    // A baseball bat swings on the accelerator's trigger; player 2's is on
    // their own pad, so the brake slot is no control here.
    if (at("Accelerator")) return family.bat ? N_("Bat") : family.accel ? slot.name : nullptr;
    if (at("Brake")) return family.bat ? nullptr : family.brake ? slot.name : nullptr;
    if (at("Right Lever")) return family.lever ? slot.name : nullptr;
    if (at("Gear Up")) return family.gate ? N_("Shift Up") : family.shift ? N_("Shift Up") : nullptr;
    if (at("Gear Down")) return family.gate ? N_("Shift Down") : family.shift ? N_("Shift Down") : nullptr;
    if (direction) {
        return family.joystick ? slot.name : nullptr;
    }
    if (button != 0) {
        if (is(F::Vehicle)) {
            // The view buttons of a driving cabinet, as many as it declares.
            if (static_cast<u32>(button) > family.vr) return nullptr;
            if (family.vr == 1) return N_("View");
            static const char* const kViews[] = {N_("View 1"), N_("View 2"), N_("View 3"), N_("View 4")};
            return kViews[button - 1];
        }
        if (fighting_m12(family.key)) {
            static const char* const kPkg[] = {N_("Punch"), N_("Kick"), N_("Guard"), nullptr};
            return kPkg[button - 1];
        }
        if (is(F::Buttons3)) {
            return button <= 3 ? slot.name : nullptr;
        }
        return slot.name;
    }
    return slot.name;
}

/// Whether a control under the hands also carries the four digital
/// directions: a joystick's switches, or a d-pad that doubles a yoke.
bool directions_merged(const CabinetControl& c)
{
    using Kind = CabinetControl::Kind;
    switch (c.kind) {
        case Kind::Button: case Kind::Coin: case Kind::Pedal: case Kind::Shift: case Kind::Gate: return false;
        default: return c.buttons[0] >= 0 && c.buttons[1] >= 0 && c.buttons[2] >= 0 && c.buttons[3] >= 0;
    }
}

/// Whether a slot name is one of the generic directions.
bool generic_direction(const char* name)
{
    return name != nullptr
        && (std::strcmp(name, "Move Up") == 0 || std::strcmp(name, "Move Down") == 0
            || std::strcmp(name, "Move Left") == 0 || std::strcmp(name, "Move Right") == 0);
}

/// The cabinet of a game, from its slots' names: nullptr family is the
/// default layout, with every slot.
CabinetPanel cabinet_panel(const Gui::GameFamily* family)
{
    using Kind = CabinetControl::Kind;
    CabinetPanel panel;
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        panel.slot_names[i] = family != nullptr        ? cabinet_slot_name(*family, kSlots[i])
                            : kSlots[i].kind == SlotKind::Axis ? nullptr  // the stick's own Y in the default layout
                                                       : kSlots[i].name;
    }
    const auto name_of = [&](const char* slot) -> const char* {
        for (size_t i = 0; i < std::size(kSlots); ++i) {
            if (std::strcmp(kSlots[i].name, slot) == 0) return panel.slot_names[i];
        }
        return nullptr;
    };
    const auto role = [](Config::PadRole r) { return static_cast<s32>(r); };
    const auto axis = [](Config::PadAxisRole r) { return static_cast<s32>(r); };
    const auto add  = [&](Kind kind, const char* name) -> CabinetControl& {
        panel.controls.push_back({kind, name});
        return panel.controls.back();
    };
    // A button; one of the same name already there takes the binding too.
    const auto add_button = [&](const char* name, s32 r) -> CabinetControl& {
        for (CabinetControl& c : panel.controls) {
            if (c.kind == Kind::Button && std::strcmp(c.name, name) == 0) {
                for (s32& held : c.buttons) {
                    if (held < 0) { held = r; break; }
                }
                return c;
            }
        }
        CabinetControl& c = add(Kind::Button, name);
        c.buttons[0]      = r;
        return c;
    };

    // The main control under the player's hands.
    const char* steer = name_of("Steer / Aim");
    const bool  twin  = family != nullptr
                    && (rom::has_input(static_cast<rom::InputFlags>(family->inputs), rom::InputFlags::TwinJoysticks)
                        || family->key == "von");
    const char* dir_names[4] = {name_of("Move Up"), name_of("Move Down"), name_of("Move Left"), name_of("Move Right")};
    const bool  all_dirs     = std::all_of(std::begin(dir_names), std::end(dir_names), [](const char* n) { return n != nullptr; });
    bool joystick_took_directions = false;
    if (steer != nullptr) {
        Kind kind = Kind::Stick;
        if (family == nullptr) {
            kind = Kind::Joystick;
        } else if (family->gun) {
            kind = Kind::Gun;
        } else {
            switch (family->steer) {
                case Gui::GameFamily::Steer::Wheel:     kind = Kind::Wheel; break;
                case Gui::GameFamily::Steer::Handlebar: kind = Kind::Handlebar; break;
                case Gui::GameFamily::Steer::Stick:     kind = Kind::Stick; break;
                case Gui::GameFamily::Steer::Gun:       kind = Kind::Gun; break;
                case Gui::GameFamily::Steer::Board:     kind = Kind::Board; break;
                case Gui::GameFamily::Steer::Rod:       kind = Kind::Rod; break;
                case Gui::GameFamily::Steer::None:      kind = Kind::Joystick; break;
            }
        }
        CabinetControl& c = add(kind, steer);
        c.axes            = {axis(Config::PadAxisRole::AimX), axis(Config::PadAxisRole::AimY)};
        // Directions that kept their generic names only double the main
        // control (Desert Tank's d-pad steers and tilts the turret), so
        // they ride on it rather than stand as four buttons.
        const bool generic_dirs = all_dirs && std::all_of(std::begin(dir_names), std::end(dir_names), generic_direction);
        if (all_dirs && (kind == Kind::Joystick || generic_dirs)) {
            c.buttons = {role(Config::PadRole::Up), role(Config::PadRole::Down), role(Config::PadRole::Left),
                         role(Config::PadRole::Right)};
            joystick_took_directions = true;
        }
    }
    if (const char* name = name_of("Elevation"); name != nullptr) {
        CabinetControl& c = add(Kind::Stick, name);
        c.axes            = {-1, axis(Config::PadAxisRole::AimY)};
    }
    if (steer == nullptr && all_dirs) {
        // A digital joystick alone (the fighting games of Model 3).
        CabinetControl& c = add(Kind::Joystick, N_("Joystick"));
        c.buttons = {role(Config::PadRole::Up), role(Config::PadRole::Down), role(Config::PadRole::Left),
                     role(Config::PadRole::Right)};
        joystick_took_directions = true;
    }
    // Virtual On: the left lever with its shot and dash, then the right side.
    if (twin) {
        if (const char* name = name_of("Brake"); name != nullptr) add_button(name, -1).axes[0] = axis(Config::PadAxisRole::Brake);
        if (const char* name = name_of("Gear Down"); name != nullptr) add_button(name, role(Config::PadRole::GearDown));
        CabinetControl& c = add(Kind::Joystick, N_("Right Lever"));
        c.axes            = {axis(Config::PadAxisRole::LeverX), axis(Config::PadAxisRole::LeverY)};
        c.buttons = {role(Config::PadRole::Button4), role(Config::PadRole::Button1), role(Config::PadRole::Button3),
                     role(Config::PadRole::Button2)};
        if (const char* name = name_of("Accelerator"); name != nullptr) add_button(name, -1).axes[0] = axis(Config::PadAxisRole::Accel);
        if (const char* name = name_of("Gear Up"); name != nullptr) add_button(name, role(Config::PadRole::GearUp));
        add_button(N_("Start"), role(Config::PadRole::Start));
        add(Kind::Coin, N_("Coin")).buttons[0] = role(Config::PadRole::Coin);
        return panel;
    }
    // Pedals, or what sits on the triggers: a gun's trigger.
    // Pedals: brake at the left and accelerator at the right, as the car has
    // them. A gun's triggers are buttons: the trigger first, then the missile
    // or reload.
    const bool pedals_are_buttons = (family != nullptr && family->gun) || twin;
    if (pedals_are_buttons) {
        if (const char* name = name_of("Accelerator"); name != nullptr) add_button(name, -1).axes[0] = axis(Config::PadAxisRole::Accel);
        if (const char* name = name_of("Brake"); name != nullptr) add_button(name, -1).axes[0] = axis(Config::PadAxisRole::Brake);
    } else if (family != nullptr && family->bat) {
        if (const char* name = name_of("Accelerator"); name != nullptr) add(Kind::Bat, name).axes[0] = axis(Config::PadAxisRole::Accel);
    } else {
        if (const char* name = name_of("Brake"); name != nullptr) add(Kind::Pedal, name).axes[0] = axis(Config::PadAxisRole::Brake);
        if (const char* name = name_of("Accelerator"); name != nullptr) add(Kind::Pedal, name).axes[0] = axis(Config::PadAxisRole::Accel);
    }
    // The shifter.
    {
        const char* up   = name_of("Gear Up");
        const char* down = name_of("Gear Down");
        if (up != nullptr || down != nullptr) {
            const NamedSlot* lever   = family != nullptr ? named_slot(family->key, "Shifter") : nullptr;
            const bool       shifter = family == nullptr || family->gate || family->shift || lever != nullptr;
            if (shifter && !twin) {
                CabinetControl& c = add(family != nullptr && family->gate ? Kind::Gate : Kind::Shift,
                                        lever != nullptr ? lever->name : N_("Shifter"));
                c.buttons[0]      = role(Config::PadRole::GearUp);
                c.buttons[1]      = role(Config::PadRole::GearDown);
            } else {
                if (up != nullptr) add_button(up, role(Config::PadRole::GearUp));
                if (down != nullptr) add_button(down, role(Config::PadRole::GearDown));
            }
        }
    }
    // The second stick: Virtual On's right lever is its four buttons as well.
    if (twin) {
        CabinetControl& c = add(Kind::Joystick, N_("Right Lever"));
        c.axes            = {axis(Config::PadAxisRole::LeverX), axis(Config::PadAxisRole::LeverY)};
        c.buttons = {role(Config::PadRole::Button4), role(Config::PadRole::Button1), role(Config::PadRole::Button3),
                     role(Config::PadRole::Button2)};
    } else if (const char* name = name_of("Right Lever"); name != nullptr) {
        CabinetControl& c = add(Kind::Stick, name);
        c.axes            = {axis(Config::PadAxisRole::LeverX), axis(Config::PadAxisRole::LeverY)};
    }
    // The buttons, then the directions that are buttons of their own (Wing
    // War's views, Star Wars Trilogy's triggers) when no joystick took them,
    // then Start.
    if (!twin) {
        const std::pair<const char*, Config::PadRole> buttons[] = {
            {"Button 1", Config::PadRole::Button1}, {"Button 2", Config::PadRole::Button2},
            {"Button 3", Config::PadRole::Button3}, {"Button 4", Config::PadRole::Button4}};
        for (const auto& [slot, r] : buttons) {
            if (const char* name = name_of(slot); name != nullptr) {
                add_button(name, role(r));
            }
        }
    }
    if (!joystick_took_directions) {
        const Config::PadRole dirs[] = {Config::PadRole::Up, Config::PadRole::Down, Config::PadRole::Left,
                                        Config::PadRole::Right};
        for (int d = 0; d < 4; ++d) {
            if (dir_names[d] != nullptr) add_button(dir_names[d], role(dirs[d]));
        }
    }
    add_button(N_("Start"), role(Config::PadRole::Start));
    add(Kind::Coin, N_("Coin")).buttons[0] = role(Config::PadRole::Coin);
    return panel;
}

/// Whether a pad binding (a button, or an axis past half) is held, from the
/// pad's buttons and its axes read this frame.
bool binding_held_now(const Input* input, u32 player, s32 binding, const std::array<s16, SDL_GAMEPAD_AXIS_COUNT>& axes)
{
    if (binding < 0 || input == nullptr) return false;
    if (binding >= Config::kPadAxisMinus) {
        const s32 a = binding - Config::kPadAxisMinus;
        return a < SDL_GAMEPAD_AXIS_COUNT && axes[static_cast<usize>(a)] < -16384;
    }
    if (binding >= Config::kPadAxisPlus) {
        const s32 a = binding - Config::kPadAxisPlus;
        return a < SDL_GAMEPAD_AXIS_COUNT && axes[static_cast<usize>(a)] > 16384;
    }
    return input->pad_physical_down(player, binding);
}

/// Map all waits this long for each control before moving on.
constexpr float kMapSeconds = 6.0f;

/// The Gamepad page's views, in the order of the View switch.
constexpr int kViewGamepad   = 0;
constexpr int kViewCabinet   = 1;
constexpr int kViewShortcuts = 2;

/// The slot (index into kSlots) a button role or an axis role is bound
/// through; -1 for none.
int slot_of_button(s32 role)
{
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        if (kSlots[i].kind == SlotKind::Button && static_cast<s32>(kSlots[i].role) == role) return static_cast<int>(i);
    }
    return -1;
}
int slot_of_axis(s32 role)
{
    if (role < 0) return -1;
    // A pedal's slot is its own, as is a single axis's; a stick's is its X
    // axis, and covers its Y where no single-axis slot claims it.
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        const Slot& s = kSlots[i];
        if ((s.kind == SlotKind::Pedal || s.kind == SlotKind::Axis) && static_cast<s32>(s.role) == role) return static_cast<int>(i);
    }
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        const Slot& s = kSlots[i];
        if (s.kind == SlotKind::Stick && (static_cast<s32>(s.role) == role || static_cast<s32>(s.role) + 1 == role)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// The slots Map all walks for a control, in the order a player would be
/// asked for them: a digital joystick's four directions, a stick's one slot,
/// a shifter's up then down, a button's first binding.
std::vector<u32> control_slots(const CabinetControl& c)
{
    using Kind = CabinetControl::Kind;
    std::vector<u32> out;
    const auto push = [&](int si) {
        if (si >= 0 && std::find(out.begin(), out.end(), static_cast<u32>(si)) == out.end()) out.push_back(static_cast<u32>(si));
    };
    switch (c.kind) {
        case Kind::Joystick:
            if (c.buttons[0] >= 0) {
                for (const s32 b : c.buttons) push(slot_of_button(b));
            } else {
                push(slot_of_axis(c.axes[0]));
            }
            break;
        case Kind::Shift:
        case Kind::Gate:
            push(slot_of_button(c.buttons[0]));
            push(slot_of_button(c.buttons[1]));
            break;
        case Kind::Button:
        case Kind::Coin:
            if (c.buttons[0] >= 0) {
                push(slot_of_button(c.buttons[0]));
            } else {
                push(slot_of_axis(c.axes[0]));
            }
            break;
        case Kind::Pedal:
            push(slot_of_axis(c.axes[0]));
            break;
        default:
            push(slot_of_axis(c.axes[0] >= 0 ? c.axes[0] : c.axes[1]));
            break;
    }
    return out;
}

/// A cabinet drawn as its control panel: where each control went, to click
/// and to label.
struct PanelCell {
    int    control;  ///< index into the panel's controls
    ImVec2 a, b;     ///< the cell, labels included
    ImVec2 centre;   ///< of the control's drawing
    float  r;        ///< its radius or half size
};

/// One control's drawing at a place, lit as it is now.
void draw_control_glyph(ui::Kit& kit, const CabinetControl& c, ImVec2 centre, float r, bool chosen)
{
    using Kind = CabinetControl::Kind;
    const ui::Palette& pal  = kit.palette();
    ImDrawList*        draw = ImGui::GetWindowDrawList();
    const float        stroke = std::max(kit.stroke(), kit.px(1.5f));
    const ImU32        ink  = chosen ? pal.accent : pal.muted;
    const ImU32        lit  = pal.accent;
    const ImU32        fill = pal.card;
    const ImU32        col  = c.lit ? lit : ink;
    const float        cx = centre.x, cy = centre.y;
    switch (c.kind) {
        case Kind::Wheel: {
            const float a = c.x * 1.4f;
            draw->AddCircle(ImVec2(cx, cy), r, col, 0, stroke * 1.8f);
            draw->AddCircle(ImVec2(cx, cy), r * 0.86f, ui::with_alpha(col, 0.35f), 0, stroke);
            draw->AddCircleFilled(ImVec2(cx, cy), r * 0.2f, col);
            for (int k = 0; k < 3; ++k) {
                const float t = a + static_cast<float>(k) * 2.0944f + 1.5708f;
                draw->AddLine(ImVec2(cx, cy), ImVec2(cx + std::cos(t) * r, cy + std::sin(t) * r), col, stroke * 1.2f);
            }
            break;
        }
        case Kind::Handlebar: {
            const float a = c.x * 0.5f;
            const ImVec2 l(cx - std::cos(a) * r, cy - std::sin(a) * r);
            const ImVec2 rr(cx + std::cos(a) * r, cy + std::sin(a) * r);
            draw->AddLine(l, rr, col, stroke * 1.6f);
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + r * 0.8f), col, stroke);
            draw->AddCircleFilled(l, r * 0.14f, col);
            draw->AddCircleFilled(rr, r * 0.14f, col);
            break;
        }
        case Kind::Joystick:
        case Kind::Stick: {
            draw->AddCircleFilled(ImVec2(cx, cy), r, ui::with_alpha(fill, 0.6f));
            draw->AddCircle(ImVec2(cx, cy), r, chosen ? pal.accent : pal.edge, 0, stroke);
            const float kx = cx + c.x * r * 0.5f, ky = cy + c.y * r * 0.5f;
            draw->AddLine(ImVec2(cx, cy), ImVec2(kx, ky), col, stroke * 1.6f);
            draw->AddCircleFilled(ImVec2(kx, ky), r * 0.32f, c.lit ? lit : fill);
            draw->AddCircle(ImVec2(kx, ky), r * 0.32f, col, 0, stroke);
            break;
        }
        case Kind::Gun: {
            // A pistol in profile: grip, body, barrel; the sight where it aims.
            const float u = r / 10.0f;
            const ImVec2 body[] = {ImVec2(cx - 9 * u, cy - 3 * u), ImVec2(cx + 10 * u, cy - 3 * u), ImVec2(cx + 10 * u, cy + 1 * u),
                                   ImVec2(cx - 1 * u, cy + 1 * u), ImVec2(cx - 3 * u, cy + 9 * u), ImVec2(cx - 8 * u, cy + 9 * u),
                                   ImVec2(cx - 6 * u, cy + 1 * u), ImVec2(cx - 9 * u, cy + 1 * u)};
            draw->AddConvexPolyFilled(body, 4, ui::with_alpha(fill, 0.8f));
            draw->AddPolyline(body, 8, col, ImDrawFlags_Closed, stroke);
            draw->AddLine(ImVec2(cx - 2 * u, cy + 2 * u), ImVec2(cx - 1 * u, cy + 4 * u), col, stroke);  // the trigger
            const float ax = cx + 3 * u + c.x * 6 * u, ay = cy - 7 * u + c.y * 3 * u;
            draw->AddCircle(ImVec2(ax, ay), 2.4f * u, col, 0, stroke);
            draw->AddLine(ImVec2(ax - 3.6f * u, ay), ImVec2(ax + 3.6f * u, ay), col, stroke);
            draw->AddLine(ImVec2(ax, ay - 3.6f * u), ImVec2(ax, ay + 3.6f * u), col, stroke);
            break;
        }
        case Kind::Board: {
            const float a = c.x * 0.35f;
            const ImVec2 l(cx - std::cos(a) * r, cy - std::sin(a) * r * 0.5f);
            const ImVec2 rr(cx + std::cos(a) * r, cy + std::sin(a) * r * 0.5f);
            draw->AddLine(l, rr, col, stroke * 3.5f);
            draw->AddCircleFilled(ImVec2(cx - r * 0.45f, cy - r * 0.2f), r * 0.1f, col);
            draw->AddCircleFilled(ImVec2(cx + r * 0.45f, cy + r * 0.2f), r * 0.1f, col);
            break;
        }
        case Kind::Rod: {
            const float a = -0.6f + c.y * 0.4f;
            const ImVec2 base(cx - r * 0.6f, cy + r * 0.7f);
            draw->AddLine(base, ImVec2(base.x + std::cos(a) * r * 1.5f, base.y + std::sin(a) * r * 1.5f), col, stroke * 1.6f);
            draw->AddCircle(ImVec2(cx - r * 0.3f, cy + r * 0.5f), r * 0.18f, col, 0, stroke);
            break;
        }
        case Kind::Bat: {
            const float a = -1.2f + c.x * 1.6f;  // the swing, from the shoulder round
            draw->AddLine(ImVec2(cx, cy + r * 0.8f), ImVec2(cx + std::cos(a) * r * 1.6f, cy + r * 0.8f + std::sin(a) * r * 1.6f), col, stroke * 2.8f);
            break;
        }
        case Kind::Pedal: {
            const float pw = r * 1.0f, ph = r * 1.7f;
            const ImVec2 a(cx - pw * 0.5f, cy - ph * 0.5f), b(cx + pw * 0.5f, cy + ph * 0.5f);
            draw->AddRectFilled(a, b, fill, kit.px(3.0f));
            draw->AddRectFilled(ImVec2(a.x, b.y - ph * std::clamp(c.x, 0.0f, 1.0f)), b, ui::with_alpha(lit, 0.7f), kit.px(3.0f));
            draw->AddRect(a, b, col, kit.px(3.0f), 0, stroke);
            for (int k = 1; k < 4; ++k) {
                const float y = a.y + ph * static_cast<float>(k) / 4.0f;
                draw->AddLine(ImVec2(a.x + pw * 0.2f, y), ImVec2(b.x - pw * 0.2f, y), ui::with_alpha(col, 0.35f), stroke);
            }
            break;
        }
        case Kind::Shift: {
            const bool up = c.buttons[2] >= 0, down = c.buttons[3] >= 0;
            const float h = r * 0.7f;
            const ImVec2 u[] = {ImVec2(cx, cy - h * 1.4f), ImVec2(cx - h * 0.75f, cy - h * 0.2f), ImVec2(cx + h * 0.75f, cy - h * 0.2f)};
            const ImVec2 d[] = {ImVec2(cx, cy + h * 1.4f), ImVec2(cx - h * 0.75f, cy + h * 0.2f), ImVec2(cx + h * 0.75f, cy + h * 0.2f)};
            if (up) draw->AddTriangleFilled(u[0], u[1], u[2], lit); else draw->AddTriangle(u[0], u[1], u[2], ink, stroke);
            if (down) draw->AddTriangleFilled(d[0], d[1], d[2], lit); else draw->AddTriangle(d[0], d[1], d[2], ink, stroke);
            break;
        }
        case Kind::Gate: {
            const float gw = r * 0.8f, gh = r * 0.9f;
            draw->AddRectFilled(ImVec2(cx - gw * 1.5f, cy - gh * 1.3f), ImVec2(cx + gw * 1.5f, cy + gh * 1.3f), ui::with_alpha(fill, 0.6f), kit.px(4.0f));
            draw->AddLine(ImVec2(cx - gw, cy - gh), ImVec2(cx - gw, cy + gh), ink, stroke * 1.4f);
            draw->AddLine(ImVec2(cx + gw, cy - gh), ImVec2(cx + gw, cy + gh), ink, stroke * 1.4f);
            draw->AddLine(ImVec2(cx - gw, cy), ImVec2(cx + gw, cy), ink, stroke * 1.4f);
            const float ky = c.buttons[2] >= 0 ? cy - gh * 0.8f : c.buttons[3] >= 0 ? cy + gh * 0.8f : cy;
            draw->AddCircleFilled(ImVec2(cx, ky), r * 0.24f, c.lit ? lit : fill);
            draw->AddCircle(ImVec2(cx, ky), r * 0.24f, col, 0, stroke);
            break;
        }
        case Kind::Coin: {
            // The coin door's plate with its slot; a coin above it, dropped
            // in while pressed.
            const float pw = r * 1.7f, ph = r * 1.5f;
            const ImVec2 a(cx - pw * 0.5f, cy - ph * 0.15f), b(cx + pw * 0.5f, cy + ph * 0.85f);
            draw->AddRectFilled(a, b, fill, kit.px(3.0f));
            draw->AddRect(a, b, col, kit.px(3.0f), 0, stroke);
            const float sh = ph * 0.55f;
            draw->AddRectFilled(ImVec2(cx - kit.px(2.0f), cy + ph * 0.35f - sh * 0.5f), ImVec2(cx + kit.px(2.0f), cy + ph * 0.35f + sh * 0.5f),
                                c.lit ? lit : ui::with_alpha(col, 0.8f), kit.px(1.0f));
            const float coin_r = r * 0.42f;
            const float coin_y = c.lit ? cy + ph * 0.05f : cy - ph * 0.5f;
            draw->AddCircleFilled(ImVec2(cx, coin_y), coin_r, c.lit ? lit : fill);
            draw->AddCircle(ImVec2(cx, coin_y), coin_r, col, 0, stroke);
            draw->AddCircle(ImVec2(cx, coin_y), coin_r * 0.6f, ui::with_alpha(col, 0.5f), 0, stroke * 0.7f);
            break;
        }
        case Kind::Button: {
            const float br = c.lit ? r * 0.9f : r;
            draw->AddCircleFilled(ImVec2(cx, cy + kit.px(2.0f)), r, ui::with_alpha(pal.backdrop, 0.6f));  // the shadow
            draw->AddCircleFilled(ImVec2(cx, cy), br, c.lit ? lit : fill);
            draw->AddCircle(ImVec2(cx, cy), br, col, 0, stroke);
            if (generic_direction(c.name)) {
                // An arrow the way it moves.
                const float dx = std::strcmp(c.name, "Move Left") == 0 ? -1.0f : std::strcmp(c.name, "Move Right") == 0 ? 1.0f : 0.0f;
                const float dy = std::strcmp(c.name, "Move Up") == 0 ? -1.0f : std::strcmp(c.name, "Move Down") == 0 ? 1.0f : 0.0f;
                const float l  = br * 0.5f, w = br * 0.32f;
                const ImVec2 tip(cx + dx * l, cy + dy * l);
                const ImVec2 base(cx - dx * l * 0.6f, cy - dy * l * 0.6f);
                const ImU32  ink_col = c.lit ? pal.on_accent : col;
                draw->AddLine(base, tip, ink_col, stroke * 1.4f);
                draw->AddTriangleFilled(tip, ImVec2(tip.x - dx * w + dy * w, tip.y - dy * w + dx * w),
                                        ImVec2(tip.x - dx * w - dy * w, tip.y - dy * w - dx * w), ink_col);
            }
            break;
        }
    }
}

/// The panel laid out like the cabinet's: the main control under the hands
/// at the left, pedals and shifter beside it, the buttons in rows at the
/// right and Start past them; Virtual On's two levers either side. Returns
/// the cells, drawn with their names and `texts` (what the pad has on each).
std::vector<PanelCell> draw_cabinet_panel(ui::Kit& kit, std::vector<CabinetControl>& controls,
                                          const std::vector<std::string>& texts, int chosen, ImVec2 at, float width,
                                          float* height_out)
{
    using Kind = CabinetControl::Kind;
    const ui::Palette& pal  = kit.palette();
    ImDrawList*        draw = ImGui::GetWindowDrawList();
    const float        s    = kit.px(1.0f);
    std::vector<PanelCell> cells;

    const auto is_main = [](Kind k) {
        return k == Kind::Wheel || k == Kind::Handlebar || k == Kind::Joystick || k == Kind::Stick || k == Kind::Gun
            || k == Kind::Board || k == Kind::Rod || k == Kind::Bat;
    };
    // The groups.
    std::vector<int> mains, pedals, shifters, levers, buttons;
    int              start = -1, coin = -1;
    for (int i = 0; i < static_cast<int>(controls.size()); ++i) {
        const CabinetControl& c = controls[static_cast<size_t>(i)];
        if (c.kind == Kind::Button && std::strcmp(c.name, "Start") == 0) start = i;
        else if (c.kind == Kind::Coin) coin = i;
        else if (c.kind == Kind::Button) buttons.push_back(i);
        else if (c.kind == Kind::Pedal) pedals.push_back(i);
        else if (c.kind == Kind::Shift || c.kind == Kind::Gate) shifters.push_back(i);
        else if (is_main(c.kind) && (mains.empty() || c.kind == Kind::Joystick)) mains.push_back(i);
        else levers.push_back(i);
    }
    const bool twin = mains.size() == 2;

    // Zones left to right, each a width in points.
    struct Zone {
        enum class What { Main, Pedals, Shifter, Levers, Buttons, Start, Coin } what;
        std::vector<int> items;
        float            w;
        int              cols = 1;
    };
    std::vector<Zone> zones;
    const float label_h = kit.small() * 3.0f + 6.0f * s;
    if (twin) {
        // Left lever, its buttons, Start, the right buttons, the right lever.
        std::vector<int> left, right;
        for (const int b : buttons) {
            (std::strncmp(controls[static_cast<size_t>(b)].name, "Left", 4) == 0 ? left : right).push_back(b);
        }
        zones.push_back({Zone::What::Main, {mains[0]}, 150.0f * s});
        if (!left.empty()) zones.push_back({Zone::What::Buttons, left, 72.0f * s, 1});
        if (start >= 0) zones.push_back({Zone::What::Start, {start}, 70.0f * s});
        if (coin >= 0) zones.push_back({Zone::What::Coin, {coin}, 60.0f * s});
        if (!right.empty()) zones.push_back({Zone::What::Buttons, right, 72.0f * s, 1});
        zones.push_back({Zone::What::Main, {mains[1]}, 150.0f * s});
    } else {
        if (!mains.empty()) zones.push_back({Zone::What::Main, {mains[0]}, 170.0f * s});
        if (!levers.empty()) zones.push_back({Zone::What::Levers, levers, static_cast<float>(levers.size()) * 110.0f * s});
        if (!pedals.empty()) zones.push_back({Zone::What::Pedals, pedals, static_cast<float>(pedals.size()) * 54.0f * s + 10.0f * s});
        if (!shifters.empty()) zones.push_back({Zone::What::Shifter, shifters, 76.0f * s});
        if (!buttons.empty()) {
            const int n    = static_cast<int>(buttons.size());
            const int cols = n <= 3 ? n : (n + 1) / 2;
            zones.push_back({Zone::What::Buttons, buttons, static_cast<float>(cols) * 70.0f * s + 8.0f * s, cols});
        }
        if (start >= 0) zones.push_back({Zone::What::Start, {start}, 70.0f * s});
        if (coin >= 0) zones.push_back({Zone::What::Coin, {coin}, 60.0f * s});
    }
    const float gap   = 16.0f * s;
    float       total = gap;
    for (const Zone& z : zones) total += z.w + gap;
    const float k = std::min(1.0f, (width - 8.0f * s) / std::max(total, 1.0f));
    const float body_h = 150.0f * s * std::max(k, 0.75f);
    const float panel_h = body_h + label_h + 18.0f * s;
    const float x0 = at.x + std::floor((width - total * k) * 0.5f);
    const ImVec2 pa(x0, at.y), pb(x0 + total * k, at.y + panel_h);
    // The plate.
    draw->AddRectFilled(pa, pb, ui::mix(pal.panel, pal.backdrop, 0.45f), 10.0f * s);
    draw->AddRect(pa, pb, pal.edge, 10.0f * s, 0, kit.stroke());
    draw->AddLine(ImVec2(pa.x + 14.0f * s, pa.y + 10.0f * s), ImVec2(pb.x - 14.0f * s, pa.y + 10.0f * s),
                  ui::with_alpha(pal.edge, 0.6f), kit.stroke());
    if (height_out != nullptr) *height_out = panel_h;

    const float cy = pa.y + 14.0f * s + body_h * 0.5f;  // the middle of the control row
    const float label_y = pa.y + 14.0f * s + body_h + 2.0f * s;
    // A control placed: its cell, its label under the row.
    const auto place = [&](int i, ImVec2 centre, float r, ImVec2 a, ImVec2 b, bool stacked_label, float ly) {
        const CabinetControl& c = controls[static_cast<size_t>(i)];
        // Draw, then drop the drawing so it sits just above its label: the
        // glyph's own extent is measured from the vertices it added.
        const int v0 = draw->VtxBuffer.Size;
        draw_control_glyph(kit, c, centre, r, chosen == i);
        if (!stacked_label) {
            float low = -FLT_MAX;
            for (int v = v0; v < draw->VtxBuffer.Size; ++v) low = std::max(low, draw->VtxBuffer[v].pos.y);
            const float dy = std::floor(label_y - 6.0f * s - low);
            if (dy > 0.0f) {
                for (int v = v0; v < draw->VtxBuffer.Size; ++v) draw->VtxBuffer[v].pos.y += dy;
                centre.y += dy;
            }
        }
        PanelCell cell{i, a, b, centre, r};
        cells.push_back(cell);
        const char*  name = tr(c.name);
        const float  w    = b.x - a.x;
        const float  nw   = std::min(w, ImGui::GetFont()->CalcTextSizeA(kit.small(), FLT_MAX, w, name).x);
        const float  y    = stacked_label ? ly : label_y;
        draw->AddText(ImGui::GetFont(), kit.small(), ImVec2(std::floor(centre.x - nw * 0.5f), y),
                      chosen == i ? pal.accent : c.lit ? pal.text : pal.muted, name, nullptr, w);
        const std::string& bound = texts[static_cast<size_t>(i)];
        const float bw = std::min(w, ImGui::GetFont()->CalcTextSizeA(kit.small(), FLT_MAX, w, bound.c_str()).x);
        draw->AddText(ImGui::GetFont(), kit.small(), ImVec2(std::floor(centre.x - bw * 0.5f), y + kit.small() + 2.0f * s),
                      chosen == i ? pal.accent : pal.faint, bound.c_str(), nullptr, w);
    };

    float x = x0 + gap * k;
    for (const Zone& z : zones) {
        const float zw = z.w * k;
        const float zx = x + zw * 0.5f;
        switch (z.what) {
            case Zone::What::Main: {
                const float r = std::min(zw, body_h) * 0.5f - 10.0f * s;
                place(z.items[0], ImVec2(zx, cy), r, ImVec2(x, pa.y + 8.0f * s), ImVec2(x + zw, pb.y - 4.0f * s), false, 0.0f);
                break;
            }
            case Zone::What::Pedals: {
                const float each = zw / static_cast<float>(z.items.size());
                for (size_t n = 0; n < z.items.size(); ++n) {
                    const float px_ = x + each * (static_cast<float>(n) + 0.5f);
                    const float r   = std::min(each * 0.42f, body_h * 0.28f);
                    place(z.items[n], ImVec2(px_, cy + body_h * 0.12f), r, ImVec2(x + each * static_cast<float>(n), pa.y + 8.0f * s),
                          ImVec2(x + each * static_cast<float>(n + 1), pb.y - 4.0f * s), false, 0.0f);
                }
                break;
            }
            case Zone::What::Shifter:
            case Zone::What::Levers: {
                const float each = zw / static_cast<float>(z.items.size());
                for (size_t n = 0; n < z.items.size(); ++n) {
                    const float px_ = x + each * (static_cast<float>(n) + 0.5f);
                    const float r   = std::min(each * 0.4f, body_h * 0.3f);
                    place(z.items[n], ImVec2(px_, cy), r, ImVec2(x + each * static_cast<float>(n), pa.y + 8.0f * s),
                          ImVec2(x + each * static_cast<float>(n + 1), pb.y - 4.0f * s), false, 0.0f);
                }
                break;
            }
            case Zone::What::Buttons: {
                const int   cols = std::max(1, z.cols);
                const int   rows = (static_cast<int>(z.items.size()) + cols - 1) / cols;
                const float cw   = zw / static_cast<float>(cols);
                const float r    = std::min(cw * 0.28f, 17.0f * s * std::max(k, 0.75f));
                const float row_h = body_h / static_cast<float>(rows);
                for (size_t n = 0; n < z.items.size(); ++n) {
                    const int   row = static_cast<int>(n) / cols, col = static_cast<int>(n) % cols;
                    // The upper row sits a little higher at its ends, as a
                    // panel's buttons curve under the fingers.
                    const float arc = rows == 1 ? (col == 0 || col == cols - 1 ? 4.0f * s : 0.0f) : 0.0f;
                    const float bx  = x + cw * (static_cast<float>(col) + 0.5f);
                    const float by  = pa.y + 14.0f * s + row_h * (static_cast<float>(row) + 0.5f) - (rows == 1 ? label_h * 0.5f : row_h * 0.22f) + arc;
                    const float top = pa.y + 14.0f * s + row_h * static_cast<float>(row);
                    place(z.items[n], ImVec2(bx, by), r, ImVec2(x + cw * static_cast<float>(col), top),
                          ImVec2(x + cw * static_cast<float>(col + 1), rows == 1 ? pb.y - 4.0f * s : top + row_h),
                          rows > 1, by + r + 4.0f * s);
                }
                break;
            }
            case Zone::What::Start: {
                const float r = 13.0f * s * std::max(k, 0.75f);
                place(z.items[0], ImVec2(zx, cy), r, ImVec2(x, pa.y + 8.0f * s), ImVec2(x + zw, pb.y - 4.0f * s), false, 0.0f);
                break;
            }
            case Zone::What::Coin: {
                const float r = 15.0f * s * std::max(k, 0.75f);
                place(z.items[0], ImVec2(zx, cy), r, ImVec2(x, pa.y + 8.0f * s), ImVec2(x + zw, pb.y - 4.0f * s), false, 0.0f);
                break;
            }
        }
        x += zw + gap * k;
    }
    return cells;
}

}  // namespace

bool Gui::capturing_input() const
{
    return m_capture != Capture::None || m_pad_capture_player >= 0 || m_pad_axis_capture_player >= 0
        || m_gun_capture_player >= 0;
}

void Gui::cancel_captures()
{
    m_capture                 = Capture::None;
    m_pad_capture_player      = -1;
    m_pad_axis_capture_player = -1;
    m_gun_capture_player      = -1;
    m_map_queue.clear();
    m_map_slot = -1;
    m_map_all  = false;
}

void Gui::page_gamepad(ui::Kit& kit, Config& config, Input* input)
{
    const ui::Palette& pal  = kit.palette();
    const int          pads = input != nullptr ? static_cast<int>(input->pad_count()) : 0;
    const float        s    = m_ui_scale > 0.0f ? m_ui_scale : 1.0f;

    // Follow the running game, but keep a choice made since it started.
    if (m_current_family != m_pad_family_followed) {
        m_pad_family_followed = m_current_family;
        m_pad_family          = m_current_family;
    }
    const auto select_family = [&](const std::string& key) {
        m_pad_family = key;
        cancel_captures();
    };

    kit.begin_group(tr("Gamepad"));
    if (pads > 0) {
        std::string names;
        if (input != nullptr) {
            // One entry per player, empty where no pad drives that player.
            const std::vector<std::string> per_player = input->gamepad_names();
            for (size_t p = 0; p < per_player.size(); ++p) {
                if (per_player[p].empty()) continue;
                names += (names.empty() ? "" : "  ·  ") + i18n::trf("P%d: %s", static_cast<int>(p + 1), per_player[p].c_str());
            }
        }
        kit.status(names.empty() ? i18n::trnf("%d gamepad connected.", "%d gamepads connected.",
                                              static_cast<unsigned long>(pads), pads).c_str()
                                 : names.c_str(),
                   pal.good);
    } else {
        kit.status(tr("No gamepad connected. Settings still apply once one is."), pal.faint);
    }

    // Which game's layout.
    {
        std::vector<std::string> labels = {tr("Default")};
        std::vector<std::string> keys   = {""};
        int                      current = 0;
        for (const GameFamily& family : m_families) {
            labels.push_back(config.pad_layouts.count(family.key) != 0 ? family.title + "  ·  " + tr("custom")
                                                                        : family.title);
            keys.push_back(family.key);
            if (family.key == m_pad_family) current = static_cast<int>(keys.size()) - 1;
        }
        kit.tip(tr("Every game uses the default layout until it is given one of its own. Pick a game, "
                   "change a binding, and the game keeps the change. A game shows only the controls its "
                   "cabinet has."));
        if (kit.dropdown(tr("Layout for"), nullptr, labels, current, m_pad_family_filter, sizeof m_pad_family_filter)) {
            select_family(keys[static_cast<size_t>(current)]);
        }
    }

    const GameFamily* family = nullptr;
    for (const GameFamily& f : m_families) {
        if (f.key == m_pad_family) family = &f;
    }
    // A game without its own layout is edited as a copy of its default (the
    // default layout with the game's logical changes), kept once changed.
    Config::PadLayout game_default = config.pad;
    if (!m_pad_family.empty()) {
        Config without = config;
        without.pad_layouts.erase(m_pad_family);
        game_default = Input::game_layout(without, m_pad_family, family != nullptr ? family->inputs : 0);
    }
    Config::PadLayout  scratch;
    Config::PadLayout* binds = &config.pad;
    if (!m_pad_family.empty()) {
        const auto found = config.pad_layouts.find(m_pad_family);
        if (found != config.pad_layouts.end()) {
            binds = &found->second;
        } else {
            scratch = game_default;
            binds   = &scratch;
        }
    }

    int&       edit = m_pad_edit_player;
    const auto p    = static_cast<usize>(edit);
    {
        static constexpr std::array<const char*, 2> kPlayers = {"1", "2"};
        if (kit.choice(tr("Player"), m_pad_family.empty() ? tr("Used by every game without its own layout.")
                                     : binds == &scratch  ? tr("Uses the default layout until a binding is changed.")
                                                          : tr("This game has its own layout."),
                       kPlayers, edit)) {
            cancel_captures();
        }
    }
    {
        static constexpr std::array<const char*, 3> kViews = {N_("Gamepad"), N_("Cabinet"), N_("Shortcuts")};
        kit.tip(tr("Gamepad shows the pad with every binding. Cabinet shows the game's controls as the "
                   "cabinet has them, each with what the pad does for it. Shortcuts lists what Guide held "
                   "with another button does."));
        kit.choice(tr("View"), nullptr, tr_all(kViews), m_pad_view);
    }

    // -- the cabinet's controls, and what the pad holds now ------------------
    CabinetPanel panel = cabinet_panel(family);
    std::array<s16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    if (input != nullptr) {
        input->pad_axis_baseline(static_cast<u32>(edit), axes.data());
    }
    // Which slots the page shows: the cabinet's, plus any the user bound
    // anyway, plus all of them when asked.
    std::array<bool, std::size(kSlots)> shown_slot{};
    int hidden = 0;
    for (size_t si = 0; si < std::size(kSlots); ++si) {
        const Slot& slot   = kSlots[si];
        bool        custom = false;
        if (slot.kind == SlotKind::Button) {
            custom = binds->buttons[p][slot.role] != game_default.buttons[p][slot.role];
        } else if (slot.kind == SlotKind::Stick) {
            for (const u32 a : {slot.role, slot.role + 1}) custom = custom || binds->axes[p][a] != game_default.axes[p][a];
        } else if (slot.kind == SlotKind::Axis) {
            custom = binds->axes[p][slot.role] != game_default.axes[p][slot.role]
                  || binds->axis_invert[p][slot.role] != game_default.axis_invert[p][slot.role];
        } else {
            custom = binds->axes[p][slot.role] != game_default.axes[p][slot.role]
                  || binds->axis_buttons[p][slot.role] != game_default.axis_buttons[p][slot.role];
        }
        const bool unused = panel.slot_names[si] == nullptr;
        shown_slot[si]    = !unused || custom || m_pad_show_all;
        if (unused && !shown_slot[si]) ++hidden;
    }
    // What the pad has on a slot, as text.
    const auto slot_text = [&](size_t si) -> std::string {
        const Slot& slot = kSlots[si];
        if (slot.kind == SlotKind::Button) {
            const s32 bound = binds->buttons[p][slot.role];
            return bound < 0 ? std::string(tr("unbound")) : Input::pad_button_name(bound);
        }
        const auto role  = static_cast<Config::PadAxisRole>(slot.role);
        const s32  bound = binds->axes[p][slot.role];
        const s32  shown = bound >= 0 ? bound : default_pad_axis(role);
        if (bound == Config::kPadAxisNone) {
            const s32 button = binds->axis_buttons[p][slot.role];
            return button >= 0 ? Input::pad_button_name(button) : std::string(tr("unbound"));
        }
        if (slot.kind == SlotKind::Stick) {
            return shown == SDL_GAMEPAD_AXIS_RIGHTX || shown == SDL_GAMEPAD_AXIS_RIGHTY ? tr("Right Stick")
                 : shown == SDL_GAMEPAD_AXIS_LEFTX || shown == SDL_GAMEPAD_AXIS_LEFTY  ? tr("Left Stick")
                                                                                       : Input::pad_axis_name(shown);
        }
        if (const s32 button = binds->axis_buttons[p][slot.role]; button >= 0) {
            return Input::pad_button_name(button);
        }
        std::string value = Input::pad_axis_name(shown);
        if (binds->axis_invert[p][slot.role]) value = trf("%s (inverted)", value.c_str());
        return value;
    };
    // What the pad has on a control: its slots' texts, the alike ones merged.
    const auto control_text = [&](const CabinetControl& c) -> std::string {
        using Kind = CabinetControl::Kind;
        std::vector<std::string> parts;
        const auto add = [&](const std::string& t) {
            if (!t.empty() && std::find(parts.begin(), parts.end(), t) == parts.end()) parts.push_back(t);
        };
        if (directions_merged(c)) {
            // The four directions: one name when they share a device.
            std::vector<std::string> dirs;
            for (const s32 b : c.buttons) {
                if (const int si = slot_of_button(b); si >= 0) dirs.push_back(slot_text(static_cast<size_t>(si)));
            }
            const bool dpad = std::all_of(dirs.begin(), dirs.end(), [](const std::string& d) { return d.rfind("D-Pad", 0) == 0; });
            const bool face = dirs.size() == 4 && std::all_of(c.buttons.begin(), c.buttons.end(), [&](s32 b) {
                const int si = slot_of_button(b);
                if (si < 0) return false;
                const s32 bound = binds->buttons[p][kSlots[static_cast<size_t>(si)].role];
                return bound >= SDL_GAMEPAD_BUTTON_SOUTH && bound <= SDL_GAMEPAD_BUTTON_NORTH;
            });
            if (dpad && !dirs.empty()) add(tr("D-Pad"));
            else if (face) add(tr("Face buttons"));
            else for (const std::string& d : dirs) add(d);
            if (c.axes[0] >= 0) {
                if (const int si = slot_of_axis(c.axes[0]); si >= 0) add(slot_text(static_cast<size_t>(si)));
            }
        } else {
            for (const s32 b : c.buttons) {
                if (b >= 0 && c.kind != Kind::Shift && c.kind != Kind::Gate) {
                    if (const int si = slot_of_button(b); si >= 0) add(slot_text(static_cast<size_t>(si)));
                }
            }
            if (c.kind == Kind::Shift || c.kind == Kind::Gate) {
                for (int n = 0; n < 2; ++n) {
                    if (const int si = slot_of_button(c.buttons[n]); si >= 0) add(slot_text(static_cast<size_t>(si)));
                }
            }
            if (const s32 a = c.axes[0] >= 0 ? c.axes[0] : c.axes[1]; a >= 0) {
                if (const int si = slot_of_axis(a); si >= 0) add(slot_text(static_cast<size_t>(si)));
            }
        }
        std::string out;
        for (const std::string& part : parts) out += (out.empty() ? "" : " · ") + part;
        return out;
    };

    // An axis role's reading now, -1..1 for a stick and 0..1 for a pedal.
    const auto axis_value = [&](s32 role) -> float {
        if (role < 0) return 0.0f;
        const auto r     = static_cast<u32>(role);
        const s32  bound = binds->axes[p][r];
        const s32  which = bound >= 0 ? bound : bound == Config::kPadAxisNone ? -1 : default_pad_axis(static_cast<Config::PadAxisRole>(r));
        float v = 0.0f;
        if (which >= 0 && which < SDL_GAMEPAD_AXIS_COUNT) v = static_cast<float>(axes[static_cast<usize>(which)]) / 32767.0f;
        if (binds->axis_invert[p][r]) v = -v;
        const bool trigger = which == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || which == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        const bool pedal   = r == static_cast<u32>(Config::PadAxisRole::Accel) || r == static_cast<u32>(Config::PadAxisRole::Brake);
        if (!trigger && pedal) v = (v + 1.0f) * 0.5f;
        if (const s32 button = binds->axis_buttons[p][r]; button >= 0 && binding_held_now(input, static_cast<u32>(edit), button, axes)) {
            v = 1.0f;
        }
        return std::clamp(v, -1.0f, 1.0f);
    };
    const auto button_held = [&](s32 role) {
        return role >= 0 && binding_held_now(input, static_cast<u32>(edit), binds->buttons[p][static_cast<u32>(role)], axes);
    };
    for (CabinetControl& c : panel.controls) {
        using Kind = CabinetControl::Kind;
        switch (c.kind) {
            case Kind::Joystick:
            default:
                c.x = axis_value(c.axes[0]);
                c.y = axis_value(c.axes[1]);
                if (directions_merged(c)) {
                    if (button_held(c.buttons[0])) c.y = -1.0f;
                    if (button_held(c.buttons[1])) c.y = 1.0f;
                    if (button_held(c.buttons[2])) c.x = -1.0f;
                    if (button_held(c.buttons[3])) c.x = 1.0f;
                }
                c.lit = std::abs(c.x) > 0.5f || std::abs(c.y) > 0.5f;
                break;
            case Kind::Pedal:
                c.x   = axis_value(c.axes[0]);
                c.lit = c.x > 0.5f;
                break;
            case Kind::Shift:
            case Kind::Gate: {
                const bool up = button_held(c.buttons[0]), down = button_held(c.buttons[1]);
                c.buttons[2] = up ? 1 : -1;
                c.buttons[3] = down ? 1 : -1;
                c.lit        = up || down;
                break;
            }
            case Kind::Button:
            case Kind::Coin:
                c.lit = (c.axes[0] >= 0 && axis_value(c.axes[0]) > 0.5f)
                     || std::any_of(c.buttons.begin(), c.buttons.end(), button_held);
                break;
        }
    }

    // -- capturing: one slot at a time, from a click or from Map all ---------
    const auto start_slot = [&](u32 si) {
        const Slot& slot = kSlots[si];
        m_map_slot       = static_cast<int>(si);
        if (slot.kind == SlotKind::Button) {
            m_pad_axis_capture_player = -1;
            m_pad_capture_player      = edit;
            m_pad_capture_role        = slot.role;
        } else {
            m_pad_capture_player      = -1;
            m_pad_axis_capture_player = edit;
            m_pad_axis_capture_role   = slot.role;
        }
        m_map_seconds = 0.0f;
        if (input != nullptr) input->pad_axis_baseline(static_cast<u32>(edit), m_pad_axis_baseline.data());
    };
    const auto start_steps = [&](std::vector<u32> steps, bool all) {
        cancel_captures();
        if (steps.empty()) return;
        m_map_queue = std::move(steps);
        m_map_at    = 0;
        m_map_all   = all;
        start_slot(m_map_queue[0]);
    };
    const auto advance = [&]() {
        m_pad_capture_player      = -1;
        m_pad_axis_capture_player = -1;
        if (++m_map_at < m_map_queue.size()) {
            start_slot(m_map_queue[m_map_at]);
        } else {
            m_map_queue.clear();
            m_map_slot = -1;
            m_map_all  = false;
        }
    };
    // The slot being captured now, if its capture is still the page's.
    const bool capturing_here = m_map_slot >= 0 && (m_pad_capture_player == edit || m_pad_axis_capture_player == edit);
    if (m_map_slot >= 0 && !capturing_here) {
        m_map_queue.clear();
        m_map_slot = -1;
        m_map_all  = false;
    }
    if (capturing_here) {
        const Slot& slot = kSlots[static_cast<size_t>(m_map_slot)];
        m_map_seconds += ImGui::GetIO().DeltaTime;
        bool done = false;
        if (input != nullptr) {
            if (slot.kind == SlotKind::Stick) {
                // Either axis of a stick binds the whole stick.
                bool      positive = true;
                const s32 got      = input->captured_pad_axis(static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                const bool right = got == SDL_GAMEPAD_AXIS_RIGHTX || got == SDL_GAMEPAD_AXIS_RIGHTY;
                const bool left  = got == SDL_GAMEPAD_AXIS_LEFTX || got == SDL_GAMEPAD_AXIS_LEFTY;
                if (left || right) {
                    const u32 x = slot.role, y = slot.role + 1;
                    binds->axes[p][x]        = right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
                    binds->axes[p][y]        = right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
                    binds->axis_invert[p][x] = false;
                    binds->axis_invert[p][y] = false;
                    done = true;
                }
            } else if (slot.kind == SlotKind::Axis) {
                // One stick axis. The way it is moved at capture is the way
                // that reads "up": pull the stick back for a turret that
                // rises as the yoke comes back.
                bool      positive = true;
                const s32 got      = input->captured_pad_axis(static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                if (got >= SDL_GAMEPAD_AXIS_LEFTX && got <= SDL_GAMEPAD_AXIS_RIGHTY) {
                    binds->axes[p][slot.role]        = got;
                    binds->axis_invert[p][slot.role] = positive;
                    done = true;
                }
            } else if (slot.kind == SlotKind::Pedal) {
                bool      positive = true;
                const s32 got      = input->captured_pad_axis(static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                const s32 button   = input->pressed_pad_button(static_cast<u32>(edit));
                if (got >= 0) {
                    binds->axes[p][slot.role]         = got;
                    binds->axis_invert[p][slot.role]  = !positive;
                    binds->axis_buttons[p][slot.role] = -1;
                    done = true;
                } else if (button >= 0) {
                    binds->axis_buttons[p][slot.role] = button;
                    done = true;
                }
            } else {
                s32 got = input->pressed_pad_button(static_cast<u32>(edit));
                if (got < 0) {
                    bool      positive = true;
                    const s32 axis     = input->captured_pad_axis(static_cast<u32>(edit), m_pad_axis_baseline.data(), &positive);
                    if (axis >= 0) got = (positive ? Config::kPadAxisPlus : Config::kPadAxisMinus) + axis;
                }
                if (got >= 0) {
                    binds->buttons[p][slot.role] = got;
                    done = true;
                }
            }
        }
        // Map all moves on by itself when nothing comes.
        if (done || (m_map_all && m_map_seconds >= kMapSeconds)) {
            advance();
        }
    }
    const auto capturing_slot = [&](size_t si) { return m_map_slot == static_cast<int>(si) && capturing_here; };
    const auto slot_clear = [&](const Slot& slot) {
        if (slot.kind == SlotKind::Stick) {
            for (const u32 axis : {slot.role, slot.role + 1}) {
                binds->axes[p][axis]        = -1;
                binds->axis_invert[p][axis] = false;
            }
        } else if (slot.kind == SlotKind::Pedal || slot.kind == SlotKind::Axis) {
            binds->axes[p][slot.role]         = -1;
            binds->axis_invert[p][slot.role]  = false;
            binds->axis_buttons[p][slot.role] = -1;
        } else {
            binds->buttons[p][slot.role] = -1;
        }
    };

    {
        const char* reset  = m_pad_family.empty() ? tr("Reset to SDL defaults") : tr("Use default layout");
        const char* unused = m_pad_show_all ? tr("Hide unused") : tr("Show unused");
        const std::array<const char*, 3> labels  = {tr("Map all"), reset, unused};
        const std::array<bool, 3>        enabled = {m_map_slot < 0, m_pad_family.empty() || binds != &scratch, !m_pad_family.empty()};
        kit.tip(tr("Map all asks for each control of the cabinet in turn: press the pad input for it, "
                   "or wait and it moves on. Show unused adds the slots this cabinet has no control for, "
                   "to bind them anyway."));
        const int clicked = kit.actions(tr("Layout"), nullptr, labels, enabled, ui::Kit::Tone::Primary);
        if (clicked == 0) {
            std::vector<u32> steps;
            for (const CabinetControl& c : panel.controls) {
                for (const u32 si : control_slots(c)) {
                    if (shown_slot[si] && std::find(steps.begin(), steps.end(), si) == steps.end()) steps.push_back(si);
                }
            }
            start_steps(std::move(steps), true);
        } else if (clicked == 1) {
            if (m_pad_family.empty()) {
                const Config::PadLayout defaults;
                binds->buttons[p]      = defaults.buttons[p];
                binds->axes[p]         = defaults.axes[p];
                binds->axis_invert[p]  = defaults.axis_invert[p];
                binds->axis_buttons[p] = defaults.axis_buttons[p];
            } else {
                config.pad_layouts.erase(m_pad_family);
                scratch = game_default;
                binds   = &scratch;
            }
            cancel_captures();
        } else if (clicked == 2) {
            m_pad_show_all = !m_pad_show_all;
        }
    }

    // -- the picture: the cabinet, the pad or the shortcuts -------------------
    kit.divider(true);
    ImDrawList*  draw  = ImGui::GetWindowDrawList();
    const float  padx  = kit.px(12.0f);
    const ImVec2 top   = ImGui::GetCursorScreenPos();
    const float  width = ImGui::GetContentRegionAvail().x;
    const float  avail = width - padx * 2.0f;
    bool         first_item = true;
    // The control whose slot is being captured, for the cabinet.
    int chosen_control = -1;
    if (capturing_here) {
        for (size_t i = 0; i < panel.controls.size(); ++i) {
            const std::vector<u32> slots = control_slots(panel.controls[i]);
            if (std::find(slots.begin(), slots.end(), static_cast<u32>(m_map_slot)) != slots.end()) chosen_control = static_cast<int>(i);
        }
    }

    if (m_pad_view == kViewCabinet) {
        // The cabinet, laid out as its panel; a click or Enter on a control
        // binds it, Delete or a right click clears it.
        std::vector<std::string> texts;
        for (const CabinetControl& c : panel.controls) texts.push_back(control_text(c));
        // Unused slots the user asked to see, as plain buttons after the rest.
        if (m_pad_show_all) {
            for (size_t si = 0; si < std::size(kSlots); ++si) {
                if (panel.slot_names[si] != nullptr || !shown_slot[si]) continue;
                CabinetControl extra{CabinetControl::Kind::Button, kSlots[si].name};
                if (kSlots[si].kind == SlotKind::Button) extra.buttons[0] = static_cast<s32>(kSlots[si].role);
                else extra.axes[0] = static_cast<s32>(kSlots[si].role);
                panel.controls.push_back(extra);
                texts.push_back(slot_text(si));
            }
        }
        float panel_h = 0.0f;
        const std::vector<PanelCell> cells = draw_cabinet_panel(kit, panel.controls, texts, chosen_control,
                                                                ImVec2(top.x + padx, top.y + kit.px(4.0f)), avail, &panel_h);
        for (const PanelCell& cell : cells) {
            const CabinetControl& c     = panel.controls[static_cast<size_t>(cell.control)];
            const std::vector<u32> slots = control_slots(c);
            ImGui::PushID(cell.control);
            ImGui::SetCursorScreenPos(cell.a);
            const bool pressed = ImGui::InvisibleButton("control", ImVec2(cell.b.x - cell.a.x, cell.b.y - cell.a.y),
                                                        ImGuiButtonFlags_EnableNav);
            if (first_item) {
                kit.focus_if_asked();
                first_item = false;
            }
            kit.nav_item();
            const bool hovered = ImGui::IsItemHovered();
            const bool focused = ImGui::IsItemFocused();
            ImGui::PopID();
            if (hovered || (focused && GImGui->NavCursorVisible)) {
                draw->AddRect(cell.a, cell.b, hovered ? pal.hover : ui::with_alpha(pal.accent, 0.0f), kit.px(6.0f), 0, kit.stroke());
                if (focused && GImGui->NavCursorVisible) kit.focus_ring(draw, cell.a, cell.b, kit.px(6.0f));
                if (hovered) {
                    std::string tip = slots.size() > 1 ? tr("Click, then press each of its inputs in turn. Right-click or Delete clears them.")
                                                       : tr("Click, then press the pad input for it. Right-click or Delete clears it.");
                    tooltip(tip.c_str());
                }
            }
            if (chosen_control == cell.control) {
                draw->AddRect(cell.a, cell.b, pal.accent, kit.px(6.0f), 0, kit.px(2.0f));
            }
            if (pressed && !slots.empty()) {
                if (chosen_control == cell.control && hovered) {
                    cancel_captures();
                } else {
                    start_steps(slots, false);
                }
            }
            const bool clear = ImGui::IsItemClicked(ImGuiMouseButton_Right)
                            || (focused && chosen_control != cell.control
                                && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)
                                    || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)));
            if (clear) {
                for (const u32 si : slots) slot_clear(kSlots[si]);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + panel_h + kit.px(10.0f)));
        ImGui::Dummy(ImVec2(width, 0.0f));
    } else {
        // The pad, with a slot for every control the cabinet has around it.
        const PadArtLayout& layout = kPadArt;
        const PadArt*       art    = pad_art();
        const ImVec2        origin(top.x + padx, top.y);
        const float         slot_w = std::min(196.0f * s, std::floor(avail * 0.24f));
        const float         slot_h = 28.0f * s;
        const float         gap    = 22.0f * s;
        const float         img_w  = std::clamp(avail - 2.0f * (slot_w + gap), 220.0f * s, 620.0f * s);
        const float         img_h  = img_w * (art != nullptr ? art->h / art->w : 0.65f);
        const ImVec2        img0(std::floor(origin.x + (avail - img_w) * 0.5f), origin.y + 36.0f * s);
        const ImVec2        img1(img0.x + img_w, img0.y + img_h);
        if (art != nullptr) {
            draw->AddImage(reinterpret_cast<ImTextureID>(m_backend->texture_imgui_id(art->handle)), img0, img1,
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ui::with_alpha(pal.text, 0.9f));
        } else {
            draw->AddRect(img0, img1, pal.edge, 12.0f * s);
        }
        const auto at = [&](ImVec2 f) { return ImVec2(img0.x + f.x * img_w, img0.y + f.y * img_h); };
        // Light every button that is down.
        if (input != nullptr) {
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                const ImVec2 f = art_button_point(layout, b);
                if (f.x >= 0.0f && input->pad_physical_down(static_cast<u32>(edit), b)) {
                    draw->AddCircle(at(f), 10.0f * s, pal.accent, 0, 2.5f * s);
                }
            }
        }
        const float font    = ImGui::GetFontSize();
        const float pitch   = img_h / 6.0f;
        const float left_x  = origin.x;
        const float right_x = origin.x + avail - slot_w;
        const float below_y = img1.y + 20.0f * s;
        int         rows[4] = {0, 0, 0, 0};
        const auto  label   = [&](ImVec2 pos, const char* name, const std::string& value, bool unused, bool capturing) {
            const ImVec2 end(pos.x + slot_w, pos.y + slot_h);
            const float  text_y = pos.y + (slot_h - font) * 0.5f;
            draw->PushClipRect(pos, end, true);
            draw->AddText(ImVec2(pos.x + 10.0f * s, text_y), unused ? pal.faint : pal.text, name);
            const ImU32 value_col = capturing ? pal.accent : unused ? pal.faint : pal.muted;
            const float value_w   = ImGui::CalcTextSize(value.c_str()).x;
            draw->AddText(ImVec2(end.x - 10.0f * s - value_w, text_y), value_col, value.c_str());
            draw->PopClipRect();
        };
        const auto connect = [&](int column, ImVec2 anchor, ImVec2 target, ImU32 colour, bool unused) {
            if (target.x < 0.0f) return;
            const ImVec2 dot  = at(target);
            const ImU32  line = (colour & 0x00ffffffu) | (unused ? 0x40000000u : 0x80000000u);
            if (column < 2) {
                const ImVec2 edge(column == 0 ? img0.x - 8.0f * s : img1.x + 8.0f * s, anchor.y);
                draw->AddLine(anchor, edge, line, 1.5f * s);
                draw->AddLine(edge, dot, line, 1.5f * s);
            } else {
                draw->AddLine(anchor, dot, line, 1.5f * s);
            }
            draw->AddCircleFilled(dot, 4.5f * s, unused ? ui::with_alpha(colour, 0.4f) : colour);
        };

        if (m_pad_view == kViewGamepad) {
            for (size_t si = 0; si < std::size(kSlots); ++si) {
                const Slot& slot = kSlots[si];
                if (!shown_slot[si]) continue;
                const bool unused = panel.slot_names[si] == nullptr;
                const int  row    = rows[slot.column]++;
                ImVec2     pos, anchor;
                if (slot.column == 0) {
                    pos    = ImVec2(left_x, img0.y + row * pitch - slot_h * 0.5f);
                    anchor = ImVec2(pos.x + slot_w, pos.y + slot_h * 0.5f);
                } else if (slot.column == 1) {
                    pos    = ImVec2(right_x, img0.y + row * pitch - slot_h * 0.5f);
                    anchor = ImVec2(pos.x, pos.y + slot_h * 0.5f);
                } else {
                    const float stick_x = at(slot.column == 2 ? layout.lstick : layout.rstick).x;
                    const float inset   = 30.0f * s;
                    pos    = ImVec2(slot.column == 2 ? stick_x + inset - slot_w : stick_x - inset, below_y);
                    anchor = ImVec2(stick_x, pos.y);
                }
                // Where it sits on the picture, and in what colour.
                ImVec2 target = kOffArt;
                ImU32  colour = button_colour(-1);
                if (slot.kind == SlotKind::Button) {
                    const s32 bound = binds->buttons[p][slot.role];
                    target          = art_binding_point(layout, bound);
                    colour          = button_colour(bound);
                } else {
                    const s32 bound = binds->axes[p][slot.role];
                    const s32 shown = bound >= 0 ? bound : bound == Config::kPadAxisNone ? -1
                                                         : default_pad_axis(static_cast<Config::PadAxisRole>(slot.role));
                    target          = art_axis_point(layout, shown);
                    if (const s32 button = binds->axis_buttons[p][slot.role]; slot.kind == SlotKind::Pedal && button >= 0) {
                        target = art_binding_point(layout, button);
                        colour = button_colour(button);
                    }
                }
                const bool capturing = capturing_slot(si);
                ImGui::PushID(slot.name);
                ImGui::SetCursorScreenPos(pos);
                const bool pressed = ImGui::InvisibleButton("slot", ImVec2(slot_w, slot_h), ImGuiButtonFlags_EnableNav);
                if (first_item) {
                    kit.focus_if_asked();
                    first_item = false;
                }
                kit.nav_item();
                const bool hovered = ImGui::IsItemHovered();
                const bool focused = ImGui::IsItemFocused();
                ImGui::PopID();
                if (hovered) {
                    tooltip(slot.kind == SlotKind::Stick ? tr("Click, then push a stick. Right-click to go back to the default.")
                            : slot.kind == SlotKind::Axis  ? tr("Click, then move a stick the way that should read as up. Right-click for the default.")
                            : slot.kind == SlotKind::Pedal ? tr("Click, then pull the trigger, push a stick or press a button. Right-click for the default.")
                                                           : tr("Click, then press a button or pull a trigger. Right-click to unbind."));
                }
                if (pressed) {
                    if (capturing && hovered) cancel_captures();
                    else start_steps({static_cast<u32>(si)}, false);
                }
                const bool clear = ImGui::IsItemClicked(ImGuiMouseButton_Right)
                                || (focused && !capturing
                                    && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)
                                        || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)));
                if (clear) slot_clear(slot);

                const ImVec2 end(pos.x + slot_w, pos.y + slot_h);
                const ImU32  bg = capturing ? ui::with_alpha(pal.accent, 0.22f)
                                : hovered   ? ui::mix(pal.card, pal.text, 0.08f)
                                            : pal.card;
                draw->AddRectFilled(pos, end, bg, kit.px(4.0f));
                draw->AddRect(pos, end, capturing ? pal.accent : pal.edge, kit.px(4.0f), 0, kit.stroke());
                if (focused && GImGui->NavCursorVisible) kit.focus_ring(draw, pos, end, kit.px(4.0f));
                const char* name = unused ? slot.name : panel.slot_names[si];
                label(pos, tr(name), capturing ? std::string(slot.kind == SlotKind::Button ? tr("press a button...") : tr("operate it..."))
                                               : slot_text(si), unused, capturing);
                connect(slot.column, anchor, target, colour, unused);
            }
        } else {
            // The Guide shortcuts: Guide is held first, then the other control.
            struct Shortcut {
                const char* name;
                s32         binding;
                const char* value;  ///< in place of the control's name, when set
                int         column;
                int         row;
            };
            // The same layout as Batocera's hotkeys.
            static const Shortcut kShortcuts[] = {
                {N_("Screenshot"),    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, nullptr,          0, 1},
                {N_("Service"),       SDL_GAMEPAD_BUTTON_LEFT_STICK,    nullptr,          0, 3},
                {N_("Rewind"),        SDL_GAMEPAD_BUTTON_DPAD_LEFT,     N_("Left held"),  0, 4},
                {N_("Fast-forward"),  SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    N_("Right held"), 0, 5},
                {N_("Exit"),          SDL_GAMEPAD_BUTTON_START,         nullptr,          1, 0},
                {N_("Quick-load"),    SDL_GAMEPAD_BUTTON_NORTH,         nullptr,          1, 2},
                {N_("Quick-save"),    SDL_GAMEPAD_BUTTON_WEST,          nullptr,          1, 3},
                {N_("Settings menu"), SDL_GAMEPAD_BUTTON_SOUTH,         nullptr,          1, 4},
                {N_("Test"),          SDL_GAMEPAD_BUTTON_RIGHT_STICK,   nullptr,          1, 5},
            };
            const auto box = [&](ImVec2 pos, const char* name, const std::string& value) {
                const ImVec2 end(pos.x + slot_w, pos.y + slot_h);
                draw->AddRectFilled(pos, end, pal.card, kit.px(4.0f));
                draw->AddRect(pos, end, pal.edge, kit.px(4.0f), 0, kit.stroke());
                label(pos, name, value, false, false);
            };
            const ImVec2 guide = at(layout.guide);
            const ImVec2 guide_pos(guide.x - slot_w * 0.5f, origin.y + 2.0f * s);
            box(guide_pos, tr("Hold first"), Input::pad_button_name(SDL_GAMEPAD_BUTTON_GUIDE));
            draw->AddLine(ImVec2(guide.x, guide_pos.y + slot_h), guide, pal.accent, 1.5f * s);
            draw->AddCircle(guide, 14.0f * s, pal.accent, 0, 2.5f * s);
            for (const Shortcut& shortcut : kShortcuts) {
                const float  y = img0.y + static_cast<float>(shortcut.row) * pitch - slot_h * 0.5f;
                const ImVec2 pos(shortcut.column == 0 ? left_x : right_x, y);
                box(pos, tr(shortcut.name),
                    shortcut.value != nullptr ? std::string(tr(shortcut.value)) : Input::pad_button_name(shortcut.binding));
                const ImVec2 anchor(shortcut.column == 0 ? pos.x + slot_w : pos.x, y + slot_h * 0.5f);
                connect(shortcut.column, anchor, art_binding_point(layout, shortcut.binding), button_colour(shortcut.binding), false);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, below_y + slot_h + 6.0f * s));
        ImGui::Dummy(ImVec2(width, 0.0f));
    }
    if (binds == &scratch && scratch != game_default) {
        config.pad_layouts[m_pad_family] = scratch;
    }

    // The capture in progress, with Skip and Stop while Map all runs.
    if (capturing_here && m_map_slot >= 0) {
        const Slot&  slot = kSlots[static_cast<size_t>(m_map_slot)];
        const char*  name = panel.slot_names[static_cast<size_t>(m_map_slot)] != nullptr
                              ? panel.slot_names[static_cast<size_t>(m_map_slot)]
                              : slot.name;
        const std::string ask = slot.kind == SlotKind::Button ? trf("Press the pad button for %s", tr(name))
                              : slot.kind == SlotKind::Stick  ? trf("Push the stick for %s", tr(name))
                              : slot.kind == SlotKind::Axis   ? trf("Move the stick the way that raises %s", tr(name))
                                                              : trf("Pull the trigger or push the stick for %s", tr(name));
        const std::string about = m_map_all
            ? trf("%zu of %zu   ·   %d s, then the next   ·   Escape stops", m_map_at + 1, m_map_queue.size(),
                  static_cast<int>(std::ceil(std::max(0.0f, kMapSeconds - m_map_seconds))))
            : std::string(tr("Escape cancels"));
        static constexpr std::array<const char*, 2> kSkipStop = {N_("Skip"), N_("Stop")};
        const std::array<bool, 2>                   enabled   = {m_map_all, true};
        const int clicked = kit.actions(ask.c_str(), about.c_str(), tr_all(kSkipStop), enabled, ui::Kit::Tone::Plain,
                                        {.id = "mapping"});
        if (clicked == 0) advance();
        if (clicked == 1) cancel_captures();
    } else if (m_pad_view == kViewShortcuts) {
        kit.paragraph(nullptr, tr("Hold Guide, then press the other button; the game does not see them. Exit "
                                  "goes back to the games when there are some. Service and Test always work."));
    } else if (hidden > 0) {
        kit.paragraph(nullptr, i18n::trnf("Click a control or press Enter on it, then press the pad input for it. "
                                          "Right-click, Delete or X clears it. %d slot this cabinet has no control "
                                          "for is hidden: Show unused lists it.",
                                          "Click a control or press Enter on it, then press the pad input for it. "
                                          "Right-click, Delete or X clears it. %d slots this cabinet has no control "
                                          "for are hidden: Show unused lists them.",
                                          static_cast<unsigned long>(hidden), hidden).c_str());
    } else {
        kit.paragraph(nullptr, tr("Click a control or press Enter on it, then press the pad input for it. "
                                  "Right-click, Delete or X clears it. The left stick also moves; the shoulders "
                                  "also act as Button 3/4."));
    }
    kit.end_group();

    // -- sticks and rumble ---------------------------------------------------
    kit.begin_group(tr("Sticks"));
    {
        int  deadzone = static_cast<int>(config.pad_stick_deadzone);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", deadzone);
        kit.tip(tr("Stick travel about centre that reads as centre. Let go of the stick and raise this until "
                   "the marker below sits inside the shaded zone. Also applies to stick aiming."));
        if (kit.slider(tr("Stick deadzone"), nullptr, deadzone, 0, 40, 1, shown)) {
            config.pad_stick_deadzone = static_cast<u32>(deadzone);
        }
        const float  bar_w = kit.px(220.0f);
        const ImVec2 bar_at = kit.custom(tr("Stick position"), tr("The stick now, against the deadzone."), bar_w,
                                         ImGui::GetFrameHeight());
        ImGui::SetCursorScreenPos(bar_at);
        ImGui::PushItemWidth(bar_w);
        centre_bar("stick_centre", input != nullptr ? input->pad_steer_raw(static_cast<u32>(edit)) : 0,
                   config.pad_stick_deadzone);
        ImGui::PopItemWidth();
    }
    {
        int  sensitivity = static_cast<int>(config.pad_stick_sensitivity);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", sensitivity);
        kit.tip(tr("Response of a stick's travel past its deadzone. Full deflection is always full lock; under "
                   "100% the middle of the travel does less, over 100% it does more."));
        if (kit.slider(tr("Stick sensitivity"), nullptr, sensitivity, 25, 300, 5, shown)) {
            config.pad_stick_sensitivity = static_cast<u32>(sensitivity);
        }
    }
    kit.end_group();

    kit.begin_group(tr("Rumble"));
    kit.tip(tr("Vibrates on the game's impacts in driving games. Other games stay quiet."));
    kit.toggle(tr("Rumble"), tr("The game's impacts, in driving games."), config.pad_rumble);
    {
        int  strength = static_cast<int>(config.pad_rumble_strength);
        char shown[16];
        std::snprintf(shown, sizeof shown, "%d%%", strength);
        if (kit.slider(tr("Rumble strength"), nullptr, strength, 0, 100, 10, shown, {.disabled = !config.pad_rumble})) {
            config.pad_rumble_strength = static_cast<u32>(strength);
        }
    }
    kit.tip(tr("An added vibration that grows as you steer. It is not from the game. Turn it off to feel only "
               "the game's impacts."));
    kit.toggle(tr("Steering vibration"), tr("Grows as you steer; not from the game."), config.pad_rumble_cornering,
               {.disabled = !config.pad_rumble});
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Input: wheel
// ---------------------------------------------------------------------------

void Gui::page_wheel(ui::Kit& kit, Config& config, Input* input)
{
    const ui::Palette& pal       = kit.palette();
    const bool         connected = input != nullptr && input->wheel_connected();
    char               shown[32];
    const auto pct = [&](int v) {
        std::snprintf(shown, sizeof shown, "%d%%", v);
        return shown;
    };

    kit.begin_group(tr("Wheel"));
    kit.status(connected ? tr("Wheel connected.") : tr("No wheel connected. Settings still apply once one is."),
               connected ? pal.good : pal.faint);
    kit.tip(tr("Wheel force on driving games: centring and cornering forces decoded from the game's own "
               "drive-board commands, plus an impact jolt when you hit something."));
    kit.toggle(tr("Force feedback"), tr("The drive board's forces, on a wheel that can make them."), config.wheel_ffb);
    const ui::Kit::Opts ffb_off{.disabled = !config.wheel_ffb};
    {
        int v = static_cast<int>(config.wheel_ffb_strength);
        kit.tip(tr("A gain on every force of the drive board. At 100%, the board's full power (63) gives the "
                   "wheel's full force, every force in proportion. 200% doubles every force: those above half "
                   "the board's power (32, Sega Rally's strongest torque) then saturate."));
        if (kit.slider(tr("Resistance"), nullptr, v, 0, 200, 10, pct(v), ffb_off)) {
            config.wheel_ffb_strength = static_cast<u32>(v);
        }
    }
    {
        int v = static_cast<int>(config.wheel_panel_spring);
        kit.tip(tr("The centring spring of the Sega Rally Championship and Daytona USA cabinets: very light, "
                   "it brings the wheel back and is barely felt when held against. A share of the resistance "
                   "above. The other games' cabinets drive the wheel with no spring, and ignore this setting."));
        if (kit.slider(tr("Panel spring"), tr("Sega Rally Championship and Daytona USA only, all versions."), v, 0,
                       100, 5, pct(v), ffb_off)) {
            config.wheel_panel_spring = static_cast<u32>(v);
        }
    }
    {
        int v = static_cast<int>(config.wheel_drive_dip);
        std::snprintf(shown, sizeof shown, "%d", v);
        kit.tip(tr("The motor strength set on the drive board's DIP switches, the switches ON counted in binary: "
                   "0 = all OFF. Sega Rally: switches 1-3, 0 to 7 added to every torque. Daytona USA: switches "
                   "1-3, 4 times that. Indy 500, Touring Car, Over Rev, Super GT 24h: switches 1-1 to 1-3, 0 to "
                   "7 added to every force."));
        if (kit.slider(tr("Drive board DIP strength"), tr("The operator's setting, 0 to 7."), v, 0, 7, 1, shown, ffb_off)) {
            config.wheel_drive_dip = static_cast<u32>(v);
        }
    }
    kit.tip(tr("For a wheel whose driver pushes the wrong way: it pulls away from the centre instead of "
               "bringing it back, and the game's forces go the opposite way."));
    kit.toggle(tr("Invert force feedback"), nullptr, config.wheel_ffb_invert, ffb_off);
    kit.tip(tr("Vibrates on the game's impacts, for wheels without force feedback. It never runs alongside "
               "force feedback."));
    kit.toggle(tr("Rumble"), tr("For wheels without force feedback."), config.wheel_rumble);
    {
        int v = static_cast<int>(config.wheel_rumble_strength);
        if (kit.slider(tr("Rumble strength"), nullptr, v, 0, 100, 10, pct(v), {.disabled = !config.wheel_rumble})) {
            config.wheel_rumble_strength = static_cast<u32>(v);
        }
    }
    kit.tip(tr("An added vibration that grows with the throttle. It is not from the game. Turn it off to feel "
               "only the game's force feedback."));
    kit.toggle(tr("Engine vibration"), tr("Grows with the throttle; not from the game."), config.wheel_rumble_engine,
               {.disabled = !config.wheel_rumble});
    kit.end_group();

    kit.begin_group(tr("Steering"));
    {
        // Common wheel rotation ranges rather than a free slider: a wheel is
        // set to one of these, and 270 matches the Model 2 cabinet.
        static constexpr std::array<u32, 9> kRanges = {200, 240, 270, 360, 400, 540, 720, 900, 1080};
        static constexpr std::array<const char*, 9> kRangeLabels = {"200", "240", "270", "360", "400", "540", "720", "900", "1080"};
        int current = 2;
        for (size_t i = 0; i < kRanges.size(); ++i) {
            if (kRanges[i] == config.wheel_steer_degrees) current = static_cast<int>(i);
        }
        kit.tip(tr("Set this to your wheel's own rotation range. The lock angle below is mapped onto it, so "
                   "matching your wheel gives arcade-like response."));
        if (kit.choice(tr("Steering range"), tr("Your wheel's own rotation, in degrees."), kRangeLabels, current)) {
            config.wheel_steer_degrees = kRanges[static_cast<size_t>(current)];
        }
    }
    {
        int v = static_cast<int>(config.wheel_lock_degrees);
        std::snprintf(shown, sizeof shown, "%d deg", v);
        kit.tip(tr("How far to physically turn for full game lock (total, so half each side of centre). Lower "
                   "is more sensitive."));
        if (kit.slider(tr("Lock angle"), tr("Rotation to full lock, both sides together."), v, 180, 270, 5, shown)) {
            config.wheel_lock_degrees = static_cast<u32>(v);
        }
    }
    {
        int v = static_cast<int>(config.wheel_steer_deadzone);
        kit.tip(tr("Wheel travel about centre that reads as centre. Most wheels want none; a little covers one "
                   "whose centre wanders. The bar below shows the wheel now against the shaded zone."));
        if (kit.slider(tr("Centre deadzone"), nullptr, v, 0, 20, 1, pct(v))) {
            config.wheel_steer_deadzone = static_cast<u32>(v);
        }
        const float  bar_w  = kit.px(220.0f);
        const ImVec2 bar_at = kit.custom(tr("Wheel position"), tr("The wheel now, against the deadzone."), bar_w,
                                         ImGui::GetFrameHeight());
        ImGui::SetCursorScreenPos(bar_at);
        ImGui::PushItemWidth(bar_w);
        centre_bar("wheel_centre", connected ? input->wheel_steer_raw() : 0, config.wheel_steer_deadzone);
        ImGui::PopItemWidth();
    }
    kit.end_group();

    // -- axis calibration ---------------------------------------------------
    kit.begin_group(tr("Axes"));
    kit.paragraph(nullptr, tr("Auto-detected. Calibrate one if steering or a pedal is wrong: click its value, "
                              "then operate that control."));
    struct AxisRow { const char* name; s32* axis; bool* invert; bool* half; };
    const AxisRow axis_rows[] = {
        {N_("Steering"), &config.wheel_steer_axis, nullptr, nullptr},
        {N_("Accelerator"), &config.wheel_accel_axis, &config.wheel_accel_invert, &config.wheel_accel_half},
        {N_("Brake"), &config.wheel_brake_axis, &config.wheel_brake_invert, &config.wheel_brake_half},
    };
    static constexpr std::array<const char*, 1> kAuto = {N_("Auto")};
    const std::array<const char*, 1>            auto_label = tr_all(kAuto);
    for (int row = 0; row < 3; ++row) {
        const AxisRow& r         = axis_rows[row];
        const bool     capturing = m_capture == Capture::Axis && m_capture_axis == row;
        std::string    value;
        if (capturing) {
            value = tr("operate it...");
        } else if (*r.axis < 0) {
            value = tr("auto");
        } else if (r.half != nullptr && *r.half) {
            const bool low = r.invert != nullptr && *r.invert;
            value = low ? trf("axis %d (half, -)", *r.axis) : trf("axis %d (half, +)", *r.axis);
        } else if (r.invert != nullptr && *r.invert) {
            value = trf("axis %d (inverted)", *r.axis);
        } else {
            value = trf("axis %d", *r.axis);
        }
        ui::Kit::Opts opts;
        opts.id       = r.name;
        opts.disabled = !connected;
        const int clicked = kit.binding(tr(r.name), nullptr, value.c_str(), capturing, auto_label, opts);
        if (clicked == 0) {
            if (capturing) {
                m_capture = Capture::None;
            } else {
                m_capture      = Capture::Axis;
                m_capture_axis = row;
                if (input != nullptr) {
                    input->wheel_axis_baseline(m_axis_baseline.data(),
                                               std::min<int>(input->wheel_axis_count(),
                                                             static_cast<int>(m_axis_baseline.size())));
                }
            }
        } else if (clicked == 1) {
            *r.axis = -1;
            if (r.invert != nullptr) *r.invert = false;
            if (r.half != nullptr) *r.half = false;
        }
        if (capturing && input != nullptr) {
            bool      positive = true;
            const s32 got      = input->captured_axis(m_axis_baseline.data(),
                                                      std::min<int>(input->wheel_axis_count(),
                                                                    static_cast<int>(m_axis_baseline.size())),
                                                      &positive);
            if (got >= 0) {
                *r.axis = got;
                // A pedal read as "released high, pressed low" is inverted; a
                // downward move at capture time means exactly that.
                if (r.invert != nullptr) *r.invert = !positive;
                // A pedal that rested at the centre of its axis, rather than at
                // an end, uses half of it: the half it moved into.
                if (r.half != nullptr) {
                    *r.half = std::abs(static_cast<int>(m_axis_baseline[static_cast<usize>(got)])) <= 16384;
                }
                m_capture = Capture::None;
            }
        }
    }
    kit.end_group();

    // -- button binding -----------------------------------------------------
    kit.begin_group(tr("Buttons"));
    kit.paragraph(nullptr, tr("Click a value, then press the wheel button for that control."));
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
        {N_("Gear 1"),     Config::WheelRole::Gear1},
        {N_("Gear 2"),     Config::WheelRole::Gear2},
        {N_("Gear 3"),     Config::WheelRole::Gear3},
        {N_("Gear 4"),     Config::WheelRole::Gear4},
        {N_("Neutral"),    Config::WheelRole::GearNeutral},
        {N_("Test"),       Config::WheelRole::Test},
        {N_("Service"),    Config::WheelRole::Service},
        {N_("Menu"),       Config::WheelRole::Menu},
    };
    static constexpr std::array<const char*, 1> kClear = {N_("Clear")};
    const std::array<const char*, 1>            clear_label = tr_all(kClear);
    for (const ButtonRow& r : button_rows) {
        const u32   role_index = static_cast<u32>(r.role);
        const s32   bound      = config.wheel_buttons[role_index];
        const bool  capturing  = m_capture == Capture::Button && m_capture_role == role_index;
        std::string value      = capturing ? std::string(tr("press a button..."))
                               : bound < 0 ? std::string(tr("unbound"))
                                           : trf("button %d", bound);
        ui::Kit::Opts opts;
        opts.id       = r.name;
        opts.disabled = !connected;
        const int clicked = kit.binding(tr(r.name), r.role == Config::WheelRole::Menu ? tr("This menu.") : nullptr,
                                        value.c_str(), capturing, clear_label, opts);
        if (clicked == 0) {
            if (capturing) {
                m_capture = Capture::None;
            } else {
                m_capture      = Capture::Button;
                m_capture_role = role_index;
            }
        } else if (clicked == 1) {
            config.wheel_buttons[role_index] = -1;
        }
        if (capturing && input != nullptr) {
            const s32 got = input->pressed_wheel_button();
            if (got >= 0) {
                config.wheel_buttons[role_index] = got;
                m_capture                        = Capture::None;
            }
        }
    }
    kit.end_group();

    // -- gear shifter -------------------------------------------------------
    kit.begin_group(tr("Gear shifter"));
    kit.tip(tr("With an H shifter bound to Gear 1..4: its lever in none of the gears is in neutral, as on the "
               "cabinet. Off, the last gear stays engaged until another is selected or Neutral is pressed."));
    kit.toggle(tr("Neutral when no gear is engaged"), tr("Daytona USA and Sega Rally Championship, all versions."),
               config.wheel_shifter_neutral);
    {
        int v = static_cast<int>(config.wheel_shift_neutral_ms);
        std::snprintf(shown, sizeof shown, "%d ms", v);
        kit.tip(tr("Shift up / Shift down (and a gamepad's RB / LB) from one gear to another pass through "
                   "neutral this long, as a lever crosses the gate's centre. 0 shifts straight."));
        if (kit.slider(tr("Neutral between gears"), nullptr, v, 0, 500, 10, shown)) {
            config.wheel_shift_neutral_ms = static_cast<u32>(v);
        }
    }
    kit.end_group();
}

// ---------------------------------------------------------------------------
// Input: light gun
// ---------------------------------------------------------------------------

void Gui::page_lightgun(ui::Kit& kit, Config& config, Input* input)
{
    char shown[32];

    kit.begin_group(tr("Light gun"));
    {
        static constexpr std::array<const char*, 3> kModes = {N_("Auto"), N_("On"), N_("Off")};
        int mode = std::clamp(static_cast<int>(config.lightgun), 0, 2);
        kit.tip(tr("The aiming crosshair, with the mouse pointer hidden. Auto turns it on in a gun game; On "
                   "hides the pointer in every game; Off never shows a crosshair."));
        if (kit.choice(tr("Gun mode"), tr("Crosshair and hidden pointer: in gun games, always, or never."),
                       tr_all(kModes), mode)) {
            config.lightgun = static_cast<Config::LightgunMode>(mode);
        }
    }
    kit.tip(tr("Draw Keisho's own crosshair. Turn off if the gun has its own sight (e.g. a Sinden). "
               "Positional-gun titles always draw their own regardless."));
    kit.toggle(tr("Show crosshair"), tr("Keisho's own sight."), config.lightgun_crosshair);
    kit.tip(tr("A gamepad's stick points at the screen: the crosshair sits in the centre at rest and reaches "
               "the edge at full tilt. Off, the stick moves the crosshair like a mouse."));
    kit.toggle(tr("Centred stick aim"), tr("A stick points at the screen instead of moving the sight."),
               config.lightgun_pad_centred);
    kit.tip(tr("Hide the white flash Virtua Cop, Virtua Cop 2 and House of the Dead show on each shot."));
    kit.toggle(tr("Hide shot flash"), tr("The white flash of the gun games."), config.lightgun_hide_flash);
    kit.end_group();

    // Dedicated light guns (recoil motors, per-device buttons) need the evdev
    // or Raw Input backend. Without one, the mouse is the gun.
#ifndef SM2_HAVE_LIGHTGUNS
    (void)input;
    kit.begin_group(tr("Devices"));
    kit.paragraph(nullptr, tr("By default player 1 aims with the mouse (the left button fires and the right "
                              "reloads by shooting off screen) and player 2 with a gamepad."));
    kit.end_group();
#else
    kit.begin_group(tr("Recoil"));
    kit.tip(tr("Kick the gun's rumble motor on each shot, for guns that have one (Sinden and similar). No "
               "effect on a gun without a motor."));
    kit.toggle(tr("Recoil"), tr("The gun's motor kicks on each shot."), config.lightgun_recoil);
    {
        int v = static_cast<int>(config.lightgun_recoil_strength);
        std::snprintf(shown, sizeof shown, "%d%%", v);
        if (kit.slider(tr("Strength"), nullptr, v, 0, 100, 5, shown, {.disabled = !config.lightgun_recoil})) {
            config.lightgun_recoil_strength = static_cast<u32>(v);
        }
    }
    kit.end_group();

    kit.begin_group(tr("Devices"));
    const usize guns = input != nullptr ? input->gun_count() : 0;
    if (guns > 0) {
        for (usize i = 0; i < guns; ++i) {
            const std::string name = trf("Gun %zu", i + 1);
            ui::Kit::Opts     opts;
            opts.id = name.c_str();
            kit.info(name.c_str(), trf("Player %zu", i + 1).c_str(), input->gun_name(i).c_str(), 0, opts);
        }
    } else {
#ifdef _WIN32
        kit.paragraph(nullptr, tr("No dedicated light guns detected. Player 1 aims with the mouse; the left "
                                  "button fires and the right button reloads (shoot off screen). Plug in guns in "
                                  "absolute mouse mode (GUN4IR, Sinden, AimTrak) before launching for independent "
                                  "per-player aiming."));
#else
        kit.paragraph(nullptr, tr("No dedicated light guns detected. Player 1 aims with the mouse; the left "
                                  "button fires and the right button reloads (shoot off screen). Plug in guns "
                                  "tagged ID_INPUT_GUN for independent per-player aiming."));
#endif
    }
    kit.end_group();

    {
        static constexpr std::array<const char*, 8> kRoleNames = {
            N_("Trigger"), N_("Reload/Missile"), N_("Coin"), N_("Start"),
            N_("Hat up"), N_("Hat down"), N_("Hat left"), N_("Hat right"),
        };
        static constexpr std::array<const char*, 1> kClear       = {N_("Clear")};
        const std::array<const char*, 1>            clear_label = tr_all(kClear);
        for (int p = 0; p < 2; ++p) {
            kit.begin_group(trf("Gun buttons, player %d", p + 1).c_str());
            if (p == 0) {
                kit.paragraph(nullptr, tr("Click a value, then press the button on that player's gun. With no "
                                          "gun, a mouse button binds too."));
            }
            for (u32 r = 0; r < Config::kGunRoleCount; ++r) {
                const u32         code      = config.gun_buttons[static_cast<usize>(p)][r];
                const bool        capturing = m_gun_capture_player == p && m_gun_capture_role == r;
                const std::string value     = capturing ? std::string(tr("press a button..."))
                                            : code == 0 ? std::string(tr("unbound"))
                                                        : evdev_button_name(code);
                ui::Kit::Opts opts;
                opts.id = kRoleNames[r];
                const int clicked = kit.binding(tr(kRoleNames[r]), nullptr, value.c_str(), capturing, clear_label, opts);
                if (clicked == 0) {
                    if (capturing) {
                        m_gun_capture_player = -1;
                    } else {
                        m_gun_capture_player = p;
                        m_gun_capture_role   = r;
                    }
                } else if (clicked == 1) {
                    config.gun_buttons[static_cast<usize>(p)][r] = 0;
                }
                if (capturing) {
                    u32 got = 0;
                    if (p < static_cast<int>(guns)) {
                        got = input->gun_take_last_pressed(static_cast<usize>(p));
                    }
                    // Mouse fallback so a binding can be set with no gun present:
                    // not the click that started the capture.
                    if (got == 0 && clicked < 0) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))        got = 0x110;
                        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))  got = 0x111;
                        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) got = 0x112;
                    }
                    if (got != 0) {
                        config.gun_buttons[static_cast<usize>(p)][r] = got;
                        m_gun_capture_player                         = -1;
                    }
                }
            }
            kit.end_group();
        }
    }
#endif  // SM2_HAVE_LIGHTGUNS

    kit.begin_group(tr("Sinden border"));
    kit.tip(tr("A bright frame around the game image that a Sinden gun's camera tracks for aiming. Only shown "
               "in light-gun mode."));
    kit.toggle(tr("Show border"), tr("The frame a Sinden gun's camera tracks."), config.sinden_border);
    {
        const ui::Kit::Opts off{.disabled = !config.sinden_border};
        const float  swatch = ImGui::GetFrameHeight();
        const ImVec2 at     = kit.custom(tr("Colour"), nullptr, swatch, swatch, off);
        ImGui::SetCursorScreenPos(at);
        float rgb[3] = {
            static_cast<float>((config.sinden_border_colour >> 16) & 0xff) / 255.0f,
            static_cast<float>((config.sinden_border_colour >> 8) & 0xff) / 255.0f,
            static_cast<float>(config.sinden_border_colour & 0xff) / 255.0f,
        };
        if (!config.sinden_border) ImGui::BeginDisabled();
        if (ImGui::ColorEdit3("##sinden_colour", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            const u32 r = static_cast<u32>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
            const u32 g = static_cast<u32>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
            const u32 b = static_cast<u32>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
            config.sinden_border_colour = (r << 16) | (g << 8) | b;
        }
        if (!config.sinden_border) ImGui::EndDisabled();
        int v = static_cast<int>(config.sinden_border_thickness);
        std::snprintf(shown, sizeof shown, "%d px", v);
        if (kit.slider(tr("Thickness"), tr("In game-image pixels."), v, 1, 64, 1, shown, off)) {
            config.sinden_border_thickness = static_cast<u32>(v);
        }
    }
    kit.end_group();
}

void Gui::capture_key(Config& config, u32 key)
{
    const int index  = m_capture_hotkey;
    m_capture_hotkey = -1;
    if (index < 0 || key == SDLK_ESCAPE) {
        return;
    }
    const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(key));
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    // A key already on another shortcut swaps with this one's old key.
    std::string& bound = config.hotkeys[static_cast<usize>(index)];
    for (std::string& other : config.hotkeys) {
        if (&other != &bound && !other.empty()
            && SDL_GetKeyFromName(other.c_str()) == static_cast<SDL_Keycode>(key)) {
            other = bound;
        }
    }
    bound = name;
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
// On-screen notification (transient toast, top-centred)
// ---------------------------------------------------------------------------

void Gui::notify(std::string message)
{
    m_notify_text         = std::move(message);
    m_notify_seconds_left = 2.0f;
}

void Gui::show_load_error(std::string game, std::string message,
                          std::vector<std::string> missing)
{
    m_load_error = LoadErrorDialog{std::move(game), std::move(message), std::move(missing)};
}

void Gui::draw_load_error()
{
    LoadErrorDialog& dialog = *m_load_error;
    const char*      id     = "###load_error";
    if (!dialog.opened) {
        ImGui::OpenPopup(id);
        dialog.opened = true;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f,
                                               viewport->WorkSize.y * 0.8f));
    const std::string title = std::string(tr("Could not start the game")) + id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize
                                    | ImGuiWindowFlags_NoSavedSettings)) {
        m_load_error.reset();
        return;
    }

    if (!dialog.game.empty()) {
        ImGui::TextUnformatted(dialog.game.c_str());
        ImGui::Spacing();
    }
    ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.75f);
    ImGui::TextUnformatted(dialog.message.c_str());
    ImGui::PopTextWrapPos();

    if (!dialog.missing.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Missing:"));
        const float rows   = static_cast<float>(std::min<usize>(dialog.missing.size(), 12));
        const float height = rows * ImGui::GetTextLineHeightWithSpacing()
                           + ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (ImGui::BeginChild("##missing", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
            for (const std::string& line : dialog.missing) {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        ImGui::EndChild();
        if (ImGui::Button(tr_id("Copy list"))) {
            std::string text;
            for (const std::string& line : dialog.missing) {
                text += line;
                text += '\n';
            }
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
    }

    if (ImGui::Button(tr_id("OK")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
        || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
        ImGui::CloseCurrentPopup();
        m_load_error.reset();
    }
    ImGui::EndPopup();
}

void Gui::draw_notification()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Same font and size as the FPS overlay, centred at the top.
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

void Gui::draw_crosshairs(const Config& config, const Input* input)
{
    if (input == nullptr) {
        return;
    }
    update_crosshair_images(config);

    // Each aim is a 0..1 fraction of the game image, mapped onto the letterboxed
    // rectangle the frame is presented into.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) {
        return;
    }
    const render::Letterbox box = render::compute_game_rect(
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y),
        m_present_aspect, m_present_method, m_present_area);

    // Distinct per player: green for 1, cyan for 2.
    static const std::array<ImU32, 2> colours = {
        IM_COL32(0, 255, 0, 220),
        IM_COL32(0, 200, 255, 220),
    };

    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float radius = std::max(8.0f, box.height * 0.02f);
    const auto& aims = input->gun_aims();

    // The input poll stops while paused, so a player 1 on the mouse fallback
    // tracks the live pointer instead; guns and pad cursors freeze.

    for (usize player = 0; player < aims.size(); ++player) {
        const Input::GunAim& aim = aims[player];
        if (!aim.active || aim.calibrating || aim.idle) {
            continue;
        }
        float fx = aim.x;
        float fy = aim.y;
        if (player == 0 && aim.mouse && box.width > 0.0f && box.height > 0.0f) {
            // From SDL rather than io.MousePos, which is relative to the focused
            // settings window.
            float mx = 0.0f;
            float my = 0.0f;
            SDL_GetMouseState(&mx, &my);
            fx = std::clamp((mx - box.x) / box.width, 0.0f, 1.0f);
            fy = std::clamp((my - box.y) / box.height, 0.0f, 1.0f);
        }
        const float cx = box.x + fx * box.width;
        const float cy = box.y + fy * box.height;
        if (player < m_crosshair_images.size() && m_crosshair_images[player].handle != 0) {
            const CrosshairImage& image = m_crosshair_images[player];
            const float           scale = box.height / 1080.0f;
            const ImVec2          half(image.w * scale * 0.5f, image.h * scale * 0.5f);
            list->AddImage(reinterpret_cast<ImTextureID>(m_backend->texture_imgui_id(image.handle)),
                           ImVec2(cx - half.x, cy - half.y), ImVec2(cx + half.x, cy + half.y));
            continue;
        }
        const ImU32 colour = colours[player < colours.size() ? player : 0];
        list->AddCircle(ImVec2(cx, cy), radius, colour, 24, 2.0f);
        list->AddLine(ImVec2(cx - radius * 1.6f, cy), ImVec2(cx - radius * 0.4f, cy), colour, 2.0f);
        list->AddLine(ImVec2(cx + radius * 0.4f, cy), ImVec2(cx + radius * 1.6f, cy), colour, 2.0f);
        list->AddLine(ImVec2(cx, cy - radius * 1.6f), ImVec2(cx, cy - radius * 0.4f), colour, 2.0f);
        list->AddLine(ImVec2(cx, cy + radius * 0.4f), ImVec2(cx, cy + radius * 1.6f), colour, 2.0f);
    }
}

// Targets for a host gun calibration tool (Batocera's), drawn in window space
// and regardless of the crosshair setting.
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

    // Frame the letterboxed game image, including widescreen side columns, which
    // is what the gun's camera sees.
    const render::Letterbox box = render::compute_letterbox(
        static_cast<u32>(io.DisplaySize.x), static_cast<u32>(io.DisplaySize.y),
        m_present_aspect, m_present_method, m_present_area);

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
            case DirPickerTarget::Model2RomDir:  config.model2_rom_dir = m_dir_picker_path; break;
            case DirPickerTarget::Model1RomDir:  config.model1_rom_dir = m_dir_picker_path; break;
            case DirPickerTarget::Model3RomDir:  config.model3_rom_dir = m_dir_picker_path; break;
            case DirPickerTarget::NvramDir:      config.nvram_dir      = m_dir_picker_path; break;
            case DirPickerTarget::ScreenshotDir: config.screenshot_dir = m_dir_picker_path; break;
            case DirPickerTarget::BezelDir: config.bezel_dir = m_dir_picker_path; break;
            case DirPickerTarget::CrosshairDir: config.crosshair_dir = m_dir_picker_path; break;
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

    // Load anything already cached: the scraper never sends Ready for a set it
    // has cached.
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

bool Gui::picker_escape()
{
    if (!m_picker_enabled || (!m_picker_searching && m_picker_search[0] == '\0')) {
        return false;
    }
    m_picker_search[0]     = '\0';
    m_picker_drop_search   = true;
    m_picker_scroll_to_sel = true;
    return true;
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
    poll_scraper_and_load_metadata();

    // Show the pointer in case a previous state (light-gun mode) hid it.
    if (!SDL_CursorVisible()) {
        SDL_ShowCursor();
    }

    const ui::Palette& p = *m_palette;
    ui::Kit            kit(p, m_ui_scale > 0.0f ? m_ui_scale : 1.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
                                     | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(p.backdrop));
    if (!ImGui::Begin("##picker", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }
    ImDrawList*  list   = ImGui::GetWindowDrawList();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 room   = viewport->WorkSize;
    const float  margin = kit.px(18.0f);
    const float  font   = ImGui::GetFontSize();
    const ImGuiIO& io   = ImGui::GetIO();

    // -- the games shown: by system and by the search ------------------------
    static constexpr std::array<const char*, 4> kSystems = {N_("All"), "Model 1", "Model 2", "Model 3"};
    const std::array<const char*, 4>            systems  = tr_all(kSystems);
    std::string needle = m_picker_search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<int> shown;  // indices into m_picker_entries, in the order drawn
    for (int i = 0; i < static_cast<int>(m_picker_entries.size()); ++i) {
        const PickerEntry& e = m_picker_entries[static_cast<usize>(i)];
        if (m_picker_system != 0 && e.system != kSystems[static_cast<usize>(m_picker_system)]) continue;
        if (!needle.empty()) {
            std::string hay = e.title + " " + e.name;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) == std::string::npos) continue;
        }
        shown.push_back(i);
    }
    // Grouped by system when every system shows: Model 1, 2, 3, then titles.
    const bool grouped = m_picker_system == 0;
    if (grouped) {
        std::stable_sort(shown.begin(), shown.end(), [&](int a, int b) {
            return m_picker_entries[static_cast<usize>(a)].system < m_picker_entries[static_cast<usize>(b)].system;
        });
    }

    // -- the head: the title, the systems, the search ------------------------
    const float head_top = origin.y + margin;
    const float head_h   = kit.px(30.0f);
    {
        const float big = std::round(font * 1.35f);
        list->AddText(ImGui::GetFont(), big, ImVec2(origin.x + margin, head_top + (head_h - big) * 0.5f), p.text,
                      tr("Games"));
        const float title_w = ImGui::GetFont()->CalcTextSizeA(big, FLT_MAX, 0.0f, tr("Games")).x;
        const std::string count = trf("%zu of %zu", shown.size(), m_picker_entries.size());
        list->AddText(ImVec2(origin.x + margin + title_w + kit.px(10.0f), head_top + (head_h - font) * 0.5f),
                      p.faint, count.c_str());

        // The settings cog at the far right, the search beside it.
        const float  cog_d = kit.px(26.0f);
        const ImVec2 cog_at(origin.x + room.x - margin - cog_d, head_top + (head_h - cog_d) * 0.5f);
        {
            ImGui::SetCursorScreenPos(cog_at);
            const bool pressed = ImGui::InvisibleButton("settings_cog", ImVec2(cog_d, cog_d));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 centre(cog_at.x + cog_d * 0.5f, cog_at.y + cog_d * 0.5f);
            list->AddCircleFilled(centre, cog_d * 0.5f, hovered ? ui::mix(p.card, p.text, 0.10f) : p.card);
            list->AddCircle(centre, cog_d * 0.5f, p.edge, 0, kit.stroke());
            ui::Kit::icon(list, ui::Kit::Icon::Gear, centre, kit.px(14.0f), hovered ? p.text : p.muted, kit.stroke());
            if (hovered) tooltip(trf("Settings (%s)", hotkey_label(config, Config::Hotkey::Overlay)).c_str());
            if (pressed) show();
        }
        const float  search_w = kit.px(230.0f);
        const ImVec2 search_at(cog_at.x - kit.px(8.0f) - search_w, head_top + (head_h - kit.px(26.0f)) * 0.5f);
        // A letter or digit typed with nothing else taking it starts a search:
        // the field then takes the rest. (SDL sends text only to a field, so
        // the first key is read as a key.)
        if (!io.WantTextInput && !m_load_error.has_value() && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
            std::string typed;
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
                    typed += static_cast<char>((io.KeyShift ? 'A' : 'a') + (k - ImGuiKey_A));
                }
            }
            for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) typed += static_cast<char>('0' + (k - ImGuiKey_0));
            }
            if (!typed.empty()) {
                const size_t have = std::strlen(m_picker_search);
                std::snprintf(m_picker_search + have, sizeof m_picker_search - have, "%s", typed.c_str());
                m_picker_focus_search = true;
                m_picker_scroll_to_sel = true;
            }
        }
        ImGui::SetCursorScreenPos(search_at);
        ImGui::SetNextItemWidth(search_w);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kit.px(10.0f), (kit.px(26.0f) - font) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kit.px(13.0f));
        if (m_picker_focus_search) {
            ImGui::SetKeyboardFocusHere();
            m_picker_focus_search = false;
        }
        if (m_picker_drop_search) {
            ImGui::ClearActiveID();
            m_picker_drop_search = false;
        }
        if (ImGui::InputTextWithHint("##search", tr("Search"), m_picker_search, sizeof m_picker_search)) {
            m_picker_scroll_to_sel = true;
        }
        const bool searching = ImGui::IsItemActive();
        m_picker_searching   = searching;
        ImGui::PopStyleVar(2);
        if (m_picker_search[0] != '\0' && !searching) {
            // A small clear mark inside the field.
            const ImVec2 c(search_at.x + search_w - kit.px(14.0f), search_at.y + kit.px(13.0f));
            ui::Kit::icon(list, ui::Kit::Icon::Cross, c, kit.px(9.0f), p.faint, kit.stroke());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && ImGui::IsMouseHoveringRect(ImVec2(c.x - kit.px(10.0f), c.y - kit.px(10.0f)),
                                              ImVec2(c.x + kit.px(10.0f), c.y + kit.px(10.0f)))) {
                m_picker_search[0] = '\0';
            }
        }

        // The systems in the middle.
        const ImVec2 seg_size = kit.segments_size(systems);
        const ImVec2 seg_at(std::floor(origin.x + (room.x - seg_size.x) * 0.5f),
                            head_top + std::floor((head_h - seg_size.y) * 0.5f));
        if (kit.segments_at("systems", seg_at, systems, m_picker_system)) {
            m_picker_scroll_to_sel = true;
        }
    }
    // The accent line under the head, fading at its ends.
    {
        const float y  = head_top + head_h + kit.px(10.0f);
        const float x0 = origin.x + margin, x1 = origin.x + room.x - margin;
        const float fade = (x1 - x0) * 0.1f;
        list->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + fade, y + kit.stroke()), ui::with_alpha(p.accent, 0.0f),
                                      p.accent, p.accent, ui::with_alpha(p.accent, 0.0f));
        list->AddRectFilled(ImVec2(x0 + fade, y), ImVec2(x1 - fade, y + kit.stroke()), p.accent);
        list->AddRectFilledMultiColor(ImVec2(x1 - fade, y), ImVec2(x1, y + kit.stroke()), p.accent,
                                      ui::with_alpha(p.accent, 0.0f), ui::with_alpha(p.accent, 0.0f), p.accent);
    }

    // -- the foot: the keys ----------------------------------------------------
    const float foot_h = kit.px(30.0f);
    {
        std::string hint = trf("Enter: play   %s: settings   %s: quit", hotkey_label(config, Config::Hotkey::Overlay),
                               hotkey_label(config, Config::Hotkey::Quit));
        if (io.BackendFlags & ImGuiBackendFlags_HasGamepad) {
            hint = std::string(tr("A: play")) + "   " + hint;
        }
        const float w = ImGui::CalcTextSize(hint.c_str()).x;
        list->AddText(ImVec2(origin.x + room.x - margin - w, origin.y + room.y - foot_h + (foot_h - font) * 0.5f),
                      p.faint, hint.c_str());
        list->AddText(ImVec2(origin.x + margin, origin.y + room.y - foot_h + (foot_h - font) * 0.5f), p.faint,
                      tr("Type to search. Arrows or the D-pad choose."));
    }

    if (m_picker_entries.empty()) {
        const ImVec2 at(origin.x + margin, head_top + head_h + kit.px(30.0f));
        list->AddText(ImGui::GetFont(), font, at, p.muted,
                      tr("No ROM archives were found in the ROM folders. Set them in Settings > Paths, then relaunch."),
                      nullptr, room.x - margin * 2.0f);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }

    // -- the grid and the sheet ------------------------------------------------
    const float  body_top = head_top + head_h + kit.px(24.0f);
    const float  body_bot = origin.y + room.y - foot_h - kit.px(6.0f);
    const float  gap      = kit.px(14.0f);
    const float  sheet_w  = std::floor(std::clamp(room.x * 0.30f, kit.px(240.0f), kit.px(380.0f)));
    const float  grid_w   = room.x - margin * 2.0f - sheet_w - gap;
    const float  tiles_w  = grid_w - ImGui::GetStyle().ScrollbarSize;  // the grid scrolls
    const ImVec2 grid_at(origin.x + margin, body_top);
    const ImVec2 sheet_at(grid_at.x + grid_w + gap, body_top);

    // Tiles: as many columns as keep a tile wide enough, 2 to 8.
    const float tile_gap = kit.px(12.0f);
    int         columns  = 2;
    for (int c = 8; c >= 2; --c) {
        if ((tiles_w - kit.px(8.0f) - static_cast<float>(c - 1) * tile_gap) / static_cast<float>(c) >= kit.px(132.0f)) {
            columns = c;
            break;
        }
    }
    const float tile_w  = std::floor((tiles_w - kit.px(8.0f) - static_cast<float>(columns - 1) * tile_gap) / static_cast<float>(columns));
    const float art_h   = std::floor(tile_w * 1.15f);
    const float cap_h   = std::round(kit.small() * 2.0f + kit.px(14.0f));
    const float tile_h  = art_h + cap_h;
    const float head_gh = kit.px(26.0f);  // a system's heading over its rows

    // Where each shown game's tile sits, with a heading before each system.
    struct Spot {
        int    column, row;
        ImVec2 pos;  // relative to the grid's content top-left
    };
    std::vector<Spot>                           spots(shown.size());
    std::vector<std::pair<std::string, float>> headings;  // system, y
    {
        float       y      = 0.0f;
        int         column = 0;
        int         row    = 0;
        std::string system;
        for (usize i = 0; i < shown.size(); ++i) {
            const PickerEntry& e = m_picker_entries[static_cast<usize>(shown[i])];
            if (grouped && e.system != system) {
                if (column != 0) {
                    y += tile_h + tile_gap;
                    column = 0;
                    ++row;
                }
                if (i != 0) y += kit.px(8.0f);
                headings.emplace_back(e.system, y);
                y += head_gh;
                system = e.system;
            }
            if (column == columns) {
                column = 0;
                ++row;
                y += tile_h + tile_gap;
            }
            spots[i] = {column, row, ImVec2(static_cast<float>(column) * (tile_w + tile_gap), y)};
            ++column;
        }
    }
    const float content_h = spots.empty() ? 0.0f : spots.back().pos.y + tile_h + kit.px(8.0f);

    // The selection, as a place in the shown list.
    int sel = -1;
    for (usize i = 0; i < shown.size(); ++i) {
        if (shown[i] == m_picker_selected) sel = static_cast<int>(i);
    }
    if (sel < 0 && !shown.empty()) {
        sel                    = 0;
        m_picker_selected      = shown[0];
        m_picker_scroll_to_sel = true;
    }

    // Keys and the D-pad move the choice over the tiles as they lie; Enter or
    // A plays. Not while the search field is typed in.
    const auto pressed = [&](ImGuiKey k) {
        return !m_load_error.has_value() && !io.WantTextInput && ImGui::IsKeyPressed(k, true);
    };
    if (sel >= 0) {
        int next = sel;
        if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight)) next = std::min(sel + 1, static_cast<int>(shown.size()) - 1);
        if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft)) next = std::max(sel - 1, 0);
        const int step = (pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown)) ? 1
                       : (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp))     ? -1
                                                                                           : 0;
        if (step != 0) {
            // The tile of the row next to it, in its column or the last of a
            // shorter row.
            const int to = spots[static_cast<usize>(sel)].row + step;
            int       best = -1;
            for (usize i = 0; i < spots.size(); ++i) {
                if (spots[i].row == to && (best < 0 || spots[i].column <= spots[static_cast<usize>(sel)].column)) best = static_cast<int>(i);
            }
            if (best >= 0) next = best;
        }
        if (next != sel) {
            sel                    = next;
            m_picker_selected      = shown[static_cast<usize>(sel)];
            m_picker_scroll        = 0.0f;
            m_picker_scroll_to_sel = true;
            m_picker_art_timer     = 0.0f;
            m_picker_art_index     = 0;
        }
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_GamepadFaceDown)) {
            m_pending_launch = m_picker_entries[static_cast<usize>(m_picker_selected)].name;
        }
        // Enter in the search plays the first match.
        if (io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
            m_pending_launch = m_picker_entries[static_cast<usize>(m_picker_selected)].name;
        }
    }

    constexpr float kArtDwellSeconds = 2.0f;
    m_picker_art_timer += io.DeltaTime;
    if (m_picker_art_timer >= kArtDwellSeconds) {
        m_picker_art_timer = 0.0f;
        ++m_picker_art_index;
    }

    // -- the grid --------------------------------------------------------------
    ImGui::SetCursorScreenPos(grid_at);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##grid", ImVec2(grid_w, body_bot - body_top), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImDrawList* grid = ImGui::GetWindowDrawList();
    const ImVec2 content(ImGui::GetCursorScreenPos().x + kit.px(4.0f), ImGui::GetCursorScreenPos().y + kit.px(4.0f));
    if (shown.empty()) {
        grid->AddText(ImVec2(content.x, content.y + kit.px(8.0f)), p.muted, tr("No game matches."));
    }
    for (const auto& [system, y] : headings) {
        kit.heading(grid, ImVec2(content.x + kit.px(2.0f), content.y + y + kit.px(4.0f)), p.faint, system.c_str());
    }
    for (usize i = 0; i < shown.size(); ++i) {
        PickerEntry& entry  = m_picker_entries[static_cast<usize>(shown[i])];
        const bool   is_sel = static_cast<int>(i) == sel;
        const ImVec2 p0(content.x + spots[i].pos.x, content.y + spots[i].pos.y);
        const ImVec2 p1(p0.x + tile_w, p0.y + tile_h);

        // Only tiles in or near view load their art and draw.
        const float view_top = ImGui::GetWindowPos().y - tile_h, view_bot = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() + tile_h;
        if (p1.y < view_top || p0.y > view_bot) {
            if (is_sel && m_picker_scroll_to_sel) {
                ImGui::SetScrollY(std::max(0.0f, spots[i].pos.y - (body_bot - body_top - tile_h) * 0.5f));
                m_picker_scroll_to_sel = false;
            }
            continue;
        }
        if (entry.art_state == PickerEntry::Art::Unknown && m_picker_backend != nullptr && m_picker_scraper != nullptr) {
            const Scraper::Metadata meta = m_picker_scraper->load_metadata(entry.name, entry.title);
            for (const std::string& image_path : meta.image_paths) {
                int                        iw = 0;
                int                        ih = 0;
                std::vector<unsigned char> rgba;
                if (decode_image_rgba(image_path, &iw, &ih, &rgba) && iw > 0 && ih > 0) {
                    const render::Backend::TextureHandle handle = m_picker_backend->create_texture(
                        static_cast<u32>(iw), static_cast<u32>(ih), rgba.data());
                    if (handle != 0) {
                        entry.art.push_back({handle, static_cast<float>(iw), static_cast<float>(ih)});
                    }
                }
            }
            entry.art_state = entry.art.empty() ? PickerEntry::Art::None : PickerEntry::Art::Loaded;
        }

        ImGui::PushID(shown[i]);
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##tile", ImVec2(tile_w, tile_h))) {
            if (m_picker_selected == shown[i]) {
                m_pending_launch = entry.name;
            } else {
                m_picker_selected  = shown[i];
                m_picker_scroll    = 0.0f;
                m_picker_art_timer = 0.0f;
                m_picker_art_index = 0;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_pending_launch = entry.name;
        }
        if (is_sel && m_picker_scroll_to_sel) {
            const float y_in = spots[i].pos.y - ImGui::GetScrollY();
            const float view = body_bot - body_top;
            if (y_in < 0.0f || y_in + tile_h > view) {
                ImGui::SetScrollY(std::max(0.0f, spots[i].pos.y - (view - tile_h) * 0.5f));
            }
            m_picker_scroll_to_sel = false;
        }
        ImGui::PopID();

        const float r = kit.px(6.0f);
        grid->AddRectFilled(p0, p1, hovered && !is_sel ? ui::mix(p.card, p.text, 0.06f) : p.card, r);
        const ImVec2 a0(p0.x + kit.px(4.0f), p0.y + kit.px(4.0f));
        const ImVec2 a1(p1.x - kit.px(4.0f), p0.y + art_h - kit.px(2.0f));
        if (entry.art_state == PickerEntry::Art::Loaded && !entry.art.empty() && m_picker_backend != nullptr) {
            // The highlighted game cycles its images; others show the first.
            int img = 0;
            if (is_sel && entry.art.size() > 1) img = m_picker_art_index % static_cast<int>(entry.art.size());
            const PickerEntry::Art1& art = entry.art[static_cast<usize>(img)];
            ImVec2 d0 = a0, d1 = a1;
            if (art.w > 0.0f && art.h > 0.0f) {
                const float aw = a1.x - a0.x, ah = a1.y - a0.y;
                const float k  = std::min(aw / art.w, ah / art.h);
                const float dw = std::floor(art.w * k), dh = std::floor(art.h * k);
                d0 = ImVec2(std::floor(a0.x + (aw - dw) * 0.5f), std::floor(a0.y + (ah - dh) * 0.5f));
                d1 = ImVec2(d0.x + dw, d0.y + dh);
            }
            grid->AddImageRounded(reinterpret_cast<ImTextureID>(m_picker_backend->texture_imgui_id(art.handle)), d0,
                                  d1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, kit.px(3.0f));
        } else {
            // No picture: the title large in the art's place, faintly.
            grid->AddRectFilled(a0, a1, ui::with_alpha(p.text, 0.03f), kit.px(3.0f));
            grid->PushClipRect(a0, a1, true);
            grid->AddText(ImGui::GetFont(), std::round(font * 1.15f), ImVec2(a0.x + kit.px(10.0f), a0.y + kit.px(10.0f)),
                          p.faint, entry.title.c_str(), nullptr, a1.x - a0.x - kit.px(20.0f));
            grid->PopClipRect();
        }
        // The caption: the title on up to two lines.
        grid->PushClipRect(ImVec2(p0.x, p0.y + art_h), p1, true);
        grid->AddText(ImGui::GetFont(), kit.small(), ImVec2(p0.x + kit.px(8.0f), p0.y + art_h + kit.px(6.0f)),
                      is_sel ? p.text : p.muted, entry.title.c_str(), nullptr, tile_w - kit.px(16.0f));
        grid->PopClipRect();
        if (is_sel) {
            kit.focus_ring(grid, p0, p1, r);
        } else {
            grid->AddRect(p0, p1, p.edge, r, 0, kit.stroke());
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(content.x, content.y + content_h));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    ImGui::EndChild();

    // -- the sheet: the game chosen ------------------------------------------
    {
        const ImVec2 s0 = sheet_at;
        const ImVec2 s1(sheet_at.x + sheet_w, body_bot);
        list->AddRectFilled(s0, s1, p.panel, kit.px(8.0f));
        list->AddRect(s0, s1, p.edge, kit.px(8.0f), 0, kit.stroke());
        if (sel >= 0) {
            const PickerEntry& cur  = m_picker_entries[static_cast<usize>(m_picker_selected)];
            const float        pad  = kit.px(16.0f);
            const float        wrap = sheet_w - pad * 2.0f;
            float              y    = s0.y + pad;
            const float        big  = std::round(font * 1.3f);
            list->AddText(ImGui::GetFont(), big, ImVec2(s0.x + pad, y), p.text, cur.title.c_str(), nullptr, wrap);
            y += ImGui::GetFont()->CalcTextSizeA(big, FLT_MAX, wrap, cur.title.c_str()).y + kit.px(6.0f);
            std::string sub = cur.manufacturer;
            if (!cur.year.empty()) sub += sub.empty() ? cur.year : "  ·  " + cur.year;
            if (!sub.empty()) {
                list->AddText(ImVec2(s0.x + pad, y), p.muted, sub.c_str());
                y += font + kit.px(2.0f);
            }
            const std::string set = cur.system.empty() ? cur.name : cur.system + "  ·  " + cur.name;
            list->AddText(ImGui::GetFont(), kit.small(), ImVec2(s0.x + pad, y), p.faint, set.c_str());
            y += kit.small() + kit.px(10.0f);
            list->AddLine(ImVec2(s0.x + pad, y), ImVec2(s1.x - pad, y), p.line, kit.stroke());
            y += kit.px(10.0f);

            // The Play button at the foot of the sheet; the description above it.
            const ImVec2 play_size = kit.button_size(tr("Play"));
            const ImVec2 play_at(s0.x + pad, s1.y - pad - play_size.y);
            if (kit.button_at(tr("Play"), play_at, ui::Kit::Tone::Primary)) {
                m_pending_launch = cur.name;
            }
            list->AddText(ImVec2(play_at.x + play_size.x + kit.px(10.0f), play_at.y + (play_size.y - font) * 0.5f), p.faint,
                          tr("Enter"));

            // Auto-scroll: hold at the top, creep up until fully past, hold, loop.
            // m_picker_scroll is seconds since the pick. Text that fits never scrolls.
            const float desc_h = play_at.y - kit.px(12.0f) - y;
            ImGui::SetCursorScreenPos(ImVec2(s0.x + pad, y));
            ImGui::BeginChild("##desc", ImVec2(wrap, std::max(desc_h, font)), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                  | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
            if (cur.description.empty()) {
                const bool still_fetching = !cur.metadata_loaded && m_picker_scraper != nullptr
                                         && m_picker_scraper->scraping_available();
                ImGui::TextDisabled("%s", still_fetching ? tr("Fetching description...") : tr("No description available."));
            } else {
                const float  avail_h = ImGui::GetContentRegionAvail().y;
                const ImVec2 size    = ImGui::CalcTextSize(cur.description.c_str(), nullptr, false, wrap);
                float        offset  = 0.0f;
                if (size.y > avail_h) {
                    constexpr float kHoldSeconds  = 2.5f;
                    constexpr float kPixelsPerSec = 22.0f;
                    const float     scroll_px     = (size.y - avail_h) + 24.0f;
                    const float     scroll_secs   = scroll_px / kPixelsPerSec;
                    const float     cycle         = kHoldSeconds + scroll_secs + kHoldSeconds;
                    m_picker_scroll += io.DeltaTime;
                    float t = m_picker_scroll;
                    if (t > cycle) {
                        t               = 0.0f;
                        m_picker_scroll = 0.0f;
                    }
                    if (t <= kHoldSeconds) {
                        offset = 0.0f;
                    } else if (t <= kHoldSeconds + scroll_secs) {
                        offset = (t - kHoldSeconds) * kPixelsPerSec;
                    } else {
                        offset = scroll_px;
                    }
                }
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offset);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.muted));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(cur.description.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace sm2::osd
