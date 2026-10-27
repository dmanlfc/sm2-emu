//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "core/config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sm2 {
namespace {

constexpr const char* kFileName = "sm2-emu.ini";

/// Mirrors render::kMaxRenderScale; kept local to avoid pulling in the render
/// backend header just for a clamp bound.
constexpr u32 kMaxRenderScale = 8;

[[nodiscard]] std::string trim(std::string_view text)
{
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(text.begin(), text.end(), not_space);
    auto end   = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

[[nodiscard]] std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Accepts every reasonable spelling so a hand-edited file is forgiving.
[[nodiscard]] bool parse_bool(const std::string& value, bool* out)
{
    const std::string text = lowered(value);
    if (text == "true" || text == "yes" || text == "on" || text == "1") {
        *out = true;
        return true;
    }
    if (text == "false" || text == "no" || text == "off" || text == "0") {
        *out = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_u32(const std::string& value, u32* out)
{
    std::istringstream stream(value);
    unsigned long      parsed = 0;
    stream >> parsed;
    if (stream.fail() || !stream.eof()) {
        return false;
    }
    *out = static_cast<u32>(parsed);
    return true;
}

/// Present-stage scaling method <-> ini token. Returns false on an unknown
/// spelling so the caller can report it and keep the default.
[[nodiscard]] bool parse_scaling_method(const std::string& value, ScalingMethod* out)
{
    const std::string text = lowered(value);
    if (text == "nearest") { *out = ScalingMethod::Nearest; return true; }
    if (text == "bilinear") { *out = ScalingMethod::Bilinear; return true; }
    if (text == "sharp" || text == "sharp-bilinear" || text == "sharp_bilinear") {
        *out = ScalingMethod::SharpBilinear;
        return true;
    }
    if (text == "integer") { *out = ScalingMethod::Integer; return true; }
    return false;
}

[[nodiscard]] const char* scaling_method_name(ScalingMethod method)
{
    switch (method) {
        case ScalingMethod::Nearest:       return "nearest";
        case ScalingMethod::Bilinear:      return "bilinear";
        case ScalingMethod::SharpBilinear: return "sharp";
        case ScalingMethod::Integer:       return "integer";
    }
    return "sharp";
}

[[nodiscard]] bool parse_aspect_mode(const std::string& value, AspectMode* out)
{
    const std::string text = lowered(value);
    if (text == "4:3" || text == "4-3" || text == "fourthree") {
        *out = AspectMode::FourThree;
        return true;
    }
    if (text == "square" || text == "square-pixel" || text == "squarepixel") {
        *out = AspectMode::SquarePixel;
        return true;
    }
    if (text == "stretch" || text == "fill") { *out = AspectMode::Stretch; return true; }
    if (text == "16:9" || text == "16-9" || text == "widescreen") {
        *out = AspectMode::Widescreen;
        return true;
    }
    return false;
}

[[nodiscard]] const char* aspect_mode_name(AspectMode mode)
{
    switch (mode) {
        case AspectMode::FourThree:   return "4:3";
        case AspectMode::SquarePixel: return "square";
        case AspectMode::Widescreen:  return "16:9";
        case AspectMode::Stretch:     return "stretch";
    }
    return "4:3";
}

[[nodiscard]] bool parse_texture_filter(const std::string& value, TextureFilter* out)
{
    const std::string text = lowered(value);
    if (text == "faithful") { *out = TextureFilter::Faithful; return true; }
    if (text == "anisotropic" || text == "aniso") {
        *out = TextureFilter::Anisotropic;
        return true;
    }
    return false;
}

[[nodiscard]] const char* texture_filter_name(TextureFilter filter)
{
    switch (filter) {
        case TextureFilter::Faithful:    return "faithful";
        case TextureFilter::Anisotropic: return "anisotropic";
    }
    return "faithful";
}

[[nodiscard]] bool parse_translucency(const std::string& value, Translucency* out)
{
    const std::string text = lowered(value);
    if (text == "stipple") { *out = Translucency::Stipple; return true; }
    if (text == "blended") { *out = Translucency::Blended; return true; }
    return false;
}

[[nodiscard]] const char* translucency_name(Translucency mode)
{
    return mode == Translucency::Blended ? "blended" : "stipple";
}

[[nodiscard]] bool parse_upscale_2d(const std::string& value, Upscale2D* out)
{
    const std::string text = lowered(value);
    if (text == "faithful") { *out = Upscale2D::Faithful; return true; }
    if (text == "xbr")      { *out = Upscale2D::Xbr;      return true; }
    if (text == "scalefx" || text == "scale-fx") { *out = Upscale2D::ScaleFx; return true; }
    return false;
}

[[nodiscard]] const char* upscale_2d_name(Upscale2D mode)
{
    switch (mode) {
        case Upscale2D::Faithful: return "faithful";
        case Upscale2D::Xbr:      return "xbr";
        case Upscale2D::ScaleFx:  return "scalefx";
    }
    return "faithful";
}

[[nodiscard]] constexpr usize cfg_role(Config::WheelRole role)
{
    return static_cast<usize>(role);
}

/// Gun-role names as they appear in the ini, in Config::GunRole order.
constexpr std::array<const char*, Config::kGunRoleCount> kGunRoleNames = {
    "trigger", "reload", "coin", "start",
    "hat_up", "hat_down", "hat_left", "hat_right",
};

/// Pad-role names as they appear in the ini, in Config::PadRole order.
constexpr std::array<const char*, Config::kPadRoleCount> kPadRoleNames = {
    "button1", "button2", "button3", "button4",
    "up", "down", "left", "right", "start", "coin", "gear_up", "gear_down",
};

/// Shortcut names in the ini, after "key_", in Config::Hotkey order.
constexpr std::array<const char*, Config::kHotkeyCount> kHotkeyNames = {
    "exit", "quit", "overlay", "fullscreen", "screenshot", "pause",
    "quick_save", "quick_load", "fps", "fast_forward", "rewind",
};

/// Pad analogue-axis role names in the ini, in Config::PadAxisRole order.
constexpr std::array<const char*, Config::kPadAxisCount> kPadAxisNames = {
    "aim_x", "aim_y", "accel", "brake", "lever_x", "lever_y",
};

[[nodiscard]] bool parse_s32(const std::string& value, s32* out)
{
    std::istringstream stream(value);
    long               parsed = 0;
    stream >> parsed;
    if (stream.fail() || !stream.eof()) {
        return false;
    }
    *out = static_cast<s32>(parsed);
    return true;
}

[[nodiscard]] const char* bool_text(bool value)
{
    return value ? "true" : "false";
}

[[nodiscard]] const char* environment(const char* name)
{
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

/// The platform's config directory. Not SDL's preferences path, which on Linux
/// is the XDG *data* directory; config belongs in the config directory.
[[nodiscard]] std::filesystem::path config_directory()
{
#if defined(_WIN32)
    if (const char* appdata = environment("APPDATA")) {
        return std::filesystem::path(appdata) / "sm2-emu";
    }
#elif defined(__APPLE__)
    if (const char* home = environment("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support"
             / "sm2-emu";
    }
#else
    if (const char* xdg = environment("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "sm2-emu";
    }
    if (const char* home = environment("HOME")) {
        return std::filesystem::path(home) / ".config" / "sm2-emu";
    }
#endif
    return std::filesystem::path();  // no home directory; caller falls back to cwd
}

}  // namespace

bool parse_log_level(const std::string& name, log::Level* out_level)
{
    const std::string text = lowered(name);
    if (text == "trace") {
        *out_level = log::Level::Trace;
    } else if (text == "debug") {
        *out_level = log::Level::Debug;
    } else if (text == "info") {
        *out_level = log::Level::Info;
    } else if (text == "warning" || text == "warn") {
        *out_level = log::Level::Warning;
    } else if (text == "error") {
        *out_level = log::Level::Error;
    } else {
        return false;
    }
    return true;
}

std::string default_config_path()
{
    // A file in the working directory wins, keeping a checked-out copy self-contained.
    std::error_code error;
    if (std::filesystem::exists(kFileName, error) && !error) {
        return kFileName;
    }

    const std::filesystem::path directory = config_directory();
    if (directory.empty()) {
        return kFileName;
    }
    return (directory / kFileName).string();
}

std::string data_directory(bool config_in_cwd)
{
    // A dev checkout keeps its data local so the tree stays self-contained.
    if (config_in_cwd) {
        return ".";
    }

#if defined(_WIN32)
    if (const char* appdata = environment("APPDATA")) {
        return (std::filesystem::path(appdata) / "sm2-emu").string();
    }
#elif defined(__APPLE__)
    if (const char* home = environment("HOME")) {
        return (std::filesystem::path(home) / "Library" / "Application Support"
                / "sm2-emu").string();
    }
#else
    // XDG_DATA_HOME, not the config dir's XDG_CONFIG_HOME.
    if (const char* xdg = environment("XDG_DATA_HOME")) {
        return (std::filesystem::path(xdg) / "sm2-emu").string();
    }
    if (const char* home = environment("HOME")) {
        return (std::filesystem::path(home) / ".local" / "share" / "sm2-emu").string();
    }
#endif
    // No home directory: fall back to the working directory.
    return ".";
}

void resolve_default_paths(Config* config, bool config_in_cwd,
                           const std::string& config_dir)
{
    const std::filesystem::path base = data_directory(config_in_cwd);
    if (config->nvram_dir.empty()) {
        config->nvram_dir = (base / "saves").string();
    }
    if (config->screenshot_dir.empty()) {
        config->screenshot_dir = (base / "screenshots").string();
    }
    // Save states live in a `states` subdirectory of the saves directory, so a
    // user who relocates their saves (--nvram / nvram_dir) takes their states
    // with them. Derived from the now-final nvram_dir; a runtime field only,
    // not read from or written to the ini.
    config->states_dir = (std::filesystem::path(config->nvram_dir) / "states").string();
    // Artwork always sits beside the config file; not user-adjustable, so set
    // unconditionally (never read from the ini).
    {
        const std::filesystem::path dir =
            config_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(config_dir);
        config->artwork_dir = (dir / "artwork").string();
    }
    // rom_dir intentionally left empty: no sensible default.
}

namespace {

/// The section holding the default gamepad layout.
constexpr const char* kDefaultPadSection = "gamepad";

/// The sections named after the overlay's tabs; any other is a game family's.
constexpr std::array<const char*, 8> kTabSections = {
    "video", "audio", "paths", "gamepad", "wheel", "lightgun", "network", "system",
};

/// What a pad1_*/pad2_* key did to `layout`.
enum class PadKey { NotPad, Unknown, BadValue, Set };

[[nodiscard]] bool is_pad_key(const std::string& key)
{
    return key.rfind("pad1_button_", 0) == 0 || key.rfind("pad2_button_", 0) == 0
        || key.rfind("pad1_axis_", 0) == 0 || key.rfind("pad2_axis_", 0) == 0;
}

[[nodiscard]] PadKey parse_pad_key(const std::string& key, const std::string& value,
                                   Config::PadLayout* layout)
{
    if (!is_pad_key(key)) {
        return PadKey::NotPad;
    }
    const bool is_button = key.compare(4, 8, "_button_") == 0;
    const usize player = key[3] == '2' ? 1 : 0;
    const auto  result = [](bool ok) { return ok ? PadKey::Set : PadKey::BadValue; };
    if (is_button) {
        const std::string role = key.substr(std::strlen("pad1_button_"));
        for (u32 r = 0; r < Config::kPadRoleCount; ++r) {
            if (role == kPadRoleNames[r]) {
                return result(parse_s32(value, &layout->buttons[player][r]));
            }
        }
        return PadKey::Unknown;
    }
    const std::string role = key.substr(std::strlen("pad1_axis_"));
    for (u32 r = 0; r < Config::kPadAxisCount; ++r) {
        if (role == kPadAxisNames[r]) {
            return result(parse_s32(value, &layout->axes[player][r]));
        }
        if (role == std::string(kPadAxisNames[r]) + "_invert") {
            return result(parse_bool(value, &layout->axis_invert[player][r]));
        }
        if (role == std::string(kPadAxisNames[r]) + "_button") {
            return result(parse_s32(value, &layout->axis_buttons[player][r]));
        }
    }
    return PadKey::Unknown;
}

void write_pad_layout(std::ostream& out, const Config::PadLayout& layout)
{
    for (u32 p = 0; p < 2; ++p) {
        for (u32 r = 0; r < Config::kPadRoleCount; ++r) {
            out << "pad" << (p + 1) << "_button_" << kPadRoleNames[r] << " = "
                << layout.buttons[p][r] << "\n";
        }
    }
    for (u32 p = 0; p < 2; ++p) {
        for (u32 r = 0; r < Config::kPadAxisCount; ++r) {
            const auto role     = static_cast<Config::PadAxisRole>(r);
            const bool is_pedal = role == Config::PadAxisRole::Accel || role == Config::PadAxisRole::Brake;
            const bool is_aim   = role == Config::PadAxisRole::AimX || role == Config::PadAxisRole::AimY;
            out << "pad" << (p + 1) << "_axis_" << kPadAxisNames[r] << " = "
                << layout.axes[p][r] << "\n";
            if (is_pedal || is_aim) {
                out << "pad" << (p + 1) << "_axis_" << kPadAxisNames[r] << "_invert = "
                    << bool_text(layout.axis_invert[p][r]) << "\n";
            }
            if (is_pedal) {
                out << "pad" << (p + 1) << "_axis_" << kPadAxisNames[r] << "_button = "
                    << layout.axis_buttons[p][r] << "\n";
            }
        }
    }
}

}  // namespace

bool load_config(const std::string& path, Config* out, std::vector<std::string>* problems)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return true;  // nothing to load is not a failure
    }

    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string line;
    u32         number = 0;
    std::string section;  // the [section] a line is in, empty before one

    struct FamilyKey {
        std::string family, key, value;
        u32         line;
    };
    std::vector<FamilyKey> family_keys;
    while (std::getline(file, line)) {
        ++number;

        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        if (trimmed[0] == '[') {
            const usize close = trimmed.find(']');
            section = lowered(trim(trimmed.substr(1, close == std::string::npos
                                                         ? std::string::npos : close - 1)));
            continue;
        }

        const usize separator = trimmed.find('=');
        if (separator == std::string::npos) {
            problems->push_back(path + ":" + std::to_string(number)
                                + ": expected key = value");
            continue;
        }

        const std::string key   = lowered(trim(trimmed.substr(0, separator)));
        const std::string value = trim(trimmed.substr(separator + 1));

        const auto bad_value = [&]() {
            problems->push_back(path + ":" + std::to_string(number) + ": '" + key
                                + "' does not accept '" + value + "'");
        };

        const bool family_section =
            !section.empty()
            && std::find(kTabSections.begin(), kTabSections.end(), section) == kTabSections.end();
        if (family_section && is_pad_key(key)) {
            // Applied over the default once the whole file is read, so a game's
            // section may be partial and may come before [gamepad].
            family_keys.push_back({section, key, value, number});
            continue;
        }
        if (key.rfind("key_", 0) == 0) {
            const auto name = std::find_if(kHotkeyNames.begin(), kHotkeyNames.end(),
                                           [&](const char* n) { return key.substr(4) == n; });
            if (name != kHotkeyNames.end()) {
                out->hotkeys[static_cast<usize>(name - kHotkeyNames.begin())] = value;
                continue;
            }
        }
        if (const PadKey pad = parse_pad_key(key, value, &out->pad); pad != PadKey::NotPad) {
            if (pad == PadKey::BadValue) {
                bad_value();
            } else if (pad == PadKey::Unknown) {
                problems->push_back(path + ":" + std::to_string(number)
                                    + ": unknown setting '" + key + "'");
            }
            continue;
        }
        if (key == "vsync") {
            if (!parse_bool(value, &out->vsync)) {
                bad_value();
            }
        } else if (key == "throttle") {
            if (!parse_bool(value, &out->throttle)) {
                bad_value();
            }
        } else if (key == "fullscreen") {
            if (!parse_bool(value, &out->fullscreen)) {
                bad_value();
            }
        } else if (key == "show_fps") {
            if (!parse_bool(value, &out->show_fps)) {
                bad_value();
            }
        } else if (key == "software_async") {
            if (!parse_bool(value, &out->software_async)) {
                bad_value();
            }
        } else if (key == "software_slow_cores") {
            if (!parse_bool(value, &out->software_slow_cores)) {
                bad_value();
            }
        } else if (key == "language") {
            out->language = value.empty() ? "auto" : value;
        } else if (key == "show_notifications") {
            if (!parse_bool(value, &out->show_notifications)) {
                bad_value();
            }
        } else if (key == "rewind") {
            if (!parse_bool(value, &out->rewind)) {
                bad_value();
            }
        } else if (key == "rewind_buffer_mb") {
            if (!parse_u32(value, &out->rewind_buffer_mb)) {
                bad_value();
            }
        } else if (key == "lightgun") {
            if (!parse_bool(value, &out->lightgun)) {
                bad_value();
            }
        } else if (key == "lightgun_crosshair") {
            if (!parse_bool(value, &out->lightgun_crosshair)) {
                bad_value();
            }
        } else if (key == "lightgun_pad_centred") {
            if (!parse_bool(value, &out->lightgun_pad_centred)) {
                bad_value();
            }
        } else if (key == "lightgun_hide_flash") {
            if (!parse_bool(value, &out->lightgun_hide_flash)) {
                bad_value();
            }
        } else if (key == "lightgun_recoil") {
            if (!parse_bool(value, &out->lightgun_recoil)) {
                bad_value();
            }
        } else if (key == "lightgun_recoil_strength") {
            if (!parse_u32(value, &out->lightgun_recoil_strength)) {
                bad_value();
            }
        } else if (key == "sinden_border") {
            if (!parse_bool(value, &out->sinden_border)) {
                bad_value();
            }
        } else if (key == "sinden_border_colour") {
            // Accepts hex (0xRRGGBB) or decimal.
            const int base = value.rfind("0x", 0) == 0 ? 16 : 10;
            char*     end  = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, base);
            if (end == value.c_str() || *end != '\0') {
                bad_value();
            } else {
                out->sinden_border_colour = static_cast<u32>(parsed) & 0xffffff;
            }
        } else if (key == "sinden_border_thickness") {
            if (!parse_u32(value, &out->sinden_border_thickness)) {
                bad_value();
            }

        } else if (key == "validation") {
            if (!parse_bool(value, &out->validation)) {
                bad_value();
            }
        } else if (key == "window_width") {
            if (!parse_u32(value, &out->window_width)) {
                bad_value();
            }
        } else if (key == "window_height") {
            if (!parse_u32(value, &out->window_height)) {
                bad_value();
            }
        } else if (key == "render_scale") {
            u32 scale = 1;
            if (!parse_u32(value, &scale)) {
                bad_value();
            } else if (scale < 1 || scale > kMaxRenderScale) {
                // An out-of-range value is reported but clamped rather than
                // rejected, so a file from a future version still starts.
                problems->push_back(path + ":" + std::to_string(number) + ": '" + key
                                    + "' out of range 1.." + std::to_string(kMaxRenderScale)
                                    + "; using " + std::to_string(std::clamp(scale, 1u, kMaxRenderScale)));
                out->render_scale = std::clamp(scale, 1u, kMaxRenderScale);
            } else {
                out->render_scale = scale;
            }
        } else if (key == "scaling_method") {
            if (!parse_scaling_method(value, &out->scaling_method)) {
                bad_value();
            }
        } else if (key == "aspect_mode") {
            if (!parse_aspect_mode(value, &out->aspect_mode)) {
                bad_value();
            }
        } else if (key == "crt_enabled") {
            if (!parse_bool(value, &out->crt_enabled)) {
                bad_value();
            }
        } else if (key == "crt_scanline_strength") {
            if (!parse_u32(value, &out->crt_scanline_strength)) {
                bad_value();
            }
        } else if (key == "crt_mask_strength") {
            if (!parse_u32(value, &out->crt_mask_strength)) {
                bad_value();
            }
        } else if (key == "crt_glow_strength") {
            if (!parse_u32(value, &out->crt_glow_strength)) {
                bad_value();
            }
        } else if (key == "crt_curvature") {
            if (!parse_u32(value, &out->crt_curvature)) {
                bad_value();
            }
        } else if (key == "bezel") {
            if (!parse_bool(value, &out->bezel)) {
                bad_value();
            }
        } else if (key == "texture_filter") {
            if (!parse_texture_filter(value, &out->texture_filter)) {
                bad_value();
            }
        } else if (key == "anisotropy") {
            if (!parse_u32(value, &out->anisotropy)) {
                bad_value();
            }
        } else if (key == "upscale_2d") {
            if (!parse_upscale_2d(value, &out->upscale_2d)) {
                bad_value();
            }
        } else if (key == "translucency") {
            if (!parse_translucency(value, &out->translucency)) {
                bad_value();
            }
        } else if (key == "extended_draw_distance") {
            if (!parse_bool(value, &out->extended_draw_distance)) {
                bad_value();
            }
        } else if (key == "custom_textures") {
            if (!parse_bool(value, &out->custom_textures)) {
                bad_value();
            }
        } else if (key == "dump_textures") {
            if (!parse_bool(value, &out->dump_textures)) {
                bad_value();
            }
        } else if (key == "gpu") {
            out->gpu = value;
        } else if (key == "graphics_backend") {
            const std::string choice = lowered(value);
            if (choice.empty() || choice == "software" || choice == "vulkan"
                || choice == "opengl") {
                out->graphics_backend = choice;
            } else {
                bad_value();
            }
        } else if (key == "link_enabled") {
            if (!parse_bool(value, &out->link_enabled)) {
                bad_value();
            }
        } else if (key == "link_local_ip") {
            out->link_local_ip = value;
        } else if (key == "link_subnet_mask") {
            out->link_subnet_mask = value;
        } else if (key == "link_port") {
            if (!parse_u32(value, &out->link_port)) {
                bad_value();
            }
        } else if (key == "link_next_ip") {
            out->link_next_ip = value;
        } else if (key == "link_next_port") {
            if (!parse_u32(value, &out->link_next_port)) {
                bad_value();
            }
        } else if (key == "link_cabinet_index") {
            if (!parse_u32(value, &out->link_cabinet_index)) {
                bad_value();
            }
        } else if (key == "wheel_ffb") {
            if (!parse_bool(value, &out->wheel_ffb)) {
                bad_value();
            }
        } else if (key == "wheel_ffb_strength") {
            if (!parse_u32(value, &out->wheel_ffb_strength)) {
                bad_value();
            }
        } else if (key == "wheel_panel_spring") {
            if (!parse_u32(value, &out->wheel_panel_spring)) {
                bad_value();
            }
        } else if (key == "wheel_indy_deadband") {
            if (!parse_u32(value, &out->wheel_indy_deadband)) {
                bad_value();
            }
        } else if (key == "wheel_indy_full_power") {
            if (!parse_u32(value, &out->wheel_indy_full_power)) {
                bad_value();
            }
        } else if (key == "wheel_drive_dip") {
            if (!parse_u32(value, &out->wheel_drive_dip)) {
                bad_value();
            }
        } else if (key == "input_background") {
            if (!parse_bool(value, &out->input_background)) {
                bad_value();
            }
        } else if (key == "wheel_ffb_invert") {
            if (!parse_bool(value, &out->wheel_ffb_invert)) {
                bad_value();
            }
        } else if (key == "wheel_rumble") {
            if (!parse_bool(value, &out->wheel_rumble)) {
                bad_value();
            }
        } else if (key == "wheel_rumble_strength") {
            if (!parse_u32(value, &out->wheel_rumble_strength)) {
                bad_value();
            }
        } else if (key == "wheel_rumble_engine") {
            if (!parse_bool(value, &out->wheel_rumble_engine)) {
                bad_value();
            }
        } else if (key == "pad_rumble") {
            if (!parse_bool(value, &out->pad_rumble)) {
                bad_value();
            }
        } else if (key == "outputs_network") {
            if (!parse_bool(value, &out->outputs_network)) {
                bad_value();
            }
        } else if (key == "outputs_network_port") {
            if (!parse_u32(value, &out->outputs_network_port)) {
                bad_value();
            }
        } else if (key == "outputs_windows") {
            if (!parse_bool(value, &out->outputs_windows)) {
                bad_value();
            }
        } else if (key == "pad_rumble_strength") {
            if (!parse_u32(value, &out->pad_rumble_strength)) {
                bad_value();
            }
        } else if (key == "pad_rumble_cornering") {
            if (!parse_bool(value, &out->pad_rumble_cornering)) {
                bad_value();
            }
        } else if (key == "pad_stick_sensitivity") {
            if (!parse_u32(value, &out->pad_stick_sensitivity)) {
                bad_value();
            }
        } else if (key == "pad_shortcuts") {
            if (!parse_bool(value, &out->pad_shortcuts)) {
                bad_value();
            }
        } else if (key == "pad1_device") {
            out->pad_device[0] = value;
        } else if (key == "pad2_device") {
            out->pad_device[1] = value;
        } else if (key == "pad_stick_deadzone") {
            if (!parse_u32(value, &out->pad_stick_deadzone)) {
                bad_value();
            }
        } else if (key == "wheel_lock_degrees") {
            if (!parse_u32(value, &out->wheel_lock_degrees)) {
                bad_value();
            }
        } else if (key == "wheel_steer_deadzone") {
            if (!parse_u32(value, &out->wheel_steer_deadzone)) {
                bad_value();
            }
        } else if (key == "wheel_steer_degrees") {
            if (!parse_u32(value, &out->wheel_steer_degrees)) {
                bad_value();
            }
        } else if (key == "wheel_button_start") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Start)])) {
                bad_value();
            }
        } else if (key == "wheel_button_coin") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Coin)])) {
                bad_value();
            }
        } else if (key == "wheel_button_1") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Button1)])) {
                bad_value();
            }
        } else if (key == "wheel_button_2") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Button2)])) {
                bad_value();
            }
        } else if (key == "wheel_button_3") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Button3)])) {
                bad_value();
            }
        } else if (key == "wheel_button_4") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Button4)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_up") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::GearUp)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_down") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::GearDown)])) {
                bad_value();
            }
        } else if (key == "wheel_button_test") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Test)])) {
                bad_value();
            }
        } else if (key == "wheel_button_service") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Service)])) {
                bad_value();
            }
        } else if (key == "wheel_button_menu") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Menu)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_1") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Gear1)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_2") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Gear2)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_3") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Gear3)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_4") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::Gear4)])) {
                bad_value();
            }
        } else if (key == "wheel_button_gear_neutral") {
            if (!parse_s32(value, &out->wheel_buttons[cfg_role(Config::WheelRole::GearNeutral)])) {
                bad_value();
            }
        } else if (key == "wheel_shifter_neutral") {
            if (!parse_bool(value, &out->wheel_shifter_neutral)) {
                bad_value();
            }
        } else if (key == "wheel_shift_neutral_ms") {
            if (!parse_u32(value, &out->wheel_shift_neutral_ms)) {
                bad_value();
            }
        } else if (key == "wheel_steer_axis") {
            if (!parse_s32(value, &out->wheel_steer_axis)) {
                bad_value();
            }
        } else if (key == "wheel_accel_axis") {
            if (!parse_s32(value, &out->wheel_accel_axis)) {
                bad_value();
            }
        } else if (key == "wheel_brake_axis") {
            if (!parse_s32(value, &out->wheel_brake_axis)) {
                bad_value();
            }
        } else if (key == "wheel_accel_invert") {
            if (!parse_bool(value, &out->wheel_accel_invert)) {
                bad_value();
            }
        } else if (key == "wheel_brake_invert") {
            if (!parse_bool(value, &out->wheel_brake_invert)) {
                bad_value();
            }
        } else if (key == "wheel_accel_half") {
            if (!parse_bool(value, &out->wheel_accel_half)) {
                bad_value();
            }
        } else if (key == "wheel_brake_half") {
            if (!parse_bool(value, &out->wheel_brake_half)) {
                bad_value();
            }
        } else if (key == "gun1_device" || key == "gun2_device") {
            out->gun_devices[key[3] == '2' ? 1 : 0] = value;
        } else if (key.rfind("gun1_button_", 0) == 0 || key.rfind("gun2_button_", 0) == 0) {
            const usize player = key[3] == '2' ? 1 : 0;
            const std::string role = key.substr(std::strlen("gun1_button_"));
            bool matched = false;
            for (u32 r = 0; r < Config::kGunRoleCount; ++r) {
                if (role == kGunRoleNames[r]) {
                    if (!parse_u32(value, &out->gun_buttons[player][r])) bad_value();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                problems->push_back(path + ":" + std::to_string(number)
                                    + ": unknown setting '" + key + "'");
            }
        } else if (key == "game_volume") {
            if (!parse_bool(value, &out->game_volume)) {
                bad_value();
            }
        } else if (key.rfind("volume_", 0) == 0 && key.size() > 7) {
            u32 percent = 0;
            if (parse_u32(value, &percent)) {
                out->game_volumes[key.substr(7)] =
                    std::min(percent, Config::kMaxGameVolume);
            } else {
                bad_value();
            }
        } else if (key == "rom_dir") {
            out->rom_dir = value;
        } else if (key == "nvram_dir") {
            out->nvram_dir = value;
        } else if (key == "screenshot_dir") {
            out->screenshot_dir = value;
        } else if (key == "bezel_dir") {
            out->bezel_dir = value;
        } else if (key == "crosshair_dir") {
            out->crosshair_dir = value;
        } else if (key == "scrape_artwork") {
            if (!parse_bool(value, &out->scrape_artwork)) {
                bad_value();
            }
        } else if (key == "log_level") {
            log::Level level = log::Level::Info;
            if (parse_log_level(value, &level)) {
                out->log_level = lowered(value);
            } else {
                bad_value();
            }
        } else {
            // Reported but not fatal. A file written by a later version must not
            // stop this one from starting.
            problems->push_back(path + ":" + std::to_string(number)
                                + ": unknown setting '" + key + "'");
        }
    }

    for (const FamilyKey& entry : family_keys) {
        Config::PadLayout& layout = out->pad_layouts.try_emplace(entry.family, out->pad).first->second;
        const PadKey pad = parse_pad_key(entry.key, entry.value, &layout);
        if (pad == PadKey::BadValue) {
            problems->push_back(path + ":" + std::to_string(entry.line) + ": '" + entry.key
                                + "' does not accept '" + entry.value + "'");
        } else if (pad == PadKey::Unknown) {
            problems->push_back(path + ":" + std::to_string(entry.line)
                                + ": unknown setting '" + entry.key + "'");
        }
    }

    // A window smaller than the raster is not useful and a zero one is not valid.
    out->window_width  = std::max(out->window_width, 256u);
    out->window_height = std::max(out->window_height, 192u);
    out->wheel_ffb_strength    = std::min(out->wheel_ffb_strength, 200u);
    out->wheel_panel_spring    = std::min(out->wheel_panel_spring, 100u);
    out->wheel_indy_full_power = std::clamp(out->wheel_indy_full_power, 1u, 63u);
    out->wheel_indy_deadband   = std::min(out->wheel_indy_deadband, out->wheel_indy_full_power - 1);
    out->wheel_drive_dip       = std::min(out->wheel_drive_dip, 7u);
    out->wheel_shift_neutral_ms = std::min(out->wheel_shift_neutral_ms, 500u);
    out->wheel_rumble_strength = std::min(out->wheel_rumble_strength, 100u);
    out->pad_rumble_strength = std::min(out->pad_rumble_strength, 100u);
    out->pad_stick_sensitivity = std::clamp(out->pad_stick_sensitivity, 25u, 300u);
    out->pad_stick_deadzone    = std::min(out->pad_stick_deadzone, 40u);
    out->wheel_steer_deadzone  = std::min(out->wheel_steer_deadzone, 20u);
    if (out->outputs_network_port == 0 || out->outputs_network_port > 65535) {
        out->outputs_network_port = 8000;
    }
    // A sane rotation range: tight enough to be usable, and never zero (which
    // would divide by zero when scaling the steering).
    out->wheel_steer_degrees = std::clamp(out->wheel_steer_degrees, 90u, 1080u);
    out->wheel_lock_degrees  = std::clamp(out->wheel_lock_degrees, 180u, 270u);
    out->render_scale        = std::clamp(out->render_scale, 1u, kMaxRenderScale);
    out->crt_scanline_strength = std::min(out->crt_scanline_strength, 100u);
    out->crt_mask_strength     = std::min(out->crt_mask_strength, 100u);
    out->crt_glow_strength     = std::min(out->crt_glow_strength, 100u);
    out->crt_curvature         = std::min(out->crt_curvature, 100u);
    // Anisotropy is a tap ceiling; clamp to a sane 1..16 here, and the backend
    // further clamps to the device's maxSamplerAnisotropy at use time.
    out->anisotropy            = std::clamp(out->anisotropy, 1u, 16u);
    // A UDP port is 16-bit; floor at 1 since 0 binds an ephemeral port.
    out->link_port      = std::clamp(out->link_port, 1u, 65535u);
    out->link_next_port = std::clamp(out->link_next_port, 1u, 65535u);
    return true;
}

