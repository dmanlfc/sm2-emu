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
#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sm2 {
namespace {

constexpr const char* kFileName = "sm2-emu.ini";

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

/// Accepts every spelling a person might reasonably write, because rejecting `yes`
/// in a hand-edited file is not a helpful thing to do.
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

[[nodiscard]] constexpr usize cfg_role(Config::WheelRole role)
{
    return static_cast<usize>(role);
}

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

/// The platform's directory for a program's configuration.
///
/// Written out rather than taken from SDL because SDL offers only its preferences
/// path, which on Linux is the XDG *data* directory. Configuration belongs in the
/// config directory, and a user who has moved theirs expects that to be honoured.
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
    // No home directory at all. The working directory is the only place left.
    return std::filesystem::path();
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
    // A file beside the binary's working directory wins. That is what a build tree
    // wants, and it makes a checked-out copy self-contained.
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
    while (std::getline(file, line)) {
        ++number;

        // Section headers are accepted and ignored: there is only one group of
        // settings, but a file that has grown one should still load.
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';'
            || trimmed[0] == '[') {
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
        } else if (key == "lightgun") {
            if (!parse_bool(value, &out->lightgun)) {
                bad_value();
            }
        } else if (key == "lightgun_crosshair") {
            if (!parse_bool(value, &out->lightgun_crosshair)) {
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
        } else if (key == "gpu") {
            out->gpu = value;
        } else if (key == "wheel_ffb") {
            if (!parse_bool(value, &out->wheel_ffb)) {
                bad_value();
            }
        } else if (key == "wheel_ffb_strength") {
            if (!parse_u32(value, &out->wheel_ffb_strength)) {
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
        } else if (key == "wheel_lock_degrees") {
            if (!parse_u32(value, &out->wheel_lock_degrees)) {
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
        } else if (key == "nvram_dir") {
            out->nvram_dir = value;
        } else if (key == "games_xml") {
            out->games_xml = value;
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

    // A window smaller than the raster is not useful and a zero one is not valid.
    out->window_width  = std::max(out->window_width, 256u);
    out->window_height = std::max(out->window_height, 192u);
    out->wheel_ffb_strength    = std::min(out->wheel_ffb_strength, 100u);
    out->wheel_rumble_strength = std::min(out->wheel_rumble_strength, 100u);
    // A sane rotation range: tight enough to be usable, and never zero (which
    // would divide by zero when scaling the steering).
    out->wheel_steer_degrees = std::clamp(out->wheel_steer_degrees, 90u, 1080u);
    out->wheel_lock_degrees  = std::clamp(out->wheel_lock_degrees, 180u, 270u);
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

    out << "# sm2-emu settings.\n"
        << "#\n"
        << "# Every setting below is at its default. Anything given on the command\n"
        << "# line overrides what is here, and anything missing here keeps its\n"
        << "# default, so deleting a line is the same as never writing it.\n"
        << "\n"
        << "# Wait for the display's vertical blank before presenting. Off can tear\n"
        << "# but shows a frame as soon as it is ready.\n"
        << "vsync = " << bool_text(config.vsync) << "\n"
        << "\n"
        << "# Hold the machine to its own 57.5245 Hz. Off runs as fast as this\n"
        << "# computer manages, which is only useful for captures and benchmarks.\n"
        << "throttle = " << bool_text(config.throttle) << "\n"
        << "\n"
        << "fullscreen = " << bool_text(config.fullscreen) << "\n"
        << "show_fps = " << bool_text(config.show_fps) << "\n"
        << "lightgun = " << bool_text(config.lightgun) << "\n"
        << "lightgun_crosshair = " << bool_text(config.lightgun_crosshair) << "\n"
        << "lightgun_recoil = " << bool_text(config.lightgun_recoil) << "\n"
        << "lightgun_recoil_strength = " << config.lightgun_recoil_strength << "\n"
        << "sinden_border = " << bool_text(config.sinden_border) << "\n"
        << "sinden_border_colour = 0x" << std::hex << std::setw(6)
        << std::setfill('0') << (config.sinden_border_colour & 0xffffff) << std::dec
        << std::setfill(' ') << "\n"
        << "sinden_border_thickness = " << config.sinden_border_thickness << "\n"
        << "window_width = " << config.window_width << "\n"
        << "window_height = " << config.window_height << "\n"
        << "\n"
        << "# Exact device name as --list-gpus prints it. Empty picks the best one.\n"
        << "gpu = " << config.gpu << "\n"
        << "\n"
        << "# Steering-wheel force feedback: a synthesised centring spring (the\n"
        << "# drive board is not emulated, so this is a feel, not the real motor\n"
        << "# force). Strength is 0..100 percent of the wheel's maximum torque.\n"
        << "wheel_ffb = " << bool_text(config.wheel_ffb) << "\n"
        << "wheel_ffb_strength = " << config.wheel_ffb_strength << "\n"
        << "# Your wheel's own physical rotation range (a G-series PC wheel is\n"
        << "# ~900). The cabinet's ~240 of lock is mapped onto it, so matching\n"
        << "# your wheel gives arcade-like response.\n"
        << "wheel_steer_degrees = " << config.wheel_steer_degrees << "\n"
        << "# Physical rotation (total) at which the game reaches full lock;\n"
        << "# lower is more sensitive. 180..270.\n"
        << "wheel_lock_degrees = " << config.wheel_lock_degrees << "\n"
        << "# Which wheel button drives each control (numbering varies by wheel;\n"
        << "# -1 unbinds). Set these in the GUI's Wheel tab. Buttons 1..4 are the\n"
        << "# arcade buttons, which is where a cabinet's VR/view buttons land too.\n"
        << "wheel_button_start = " << config.wheel_buttons[cfg_role(Config::WheelRole::Start)] << "\n"
        << "wheel_button_coin = " << config.wheel_buttons[cfg_role(Config::WheelRole::Coin)] << "\n"
        << "wheel_button_1 = " << config.wheel_buttons[cfg_role(Config::WheelRole::Button1)] << "\n"
        << "wheel_button_2 = " << config.wheel_buttons[cfg_role(Config::WheelRole::Button2)] << "\n"
        << "wheel_button_3 = " << config.wheel_buttons[cfg_role(Config::WheelRole::Button3)] << "\n"
        << "wheel_button_4 = " << config.wheel_buttons[cfg_role(Config::WheelRole::Button4)] << "\n"
        << "wheel_button_gear_up = " << config.wheel_buttons[cfg_role(Config::WheelRole::GearUp)] << "\n"
        << "wheel_button_gear_down = " << config.wheel_buttons[cfg_role(Config::WheelRole::GearDown)] << "\n"
        << "wheel_button_test = " << config.wheel_buttons[cfg_role(Config::WheelRole::Test)] << "\n"
        << "wheel_button_service = " << config.wheel_buttons[cfg_role(Config::WheelRole::Service)] << "\n"
        << "wheel_button_menu = " << config.wheel_buttons[cfg_role(Config::WheelRole::Menu)] << "\n"
        << "# Wheel axes, or -1 to auto-detect (steering axis 0; pedals by rest\n"
        << "# position). Set by the GUI calibration when a wheel differs.\n"
        << "wheel_steer_axis = " << config.wheel_steer_axis << "\n"
        << "wheel_accel_axis = " << config.wheel_accel_axis << "\n"
        << "wheel_brake_axis = " << config.wheel_brake_axis << "\n"
        << "wheel_accel_invert = " << bool_text(config.wheel_accel_invert) << "\n"
        << "wheel_brake_invert = " << bool_text(config.wheel_brake_invert) << "\n"
        << "\n"
        << "# Where operator settings and the EEPROM image are kept.\n"
        << "nvram_dir = " << config.nvram_dir << "\n"
        << "\n"
        << "# ROM database to use instead of searching the usual places.\n"
        << "games_xml = " << config.games_xml << "\n"
        << "\n"
        << "# Vulkan validation layers. Slow, and only useful when developing.\n"
        << "validation = " << bool_text(config.validation) << "\n"
        << "\n"
        << "# trace, debug, info, warning or error.\n"
        << "log_level = " << config.log_level << "\n";

    return out.good();
}

}  // namespace sm2