bool save_config(const std::string& path, const Config& config)
{
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }

    const auto wheel_button = [&](Config::WheelRole role) {
        return config.wheel_buttons[cfg_role(role)];
    };

    out << "# sm2-emu settings\n"
        << "\n[video]\n"
        << "graphics_backend = " << config.graphics_backend << "\n"
        << "gpu = " << config.gpu << "\n"
        << "vsync = " << bool_text(config.vsync) << "\n"
        << "throttle = " << bool_text(config.throttle) << "\n"
        << "fullscreen = " << bool_text(config.fullscreen) << "\n"
        << "window_width = " << config.window_width << "\n"
        << "window_height = " << config.window_height << "\n"
        << "show_fps = " << bool_text(config.show_fps) << "\n"
        << "show_notifications = " << bool_text(config.show_notifications) << "\n"
        << "render_scale = " << config.render_scale << "\n"
        << "scaling_method = " << scaling_method_name(config.scaling_method) << "\n"
        << "aspect_mode = " << aspect_mode_name(config.aspect_mode) << "\n"
        << "bezel = " << bool_text(config.bezel) << "\n"
        << "crt_enabled = " << bool_text(config.crt_enabled) << "\n"
        << "crt_scanline_strength = " << config.crt_scanline_strength << "\n"
        << "crt_mask_strength = " << config.crt_mask_strength << "\n"
        << "crt_glow_strength = " << config.crt_glow_strength << "\n"
        << "crt_curvature = " << config.crt_curvature << "\n"
        << "texture_filter = " << texture_filter_name(config.texture_filter) << "\n"
        << "anisotropy = " << config.anisotropy << "\n"
        << "upscale_2d = " << upscale_2d_name(config.upscale_2d) << "\n"
        << "translucency = " << translucency_name(config.translucency) << "\n"
        << "extended_draw_distance = " << bool_text(config.extended_draw_distance) << "\n"
        << "custom_textures = " << bool_text(config.custom_textures) << "\n"
        << "dump_textures = " << bool_text(config.dump_textures) << "\n"
        << "software_async = " << bool_text(config.software_async) << "\n"
        << "software_slow_cores = " << bool_text(config.software_slow_cores) << "\n"
        << "\n[audio]\n"
        << "game_volume = " << bool_text(config.game_volume) << "\n";
    for (const auto& [family, percent] : config.game_volumes) {
        if (percent != Config::kDefaultGameVolume) {
            out << "volume_" << family << " = " << percent << "\n";
        }
    }
    out << "\n[paths]\n"
        << "rom_dir = " << config.rom_dir << "\n"
        << "nvram_dir = " << config.nvram_dir << "\n"
        << "screenshot_dir = " << config.screenshot_dir << "\n"
        << "bezel_dir = " << config.bezel_dir << "\n"
        << "crosshair_dir = " << config.crosshair_dir << "\n"
        << "scrape_artwork = " << bool_text(config.scrape_artwork) << "\n"
        << "\n[wheel]\n"
        << "wheel_steer_degrees = " << config.wheel_steer_degrees << "\n"
        << "wheel_lock_degrees = " << config.wheel_lock_degrees << "\n"
        << "wheel_steer_deadzone = " << config.wheel_steer_deadzone << "\n"
        << "wheel_steer_axis = " << config.wheel_steer_axis << "\n"
        << "wheel_accel_axis = " << config.wheel_accel_axis << "\n"
        << "wheel_brake_axis = " << config.wheel_brake_axis << "\n"
        << "wheel_accel_invert = " << bool_text(config.wheel_accel_invert) << "\n"
        << "wheel_brake_invert = " << bool_text(config.wheel_brake_invert) << "\n"
        << "wheel_accel_half = " << bool_text(config.wheel_accel_half) << "\n"
        << "wheel_brake_half = " << bool_text(config.wheel_brake_half) << "\n"
        << "wheel_ffb = " << bool_text(config.wheel_ffb) << "\n"
        << "wheel_ffb_strength = " << config.wheel_ffb_strength << "\n"
        << "wheel_ffb_invert = " << bool_text(config.wheel_ffb_invert) << "\n"
        << "wheel_panel_spring = " << config.wheel_panel_spring << "\n"
        << "wheel_indy_deadband = " << config.wheel_indy_deadband << "\n"
        << "wheel_indy_full_power = " << config.wheel_indy_full_power << "\n"
        << "wheel_drive_dip = " << config.wheel_drive_dip << "\n"
        << "wheel_rumble = " << bool_text(config.wheel_rumble) << "\n"
        << "wheel_rumble_strength = " << config.wheel_rumble_strength << "\n"
        << "wheel_rumble_engine = " << bool_text(config.wheel_rumble_engine) << "\n"
        << "wheel_shifter_neutral = " << bool_text(config.wheel_shifter_neutral) << "\n"
        << "wheel_shift_neutral_ms = " << config.wheel_shift_neutral_ms << "\n"
        << "wheel_button_start = " << wheel_button(Config::WheelRole::Start) << "\n"
        << "wheel_button_coin = " << wheel_button(Config::WheelRole::Coin) << "\n"
        << "wheel_button_1 = " << wheel_button(Config::WheelRole::Button1) << "\n"
        << "wheel_button_2 = " << wheel_button(Config::WheelRole::Button2) << "\n"
        << "wheel_button_3 = " << wheel_button(Config::WheelRole::Button3) << "\n"
        << "wheel_button_4 = " << wheel_button(Config::WheelRole::Button4) << "\n"
        << "wheel_button_gear_up = " << wheel_button(Config::WheelRole::GearUp) << "\n"
        << "wheel_button_gear_down = " << wheel_button(Config::WheelRole::GearDown) << "\n"
        << "wheel_button_gear_1 = " << wheel_button(Config::WheelRole::Gear1) << "\n"
        << "wheel_button_gear_2 = " << wheel_button(Config::WheelRole::Gear2) << "\n"
        << "wheel_button_gear_3 = " << wheel_button(Config::WheelRole::Gear3) << "\n"
        << "wheel_button_gear_4 = " << wheel_button(Config::WheelRole::Gear4) << "\n"
        << "wheel_button_gear_neutral = " << wheel_button(Config::WheelRole::GearNeutral) << "\n"
        << "wheel_button_test = " << wheel_button(Config::WheelRole::Test) << "\n"
        << "wheel_button_service = " << wheel_button(Config::WheelRole::Service) << "\n"
        << "wheel_button_menu = " << wheel_button(Config::WheelRole::Menu) << "\n"
        << "\n[lightgun]\n"
        << "lightgun = " << bool_text(config.lightgun) << "\n"
        << "lightgun_crosshair = " << bool_text(config.lightgun_crosshair) << "\n"
        << "lightgun_hide_flash = " << bool_text(config.lightgun_hide_flash) << "\n"
        << "lightgun_pad_centred = " << bool_text(config.lightgun_pad_centred) << "\n"
        << "lightgun_recoil = " << bool_text(config.lightgun_recoil) << "\n"
        << "lightgun_recoil_strength = " << config.lightgun_recoil_strength << "\n"
        << "sinden_border = " << bool_text(config.sinden_border) << "\n"
        << "sinden_border_colour = 0x" << std::hex << std::setw(6)
        << std::setfill('0') << (config.sinden_border_colour & 0xffffff) << std::dec
        << std::setfill(' ') << "\n"
        << "sinden_border_thickness = " << config.sinden_border_thickness << "\n";
    for (u32 p = 0; p < 2; ++p) {
        for (u32 r = 0; r < Config::kGunRoleCount; ++r) {
            out << "gun" << (p + 1) << "_button_" << kGunRoleNames[r] << " = "
                << config.gun_buttons[p][r] << "\n";
        }
    }
    out << "gun1_device = " << config.gun_devices[0] << "\n"
        << "gun2_device = " << config.gun_devices[1] << "\n";
    out << "\n[network]\n"
        << "link_enabled = " << bool_text(config.link_enabled) << "\n"
        << "link_local_ip = " << config.link_local_ip << "\n"
        << "link_subnet_mask = " << config.link_subnet_mask << "\n"
        << "link_port = " << config.link_port << "\n"
        << "link_next_ip = " << config.link_next_ip << "\n"
        << "link_next_port = " << config.link_next_port << "\n"
        << "link_cabinet_index = " << config.link_cabinet_index << "\n"
        << "outputs_network = " << bool_text(config.outputs_network) << "\n"
        << "outputs_network_port = " << config.outputs_network_port << "\n"
        << "outputs_windows = " << bool_text(config.outputs_windows) << "\n"
        << "\n[system]\n"
        << "language = " << config.language << "\n"
        << "input_background = " << bool_text(config.input_background) << "\n"
        << "rewind = " << bool_text(config.rewind) << "\n"
        << "rewind_buffer_mb = " << config.rewind_buffer_mb << "\n";
    for (u32 h = 0; h < Config::kHotkeyCount; ++h) {
        out << "key_" << kHotkeyNames[h] << " = " << config.hotkeys[h] << "\n";
    }
    out << "log_level = " << config.log_level << "\n"
        << "validation = " << bool_text(config.validation) << "\n";

    // Game sections run to the end of the file, so they and [gamepad] come last.
    out << "\n# A [family] section after [gamepad], named after the parent set,\n"
        << "# replaces its bindings for every version of that game.\n"
        << "\n[gamepad]\n"
        << "pad_rumble = " << bool_text(config.pad_rumble) << "\n"
        << "pad_rumble_strength = " << config.pad_rumble_strength << "\n"
        << "pad_rumble_cornering = " << bool_text(config.pad_rumble_cornering) << "\n"
        << "pad_stick_sensitivity = " << config.pad_stick_sensitivity << "\n"
        << "pad_stick_deadzone = " << config.pad_stick_deadzone << "\n"
        << "pad_shortcuts = " << bool_text(config.pad_shortcuts) << "\n"
        << "pad1_device = " << config.pad_device[0] << "\n"
        << "pad2_device = " << config.pad_device[1] << "\n";
    write_pad_layout(out, config.pad);
    for (const auto& [family, layout] : config.pad_layouts) {
        out << "\n[" << family << "]\n";
        write_pad_layout(out, layout);
    }

    return out.good();
}

}  // namespace sm2
