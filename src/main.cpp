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
//
// Loads a ROM set, runs the machine, and presents its output. Settings come from
// the configuration file first and the command line second, so a flag always wins
// over a file.

#include "core/config.h"
#include "core/log.h"
#include "core/net.h"
#include "core/profiler.h"
#include "core/types.h"
#include "hw/comm_udp.h"
#include "hw/m1audio.h"
#include "hw/m2comm.h"
#include "hw/machine_factory.h"
#include "hw/model2.h"
#include "hw/model1.h"
#include "hw/model2_original.h"
#include "hw/sound_board.h"
#include "hw/model2b.h"
#include "hw/model2c.h"
#include "hw/model3/model3.h"
#include "hw/model2_debug.h"
#include "hw/model2_softrender.h"
#include "hw/model2_softrender_async.h"
#include "hw/rewind.h"
#include "hw/save_state_io.h"
#include "osd/audio.h"
#include "osd/outputs.h"
#include "osd/frame_pacer.h"
#include "osd/gui.h"
#include "osd/i18n.h"
#include "osd/input.h"
#include "osd/scraper.h"
#include "osd/window.h"
#include "render/backend.h"
#include "render/model3/soft_backend.h"
#include "render/model3/texture_dump.h"
#include "render/texture_dump.h"
#include "render/texture_replace.h"
#include "rom/game_db.h"
#include "rom/rom_loader.h"

#include <map>
#include <memory>

#include <SDL3/SDL.h>

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(SM2_HAVE_VULKAN)
// enumerate_render_devices() is Vulkan-only; a Vulkan-less build lists none.
namespace sm2::render {
std::vector<std::string> enumerate_render_devices() { return {}; }
}  // namespace sm2::render
#endif

namespace {

/// The colour ramp's black level for the brightness setting: 50% is the
/// default 40, 0% is 64 and 100% is 16.
[[nodiscard]] sm2::u32 black_level_for(const sm2::Config& config)
{
    return 64 - (std::min<sm2::u32>(config.brightness, 100) * 48 + 50) / 100;
}

/// --graphics-backend's values. Which GL flavour `Opengl` means is fixed at
/// build time.
enum class GraphicsBackendChoice {
    Software,
    Vulkan,
    Opengl,
};

/// Parses --graphics-backend's argument. Returns false, naming the valid
/// choices, on anything else.
[[nodiscard]] bool parse_graphics_backend(const char* value, GraphicsBackendChoice* out)
{
    if (std::strcmp(value, "software") == 0) {
        *out = GraphicsBackendChoice::Software;
        return true;
    }
    if (std::strcmp(value, "vulkan") == 0) {
        *out = GraphicsBackendChoice::Vulkan;
        return true;
    }
    if (std::strcmp(value, "opengl") == 0) {
        *out = GraphicsBackendChoice::Opengl;
        return true;
    }
    SM2_ERROR("--graphics-backend does not accept '%s' (valid: software, vulkan, opengl)",
              value);
    return false;
}

/// Lets SDL deliver joystick events while the window is not in front. The SDL
/// environment variable still takes precedence.
void apply_input_background(bool on)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
}

/// Build the input layer's wheel settings from the persisted config.
[[nodiscard]] sm2::osd::Input::WheelSettings wheel_settings_from(const sm2::Config& c)
{
    sm2::osd::Input::WheelSettings w;
    w.ffb             = c.wheel_ffb;
    w.strength        = c.wheel_ffb_strength;
    w.panel_spring    = c.wheel_panel_spring;
    w.indy_deadband   = c.wheel_indy_deadband;
    w.indy_full_power = c.wheel_indy_full_power;
    w.drive_dip       = c.wheel_drive_dip;
    w.ffb_invert      = c.wheel_ffb_invert;
    w.steer_degrees   = c.wheel_steer_degrees;
    w.lock_degrees    = c.wheel_lock_degrees;
    w.steer_deadzone  = c.wheel_steer_deadzone;
    w.rumble          = c.wheel_rumble;
    w.rumble_strength = c.wheel_rumble_strength;
    w.rumble_engine   = c.wheel_rumble_engine;
    w.buttons       = c.wheel_buttons;
    w.shifter_neutral  = c.wheel_shifter_neutral;
    w.shift_neutral_ms = c.wheel_shift_neutral_ms;
    w.steer_axis    = c.wheel_steer_axis;
    w.accel_axis    = c.wheel_accel_axis;
    w.brake_axis    = c.wheel_brake_axis;
    w.accel_invert  = c.wheel_accel_invert;
    w.brake_invert  = c.wheel_brake_invert;
    w.accel_half    = c.wheel_accel_half;
    w.brake_half    = c.wheel_brake_half;
    return w;
}

/// Default when --graphics-backend is not given: whichever GPU backend was
/// compiled in, else software.
constexpr GraphicsBackendChoice kDefaultGraphicsBackend =
#if defined(SM2_HAVE_OPENGL_ES)
    GraphicsBackendChoice::Opengl;
#elif defined(SM2_HAVE_VULKAN)
    GraphicsBackendChoice::Vulkan;
#elif defined(SM2_HAVE_OPENGL_DESKTOP)
    GraphicsBackendChoice::Opengl;
#else
    GraphicsBackendChoice::Software;
#endif

struct Options {
    /// Settings that persist. Loaded from the file, then overridden by flags.
    sm2::Config config;

    /// Which of those the command line set, so a flag can win over the file.
    struct Given {
        bool vsync      = false;
        bool throttle   = false;
        bool fullscreen = false;
        bool fullscreen_resolution = false;
        bool lightgun   = false;
        bool validation = false;
        bool log_level  = false;
        bool gpu        = false;
        bool model2_rom_dir = false;
        bool model1_rom_dir = false;
        bool model3_rom_dir = false;
        bool nvram_dir  = false;
        bool screenshot_dir = false;
        bool render_scale = false;
        bool graphics_backend = false;
    } given;

    bool        show_help  = false;
    bool        list_gpus  = false;
    bool        list_games = false;
    bool        list_gamepads = false;
    std::string config_path;
    std::string game;
    std::string dump_roms;
    std::string dump_tilemap;
    std::string screenshot;
    std::string dump_audio;
    std::string rom_path;

    /// Load this save state before the run loop, so --run-frames 1 --screenshot
    /// captures that exact moment.
    std::string load_state;

    /// Capture a frame every this many frames instead of only the last one. Each
    /// goes to the screenshot path with the frame number appended.
    sm2::u32 screenshot_interval = 0;

    /// Capture exactly these frames.
    std::set<sm2::u32> screenshot_frames;

    /// Insert coins and press start around this frame. Zero leaves the panel
    /// alone.
    sm2::u32 coin_at = 0;

    /// Write one line per frame recording what the geometry engine produced, for
    /// comparison against MAME's counts over a whole run.
    std::string poly_log;

    /// Write every texture the 3D samples to <saves>/textures/<game>/dump.
    bool dump_textures = false;

    /// Also render each captured frame through the CPU rasteriser and write it
    /// beside the screenshot. Both come from the same machine state, so any
    /// difference is the renderer's.
    bool soft_render = false;

    /// Draw the window with the CPU rasteriser (the one --soft-render uses)
    /// rather than the GPU.
    bool start_in_software_renderer = false;

    /// Which GPU backend is constructed. --graphics-backend software still
    /// presents through one, and also sets start_in_software_renderer.
    GraphicsBackendChoice graphics_backend = kDefaultGraphicsBackend;

    /// Run this many frames headless, report, and exit. Zero means run normally.
    sm2::u32 boot_test    = 0;

    /// Quit after this many presented frames. Zero means run until asked to stop.
    sm2::u32 run_frames   = 0;

    /// Quit after this many wall-clock seconds, in the windowed path. Unlike
    /// --run-frames this bounds real time, so --no-throttle can measure the
    /// frame count.
    sm2::u32 duration_seconds = 0;

    /// Per-stage CPU/GPU breakdown rather than just the whole-frame rate.
    /// Implies --duration's stopping behaviour when duration_seconds is zero.
    bool profile = false;


    /// Discard every profiler and frame-time sample before this presented-frame
    /// number, so near-empty attract and title screens do not skew the average.
    /// Zero measures from the first frame.
    sm2::u32 profile_after = 0;

    /// Also write --profile's per-stage table to this CSV file, one row per stage.
    std::string profile_csv;

    bool     log_unmapped = false;
};

void print_usage()
{
    std::printf(
        "Keisho " SM2_VERSION " — Sega Model 1, 2 & 3 arcade emulator\n"
        "\n"
        "Usage: keisho [options] [rom.zip]\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this message\n"
        "      --config <dir>  Directory holding keisho.ini, used for both\n"
        "                      reading and saving settings\n"
        "      --dump-textures Write every 3D texture to <saves>/textures/<game>/dump\n"
        "      --fullscreen    Start filling the screen\n"
        "      --fullscreen-resolution <WxH>  Fullscreen at a lower mode the\n"
        "                      compositor scales up (faster on slow GPUs)\n"
        "      --game <name>   Load this set specifically, for archives that\n"
        "                      hold several revisions; with no path given, the\n"
        "                      archive is found in its system's ROM folder\n"
        "      --gpu <name>    Use the device with this exact name\n"
        "      --graphics-backend <software|vulkan|opengl>\n"
        "                      Which renderer to use. 'opengl' means whichever GL\n"
        "                      flavour this binary was built with\n"
        "      --lightgun, --no-lightgun  Gun mode (crosshair, hidden cursor) on\n"
        "                      in every game, or off; by default a gun game turns\n"
        "                      it on by itself\n"
        "      --list-gamepads Show which gamepads were recognised and which\n"
        "                      player each would drive\n"
        "      --list-games    List the games in the ROM database\n"
        "      --list-gpus     List the Vulkan devices that could be used\n"
        "      --log-level <l> trace, debug, info, warning or error\n"
        "      --no-vsync      Present without waiting for vertical blank\n"
        "      --nvram <dir>   Directory for saves: NVRAM and EEPROM images\n"
        "      --render-scale <n>  Internal 3D render scale, 1..8 (default 1 =\n"
        "                      native, 8 = ~4K). GPU backends only; native under\n"
        "                      software; auto-reduced if the GPU cannot allocate it\n"
        "      --model1-rom-dir <dir>  Where the Model 1 archives live, if not\n"
        "                      in --model2-rom-dir\n"
        "      --model2-rom-dir <dir>  Where the Model 2 archives live; a game\n"
        "                      named with no path is loaded from its system's\n"
        "                      folder, else from <dir>/<name>.zip or .7z\n"
        "                      (--rom-dir is the older spelling)\n"
        "      --model3-rom-dir <dir>  Where the Model 3 archives live, if not\n"
        "                      in --model2-rom-dir\n"
        "      --screenshot-dir <dir>  Where screenshots are written\n"
        "      --load-state <file>  Restore this save state before running, so a\n"
        "                      one-frame --screenshot captures that exact moment\n"
        "\n");
    sm2::osd::Input::print_bindings();
    std::printf("\nNo ROM data is distributed with this software.\n");
}

[[nodiscard]] std::string texture_dump_directory(const sm2::Config& config,
                                                 const std::string& game)
{
    return (std::filesystem::path(config.nvram_dir) / "textures" / game / "dump").string();
}

[[nodiscard]] std::string texture_load_directory(const sm2::Config& config,
                                                 const std::string& game)
{
    return (std::filesystem::path(config.nvram_dir) / "textures" / game / "load").string();
}

[[nodiscard]] bool parse_command_line(int argc, char** argv, Options* out)
{
    for (int index = 1; index < argc; ++index) {
        const char* arg = argv[index];

        const auto takes_value = [&](const char* name, std::string* target) {
            if (std::strcmp(arg, name) != 0) {
                return false;
            }
            if (index + 1 >= argc) {
                SM2_ERROR("%s requires a value", name);
                out->show_help = true;
                return true;
            }
            *target = argv[++index];
            return true;
        };

        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            out->show_help = true;
        } else if (std::strcmp(arg, "--list-gpus") == 0) {
            out->list_gpus = true;
        } else if (std::strcmp(arg, "--list-games") == 0) {
            out->list_games = true;
        } else if (std::strcmp(arg, "--list-gamepads") == 0) {
            out->list_gamepads = true;
        } else if (std::strcmp(arg, "--validation") == 0) {
            out->config.validation = true;
            out->given.validation  = true;
        } else if (std::strcmp(arg, "--soft-render") == 0) {
            out->soft_render = true;
        } else if (std::strcmp(arg, "--graphics-backend") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--graphics-backend requires a value (software, vulkan, opengl)");
                return false;
            }
            if (!parse_graphics_backend(argv[++index], &out->graphics_backend)) {
                return false;
            }
            out->given.graphics_backend = true;
            if (out->graphics_backend == GraphicsBackendChoice::Software) {
                out->start_in_software_renderer = true;
            }
        } else if (std::strcmp(arg, "--render-scale") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--render-scale requires a value (1..%u)",
                          sm2::render::kMaxRenderScale);
                return false;
            }
            const char*    text  = argv[++index];
            char*          end   = nullptr;
            const sm2::u32 value = static_cast<sm2::u32>(std::strtoul(text, &end, 10));
            sm2::u32       scale = value;
            if (end == text || *end != '\0' || value < 1) {
                scale = 1;
            } else if (value > sm2::render::kMaxRenderScale) {
                scale = sm2::render::kMaxRenderScale;
            }
            if (scale != value || end == text || *end != '\0') {
                SM2_WARN("--render-scale '%s' is out of range; using %u (valid: 1..%u)",
                         text, scale, sm2::render::kMaxRenderScale);
            }
            out->config.render_scale = scale;
            out->given.render_scale  = true;
        } else if (std::strcmp(arg, "--no-vsync") == 0) {
            out->config.vsync = false;
            out->given.vsync  = true;
        } else if (std::strcmp(arg, "--no-throttle") == 0) {
            out->config.throttle = false;
            out->given.throttle  = true;
        } else if (takes_value("--fullscreen-resolution", &out->config.fullscreen_resolution)) {
            out->given.fullscreen_resolution = true;
        } else if (std::strcmp(arg, "--fullscreen") == 0) {
            out->config.fullscreen = true;
            out->given.fullscreen  = true;
        } else if (std::strcmp(arg, "--lightgun") == 0 || std::strcmp(arg, "--no-lightgun") == 0) {
            out->config.lightgun = arg[2] == 'n' ? sm2::Config::LightgunMode::Off : sm2::Config::LightgunMode::On;
            out->given.lightgun  = true;
        } else if (std::strcmp(arg, "--log-unmapped") == 0) {
            out->log_unmapped = true;
        } else if (std::strcmp(arg, "--boot-test") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--boot-test requires a frame count");
                return false;
            }
            out->boot_test =
                static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
            if (out->boot_test == 0) {
                SM2_ERROR("--boot-test needs a frame count of at least one");
                return false;
            }
        } else if (std::strcmp(arg, "--run-frames") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--run-frames requires a frame count");
                return false;
            }
            out->run_frames =
                static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
            if (out->run_frames == 0) {
                SM2_ERROR("--run-frames needs a frame count of at least one");
                return false;
            }
        } else if (std::strcmp(arg, "--duration") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--duration requires a second count");
                return false;
            }
            out->duration_seconds =
                static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
            if (out->duration_seconds == 0) {
                SM2_ERROR("--duration needs a second count of at least one");
                return false;
            }
        } else if (std::strcmp(arg, "--profile") == 0) {
            out->profile = true;
            // The second count is optional here, unlike --duration: peek at the
            // next token and consume it only if it is entirely digits, so
            // "--profile vf2.zip" does not swallow the ROM path.
            if (index + 1 < argc) {
                const char* next        = argv[index + 1];
                bool        all_digits  = next[0] != '\0';
                for (const char* c = next; *c != '\0'; ++c) {
                    if (*c < '0' || *c > '9') {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    ++index;
                    out->duration_seconds = static_cast<sm2::u32>(std::strtoul(next, nullptr, 10));
                }
            }
            if (out->duration_seconds == 0) {
                out->duration_seconds = 30;
            }
        } else if (takes_value("--profile-csv", &out->profile_csv)) {
            // handled
        } else if (std::strcmp(arg, "--profile-after") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--profile-after requires a frame number");
                return false;
            }
            out->profile_after =
                static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(arg, "--coin-at") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--coin-at requires a frame number");
                return false;
            }
            out->coin_at = static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
            if (out->coin_at == 0) {
                SM2_ERROR("--coin-at needs a frame number of at least one");
                return false;
            }
        } else if (std::strcmp(arg, "--screenshot-interval") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--screenshot-interval requires a frame count");
                return false;
            }
            out->screenshot_interval =
                static_cast<sm2::u32>(std::strtoul(argv[++index], nullptr, 10));
            if (out->screenshot_interval == 0) {
                SM2_ERROR("--screenshot-interval needs a count of at least one");
                return false;
            }
        } else if (std::strcmp(arg, "--screenshot-frames") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--screenshot-frames requires a list of frame numbers");
                return false;
            }
            const char* list = argv[++index];
            while (*list != '\0') {
                char*             end   = nullptr;
                const sm2::u32    value = static_cast<sm2::u32>(std::strtoul(list, &end, 10));
                if (end == list) {
                    SM2_ERROR("--screenshot-frames does not accept '%s'", argv[index]);
                    return false;
                }
                out->screenshot_frames.insert(value);
                list = end;
                while (*list == ',' || *list == ' ') {
                    ++list;
                }
            }
            if (out->screenshot_frames.empty()) {
                SM2_ERROR("--screenshot-frames needs at least one frame number");
                return false;
            }
        } else if (takes_value("--screenshot", &out->screenshot)) {
            // handled
        } else if (takes_value("--load-state", &out->load_state)) {
            // handled
        } else if (takes_value("--model1-rom-dir", &out->config.model1_rom_dir)) {
            out->given.model1_rom_dir = true;
        } else if (takes_value("--model2-rom-dir", &out->config.model2_rom_dir)
                   || takes_value("--rom-dir", &out->config.model2_rom_dir)) {
            out->given.model2_rom_dir = true;
        } else if (takes_value("--model3-rom-dir", &out->config.model3_rom_dir)) {
            out->given.model3_rom_dir = true;
        } else if (takes_value("--nvram", &out->config.nvram_dir)) {
            out->given.nvram_dir = true;
        } else if (takes_value("--screenshot-dir", &out->config.screenshot_dir)) {
            out->given.screenshot_dir = true;
        } else if (takes_value("--dump-audio", &out->dump_audio)) {
            // handled
        } else if (std::strcmp(arg, "--dump-textures") == 0) {
            out->dump_textures = true;
        } else if (takes_value("--poly-log", &out->poly_log)) {
        } else if (takes_value("--dump-tilemap", &out->dump_tilemap)) {
            // handled
        } else if (takes_value("--gpu", &out->config.gpu)) {
            out->given.gpu = true;
        } else if (takes_value("--log-level", &out->config.log_level)) {
            sm2::log::Level parsed = sm2::log::Level::Info;
            if (!sm2::parse_log_level(out->config.log_level, &parsed)) {
                SM2_ERROR("--log-level does not accept '%s'",
                          out->config.log_level.c_str());
                return false;
            }
            out->given.log_level = true;
        } else if (takes_value("--config", &out->config_path)) {
            // handled
        } else if (takes_value("--game", &out->game)) {
            // handled
        } else if (takes_value("--dump-roms", &out->dump_roms)) {
            // handled
        } else if (arg[0] == '-') {
            SM2_ERROR("unrecognised option '%s'", arg);
            return false;
        } else {
            out->rom_path = arg;
        }
    }
    return true;
}

/// The value `fraction` of the way up a sorted copy of `values`, nearest-rank.
/// Empty input returns zero rather than reading out of bounds.
[[nodiscard]] double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const sm2::usize rank =
        static_cast<sm2::usize>(fraction * static_cast<double>(values.size() - 1));
    return values[rank];
}

/// Insert a zero-padded frame number before a path's extension.
[[nodiscard]] std::string numbered_path(const std::string& path, sm2::u32 frame)
{
    const std::size_t dot   = path.find_last_of('.');
    const std::size_t slash = path.find_last_of('/');
    const bool  has_extension =
        dot != std::string::npos && (slash == std::string::npos || dot > slash);

    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%06u", frame);

    if (!has_extension) {
        return path + suffix;
    }
    return path.substr(0, dot) + suffix + path.substr(dot);
}

/// <dir>/<game>-YYYYMMDD-HHMMSS.png, creating `dir` if needed.
[[nodiscard]] std::string screenshot_path(const std::string& dir, const std::string& game)
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);

    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    return (std::filesystem::path(dir) / (game + "-" + stamp + ".png")).string();
}

/// The key a game's per-game settings are stored under, shared by every revision.
[[nodiscard]] std::string game_family(const sm2::rom::GameSpec& game)
{
    // Separate parents that share one slider.
    static const std::map<std::string, std::string> kSharedFamily = {
        {"zeroguna", "zerogun"},
        {"dynabb97", "dynabb"},
    };
    const std::string& parent = game.parent.empty() ? game.name : game.parent;
    const auto shared = kSharedFamily.find(parent);
    return shared != kSharedFamily.end() ? shared->second : parent;
}

/// "Sega Rally Championship - Twin/DX" -> "Sega Rally Championship".
[[nodiscard]] std::string game_title_without_cabinet(std::string title)
{
    static constexpr std::array<std::string_view, 4> kCabinets = {"Twin", "DX", "Deluxe",
                                                                   "Relay"};
    const auto is_separator = [](char c) { return c == ' ' || c == '-' || c == '/'; };
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const std::string_view cabinet : kCabinets) {
            if (title.size() > cabinet.size() && title.ends_with(cabinet)
                && is_separator(title[title.size() - cabinet.size() - 1])) {
                title.resize(title.size() - cabinet.size());
                while (!title.empty() && is_separator(title.back())) {
                    title.pop_back();
                }
                stripped = true;
            }
        }
    }
    return title;
}

/// Scale interleaved samples by `percent` of full level, saturating.
void apply_volume(std::span<const sm2::s16> in, sm2::u32 percent, std::vector<sm2::s16>* out)
{
    out->resize(in.size());
    for (sm2::usize index = 0; index < in.size(); ++index) {
        const sm2::s32 scaled = static_cast<sm2::s32>(in[index])
                              * static_cast<sm2::s32>(percent) / 100;
        (*out)[index] = static_cast<sm2::s16>(std::clamp(scaled, -32768, 32767));
    }
}

/// A loaded machine plus the pointers the loop reaches into it for. The
/// concrete downcasts are for the boot-test diagnostics, which need a specific
/// board's registers.
struct LoadedMachine {
    sm2::rom::GameSpec                game;
    std::unique_ptr<sm2::hw::Machine> machine_owner;

    /// machine_owner's Model 1 / Model 2 interface.
    sm2::hw::Model2MachineBase* machine_iface = nullptr;

    sm2::hw::Model2*         machine      = nullptr;
    sm2::hw::Model2B*        machine_2b   = nullptr;
    sm2::hw::Model2C*        machine_2c   = nullptr;
    sm2::hw::Model2Original* machine_orig = nullptr;
    sm2::hw::Model1*         machine_m1   = nullptr;
    sm2::hw::model3::Model3* machine_m3   = nullptr;

    sm2::cpu::i960::I960* main_cpu    = nullptr;
    sm2::hw::SoundBoard*  sound_board = nullptr;
    const sm2::hw::I8251* sound_link  = nullptr;
};

/// The folder set `name` loads from: model1_rom_dir / model3_rom_dir for a
/// Model 1 / Model 3 set when one is given, else model2_rom_dir.
[[nodiscard]] const std::string& rom_dir_for(const sm2::rom::GameDatabase& database,
                                             const sm2::Config& config,
                                             const std::string& name)
{
    const sm2::rom::GameSpec* game = database.find(name);
    if (game != nullptr && game->board == sm2::rom::Board::Model1
        && !config.model1_rom_dir.empty()) {
        return config.model1_rom_dir;
    }
    if (game != nullptr && game->board == sm2::rom::Board::Model3
        && !config.model3_rom_dir.empty()) {
        return config.model3_rom_dir;
    }
    return config.model2_rom_dir;
}

/// The archive that set `name` loads from: its own <name>.zip or .7z, else,
/// for a clone, its parent's when that holds the clone as well, the way a
/// merged romset keeps every revision in the parent's archive.
[[nodiscard]] std::optional<std::filesystem::path> find_set_archive(
    const sm2::rom::GameDatabase& database, const sm2::Config& config,
    const std::string& name)
{
    const std::filesystem::path dir(rom_dir_for(database, config, name));
    if (dir.empty()) {
        return std::nullopt;
    }
    const auto archive_named = [&dir](const std::string& stem)
        -> std::optional<std::filesystem::path> {
        for (const char* ext : {".zip", ".7z"}) {
            const std::filesystem::path candidate = dir / (stem + ext);
            std::error_code error;
            if (std::filesystem::exists(candidate, error) && !error) {
                return candidate;
            }
        }
        return std::nullopt;
    };
    if (std::optional<std::filesystem::path> own = archive_named(name)) {
        return own;
    }
    const sm2::rom::GameSpec* game = database.find(name);
    if (game == nullptr || game->parent.empty()) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> parent = archive_named(game->parent);
    if (parent.has_value()
        && sm2::rom::RomLoader::holds_clone(database, parent->string(), name)) {
        return parent;
    }
    return std::nullopt;
}

/// Load, build, wire, point at NVRAM and reset a machine, for both a direct
/// launch and the picker. Audio is opened by the caller. nullopt (logged, and
/// in `error` when given) on failure.
[[nodiscard]] std::optional<LoadedMachine> load_game(sm2::rom::GameDatabase& database,
                                                     const std::string& rom_path,
                                                     const std::string& game_name,
                                                     const std::string& nvram_dir,
                                                     bool               log_unmapped,
                                                     sm2::rom::LoadError* error = nullptr)
{
    using namespace sm2;
    std::optional<rom::LoadResult> result =
        rom::RomLoader::load(database, rom_path, game_name, error);
    if (!result.has_value()) {
        return std::nullopt;
    }

    LoadedMachine out;
    out.game          = result->game;
    out.machine_owner = hw::create_machine(result->game, std::move(result->roms));
    out.machine_iface = out.machine_owner ? out.machine_owner->as_model2() : nullptr;
    out.machine_m3    = out.machine_owner ? out.machine_owner->as_model3() : nullptr;
    if (!out.machine_iface && !out.machine_m3) {
        if (error != nullptr) {
            error->game    = result->game.name;
            error->message = "The emulated machine could not be built; the log has the details.";
        }
        return std::nullopt;
    }

    // Downcast once; accessors below stay written against the concrete class.
    out.machine      = dynamic_cast<hw::Model2*>(out.machine_iface);
    out.machine_2b   = dynamic_cast<hw::Model2B*>(out.machine_iface);
    out.machine_2c   = dynamic_cast<hw::Model2C*>(out.machine_iface);
    out.machine_orig = dynamic_cast<hw::Model2Original*>(out.machine_iface);
    out.machine_m1   = dynamic_cast<hw::Model1*>(out.machine_iface);
    if (out.machine != nullptr) {
        out.main_cpu    = &out.machine->cpu();
        out.sound_board = &out.machine->sound();
        out.sound_link  = &out.machine->uart();
    } else if (out.machine_2b != nullptr) {
        out.main_cpu    = &out.machine_2b->cpu();
        out.sound_board = &out.machine_2b->sound();
        out.sound_link  = &out.machine_2b->uart();
    } else if (out.machine_2c != nullptr) {
        out.main_cpu    = &out.machine_2c->cpu();
        out.sound_board = &out.machine_2c->sound();
        out.sound_link  = &out.machine_2c->uart();
    } else if (out.machine_orig != nullptr) {
        out.main_cpu    = &out.machine_orig->cpu();
        out.sound_board = &out.machine_orig->sound();
        out.sound_link  = &out.machine_orig->uart();
    } else if (out.machine_m1 != nullptr) {
        out.sound_board = &out.machine_m1->sound();
        out.sound_link  = &out.machine_m1->uart();
    } else if (out.machine_m3 != nullptr) {
        out.sound_board = out.machine_m3->sound() ? out.machine_m3->sound()->sound_board() : nullptr;
    } else {
        SM2_ERROR("internal error: create_machine returned an unexpected machine type");
        return std::nullopt;
    }

    out.machine_owner->set_nvram_directory(nvram_dir);
    out.machine_owner->set_log_unmapped(log_unmapped);
    out.machine_owner->load_nvram();
    // init() resets before the NVRAM is in place, so reset again to let the
    // program read the settings it saved last time.
    out.machine_owner->reset();
    return out;
}

/// Attach a LAN transport to the link board when linking is on, else restore
/// the loopback. A socket that fails to bind is not fatal: the board just
/// never links.
void configure_cabinet_link(sm2::hw::Machine& machine,
                            const sm2::Config&          config)
{
    using namespace sm2;
    if (!config.link_enabled) {
        machine.comm().set_transport(nullptr);  // back to the loopback
        return;
    }

    auto transport = std::make_unique<hw::UdpTransport>(
        config.link_local_ip, static_cast<u16>(config.link_port),
        config.link_next_ip, static_cast<u16>(config.link_next_port),
        // Model 3 needs every segment; Model 1/2 drop stale frames.
        machine.as_model3() != nullptr ? 4096 : hw::UdpTransport::kBacklog);
    if (!transport->ok()) {
        SM2_WARN("cabinet link disabled: %s", transport->error().c_str());
        machine.comm().set_transport(nullptr);
        return;
    }
    machine.comm().set_transport(std::move(transport));
    machine.comm().set_link_delay(config.link_delay);
    SM2_INFO("cabinet link enabled (cabinet %u): listening %s:%u, next %s:%u",
             config.link_cabinet_index,
             config.link_local_ip.empty() ? "*" : config.link_local_ip.c_str(),
             config.link_port,
             config.link_next_ip.empty() ? "(none)" : config.link_next_ip.c_str(),
             config.link_next_port);
}

}  // namespace

/// The settings as written back: a value given on the command line holds for
/// this run only, so the file keeps its own unless it was changed since launch.
sm2::Config persisted_config(const sm2::Config& current, const Options& options,
                             const sm2::Config& launched, const sm2::Config& from_file)
{
    using sm2::Config;
    Config      c    = current;
    const auto& g    = options.given;
    auto        keep = [&](bool given, auto Config::*field) {
        if (given && c.*field == launched.*field) {
            c.*field = from_file.*field;
        }
    };
    keep(g.vsync, &Config::vsync);
    keep(g.throttle, &Config::throttle);
    keep(g.fullscreen, &Config::fullscreen);
    keep(g.fullscreen_resolution, &Config::fullscreen_resolution);
    keep(g.lightgun, &Config::lightgun);
    keep(g.validation, &Config::validation);
    keep(g.log_level, &Config::log_level);
    keep(g.gpu, &Config::gpu);
    keep(g.model1_rom_dir, &Config::model1_rom_dir);
    keep(g.model2_rom_dir, &Config::model2_rom_dir);
    keep(g.model3_rom_dir, &Config::model3_rom_dir);
    keep(g.nvram_dir, &Config::nvram_dir);
    keep(g.screenshot_dir, &Config::screenshot_dir);
    keep(g.render_scale, &Config::render_scale);
    keep(g.graphics_backend, &Config::graphics_backend);
    return c;
}

int main(int argc, char** argv)
{
    using namespace sm2;

    Options options;
    if (!parse_command_line(argc, argv, &options)) {
        print_usage();
        return 1;
    }
    if (options.show_help) {
        print_usage();
        return 0;
    }

    // --graphics-backend opengl needs a GL backend compiled in (desktop GL 4.3
    // core or GLES 3.1, which are mutually exclusive).
#if !defined(SM2_HAVE_OPENGL_DESKTOP) && !defined(SM2_HAVE_OPENGL_ES)
    if (options.graphics_backend == GraphicsBackendChoice::Opengl) {
        SM2_ERROR("--graphics-backend opengl was requested, but this binary was "
                  "built without an OpenGL backend. Rebuild with "
                  "-DSM2_BUILD_OPENGL_DESKTOP=ON or -DSM2_BUILD_OPENGL_ES=ON.");
        return 1;
    }
#endif

#if !defined(SM2_HAVE_VULKAN)
    if (options.graphics_backend == GraphicsBackendChoice::Vulkan) {
        SM2_ERROR("--graphics-backend vulkan was requested, but this binary was "
                  "built without the Vulkan backend. Rebuild with "
                  "-DSM2_BUILD_VULKAN=ON.");
        return 1;
    }
#endif

    // Command line first, then the file for everything the command line did not
    // mention, so a flag always has the last word.
    Config defaults;
#if defined(SM2_ENABLE_VALIDATION)
    // A build with validation compiled in enables it by default; a file can
    // still turn it off.
    defaults.validation = true;
#endif

    // --config names a directory; the file within it is keisho.ini.
    const std::string config_path =
        options.config_path.empty() ? default_config_path() : config_file_in(options.config_path);

    Config                   from_file = defaults;
    std::vector<std::string> problems;
    const bool               readable = load_config(config_path, &from_file, &problems);

    if (!options.given.vsync) {
        options.config.vsync = from_file.vsync;
    }
    if (!options.given.throttle) {
        options.config.throttle = from_file.throttle;
    }
    if (!options.given.fullscreen) {
        options.config.fullscreen = from_file.fullscreen;
    }
    if (!options.given.fullscreen_resolution) {
        options.config.fullscreen_resolution = from_file.fullscreen_resolution;
    }
    if (!options.given.lightgun) {
        options.config.lightgun = from_file.lightgun;
    }
    if (!options.given.validation) {
        options.config.validation = from_file.validation;
    }
    if (!options.given.gpu) {
        options.config.gpu = from_file.gpu;
    }
    if (!options.given.model1_rom_dir) {
        options.config.model1_rom_dir = from_file.model1_rom_dir;
    }
    if (!options.given.model2_rom_dir) {
        options.config.model2_rom_dir = from_file.model2_rom_dir;
    }
    if (!options.given.model3_rom_dir) {
        options.config.model3_rom_dir = from_file.model3_rom_dir;
    }
    if (!options.given.nvram_dir) {
        options.config.nvram_dir = from_file.nvram_dir;
    }
    if (!options.given.screenshot_dir) {
        options.config.screenshot_dir = from_file.screenshot_dir;
    }
    options.config.artwork_dir    = from_file.artwork_dir;
    options.config.scrape_artwork = from_file.scrape_artwork;
    if (!options.given.log_level) {
        options.config.log_level = from_file.log_level;
    }
    options.config.window_width  = from_file.window_width;
    options.config.window_height = from_file.window_height;
    if (!options.given.render_scale) {
        options.config.render_scale = from_file.render_scale;
    }
    // Present-stage and enhancement options have no command-line flags.
    options.config.scaling_method        = from_file.scaling_method;
    options.config.aspect_mode           = from_file.aspect_mode;
    options.config.crt_enabled           = from_file.crt_enabled;
    options.config.brightness            = from_file.brightness;
    options.config.crt_scanline_strength = from_file.crt_scanline_strength;
    options.config.crt_mask_strength     = from_file.crt_mask_strength;
    options.config.crt_glow_strength     = from_file.crt_glow_strength;
    options.config.crt_curvature         = from_file.crt_curvature;
    options.config.bezel                 = from_file.bezel;
    options.config.bezel_dir             = from_file.bezel_dir;
    options.config.crosshair_dir         = from_file.crosshair_dir;

    options.config.texture_filter = from_file.texture_filter;
    options.config.anisotropy     = from_file.anisotropy;
    options.config.upscale_2d     = from_file.upscale_2d;
    options.config.translucency   = from_file.translucency;
    options.config.extended_draw_distance = from_file.extended_draw_distance;
    options.config.custom_textures = from_file.custom_textures;
    options.config.dump_textures   = from_file.dump_textures;

    // Renderer: --graphics-backend wins, else the saved choice selects it. The
    // config string is kept populated either way so the GUI round-trips it.
    if (options.given.graphics_backend) {
        options.config.graphics_backend =
            options.graphics_backend == GraphicsBackendChoice::Software ? "software"
            : options.graphics_backend == GraphicsBackendChoice::Opengl ? "opengl"
                                                                        : "vulkan";
    } else {
        options.config.graphics_backend = from_file.graphics_backend;
        if (options.config.graphics_backend == "software") {
            options.graphics_backend        = GraphicsBackendChoice::Software;
            options.start_in_software_renderer = true;
        } else if (options.config.graphics_backend == "opengl") {
            options.graphics_backend = GraphicsBackendChoice::Opengl;
        } else if (options.config.graphics_backend == "vulkan") {
            options.graphics_backend = GraphicsBackendChoice::Vulkan;
        }  // empty: keep kDefaultGraphicsBackend
    }

    // Settings with no command-line flag come straight from the file, so the
    // GUI shows and round-trips what was saved.
    options.config.show_fps            = from_file.show_fps;
    options.config.software_async      = from_file.software_async;
    options.config.model3_async        = from_file.model3_async;
    options.config.software_slow_cores = from_file.software_slow_cores;
    options.config.show_notifications  = from_file.show_notifications;
    options.config.rewind              = from_file.rewind;
    options.config.rewind_buffer_mb    = from_file.rewind_buffer_mb;
    options.config.language            = from_file.language;
    options.config.theme               = from_file.theme;
    options.config.wheel_ffb           = from_file.wheel_ffb;
    options.config.wheel_ffb_strength  = from_file.wheel_ffb_strength;
    options.config.wheel_panel_spring  = from_file.wheel_panel_spring;
    options.config.wheel_indy_deadband   = from_file.wheel_indy_deadband;
    options.config.wheel_indy_full_power = from_file.wheel_indy_full_power;
    options.config.wheel_drive_dip       = from_file.wheel_drive_dip;
    options.config.wheel_ffb_invert    = from_file.wheel_ffb_invert;
    options.config.input_background    = from_file.input_background;
    options.config.wheel_steer_degrees = from_file.wheel_steer_degrees;
    options.config.wheel_lock_degrees  = from_file.wheel_lock_degrees;
    options.config.wheel_rumble          = from_file.wheel_rumble;
    options.config.wheel_rumble_strength = from_file.wheel_rumble_strength;
    options.config.wheel_rumble_engine   = from_file.wheel_rumble_engine;
    options.config.wheel_buttons       = from_file.wheel_buttons;
    options.config.wheel_shifter_neutral  = from_file.wheel_shifter_neutral;
    options.config.wheel_shift_neutral_ms = from_file.wheel_shift_neutral_ms;
    options.config.wheel_steer_axis    = from_file.wheel_steer_axis;
    options.config.wheel_accel_axis    = from_file.wheel_accel_axis;
    options.config.wheel_brake_axis    = from_file.wheel_brake_axis;
    options.config.wheel_accel_invert  = from_file.wheel_accel_invert;
    options.config.wheel_brake_invert  = from_file.wheel_brake_invert;
    options.config.wheel_accel_half    = from_file.wheel_accel_half;
    options.config.wheel_brake_half    = from_file.wheel_brake_half;

    options.config.pad_rumble           = from_file.pad_rumble;
    options.config.pad_rumble_strength  = from_file.pad_rumble_strength;
    options.config.pad_rumble_cornering = from_file.pad_rumble_cornering;
    options.config.pad_stick_sensitivity = from_file.pad_stick_sensitivity;
    options.config.pad_stick_deadzone    = from_file.pad_stick_deadzone;
    options.config.pad_shortcuts         = from_file.pad_shortcuts;
    options.config.hotkeys               = from_file.hotkeys;
    options.config.pad_device            = from_file.pad_device;
    options.config.gun_devices           = from_file.gun_devices;
    options.config.pad                   = from_file.pad;
    options.config.pad_layouts           = from_file.pad_layouts;
    options.config.wheel_steer_deadzone  = from_file.wheel_steer_deadzone;
    options.config.game_volume           = from_file.game_volume;
    options.config.music_volume          = from_file.music_volume;
    options.config.game_volumes          = from_file.game_volumes;
    options.config.outputs_network      = from_file.outputs_network;
    options.config.outputs_network_port = from_file.outputs_network_port;
    options.config.outputs_windows      = from_file.outputs_windows;

    options.config.link_enabled        = from_file.link_enabled;
    options.config.link_local_ip       = from_file.link_local_ip;
    options.config.link_subnet_mask    = from_file.link_subnet_mask;
    options.config.link_port           = from_file.link_port;
    options.config.link_next_ip        = from_file.link_next_ip;
    options.config.link_next_port      = from_file.link_next_port;
    options.config.link_cabinet_index  = from_file.link_cabinet_index;
    options.config.link_delay          = from_file.link_delay;

    options.config.lightgun_crosshair       = from_file.lightgun_crosshair;
    options.config.lightgun_hide_flash      = from_file.lightgun_hide_flash;
    options.config.lightgun_pad_centred     = from_file.lightgun_pad_centred;
    options.config.lightgun_recoil          = from_file.lightgun_recoil;
    options.config.lightgun_recoil_strength = from_file.lightgun_recoil_strength;
    options.config.sinden_border            = from_file.sinden_border;
    options.config.sinden_border_colour     = from_file.sinden_border_colour;
    options.config.sinden_border_thickness  = from_file.sinden_border_thickness;
    options.config.gun_buttons              = from_file.gun_buttons;

    // Default saves and screenshots go under the platform data dir, or stay
    // relative for an ini in the working directory. Artwork goes beside the ini.
    const bool config_in_cwd =
        options.config_path.empty() && std::filesystem::path(config_path).parent_path().empty();
    const std::string config_dir =
        std::filesystem::path(config_path).parent_path().string();
    resolve_default_paths(&options.config, config_in_cwd, config_dir);

    log::Level level = log::Level::Info;
    (void)parse_log_level(options.config.log_level, &level);
    log::set_level(level);

    SM2_INFO("Keisho %s", SM2_VERSION);

    // Platform sockets, once (a no-op except on Windows). The process reclaims
    // them at exit, so the early-return paths below need no matching shutdown.
    net::startup();

    if (!readable) {
        SM2_WARN("could not read '%s'; using defaults", config_path.c_str());
    }
    for (const std::string& problem : problems) {
        SM2_WARN("%s", problem.c_str());
    }

    const Config launched = options.config;
    const auto   to_save  = [&](const Config& current) {
        return persisted_config(current, options, launched, from_file);
    };

    // Make sure the settings and NVRAM locations exist. With no ini yet, write
    // one so there is a file to edit; an existing one is left untouched.
    {
        std::error_code error;
        if ((!std::filesystem::exists(config_path, error) || error)
            && !save_config(config_path, to_save(options.config))) {
            SM2_WARN("could not create '%s'", config_path.c_str());
        }
        if (!options.config.nvram_dir.empty()) {
            std::filesystem::create_directories(options.config.nvram_dir, error);
        }
    }

    if (options.list_gpus) {
#if !defined(SM2_HAVE_VULKAN)
        std::printf("Device enumeration is a Vulkan feature; this binary was "
                    "built without the Vulkan backend (-DSM2_BUILD_VULKAN=ON).\n");
        return 1;
#else
        const std::vector<std::string> names = render::enumerate_render_devices();
        if (names.empty()) {
            std::printf("No Vulkan devices were found.\n\n"
                        "No driver (ICD) is registered with the loader.\n"
#if defined(__APPLE__)
                        "On macOS, source the Vulkan SDK's setup-env.sh so that\n"
                        "VK_DRIVER_FILES points at MoltenVK_icd.json.\n"
#else
                        "On Linux, install your GPU's Vulkan driver package and\n"
                        "check that /usr/share/vulkan/icd.d holds a JSON manifest.\n"
#endif
            );
            return 1;
        }
        std::printf("Vulkan devices:\n");
        for (const std::string& name : names) {
            std::printf("  %s\n", name.c_str());
        }
        return 0;
#endif
    }

    if (options.list_gamepads) {
        if (!SDL_Init(0)) {
            SM2_ERROR("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }
        osd::Input input;
        const bool started = input.init(osd::Input::WheelSettings{});
        if (started) {
            const std::vector<std::string> names = input.gamepad_names();
            std::printf("Gamepads:\n");
            for (usize player = 0; player < names.size(); ++player) {
                std::printf("  player %zu: %s\n", player + 1,
                            names[player].empty() ? "(none)" : names[player].c_str());
            }
            std::printf("\nA device missing from this list either is not plugged in or\n"
                        "has no SDL gamepad mapping, in which case it is seen only as a\n"
                        "bare joystick and ignored.\n");
        }
        input.shutdown();
        SDL_Quit();
        return started ? 0 : 1;
    }

    // --game with no path: the archive is found in its ROM folder once the
    // database is loaded (a clone may live in its parent's archive). A
    // positional path wins.
    const bool any_rom_dir = !options.config.model2_rom_dir.empty()
                             || !options.config.model1_rom_dir.empty()
                             || !options.config.model3_rom_dir.empty();
    const bool game_in_rom_dir = options.rom_path.empty() && !options.game.empty()
                                 && any_rom_dir;

    // Show the picker when launched with no ROM but a ROM folder to browse.
    const bool will_show_picker = options.rom_path.empty() && options.game.empty()
                                  && any_rom_dir
                                  && !options.list_games && options.boot_test == 0;

    // As will_show_picker, but a ROM folder may be set later from the Paths tab,
    // whose rescan needs the database.
    const bool may_need_picker = options.rom_path.empty() && options.game.empty()
                                 && !options.list_games && options.boot_test == 0;

    // -- ROM database ------------------------------------------------------
    // Loaded before anything graphical, so a bad ROM path fails immediately.
    rom::GameDatabase database;
    if (options.list_games || !options.rom_path.empty() || game_in_rom_dir
        || may_need_picker) {
        const std::optional<std::string> database_path = rom::GameDatabase::locate();
        if (!database_path.has_value() || !database.load(*database_path)) {
            return 1;
        }
    }

    if (game_in_rom_dir) {
        const std::optional<std::filesystem::path> archive =
            find_set_archive(database, options.config, options.game);
        if (!archive.has_value()) {
            const rom::GameSpec* game   = database.find(options.game);
            const std::string    parent = game != nullptr && !game->parent.empty()
                                              ? ", or " + game->parent + ".zip/.7z holding it"
                                              : std::string();
            SM2_ERROR("no archive for '%s' in '%s' (looked for %s.zip and "
                      "%s.7z%s)", options.game.c_str(),
                      rom_dir_for(database, options.config, options.game).c_str(),
                      options.game.c_str(), options.game.c_str(), parent.c_str());
            return 1;
        }
        options.rom_path = archive->string();
    }

    if (options.list_games) {
        std::printf("%-12s %-14s %-34s %-6s %s\n",
                    "SET", "BOARD", "TITLE", "YEAR", "NOTES");
        for (const rom::GameSpec& game : database.games()) {
            // Printed as written, without a "v" prefix: "2.1", but also
            // "Revision B".
            std::string notes = game.version;
            if (game.preliminary) {
                notes += notes.empty() ? "preliminary" : ", preliminary";
            }
            std::printf("%-12s %-14s %-34s %-6u %s\n",
                        game.name.c_str(),
                        rom::board_name(game.board),
                        game.title.c_str(),
                        game.year,
                        notes.c_str());
        }
        return 0;
    }

    // --dump-roms is terminal and kept ahead of load_game(), which consumes the
    // RomSet into the machine.
    if (!options.rom_path.empty() && !options.dump_roms.empty()) {
        std::optional<rom::LoadResult> dump =
            rom::RomLoader::load(database, options.rom_path, options.game);
        if (!dump.has_value()) {
            return 1;
        }
        return rom::RomLoader::dump_regions(dump->roms, options.dump_roms) ? 0 : 1;
    }

    // -- the machine -------------------------------------------------------
    std::optional<LoadedMachine> loaded;
    if (!options.rom_path.empty()) {
        loaded = load_game(database, options.rom_path, options.game,
                           options.config.nvram_dir, options.log_unmapped);
        if (!loaded.has_value()) {
            return 1;
        }
        configure_cabinet_link(*loaded->machine_owner, options.config);
    } else {
        SM2_INFO("no ROM given; starting with the bring-up display only");
    }

    hw::Model2MachineBase* machine_iface = loaded.has_value() ? loaded->machine_iface
                                                              : nullptr;
    hw::Model2*         machine      = loaded.has_value() ? loaded->machine      : nullptr;
    hw::Model2B*        machine_2b   = loaded.has_value() ? loaded->machine_2b   : nullptr;
    hw::Model2C*        machine_2c   = loaded.has_value() ? loaded->machine_2c   : nullptr;
    hw::Model2Original* machine_orig = loaded.has_value() ? loaded->machine_orig : nullptr;
    hw::Model1*         machine_m1   = loaded.has_value() ? loaded->machine_m1   : nullptr;
    hw::model3::Model3* machine_m3   = loaded.has_value() ? loaded->machine_m3   : nullptr;
    hw::Machine*        machine_any  = loaded.has_value() ? loaded->machine_owner.get() : nullptr;

    cpu::i960::I960* main_cpu    = loaded.has_value() ? loaded->main_cpu    : nullptr;
    hw::SoundBoard*  sound_board = loaded.has_value() ? loaded->sound_board : nullptr;
    const hw::I8251* sound_link  = loaded.has_value() ? loaded->sound_link  : nullptr;

    // Restore a save state before anything runs, so a one-frame --screenshot
    // renders exactly that state.
    if (!options.load_state.empty()) {
        if (machine_any == nullptr) {
            SM2_ERROR("--load-state needs a ROM");
            return 1;
        }
        if (!machine_any->load_state(options.load_state)) {
            SM2_ERROR("--load-state: failed to load '%s'", options.load_state.c_str());
            return 1;
        }
        SM2_INFO("loaded save state '%s'", options.load_state.c_str());
    }

    // -- headless boot test ------------------------------------------------
    // No window: run the machine and report where it got to.
    if (options.boot_test != 0) {
        if (machine_iface == nullptr) {
            SM2_ERROR("--boot-test needs a ROM");
            return 1;
        }

        int exit_code_boot_test = 0;
        std::vector<s16> recorded;
        std::optional<render::TextureDumper> texture_dumper;
        if (options.dump_textures) {
            texture_dumper.emplace(texture_dump_directory(options.config, loaded->game.name),
                                   loaded->game.name);
        }
        if (!options.dump_audio.empty()) {
            // About 767 stereo frames per video frame.
            recorded.reserve(static_cast<usize>(options.boot_test) * 800 * 2);
        }

        // Accessors common to every board.
        struct CpuView {
            hw::Model2MachineBase* machine;
            [[nodiscard]] bool        faulted() const { return machine->main_cpu_status().faulted; }
            [[nodiscard]] bool        halted() const { return machine->main_cpu_status().halted; }
            [[nodiscard]] u64         instructions() const { return machine->main_cpu_status().instructions; }
            [[nodiscard]] std::string fault_message() const { return machine->main_cpu_status().fault_message; }
            [[nodiscard]] std::string state_string() const { return machine->main_cpu_status().state_string; }
        };
        const CpuView    the_cpu{machine_iface};
        hw::SoundBoard*  the_sound = sound_board;
        const auto&      the_uart  = *sound_link;

        SM2_INFO("running %u frame(s) headless", options.boot_test);

        std::FILE* poly_log = nullptr;
        if (!options.poly_log.empty()) {
            poly_log = std::fopen(options.poly_log.c_str(), "w");
            if (poly_log != nullptr) {
                std::fprintf(poly_log,
                             "# frame seconds kept culled clipped read_start\n");
            }
        }

        const u64 start = SDL_GetPerformanceCounter();
        for (u32 frame = 0; frame < options.boot_test; ++frame) {
            if (options.coin_at != 0) {
                const osd::Input::ScriptedPress press = osd::Input::scripted_press(
                    frame, options.coin_at, loaded->game.start1_bit);
                machine_iface->inputs().in0 = static_cast<u8>(0xff & ~press.in0);
                machine_iface->inputs().in1 = static_cast<u8>(0xff & ~press.in1);
            }
            machine_iface->set_draw_distance(options.config.extended_draw_distance);
            machine_iface->run_frame();
            if (loaded->game.lightgun.present) {
                machine_iface->video().filter_gun_flash(options.config.lightgun_hide_flash,
                                                        machine_iface->inputs().in1);
            }
            if (texture_dumper) {
                texture_dumper->scan(*machine_iface);
            }

            // A numbered software frame every so often, to find the instant that
            // lines up with a MAME capture.
            const bool wanted_frame =
                options.screenshot_frames.empty()
                    ? options.screenshot_interval != 0
                          && (frame % options.screenshot_interval) == 0
                    : options.screenshot_frames.count(frame) != 0;
            if (options.soft_render && !options.dump_tilemap.empty() && wanted_frame) {
                machine_iface->compose_video();
                hw::dump_software_frame(*machine_iface, options.dump_tilemap,
                                        static_cast<int>(frame));
            }

            if (poly_log != nullptr) {
                const hw::RenderList& list = machine_iface->render_list();
                std::fprintf(poly_log, "%u %.4f %zu %u %u %05x\n", frame,
                             static_cast<double>(frame) / 57.52, list.polygons.size(),
                             list.culled, list.clipped_away, list.stats.read_start);
            }

            // Drain every frame: the sound board drops samples nothing collects,
            // which would leave gaps in a recording.
            if (the_sound != nullptr) {
                const std::span<const s16> produced = the_sound->pending_samples();
                if (!options.dump_audio.empty()) {
                    recorded.insert(recorded.end(), produced.begin(), produced.end());
                }
                the_sound->clear_pending_samples();
            }

            if (the_cpu.faulted()) {
                SM2_ERROR("the CPU faulted on frame %u", frame);
                break;
            }
        }
        if (poly_log != nullptr) {
            std::fclose(poly_log);
        }

        const double seconds = static_cast<double>(SDL_GetPerformanceCounter() - start)
                             / static_cast<double>(SDL_GetPerformanceFrequency());

        const double emulated = static_cast<double>(machine_iface->frames())
                              / (static_cast<double>(hw::Model2::kCpuClock)
                                 / static_cast<double>(hw::Model2::kCyclesPerFrame));

        std::printf("\n=== boot test ===\n");
        std::printf("frames run        : %llu\n",
                    (unsigned long long)machine_iface->frames());
        std::printf("instructions      : %llu\n",
                    (unsigned long long)the_cpu.instructions());
        std::printf("master cycles     : %llu\n",
                    (unsigned long long)machine_iface->cycles());
        std::printf("faulted           : %s\n",
                    the_cpu.faulted() ? the_cpu.fault_message().c_str()
                                      : "no");
        std::printf("cpu state         : %s%s\n", the_cpu.state_string().c_str(),
                    the_cpu.halted() ? " HALTED" : "");
        std::printf("interrupts        : intena %03x intreq %03x\n",
                    machine_iface->intena(), machine_iface->intreq());

        if (machine) {
            const hw::CoproTgp& copro = machine->copro();
            std::printf("copro uploaded    : %u word(s)\n", copro.uploaded_words());
            std::printf("copro instructions: %llu\n",
                        (unsigned long long)copro.cpu().instructions());
            std::printf("copro state       : %s%s\n", copro.cpu().state_string().c_str(),
                        copro.cpu().halted() ? " HALTED" : "");
            std::printf("copro fifos       : in %zu (peak overflow %zu), out %zu "
                        "(peak overflow %zu)\n",
                        copro.fifo_in().size(), copro.fifo_in().peak_overflow(),
                        copro.fifo_out().size(), copro.fifo_out().peak_overflow());

            const hw::CoproTgp::Activity& work = copro.activity();
            std::printf("copro work        : %llu command(s), %llu result(s), "
                        "%llu table lookup(s)\n",
                        (unsigned long long)work.commands_received,
                        (unsigned long long)work.results_sent,
                        (unsigned long long)work.table_reads);
            std::printf("copro memory      : display list %llu read / %llu written, "
                        "data ROM %llu read\n",
                        (unsigned long long)work.buffer_reads,
                        (unsigned long long)work.buffer_writes,
                        (unsigned long long)work.data_rom_reads);
        } else if (machine_2b != nullptr) {
            const hw::CoproSharc& copro = machine_2b->copro();
            std::printf("copro uploaded    : %u 16-bit word(s)\n", copro.uploaded_words());
            std::printf("copro instructions: %llu\n",
                        (unsigned long long)copro.cpu().instructions());
            std::printf("copro state       : %s%s\n", copro.cpu().state_string().c_str(),
                        copro.cpu().halted() ? " HALTED" : "");
            std::printf("copro fifos       : in %zu (peak overflow %zu), out %zu "
                        "(peak overflow %zu)\n",
                        copro.fifo_in().size(), copro.fifo_in().peak_overflow(),
                        copro.fifo_out().size(), copro.fifo_out().peak_overflow());
        } else if (machine_orig != nullptr) {
            // The original Model 2 carries the same MB86234 as Model 2A, so the
            // same figures apply.
            const hw::CoproTgp& copro = machine_orig->copro();
            std::printf("copro uploaded    : %u word(s)\n", copro.uploaded_words());
            std::printf("copro instructions: %llu\n",
                        (unsigned long long)copro.cpu().instructions());
            std::printf("copro state       : %s%s\n", copro.cpu().state_string().c_str(),
                        copro.cpu().halted() ? " HALTED" : "");
            std::printf("copro fifos       : in %zu (peak overflow %zu), out %zu "
                        "(peak overflow %zu)\n",
                        copro.fifo_in().size(), copro.fifo_in().peak_overflow(),
                        copro.fifo_out().size(), copro.fifo_out().peak_overflow());

            const hw::CoproTgp::Activity& work = copro.activity();
            std::printf("copro work        : %llu command(s), %llu result(s), "
                        "%llu table lookup(s)\n",
                        (unsigned long long)work.commands_received,
                        (unsigned long long)work.results_sent,
                        (unsigned long long)work.table_reads);
            std::printf("copro memory      : display list %llu read / %llu written, "
                        "data ROM %llu read\n",
                        (unsigned long long)work.buffer_reads,
                        (unsigned long long)work.buffer_writes,
                        (unsigned long long)work.data_rom_reads);

            const hw::Model2Original::IoBoardReport io = machine_orig->io_board_report();
            std::printf("io board          : %s, %s, z80 pc %04x sp %04x%s\n",
                        io.kind, io.present ? "firmware loaded" : "NO FIRMWARE",
                        io.pc, io.sp, io.halted ? " HALTED" : "");
            std::printf("io board z80      : %llu instruction(s), %llu cycle(s)\n",
                        (unsigned long long)io.instructions,
                        (unsigned long long)io.cycles);
            std::printf("io board traffic  : dual-port RAM %llu read / %llu written, "
                        "%llu conversion(s), %llu output latch(es)\n",
                        (unsigned long long)io.dual_port_reads,
                        (unsigned long long)io.dual_port_writes,
                        (unsigned long long)io.analog_samples,
                        (unsigned long long)io.output_writes);
            if (io.fpga_words != 0 || io.lightgun_reads != 0 || io.interrupts != 0) {
                std::printf("io board gun fpga : %llu configuration word(s), "
                            "%llu coordinate read(s), %llu timer interrupt(s)\n",
                            (unsigned long long)io.fpga_words,
                            (unsigned long long)io.lightgun_reads,
                            (unsigned long long)io.interrupts);
            }
            std::printf("io board unmapped : %llu read(s), %llu write(s), "
                        "%llu I/O port access(es)\n",
                        (unsigned long long)io.unmapped_reads,
                        (unsigned long long)io.unmapped_writes,
                        (unsigned long long)io.io_port_accesses);
        } else if (machine_m1 != nullptr) {
            const hw::Model1Tgp& tgp = machine_m1->tgp();
            std::printf("tgp instructions  : %llu\n",
                        (unsigned long long)tgp.cpu().instructions());
            std::printf("tgp state         : %s%s\n", tgp.cpu().state_string().c_str(),
                        tgp.cpu().halted() ? " HALTED" : "");
            std::printf("tgp fifos         : in %zu, out %zu\n", tgp.fifo_in().size(),
                        tgp.fifo_out().size());
            std::printf("polygons          : %zu\n", machine_iface->render_list().polygons.size());
        } else if (machine_2c != nullptr) {
            const hw::CoproTgpx4& copro = machine_2c->copro();
            // Two host writes make one 64-bit program word; an odd count of halves
            // means the last word is half-written.
            std::printf("copro uploaded    : %u host write(s), %u program word(s)\n",
                        copro.uploaded_words(), copro.uploaded_words() / 2);
            std::printf("copro instructions: %llu\n",
                        (unsigned long long)copro.cpu().instructions());
            std::printf("copro state       : %s%s\n", copro.cpu().state_string().c_str(),
                        copro.cpu().halted() ? " HALTED" : "");
            // The MB86235 records an unimplemented opcode and stops rather than
            // aborting, so report it.
            std::printf("copro faulted     : %s\n",
                        copro.cpu().faulted() ? copro.cpu().fault_message().c_str()
                                              : "no");
            std::printf("copro fifos       : in %zu (peak overflow %zu), out %zu "
                        "(peak overflow %zu)\n",
                        copro.fifo_in().size(), copro.fifo_in().peak_overflow(),
                        copro.fifo_out().size(), copro.fifo_out().peak_overflow());
        }

        // Reported whether or not there is a board behind the link.
        {
            const hw::I8251::Counters& uart = the_uart.counters();
            std::printf("sound link        : %llu byte(s) to the board, %llu back, "
                        "%llu overrun(s), %llu status reads\n",
                        (unsigned long long)uart.bytes_sent,
                        (unsigned long long)uart.bytes_received,
                        (unsigned long long)uart.overruns,
                        (unsigned long long)uart.status_reads);
        }

        // Past this point the report is board-specific.
        auto* const the_scsp_board = dynamic_cast<hw::Model2Sound*>(the_sound);
        auto* const the_m1_board   = dynamic_cast<hw::M1Audio*>(the_sound);

        if (the_sound == nullptr) {
            std::printf("sound board       : none\n");
        } else if (!the_sound->present()) {
            std::printf("sound 68000       : no program ROM\n");
        } else if (the_scsp_board != nullptr) {
            const hw::Model2Sound::Counters& snd = the_scsp_board->counters();
            std::printf("sound 68000       : %s\n",
                        the_scsp_board->cpu().state_string().c_str());
            std::printf("sound cycles      : %llu\n",
                        (unsigned long long)the_scsp_board->cpu().cycles());
            std::printf("sound scsp        : %llu write(s), %llu read(s)\n",
                        (unsigned long long)snd.scsp_writes,
                        (unsigned long long)snd.scsp_reads);
            std::printf("sound samples     : %llu read(s), banking %llu write(s)\n",
                        (unsigned long long)snd.sample_reads,
                        (unsigned long long)snd.snd_ctrl_writes);
            std::printf("sound unmapped    : %llu read(s), %llu write(s)\n",
                        (unsigned long long)snd.unmapped_reads,
                        (unsigned long long)snd.unmapped_writes);

            const hw::Scsp::Stats& scsp = the_scsp_board->scsp().stats();
            std::printf("scsp audio        : %llu sample(s) at %u Hz, peak %d/32767, "
                        "%llu dropped\n",
                        (unsigned long long)scsp.samples, the_sound->sample_rate(),
                        scsp.peak_output, (unsigned long long)snd.samples_dropped);
            std::printf("scsp slots        : %llu key-on(s), %u sounding now\n",
                        (unsigned long long)scsp.slot_starts,
                        the_scsp_board->scsp().active_slots());
            std::printf("scsp events       : %llu timer irq(s), %llu DMA(s), "
                        "MIDI %llu in / %llu out\n",
                        (unsigned long long)scsp.timer_interrupts,
                        (unsigned long long)scsp.dma_transfers,
                        (unsigned long long)scsp.midi_in_bytes,
                        (unsigned long long)scsp.midi_out_bytes);
        } else if (the_m1_board != nullptr) {
            const hw::M1Audio::Counters& snd = the_m1_board->counters();
            std::printf("sound 68000       : %s\n",
                        the_m1_board->cpu().state_string().c_str());
            std::printf("sound cycles      : %llu\n",
                        (unsigned long long)the_m1_board->cpu().cycles());
            std::printf("sound link (board): %llu byte(s) in, %llu out, "
                        "%llu reg read(s), %llu reg write(s)\n",
                        (unsigned long long)snd.bytes_from_host,
                        (unsigned long long)snd.bytes_to_host,
                        (unsigned long long)snd.uart_reads,
                        (unsigned long long)snd.uart_writes);
            const hw::Ym3438::Stats& ym = the_m1_board->ym().stats();
            std::printf("sound ym3438      : %llu write(s), %llu status read(s) "
                        "(%llu non-zero)\n",
                        (unsigned long long)snd.ym_writes,
                        (unsigned long long)snd.ym_reads,
                        (unsigned long long)snd.ym_status_reads);
            std::printf("ym3438 audio      : %llu sample(s) at %u Hz, peak %d/32767\n",
                        (unsigned long long)ym.samples, the_m1_board->ym().native_rate(),
                        ym.peak_output);
            std::printf("ym3438 timers     : %llu A, %llu B, %llu irq change(s)\n",
                        (unsigned long long)ym.timer_a_expiries,
                        (unsigned long long)ym.timer_b_expiries,
                        (unsigned long long)ym.irq_changes);
            std::printf("sound unmapped    : %llu read(s), %llu write(s)\n",
                        (unsigned long long)snd.unmapped_reads,
                        (unsigned long long)snd.unmapped_writes);

            for (u32 chip = 0; chip < 2; ++chip) {
                const hw::MultiPcm::Stats& pcm = the_m1_board->pcm(chip).stats();
                std::printf("multipcm %u        : %llu reg write(s), %llu bank "
                            "write(s), %llu key-on(s), %u sounding now\n",
                            chip + 1, (unsigned long long)snd.pcm_writes[chip],
                            (unsigned long long)snd.bank_writes[chip],
                            (unsigned long long)pcm.key_ons,
                            the_m1_board->pcm(chip).active_voices());
                std::printf("multipcm %u audio  : %llu sample(s) at %u Hz, "
                            "peak %d/32767\n",
                            chip + 1, (unsigned long long)pcm.samples,
                            the_m1_board->pcm(chip).sample_rate(), pcm.peak_output);
            }
            std::printf("sound dropped     : %llu sample(s)\n",
                        (unsigned long long)snd.samples_dropped);
        }

        std::printf("wall time         : %.2f s for %.2f s emulated (%.2fx)\n",
                    seconds, emulated, seconds > 0.0 ? emulated / seconds : 0.0);
        std::printf("\n");
        machine_iface->log_unmapped_summary();
        machine_iface->log_burst_summary();

        // Both TGP boards read the same table ROM through the same coprocessor.
        hw::CoproTgp* tgp = machine != nullptr        ? &machine->copro()
                          : machine_orig != nullptr   ? &machine_orig->copro()
                                                      : nullptr;
        if (tgp != nullptr && !hw::run_copro_selftest(*tgp)) {
            SM2_ERROR("the coprocessor's mathematical units are out of tolerance");
            exit_code_boot_test = 1;
        }

        hw::print_render_list_summary(*machine_iface);
        hw::print_tilemap_summary(*machine_iface);

        // Compose once, at the end, through the same colour chain and compositor
        // the display uses.
        machine_iface->compose_video();
        if (!options.dump_tilemap.empty()) {
            hw::dump_tilemaps(*machine_iface, options.dump_tilemap);
            hw::dump_composed_frame(*machine_iface, options.dump_tilemap);
            hw::dump_render_list_wireframe(*machine_iface, options.dump_tilemap);
            hw::dump_display_list(*machine_iface, options.dump_tilemap);
            hw::dump_framebuffer(*machine_iface, options.dump_tilemap);
            hw::dump_software_frame(*machine_iface, options.dump_tilemap);
        }

        if (!options.dump_audio.empty()) {
            if (the_sound == nullptr) {
                SM2_WARN("--dump-audio: this board's sound hardware is not "
                         "emulated, so there is nothing to record");
            } else if (!hw::write_wav(options.dump_audio, recorded,
                                      the_sound->sample_rate())) {
                exit_code_boot_test = 1;
            }
        }

        if (texture_dumper) {
            texture_dumper->finish(machine_iface);
        }

        machine_iface->save_nvram();
        return the_cpu.faulted() ? 1 : exit_code_boot_test;
    }

    SDL_SetAppMetadata("Keisho", SM2_VERSION, "keisho");

    if (!SDL_Init(0)) {
        SM2_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    int exit_code = 0;
    {
        osd::WindowConfig window_config;
        window_config.title      = std::string("Keisho ") + SM2_VERSION;
        window_config.width      = options.config.window_width;
        window_config.height     = options.config.window_height;
        window_config.fullscreen = options.config.fullscreen;
        window_config.fullscreen_resolution = options.config.fullscreen_resolution;

        // Which GPU backend presents. `software` only replaces the drawing,
        // so it presents through OpenGL where built, else Vulkan.
        bool present_with_opengl = options.graphics_backend != GraphicsBackendChoice::Vulkan;
#if !defined(SM2_HAVE_OPENGL_DESKTOP) && !defined(SM2_HAVE_OPENGL_ES)
        present_with_opengl = false;
#endif
#if !defined(SM2_HAVE_VULKAN)
        present_with_opengl = true;
#endif

        render::BackendConfig backend_config;
        backend_config.enable_validation = options.config.validation;
        backend_config.vsync             = options.config.vsync;
        backend_config.preferred_device  = options.config.gpu;
        // The software renderer draws at native resolution.
        backend_config.render_scale =
            options.start_in_software_renderer ? 1U : options.config.render_scale;

        // Present-stage options apply to every backend, the software renderer
        // included.
        backend_config.present.scaling_method        = options.config.scaling_method;
        backend_config.present.aspect_mode           = options.config.aspect_mode;
        backend_config.present.crt_enabled           = options.config.crt_enabled;
        backend_config.present.crt_scanline_strength = options.config.crt_scanline_strength;
        backend_config.present.crt_mask_strength     = options.config.crt_mask_strength;
        backend_config.present.crt_glow_strength     = options.config.crt_glow_strength;
        backend_config.present.crt_curvature         = options.config.crt_curvature;

        // Enhancement options, faithful by default; GPU-gated inside the backend.
        backend_config.enhancement.texture_filter = options.config.texture_filter;
        backend_config.enhancement.anisotropy     = options.config.anisotropy;
        backend_config.enhancement.upscale_2d     = options.config.upscale_2d;
        backend_config.enhancement.translucency   = options.config.translucency;

        // Each factory exists only when its SM2_BUILD_* option was on.
        osd::Window                      window;
        std::unique_ptr<render::Backend> backend;
        const auto open_backend = [&](bool opengl) {
            window_config.graphics_api = opengl ? osd::GraphicsApi::OpenGl
                                                : osd::GraphicsApi::Vulkan;
#if defined(SM2_HAVE_OPENGL_ES)
            // GLES on X11 needs EGL, not GLX; force it before the GL library loads.
            if (opengl) {
                SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
            }
#endif
            if (!window.create(window_config)) {
                window.destroy();
                return false;
            }
            if (opengl) {
#if defined(SM2_HAVE_OPENGL_DESKTOP) || defined(SM2_HAVE_OPENGL_ES)
                backend = render::create_opengl_backend();
#endif
            } else {
#if defined(SM2_HAVE_VULKAN)
                backend = render::create_vulkan_backend();
#endif
            }
            if (backend && options.start_in_software_renderer) {
                // Model 3 draws on the CPU too; the GPU backend only presents.
                backend = render::create_software_model3_backend(std::move(backend));
            }
            if (!backend) {
                SM2_ERROR("no GPU backend is available to present with. Build with "
                          "-DSM2_BUILD_VULKAN=ON and/or -DSM2_BUILD_OPENGL_DESKTOP=ON "
                          "(or run headless with --boot-test).");
            } else if (!backend->init(window, backend_config)) {
                SM2_ERROR("render backend initialisation failed");
                backend.reset();
            }
            if (!backend) {
                window.destroy();
                return false;
            }
            return true;
        };

        bool opened = open_backend(present_with_opengl);
#if defined(SM2_HAVE_OPENGL_DESKTOP) || defined(SM2_HAVE_OPENGL_ES)
        // A GPU without Vulkan 1.3 can still run everything through OpenGL.
        if (!opened && !present_with_opengl) {
            SM2_WARN("Vulkan is unavailable; falling back to OpenGL");
            present_with_opengl = true;
            opened              = open_backend(true);
        }
#endif
        if (!opened) {
            SDL_Quit();
            return 1;
        }

        // Name of the constructed GPU backend, for the title bar and overlay label.
        const char* gpu_backend_name = present_with_opengl ? "OpenGL" : "Vulkan";

        // The settings overlay. Owns only ImGui's context and its SDL3 platform
        // backend; the render backend draws what it builds.
        osd::Gui gui;
        gui.set_config_path(config_path);
        gui.set_save_filter(to_save);
        gui.set_texture_backend(backend.get());
        // The renderers this build offers, most-preferred first. Software only
        // presents through a GPU backend, so it needs one present.
        {
            std::vector<std::string> renderers;
#if defined(SM2_HAVE_VULKAN)
            renderers.emplace_back("vulkan");
#endif
#if defined(SM2_HAVE_OPENGL_DESKTOP) || defined(SM2_HAVE_OPENGL_ES)
            renderers.emplace_back("opengl");
#endif
            if (!renderers.empty()) {
                renderers.emplace_back("software");
            }
            gui.set_available_renderers(std::move(renderers));
        }
        {
            // One row per family, titled after its parent set, for the Audio
            // tab's volumes and the Gamepad tab's layouts. kNoVolume lists
            // families that do not run.
            static const std::set<std::string> kNoVolume = {"rascot2"};
            std::map<std::string, std::string> titles;
            std::map<std::string, std::pair<bool, bool>> guns;
            std::map<std::string, osd::Gui::GameFamily>  uses;
            for (const rom::GameSpec& game : database.games()) {
                const std::string family = game_family(game);
                if (kNoVolume.count(family) != 0) {
                    continue;
                }
                if (game.name == family || titles.find(family) == titles.end()) {
                    titles[family] = game.title;
                }
                const bool gun = game.lightgun.present
                              || std::any_of(game.analog.begin(), game.analog.end(),
                                             [](const rom::AnalogChannel& c) {
                                                 return c.control == rom::AnalogControl::Gun1X;
                                             });
                if (gun) {
                    guns[family] = {true, game.gun_missile};
                }
                osd::Gui::GameFamily& use = uses[family];
                use.inputs |= static_cast<u32>(game.inputs);
                const bool one_player = rom::has_input(game.inputs, rom::InputFlags::Fishing);
                bool aim_y = false;
                for (const rom::AnalogChannel& channel : game.analog) {
                    switch (osd::Input::pad_axis_role(channel.control, one_player)) {
                        case Config::PadAxisRole::AimY:   aim_y = true; [[fallthrough]];
                        case Config::PadAxisRole::AimX:   use.aim = true; break;
                        case Config::PadAxisRole::Accel:  use.accel = true; break;
                        case Config::PadAxisRole::Brake:  use.brake = true; break;
                        case Config::PadAxisRole::LeverX:
                        case Config::PadAxisRole::LeverY: use.lever = true; break;
                        default: break;
                    }
                }
                use.gears = use.gears || game.gearbox || game.shift_buttons;
                use.vr    = std::max(use.vr, static_cast<u32>(game.vr_button_count));
                use.gate  = use.gate || game.gearbox;
                use.shift = use.shift || game.shift_buttons;
                use.joystick = use.joystick || rom::has_input(game.inputs, rom::InputFlags::Joystick1);
                use.bat      = use.bat || std::any_of(game.analog.begin(), game.analog.end(), [](const rom::AnalogChannel& c) {
                                   return c.control == rom::AnalogControl::Bat1;
                               });
                using Steer = osd::Gui::GameFamily::Steer;
                if (rom::has_input(game.inputs, rom::InputFlags::Fishing)) use.steer = Steer::Rod;
                for (const rom::AnalogChannel& channel : game.analog) {
                    Steer kind = Steer::None;
                    switch (channel.control) {
                        case rom::AnalogControl::Steer:     kind = Steer::Wheel; break;
                        case rom::AnalogControl::Bank:
                        case rom::AnalogControl::Handle:    kind = Steer::Handlebar; break;
                        case rom::AnalogControl::StickX:    kind = Steer::Stick; break;
                        case rom::AnalogControl::Gun1X:     kind = Steer::Gun; break;
                        case rom::AnalogControl::Slide:
                        case rom::AnalogControl::Curving:
                        case rom::AnalogControl::Swing:
                        case rom::AnalogControl::Inclining: kind = Steer::Board; break;
                        default: break;
                    }
                    // A wheel is the cabinet's main control over anything else.
                    if (kind == Steer::Wheel || (kind != Steer::None && use.steer == Steer::None)) {
                        use.steer = kind;
                    }
                }
                if (rom::has_input(game.inputs, rom::InputFlags::Harley)) use.steer = Steer::Handlebar;
                // A wheel's second axis is not steering: Desert Tank's yoke
                // also tilts the turret.
                use.tilt = use.tilt || (aim_y && (use.steer == Steer::Wheel || use.steer == Steer::Handlebar));
            }
            std::vector<osd::Gui::GameFamily> families;
            for (auto& [key, title] : titles) {
                const std::string shown = game_title_without_cabinet(title);
                const auto        gun   = guns.find(key);
                osd::Gui::GameFamily family = uses[key];
                family.key     = key;
                family.title   = shown.empty() ? key : shown;
                family.gun     = gun != guns.end();
                family.missile = gun != guns.end() && gun->second.second;
                families.push_back(std::move(family));
            }
            std::sort(families.begin(), families.end(),
                      [](const osd::Gui::GameFamily& a, const osd::Gui::GameFamily& b) {
                          return a.title < b.title;
                      });
            gui.set_game_families(std::move(families));

            // A section named after a clone belongs to its family; any other
            // unknown name is dropped so it is not saved back.
            auto& layouts = options.config.pad_layouts;
            for (auto it = layouts.begin(); it != layouts.end();) {
                if (titles.count(it->first) != 0) {
                    ++it;
                    continue;
                }
                const rom::GameSpec* clone = database.find(it->first);
                if (clone != nullptr && titles.count(game_family(*clone)) != 0) {
                    layouts.try_emplace(game_family(*clone), it->second);
                } else {
                    SM2_WARN("keisho.ini: no game family '%s'; its gamepad layout is ignored",
                             it->first.c_str());
                }
                it = layouts.erase(it);
            }
        }
        if (!gui.init(window.handle(), options.config.language)) {
            SM2_ERROR("could not initialise the GUI overlay");
            SDL_Quit();
            return 1;
        }
        // After gui.init(): the backend's own ImGui renderer backend reads
        // ImGui's global context, which must exist first.
        if (!backend->init_overlay(gui)) {
            SM2_ERROR("could not initialise the GUI overlay's renderer backend");
            SDL_Quit();
            return 1;
        }

        // Declared here so its destructor joins the thread after the loop but
        // before the backend is torn down.
        osd::Scraper scraper;

        // Build the picker's entry list from the launchable sets and turn it on.
        // Used at startup and when Esc unloads a game to return here.
        const auto show_picker = [&]() {
            // A set is launchable when it has an archive in its ROM folder:
            // its own <name>.zip / .7z, or, for a clone, its parent's merged one.
            std::vector<osd::Gui::PickerEntry> entries;
            std::vector<osd::Scraper::Entry>   scrape_list;
            for (const rom::GameSpec& game : database.games()) {
                if (!find_set_archive(database, options.config, game.name).has_value()) {
                    continue;
                }
                osd::Gui::PickerEntry entry;
                entry.name   = game.name;
                entry.title  = game.title;
                entry.system = game.board == rom::Board::Model1 ? "Model 1"
                             : game.board == rom::Board::Model3 ? "Model 3"
                                                                : "Model 2";
                entries.push_back(entry);
                scrape_list.push_back({game.name, game.title});
            }
            std::sort(entries.begin(), entries.end(),
                      [](const osd::Gui::PickerEntry& a,
                         const osd::Gui::PickerEntry& b) { return a.title < b.title; });

            SM2_INFO("game picker: %zu launchable set(s) in '%s'", entries.size(),
                     options.config.model2_rom_dir.c_str());

            // stop() first so a second start() (return-to-picker) never
            // move-assigns over a still-joinable worker thread.
            scraper.stop();
            scraper.start(options.config.artwork_dir, std::move(scrape_list),
                          options.config.scrape_artwork);
            gui.enable_picker(std::move(entries), backend.get(), &scraper);
        };

        // No ROM: show the picker if there is a ROM folder, else the settings
        // overlay so one can be set.
        if (!machine_any) {
            if (will_show_picker) {
                show_picker();
            } else {
                gui.show();
            }
        }

        // GPU names for the settings dropdown. Only Vulkan picks a device, and an
        // OpenGL or software run must not touch Vulkan at all.
        const std::vector<std::string> gpu_names =
            present_with_opengl ? std::vector<std::string>{} : render::enumerate_render_devices();

        // No gamepad is not fatal: the keyboard covers everything the cabinet has.
        osd::Input input;
        bool input_background = options.config.input_background;
        apply_input_background(input_background);
        input.set_pad_devices(options.config.pad_device);
        input.set_gun_devices(options.config.gun_devices);
        if (!input.init(wheel_settings_from(options.config))) {
            SM2_WARN("gamepads are unavailable; the keyboard still works");
        }

        // The drive board model goes into save states while this Input lives.
        hw::set_host_state([&input](Archive& ar) { input.serialize_drive_board(ar); });
        struct HostStateGuard {
            ~HostStateGuard() { hw::set_host_state({}); }
        } host_state_guard;

        // No audio device is not fatal; Audio::init reports it. SDL resamples
        // from the board's own rate.
        osd::Audio audio;
        osd::Outputs outputs;
        if (sound_board != nullptr) {
            static_cast<void>(audio.init(sound_board->sample_rate()));
        }

        // With no machine, present empty surfaces over a recognisable background.
        hw::Model2Video idle_video;

        // The CPU rasteriser, used when --graphics-backend software was chosen
        // at launch and a machine is loaded.
        hw::SoftRenderer soft_renderer;
        std::vector<u32> soft_frame(static_cast<usize>(backend->native_width())
                                        * backend->native_height(),
                                    0);
        // The same rasteriser one frame behind, on its own thread.
        hw::AsyncSoftRenderer async_soft(soft_renderer);
        async_soft.set_use_slow_cores(options.config.software_slow_cores);

        // Paced against real time, because the machine's 57.5245 Hz matches no
        // monitor's refresh rate. With vsync the longer frame wins, so a display
        // slower than 57.5 Hz needs --no-vsync.
        osd::FramePacer pacer;
        pacer.set_throttled(options.config.throttle);
        pacer.start(hw::Model2::kFrameNanoseconds);

{
            const auto key = [&](Config::Hotkey hotkey) {
                const std::string& name = options.config.hotkey(hotkey);
                return name.empty() ? "none" : name.c_str();
            };
            SM2_INFO("entering main loop; %s back to games, %s quits, %s pauses, "
                     "%s fast-forwards, %s/%s quick save/load, %s FPS, %s menu, "
                     "%s fullscreen, %s screenshot",
                     key(Config::Hotkey::Exit), key(Config::Hotkey::Quit),
                     key(Config::Hotkey::Pause), key(Config::Hotkey::FastForward),
                     key(Config::Hotkey::QuickSave), key(Config::Hotkey::QuickLoad),
                     key(Config::Hotkey::Fps), key(Config::Hotkey::Overlay),
                     key(Config::Hotkey::Fullscreen), key(Config::Hotkey::Screenshot));
        }

        /// Everything the sound board produced, when --dump-audio was given.
        std::vector<s16> recorded_audio;
        std::vector<s16> scaled_audio;  ///< the per-game volume's scratch buffer

        // Per-stage CPU timers for --profile. Always built; maybe_scope() makes
        // recording a no-op when profiling is off.
        core::StageTimer stage_run_frame("run_frame");
        core::StageTimer stage_geometry("geometry_engine");
        core::StageTimer stage_compose("tilemap_compose");
        // build(): triangulation, the copy into mapped buffers and any texture
        // decode dispatch.
        core::StageTimer stage_build("poly3d_build_and_memcpy");
        core::StageTimer stage_tilemap_upload("tilemap_upload_memcpy");
        // Everything else issuing vkCmd* this frame: 3D draws, both tilemap
        // draws, the present blit.
        core::StageTimer stage_record("command_recording");
        core::StageTimer stage_submit("submit_and_present");
        core::StageTimer stage_software("software_renderer");
        // software_async: what the main thread pays (snapshot, wait) and what
        // the render thread spent (async_draw).
        core::StageTimer stage_software_wait("software_wait");
        core::StageTimer stage_software_snapshot("software_snapshot");
        core::StageTimer stage_software_async("software_async_draw");
        // Time blocked on the GPU inside begin_frame() (Vulkan fence and
        // acquire, GL swap back-pressure).
        core::StageTimer stage_present_wait("present_wait");
        // The three cores run_frame() interleaves.
        core::StageTimer stage_cpu_i960("cpu_i960");
        core::StageTimer stage_cpu_copro("cpu_copro");
        core::StageTimer stage_cpu_sound("cpu_sound");
        // Model 3 frame build: the wait for the worker, and the build's own
        // CPU time and parts on whichever thread ran it.
        core::StageTimer stage_m3_wait("m3_build_wait");
        core::StageTimer stage_m3_build("m3_build");
        core::StageTimer stage_m3_textures("m3_textures");
        core::StageTimer stage_m3_tilegen("m3_tilegen");
        core::StageTimer stage_m3_scene("m3_scene");
        core::StageTimer stage_m3_plan("m3_plan");
        core::StageTimer stage_m3_record("m3_record_3d");
        if (machine_any) {
            machine_any->set_core_profiling(options.profile);
        }
        if (options.profile) {
            const usize expected = static_cast<usize>(options.duration_seconds) * 120;
            for (core::StageTimer* timer :
                {&stage_run_frame, &stage_geometry, &stage_compose, &stage_build,
                 &stage_tilemap_upload, &stage_record, &stage_submit, &stage_software,
                 &stage_software_wait, &stage_software_snapshot, &stage_software_async,
                 &stage_present_wait, &stage_cpu_i960, &stage_cpu_copro,
                 &stage_cpu_sound, &stage_m3_wait, &stage_m3_build, &stage_m3_textures,
                 &stage_m3_tilegen, &stage_m3_scene, &stage_m3_plan, &stage_m3_record}) {
                timer->reserve(expected);
            }
        }
        // GPU stage samples, read back once per frame. Indexed by render::GpuStage.
        std::array<std::vector<double>, static_cast<usize>(render::GpuStage::kCount)>
            gpu_stage_samples;
        bool gpu_timing_unavailable_warned = false;

        // One snapshot every other frame, so a held rewind runs at twice real time.
        constexpr u32 kRewindInterval = 2;
        hw::Rewind                   rewind;
        const hw::Machine* rewind_machine = nullptr;
        bool                         rewind_held    = false;  ///< The rewind key is down.

        u32  frames_presented = 0;   ///< Since the loop started.
        u32  frames_written_off = 0; ///< Whole frames given up after a stall.
        bool paused             = false;
        bool audio_paused_state = false; ///< Effective pause, to pause audio and pacer on change.
        bool fast_forward       = false;
        bool running            = true;
        bool quit_key_pressed   = false;  ///< The quit key went down during this run.
        bool screenshot_requested = false;  ///< Serviced next frame.

        // KEISHO_UI_SCRIPT="<ms>:<key>,<ms>:<key>,...": key presses fed to the
        // window at those times after start, by SDL key name ("F10", "Down",
        // "Return", "Page Down"). For driving the overlay from a script where no
        // input automation is available; off when unset.
        struct ScriptedKey {
            u64         at_ns;
            SDL_Keycode key;
            bool        sent = false;
        };
        std::vector<ScriptedKey> scripted_keys;
        if (const char* script = std::getenv("KEISHO_UI_SCRIPT"); script != nullptr && script[0] != '\0') {
            std::string_view rest(script);
            while (!rest.empty()) {
                const size_t comma = rest.find(',');
                std::string_view step = rest.substr(0, comma);
                rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
                const size_t colon = step.find(':');
                if (colon == std::string_view::npos) continue;
                const std::string ms_text(step.substr(0, colon));
                const std::string key_name(step.substr(colon + 1));
                const SDL_Keycode key = SDL_GetKeyFromName(key_name.c_str());
                if (key == SDLK_UNKNOWN) {
                    SM2_WARN("KEISHO_UI_SCRIPT: unknown key '%s'", key_name.c_str());
                    continue;
                }
                scripted_keys.push_back({static_cast<u64>(std::strtoull(ms_text.c_str(), nullptr, 10)) * 1000000ULL, key});
            }
        }
        bool return_to_picker_requested = false;  ///< Set by the exit key; unload + picker.
        std::string dropped_path;  ///< A file dropped on the window, launched next frame.

        // A pending save-state action from a hotkey or the overlay, serviced
        // between frames. `slot` is kQuickSlot for the hotkeys.
        enum class StateOp { Save, Load, Delete };
        struct StateAction {
            StateOp     op = StateOp::Save;
            std::string slot;
        };
        std::optional<StateAction> state_action;
        bool settings_was_visible = gui.visible();  ///< Settings visibility, last frame.
        // What the game list is built from, as it was when the settings opened:
        // the list is rebuilt only when one of these changed there.
        const auto list_inputs = [&] {
            return std::array<std::string, 4>{options.config.model2_rom_dir, options.config.model1_rom_dir,
                                              options.config.model3_rom_dir,
                                              options.config.scrape_artwork ? "scrape" : ""};
        };
        std::array<std::string, 4> list_inputs_at_open = list_inputs();
        // Fixed at launch by --graphics-backend software.
        const bool use_software_renderer = options.start_in_software_renderer;
        // 16:9 widens the frame only on a GPU renderer; elsewhere it shows 4:3.
        // Model 3's software renderer widens as its GPU renderers do.
        const auto can_widen = [&] {
            return (!use_software_renderer || machine_m3 != nullptr) && backend->capabilities().widescreen;
        };
        const auto shown_aspect = [&]() {
            return options.config.aspect_mode == AspectMode::Widescreen && !can_widen()
                       ? AspectMode::FourThree
                       : options.config.aspect_mode;
        };
        // Draw one frame behind on a second thread; off for captures, which
        // want the frame they asked for.
        const bool async_software = use_software_renderer && options.config.software_async
                                    && options.screenshot.empty() && !options.soft_render
                                    && options.screenshot_interval == 0
                                    && options.screenshot_frames.empty();
        if (use_software_renderer) {
            SM2_INFO("software renderer: %s", async_software ? "one frame behind, on its own thread"
                                                              : "synchronous");
        }
        u64  last_title_ns      = SDL_GetTicksNS();

        // --duration's deadline, and the per-frame wall-clock cost for its
        // summary, measured start-to-start so it covers the whole loop.
        const u64 run_start_ns = SDL_GetTicksNS();
        const u64 run_deadline_ns =
            options.duration_seconds != 0
                ? run_start_ns + static_cast<u64>(options.duration_seconds) * 1'000'000'000ULL
                : 0;
        std::vector<double> frame_times_ms;
        if (run_deadline_ns != 0) {
            // Reserved so reallocation does not skew the timings.
            frame_times_ms.reserve(static_cast<usize>(options.duration_seconds) * 2000);
        }

        // "Keisho — <game or 'no game'> [Vulkan|OpenGL|Software]", plus the
        // rate and pause state once the loop is running.
        const auto build_title = [&]() {
            std::string title = std::string("Keisho — ")
                              + (loaded.has_value() ? loaded->game.title : "no game")
                              + (use_software_renderer
                                    ? " [Software]"
                                    : (std::string(" [") + gpu_backend_name + "]"));
            if (paused) {
                title += " — paused";
            } else if (frames_presented != 0) {
                char rate[32];
                std::snprintf(rate, sizeof(rate), " — %.1f Hz", pacer.measured_hz());
                title += rate;
                if (!pacer.throttled()) {
                    title += " (unthrottled)";
                }
            }
            return title;
        };
        window.set_title(build_title());

        std::optional<render::TextureDumper> texture_dumper;
        std::unique_ptr<render::model3::TextureDumper> model3_dumper;
        std::string                          texture_dump_game;

        // Custom textures for the running game; the backend holds a pointer,
        // so it is cleared there before this is replaced or freed.
        std::unique_ptr<render::TextureReplacements> replacements;
        std::string                                  replacements_game;

        // Make `next` the running game, from the picker or a dropped file.
        const auto adopt_game = [&](LoadedMachine&& next) {
            async_soft.forget_machine();
            backend->wait_idle();
            backend->reset_model3();
            loaded        = std::move(next);
            machine_iface = loaded->machine_iface;
            machine       = loaded->machine;
            machine_2b    = loaded->machine_2b;
            machine_2c    = loaded->machine_2c;
            machine_orig  = loaded->machine_orig;
            machine_m1    = loaded->machine_m1;
            machine_m3    = loaded->machine_m3;
            machine_any   = loaded->machine_owner.get();
            main_cpu      = loaded->main_cpu;
            sound_board   = loaded->sound_board;
            sound_link    = loaded->sound_link;

            if (sound_board != nullptr) {
                static_cast<void>(audio.init(sound_board->sample_rate()));
            }
            configure_cabinet_link(*machine_any, options.config);
            if (gui.picker_active()) {
                scraper.stop();
                gui.release_picker_textures();
                gui.hide_picker();
            }
            window.set_title(build_title());
            pacer.resync();
        };

        const auto is_hotkey = [&](const SDL_KeyboardEvent& key, Config::Hotkey hotkey) {
            const std::string& name  = options.config.hotkey(hotkey);
            const SDL_Keycode  bound = name.empty() ? SDLK_UNKNOWN : SDL_GetKeyFromName(name.c_str());
            return bound != SDLK_UNKNOWN && key.key == bound;
        };

        while (running) {
            const u64 frame_start_ns = SDL_GetTicksNS();

            // Inputs in the background, as the wheel settings last set it.
            if (options.config.input_background != input_background) {
                input_background = options.config.input_background;
                apply_input_background(input_background);
            }

            // Scripted key presses due now (KEISHO_UI_SCRIPT): a press and a
            // release, through the same queue as the keyboard's.
            for (ScriptedKey& scripted : scripted_keys) {
                if (scripted.sent || SDL_GetTicksNS() - run_start_ns < scripted.at_ns) continue;
                scripted.sent = true;
                for (const bool down : {true, false}) {
                    SDL_Event key_event{};
                    key_event.type         = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    key_event.key.timestamp = SDL_GetTicksNS();
                    key_event.key.windowID = SDL_GetWindowID(window.handle());
                    key_event.key.key      = scripted.key;
                    key_event.key.scancode = SDL_GetScancodeFromKey(scripted.key, nullptr);
                    key_event.key.down     = down;
                    SDL_PushEvent(&key_event);
                }
            }

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // Let ImGui see every event, bar a key the System tab is
                // binding, which its keyboard navigation would also press.
                if (event.type != SDL_EVENT_KEY_DOWN || !gui.visible() || !gui.capturing_key()) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                }

                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        running = false;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        if (gui.visible() && gui.capturing_key()) {
                            // The System tab is binding a shortcut.
                            gui.capture_key(options.config, event.key.key);
                        } else if (event.key.key == SDLK_ESCAPE && gui.load_error_open()) {
                            // The dialog takes this Esc.
                        } else if (event.key.key == SDLK_ESCAPE && gui.visible() && !event.key.repeat) {
                            // Back out of the settings before anything else.
                            gui.escape();
                        } else if (event.key.key == SDLK_ESCAPE && !event.key.repeat && gui.picker_escape()) {
                            // The game list's search was cleared instead.
                        } else if (is_hotkey(event.key, Config::Hotkey::Exit)) {
                            // Return to the game picker when there is one,
                            // otherwise quit. Serviced after the event loop.
                            if (will_show_picker && machine_any != nullptr) {
                                return_to_picker_requested = true;
                            } else {
                                running = false;
                            }
                        } else if (is_hotkey(event.key, Config::Hotkey::Quit) && !event.key.repeat) {
                            quit_key_pressed = true;
                        } else if (is_hotkey(event.key, Config::Hotkey::Overlay) && !event.key.repeat) {
                            gui.toggle();
                        } else if ((is_hotkey(event.key, Config::Hotkey::Fullscreen)
                                    || (event.key.key == SDLK_F
                                        && (event.key.mod & SDL_KMOD_GUI) != 0))
                                   && !event.key.repeat) {
                            // Plus Cmd+F on macOS: the OS there reserves F11
                            // (Show Desktop), so it never reaches us.
                            options.config.fullscreen = !window.fullscreen();
                            window.set_fullscreen(options.config.fullscreen);
                        } else if (is_hotkey(event.key, Config::Hotkey::Screenshot) && !event.key.repeat) {
                            screenshot_requested = true;
                        } else if (!gui.visible()) {
                            // Only process game keys when the overlay is hidden.
                            if (is_hotkey(event.key, Config::Hotkey::Pause) && !event.key.repeat) {
                                paused = !paused;
                            } else if (is_hotkey(event.key, Config::Hotkey::QuickSave) && !event.key.repeat
                                       && machine_any != nullptr) {
                                // Quick-save / quick-load the reserved slot. Only
                                // with a game running; serviced between frames.
                                state_action = StateAction{StateOp::Save, hw::kQuickSlot};
                            } else if (is_hotkey(event.key, Config::Hotkey::QuickLoad) && !event.key.repeat
                                       && machine_any != nullptr) {
                                state_action = StateAction{StateOp::Load, hw::kQuickSlot};
                            } else if (is_hotkey(event.key, Config::Hotkey::Fps) && !event.key.repeat) {
                                options.config.show_fps = !options.config.show_fps;
                            } else if (is_hotkey(event.key, Config::Hotkey::FastForward) && !event.key.repeat) {
                                fast_forward = true;
                            } else if (is_hotkey(event.key, Config::Hotkey::Rewind)) {
                                rewind_held = true;
                            }
                        }
                        break;
                    case SDL_EVENT_DROP_FILE:
                        if (event.drop.data != nullptr) {
                            dropped_path = event.drop.data;
                        }
                        break;
                    case SDL_EVENT_KEY_UP:
                        if (is_hotkey(event.key, Config::Hotkey::FastForward)) {
                            fast_forward = false;
                        } else if (is_hotkey(event.key, Config::Hotkey::Rewind)) {
                            rewind_held = false;
                        } else if (is_hotkey(event.key, Config::Hotkey::Quit) && quit_key_pressed) {
                            running = false;
                        }
                        break;
                    default:
                        // Hotplug and axis motion events are passed on even
                        // with the overlay open, since its calibration needs them.
                        input.handle_event(event);
                        break;
                }
            }
            if (!running) {
                break;
            }

            // The pad's Guide shortcuts, read before poll() so their buttons stay
            // out of the game.
            const osd::Input::PadShortcuts pad_shortcuts =
                input.update_pad_shortcuts(options.config.pad_shortcuts);
            if (pad_shortcuts.exit) {
                // As Esc: back to the games when there are some, else quit.
                if (will_show_picker && machine_any != nullptr) {
                    return_to_picker_requested = true;
                } else {
                    running = false;
                }
            }
            if (pad_shortcuts.screenshot) {
                screenshot_requested = true;
            }
            if (!gui.visible() && machine_any != nullptr) {
                if (pad_shortcuts.save) {
                    state_action = StateAction{StateOp::Save, hw::kQuickSlot};
                } else if (pad_shortcuts.load) {
                    state_action = StateAction{StateOp::Load, hw::kQuickSlot};
                }
            }

            // Fast-forward is held rather than toggled.
            pacer.set_throttled(options.config.throttle && !fast_forward
                                && !pad_shortcuts.fast_forward);

            // Nudge the frame rate toward the audio device's true rate so the queue
            // does not drift; the deadband stops it fighting the per-frame sawtooth.
            if (sound_board != nullptr && audio.active()) {
                const s32 depth = static_cast<s32>(audio.queued_milliseconds());
                const s32 target = static_cast<s32>(osd::Audio::kTargetQueuedMilliseconds);
                const s32 error = depth - target;
                constexpr s32 kDeadbandMs = 15;
                double adjust = 0.0;
                if (error > kDeadbandMs || error < -kDeadbandMs) {
                    adjust = static_cast<double>(error) / 2000.0;
                }
                pacer.set_sync_adjust(adjust);
            }

            // Do not sample before --profile-after's frame.
            const bool profile_sample =
                options.profile
                && (options.profile_after == 0 || frames_presented >= options.profile_after);

            // The wheel's Menu-bound button toggles the overlay. Polled every frame
            // so it can also close the overlay once it has paused the game.
            if (input.menu_button_pressed() || pad_shortcuts.menu) {
                gui.toggle();
            }

            // Opening the overlay pauses the game; the manual pause holds
            // independently. Audio and the pacer follow on each transition.
            const bool effective_pause = paused || gui.visible();
            if (effective_pause != audio_paused_state) {
                audio_paused_state = effective_pause;
                audio.set_paused(effective_pause);
                pacer.resync();
                if (effective_pause) {
                    input.release_force_feedback();
                }
            }

            outputs.configure(options.config.outputs_network,
                              static_cast<u16>(options.config.outputs_network_port),
                              options.config.outputs_windows);
            if ((machine_iface != nullptr || machine_m3 != nullptr) && loaded.has_value()) {
                outputs.set_game(loaded->game.name, loaded->game.parent, loaded->game.outputs,
                                 loaded->game.model3.drive_board == rom::Model3DriveBoard::Billboard);
            } else {
                outputs.set_game({}, {});
            }
            outputs.set_paused(effective_pause);

            {
                const std::string wanted = (machine_iface != nullptr || machine_m3 != nullptr)
                                                   && loaded.has_value()
                                                   && options.config.custom_textures
                                               ? loaded->game.name
                                               : std::string();
                const bool reload = gui.take_texture_reload_request();
                if (wanted != replacements_game || (reload && !wanted.empty())) {
                    replacements_game = wanted;
                    if (wanted.empty()) {
                        backend->set_texture_replacements(nullptr);
                        replacements.reset();
                    } else {
                        if (!replacements) {
                            replacements = std::make_unique<render::TextureReplacements>();
                        }
                        backend->finish_model3();  // the build reads the pack
                        (void)replacements->load(texture_load_directory(options.config, wanted));
                        backend->set_texture_replacements(replacements.get());
                    }
                    gui.set_custom_texture_count(replacements ? replacements->size() : 0);
                }
            }

            {
                const bool wants_dump = loaded.has_value()
                                     && (options.dump_textures || options.config.dump_textures);
                const bool dumping   = wants_dump && machine_iface != nullptr;
                const bool dumping_3 = wants_dump && machine_m3 != nullptr;
                const bool same_game =
                    loaded.has_value() && texture_dump_game == loaded->game.name;
                if (texture_dumper && (!dumping || !same_game)) {
                    async_soft.wait();
                    texture_dumper->finish(same_game ? machine_iface : nullptr);
                    texture_dumper.reset();
                }
                if (model3_dumper && (!dumping_3 || !same_game)) {
                    backend->set_model3_texture_dumper(nullptr);
                    model3_dumper.reset();
                }
                if ((dumping && !texture_dumper) || (dumping_3 && !model3_dumper)) {
                    texture_dump_game = loaded->game.name;
                    const std::string directory =
                        texture_dump_directory(options.config, texture_dump_game);
                    if (dumping) {
                        texture_dumper.emplace(directory, texture_dump_game);
                    } else {
                        model3_dumper = std::make_unique<render::model3::TextureDumper>(
                            directory, texture_dump_game);
                        backend->set_model3_texture_dumper(model3_dumper.get());
                    }
                }
            }

            rewind.configure(options.config.rewind
                                 ? static_cast<usize>(options.config.rewind_buffer_mb) << 20
                                 : 0,
                             kRewindInterval);
            if (machine_any != rewind_machine) {
                rewind.clear();
                rewind_machine = machine_any;
            }
            if (gui.visible()) {
                rewind_held = false;
            }

            // Model 3 async: this pass draws the frame emulated by the pass
            // before, built on a worker while this one emulates.
            const bool m3_async = machine_m3 != nullptr && options.config.model3_async;
            const u32  m3_lag   = m3_async ? 1u : 0u;
            backend->set_model3_async(m3_async);
            // --run-frames' extra pass to show the last frame emulated.
            const bool draining = options.run_frames != 0 && frames_presented >= options.run_frames;

            bool rewinding = false;
            if (machine_any && !effective_pause && !draining) {
                // Step back a snapshot, then run a frame from it so there is a
                // picture; its sound is dropped.
                rewinding = (rewind_held || pad_shortcuts.rewind)
                         && rewind.step_back(*machine_any);

                // Inputs are levels the program polls during the frame, so they
                // are set before it runs.
                input.poll(&machine_any->inputs(), loaded->game);
                // Push the live settings (the GUI changes these) before computing
                // this frame's force, so slider and calibration take effect now.
                input.set_wheel_settings(wheel_settings_from(options.config));
                input.set_recoil(options.config.lightgun_recoil,
                                 options.config.lightgun_recoil_strength);
                input.set_gun_buttons(options.config.gun_buttons);
                input.set_pad_aim_centred(options.config.lightgun_pad_centred);
                input.set_pad_layout(osd::Input::game_layout(options.config, game_family(loaded->game),
                                                             static_cast<u32>(loaded->game.inputs)));
                input.set_pad_stick_sensitivity(options.config.pad_stick_sensitivity);
                input.set_pad_stick_deadzone(options.config.pad_stick_deadzone);
                input.set_pad_rumble(options.config.pad_rumble,
                                     options.config.pad_rumble_strength,
                                     options.config.pad_rumble_cornering);
                input.set_present_placement(shown_aspect(), options.config.scaling_method,
                                            gui.picture_area());
                input.set_sinden_border(options.config.sinden_border);
                const auto drive_writes = machine_any->take_drive_board_writes();
                input.update_drive_board(loaded->game, drive_writes.view());
                outputs.update(machine_any->lamp_latch(), drive_writes.view(),
                               machine_iface ? osd::Outputs::RamView{machine_iface->work_ram()}
                               : machine_m3  ? osd::Outputs::RamView{machine_m3->ram(), 0, true}
                                             : osd::Outputs::RamView{},
                               {machine_any->inputs().in0, machine_any->inputs().in1, machine_any->inputs().in2});
                {
                    const hw::Machine::BoardOutputs& board = machine_any->board_outputs();
                    if (board.billboard) outputs.update_billboard(board.bill);
                    input.set_board_force({board.force_feedback, board.constant, board.self_center,
                                           board.friction, board.vibrate});
                }
                input.update_force_feedback(loaded->game);
                input.update_pad_rumble(loaded->game);
                if (options.coin_at != 0) {
                    // Scripted coin, start and character confirmation, so an
                    // unattended capture reaches the game itself.
                    const osd::Input::ScriptedPress press = osd::Input::scripted_press(
                        frames_presented, options.coin_at, loaded->game.start1_bit);
                    machine_any->inputs().in0 &= static_cast<u8>(~press.in0);
                    machine_any->inputs().in1 &= static_cast<u8>(~press.in1);
                }

                if (machine_iface) {
                machine_iface->set_draw_distance(options.config.extended_draw_distance);
                machine_iface->set_music_volume(options.config.music_volume);
                machine_iface->set_black_level(black_level_for(options.config));
                }
                if (machine_m3 && machine_m3->sound() != nullptr) {
                    machine_m3->sound()->set_music_volume(options.config.music_volume);
                }
                if (machine_m3) {
                    const u32 columns =
                        shown_aspect() == AspectMode::Widescreen ? render::kWideColumns : 0;
                    const rom::SideFill fill = loaded->game.side_fill;
                    const bool widen = columns != 0 && fill != rom::SideFill::Pillarbox;
                    input.set_widescreen(columns);
                    backend->set_widescreen(columns, !widen                       ? 0u
                                                     : fill == rom::SideFill::Mirror ? 1u
                                                                                     : 2u);
                }
                if (machine_iface) {
                    // The geometry engine and the backend must agree on the
                    // width for the frame run_frame() builds.
                    const u32 columns =
                        shown_aspect() == AspectMode::Widescreen ? render::kWideColumns : 0;
                    // A frame whose 3D stays inside the 4:3 screen is a 2D one
                    // (a title, a menu with a model on it): plain side bars.
                    const float right       = static_cast<float>(columns + render::kNativeWidth);
                    const auto  reaches_side = [&](const hw::RenderPolygon& poly) {
                        for (u32 v = 0; v < poly.num_vertices; ++v) {
                            if (poly.v[v].x < static_cast<float>(columns) || poly.v[v].x > right) {
                                return true;
                            }
                        }
                        return false;
                    };
                    const auto& polygons = machine_iface->render_list().polygons;
                    machine_iface->set_widescreen(columns);
                    input.set_widescreen(columns);
                    const rom::SideFill side_fill = loaded->game.side_fill;
                    const bool fill_sides  = columns != 0
                                          && std::any_of(polygons.begin(), polygons.end(), reaches_side);
                    const bool wide_below = fill_sides && side_fill != rom::SideFill::Pillarbox;
                    // A race's HUD spreads to the edges; a menu stays whole.
                    const u32 spread = wide_below && machine_iface->hud_spread()
                                           ? u32{loaded->game.hud_layers} << 28
                                           : 0u;
                    backend->set_hud_bands(loaded->game.hud_bands);
                    backend->set_widescreen(columns, wide_below ? (1u + static_cast<u32>(side_fill)) | spread
                                                                : 0u);
                }
                if (machine_m3) {
                    backend->release_model3_frames(machine_m3->video_frame());
                }
                {
                    auto scope = core::maybe_scope(stage_run_frame, profile_sample);
                    machine_any->run_frame();
                }
                if (!rewinding) {
                    rewind.capture(*machine_any);
                }
                if (machine_iface && loaded->game.lightgun.present) {
                    machine_iface->video().filter_gun_flash(options.config.lightgun_hide_flash,
                                                            machine_iface->inputs().in1);
                }
                if (texture_dumper && machine_iface) {
                    // scan() can compose, which must not race the render thread.
                    async_soft.wait();
                    texture_dumper->scan(*machine_iface);
                }
                if (profile_sample) {
                    // The geometry engine runs inside run_frame(), so its time is
                    // read back rather than scoped here.
                    stage_geometry.record_ms(
                        static_cast<double>(machine_iface ? machine_iface->geometry_stage_nanoseconds() : 0)
                        / 1'000'000.0);
                    // The per-core split, likewise filled inside run_frame().
                    stage_cpu_i960.record_ms(
                        static_cast<double>(machine_any->i960_stage_nanoseconds())
                        / 1'000'000.0);
                    stage_cpu_copro.record_ms(
                        static_cast<double>(machine_any->copro_stage_nanoseconds())
                        / 1'000'000.0);
                    stage_cpu_sound.record_ms(
                        static_cast<double>(machine_any->sound_stage_nanoseconds())
                        / 1'000'000.0);
                }

                // About 767 stereo frames per video frame, handed over every frame
                // so the only buffering is SDL's.
                if (sound_board != nullptr && rewinding) {
                    sound_board->clear_pending_samples();
                } else if (sound_board != nullptr) {
                    const std::span<const s16> produced = sound_board->pending_samples();
                    const u32 volume = options.config.volume_for(game_family(loaded->game));
                    if (volume == Config::kDefaultGameVolume) {
                        audio.submit(produced);
                    } else {
                        apply_volume(produced, volume, &scaled_audio);
                        audio.submit(scaled_audio);
                    }
                    if (!options.dump_audio.empty()) {
                        recorded_audio.insert(recorded_audio.end(), produced.begin(),
                                              produced.end());
                    }
                    sound_board->clear_pending_samples();
                }

                if (const hw::CpuStatus cpu = machine_any->main_cpu_status(); cpu.faulted) {
                    SM2_ERROR("stopping: %s", cpu.fault_message.c_str());
                    exit_code = 1;
                    break;
                }
            }

            // The scope closes before the continue below so a skipped frame's
            // SDL_Delay is not folded into present_wait.
            bool begin_ok;
            {
                auto scope = core::maybe_scope(stage_present_wait, profile_sample);
                begin_ok = backend->begin_frame();
            }
            if (!begin_ok && backend->device_lost()) {
                SM2_ERROR("the GPU driver reset the device; quitting. If it happens again, try "
                          "--graphics-backend opengl");
                exit_code = 1;
                break;
            }
            if (!begin_ok) {
                // Minimised or the swapchain was rebuilt: yield, and abandon the
                // pacing deadline.
                SDL_Delay(16);
                pacer.resync();
                // Still honour a --duration/--profile deadline, or a window that
                // stays non-drawable never prints its summary. No frame-time
                // sample is recorded for a skipped frame.
                if (run_deadline_ns != 0 && SDL_GetTicksNS() >= run_deadline_ns) {
                    running = false;
                }
                continue;
            }

            const hw::Model2Video& video =
                machine_iface ? machine_iface->video() : idle_video;

            // SoftRenderer needs a Model 1/2 machine; the idle display uses the GPU.
            const bool draw_with_software = use_software_renderer && machine_iface;

            // Render test mode's framebuffer overlay overwrites `below` after the
            // tilemap composite and has no GPU equivalent, so it takes the CPU path.
            const bool render_test = machine_iface && machine_iface->render_test_mode();

            // Whether the GPU composes the tilemaps this frame. Independent of
            // need_cpu_compose: a --soft-render run wants both.
            const bool cpu_tilemap =
                machine_iface
                && (machine_iface->needs_cpu_tilemap() || !backend->capabilities().compute_shaders);
            const bool use_gpu_tilemap =
                machine_iface && !use_software_renderer && !render_test && !cpu_tilemap;

            // Whether the CPU tilemap composite must run: whenever something reads
            // video.below()/above() directly.
            const bool need_cpu_compose =
                machine_iface
                && (use_software_renderer || options.soft_render || render_test || cpu_tilemap);

            // The async software path composes on its render thread.
            const bool compose_on_render_thread =
                draw_with_software && async_software && !render_test && !options.soft_render;

            if (machine_iface) {
                // The render thread may still be reading the decode and pens.
                {
                    auto scope = core::maybe_scope(stage_software_wait, profile_sample);
                    async_soft.wait();
                }
                soft_renderer.set_black_level(black_level_for(options.config));
                if (compose_on_render_thread) {
                    auto scope = core::maybe_scope(stage_compose, profile_sample);
                    if (machine_iface->palette_dirty()) {
                        machine_iface->video().refresh_pens();
                        machine_iface->clear_palette_dirty();
                    }
                    machine_iface->video().prepare_compose();
                } else if (need_cpu_compose) {
                    auto scope = core::maybe_scope(stage_compose, profile_sample);
                    machine_iface->compose_video();
                } else if (use_gpu_tilemap) {
                    // compose_video()'s palette refresh, without its compose().
                    if (machine_iface->palette_dirty()) {
                        machine_iface->video().refresh_pens();
                        machine_iface->clear_palette_dirty();
                    }
                }
            }

            // Line-of-sight results go back to the machine before the frame is
            // built, so they lag by the same number of frames with or without async.
            const auto submit_m3 = [&] {
                const render::model3::LineOfSight los = backend->model3_line_of_sight();
                for (unsigned i = 0; los.frame_number != 0 && i < los.range.size(); ++i) {
                    machine_m3->set_line_of_sight(i, los.range[i]);
                }
                auto scope = core::maybe_scope(stage_build, profile_sample);
                backend->submit_model3_frame(machine_m3->video_frame());
            };
            const auto sample_m3_build = [&] {
                if (!profile_sample) {
                    return;
                }
                const render::model3::BuildTimes t = backend->model3_build_times();
                stage_m3_build.record_ms(t.total);
                stage_m3_textures.record_ms(t.textures);
                stage_m3_tilegen.record_ms(t.tilegen);
                stage_m3_scene.record_ms(t.scene);
                stage_m3_plan.record_ms(t.plan);
                if (m3_async) {
                    stage_m3_record.record_ms(t.record);
                }
            };
            if (machine_m3) {
                if (!m3_async) {
                    submit_m3();
                    sample_m3_build();
                }
            } else if (!draw_with_software) {
                // Uploads and the 3D pass first, outside any rendering scope: a
                // transfer cannot be issued inside one, and the 3D pass opens its
                // own on its offscreen target.
                {
                    auto scope = core::maybe_scope(stage_tilemap_upload, profile_sample);
                    if (use_gpu_tilemap) {
                        backend->compute_tilemap(*machine_iface, video);
                    } else {
                        backend->upload_tilemap(video.below(), video.above());
                    }
                }
                {
                    auto scope = core::maybe_scope(stage_build, profile_sample);
                    backend->submit_polygons(machine_iface, video);
                }
            }

            // Command recording starts here and ends after the present below. Not
            // timed when drawing in software, which records no GPU drawing.
            if (m3_async) {
                {
                    auto scope = core::maybe_scope(stage_m3_wait, profile_sample);
                    backend->finish_model3();
                }
                sample_m3_build();
            }

            std::optional<core::StageTimer::Scope> record_scope;
            if (profile_sample && !draw_with_software) {
                record_scope.emplace(stage_record);
            }

            if (machine_m3) {
                backend->render_model3();
            } else if (!draw_with_software) {
                backend->render_polygons();
            }

            // The hardware's three-way composite runs at native 496x384: tilemap
            // priority category zero, the 3D output, then category one. Nothing is
            // magnified until the present, so the blends use unfiltered pixels.
            if (profile_sample) {
                const render::GpuStageTimes gpu_times = backend->read_stage_times();
                if (!backend->supports_gpu_timing() && !gpu_timing_unavailable_warned) {
                    SM2_WARN("this device does not report GPU timestamps; --profile's "
                             "GPU-side figures will be empty, not zero");
                    gpu_timing_unavailable_warned = true;
                }
                for (usize stage = 0; stage < gpu_times.size(); ++stage) {
                    if (gpu_times[stage].ran) {
                        gpu_stage_samples[stage].push_back(gpu_times[stage].milliseconds);
                    }
                }
            }

            if (draw_with_software) {
                // The CPU rasteriser performs the whole three-way composite itself
                // and lands in the same native target the GPU path presents from.
                if (async_software && !render_test) {
                    // Collect and present the previous frame, then hand this one
                    // to the render thread.
                    {
                        auto scope = core::maybe_scope(stage_software_wait, profile_sample);
                        async_soft.wait();
                    }
                    if (profile_sample && async_soft.last_render_ms() > 0.0) {
                        stage_software_async.record_ms(async_soft.last_render_ms());
                    }
                    backend->submit_native_frame(soft_frame);
                    {
                        auto scope =
                            core::maybe_scope(stage_software_snapshot, profile_sample);
                        async_soft.begin(*machine_iface, soft_frame, compose_on_render_thread);
                    }
                } else {
                    {
                        auto scope = core::maybe_scope(stage_software, profile_sample);
                        soft_renderer.render(*machine_iface, machine_iface->render_list(),
                                             soft_frame);
                    }
                    backend->submit_native_frame(soft_frame);
                }
            } else if (!machine_m3) {
                // Render test mode shows the host's framebuffer bank instead of the
                // 3D pass; it is already composed into the layers.
                const bool skip_3d = machine_iface && machine_iface->render_test_mode();
                backend->composite_native_frame(video.background(), skip_3d);
            }

            // Read back the finished native frame before it is scaled.
            // Model 3 async shows the frame emulated by the pass before.
            const u32  shown_frame = frames_presented - std::min(frames_presented, m3_lag);
            const bool last_frame =
                options.run_frames != 0 && frames_presented + 1 >= options.run_frames + m3_lag;
            const bool numbered_series =
                options.screenshot_interval != 0 || !options.screenshot_frames.empty();
            const bool capture_diagnostic =
                !options.screenshot.empty() && frames_presented >= m3_lag
                && (!options.screenshot_frames.empty()
                        ? options.screenshot_frames.count(shown_frame) != 0
                        : options.screenshot_interval != 0
                              ? (shown_frame % options.screenshot_interval) == 0
                                    || last_frame
                              : last_frame || options.run_frames == 0);
            const std::string dump_frame =
                model3_dumper ? model3_dumper->take_frame_request() : std::string();
            const bool capture_this_frame =
                capture_diagnostic || screenshot_requested || !dump_frame.empty();
            if (capture_this_frame && !backend->request_capture()) {
                SM2_ERROR("frame capture failed");
                exit_code = 1;
                break;
            }
            if (capture_this_frame && options.soft_render && machine_iface) {
                // The same machine state the GPU just drew, drawn again on the CPU.
                // Numbered when a series was asked for, or a later capture would
                // overwrite the one before it.
                hw::dump_software_frame(*machine_iface,
                                        std::filesystem::path(options.screenshot)
                                            .parent_path()
                                            .string(),
                                        numbered_series
                                            ? static_cast<int>(frames_presented)
                                            : -1);
            }
            // After the dumper's request is taken: its scan runs in the build.
            if (m3_async) {
                submit_m3();
            }

            // Pushed every frame so a GUI change takes effect at once; both are
            // shader state, never a reallocation.
            {
                render::PresentOptions present;
                present.scaling_method        = options.config.scaling_method;
                present.aspect_mode           = shown_aspect();
                present.picture_area          = gui.picture_area();
                present.crt_enabled           = options.config.crt_enabled;
                present.crt_scanline_strength = options.config.crt_scanline_strength;
                present.crt_mask_strength     = options.config.crt_mask_strength;
                present.crt_glow_strength     = options.config.crt_glow_strength;
                present.crt_curvature         = options.config.crt_curvature;
                if (machine_m3) {
                    present.gain = 0.5f + static_cast<float>(std::min(options.config.brightness, 100u)) / 100.0f;
                }
                backend->set_present_options(present);

                render::EnhancementOptions enhancement;
                enhancement.texture_filter = options.config.texture_filter;
                enhancement.anisotropy     = options.config.anisotropy;
                enhancement.upscale_2d     = options.config.upscale_2d;
                enhancement.translucency   = options.config.translucency;
                backend->set_enhancement_options(enhancement);
            }

            // Then the one magnification, into the swapchain.
            backend->blit_to_swapchain();
            // Command recording ends here; the GUI overlay is left out.
            record_scope.reset();

            // Draw the ImGui overlay on top of the presented frame, while the
            // swapchain image is still in a colour-attachment layout.
            backend->begin_overlay_frame();
            {
                u32 fbw = 0;
                u32 fbh = 0;
                backend->overlay_framebuffer_size(&fbw, &fbh);
                gui.set_framebuffer_size(fbw, fbh);
            }
            gui.new_frame();
            // Live link state for the Network tab.
            if (machine_any != nullptr) {
                const hw::CommBoard& comm = machine_any->comm();
                gui.set_link_status(osd::Gui::LinkStatus{
                    options.config.link_enabled, comm.enabled(), comm.link_alive(),
                    comm.link_id(), comm.link_count()});
            }
            // GPU capabilities gate the enhancement options in the GUI.
            {
                const render::Capabilities caps = backend->capabilities();
                gui.set_enhancement_caps(caps.anisotropy, caps.max_anisotropy,
                                         caps.blended_translucency);
            }
            // Feed the States tab the game's slots (only while the overlay is up,
            // since listing them touches the filesystem).
            if (gui.visible() && machine_any != nullptr && loaded.has_value()) {
                std::vector<osd::Gui::StateSlot> gui_slots;
                for (const hw::SlotInfo& s :
                     hw::list_state_slots(options.config.states_dir, loaded->game.name)) {
                    osd::Gui::StateSlot g;
                    g.label = (s.slot == hw::kQuickSlot) ? i18n::tr("Quick")
                                                       : i18n::trf("Slot %s", s.slot.c_str());
                    g.slot      = s.slot;
                    g.occupied  = s.occupied;
                    g.timestamp = s.timestamp;
                    gui_slots.push_back(std::move(g));
                }
                gui.set_state_slots(true, std::move(gui_slots));
            } else {
                gui.set_state_slots(machine_any != nullptr, {});
            }
            gui.set_current_game(loaded.has_value() ? loaded->game.name : std::string());
            gui.set_model3_running(loaded.has_value() && loaded->game.board == rom::Board::Model3);
            gui.set_current_family(loaded.has_value() ? game_family(loaded->game)
                                                      : std::string());
            gui.set_shown_aspect(shown_aspect());
            const bool gui_active =
                gui.draw(options.config, gpu_names, pacer.measured_hz(),
                        use_software_renderer ? "Software" : gpu_backend_name, &input);
            // An overlay Save/Load/Delete button is serviced like the hotkeys.
            if (std::optional<osd::Gui::StateRequest> req = gui.take_pending_state_request()) {
                using A  = osd::Gui::StateRequest::Action;
                StateOp op = req->action == A::Save   ? StateOp::Save
                             : req->action == A::Load  ? StateOp::Load
                                                       : StateOp::Delete;
                state_action = StateAction{op, req->slot};
            }
            // Apply a fullscreen toggle from the Settings menu at once.
            if (options.config.fullscreen != window.fullscreen()) {
                window.set_fullscreen(options.config.fullscreen);
            }
            // Settings just opened: note the folders; just closed with no game
            // loaded and a folder changed: rescan them.
            if (!settings_was_visible && gui.visible()) {
                list_inputs_at_open = list_inputs();
            }
            if (settings_was_visible && !gui.visible() && machine_any == nullptr
                && list_inputs() != list_inputs_at_open) {
                show_picker();
            }
            settings_was_visible = gui.visible();
            // Always finalise the ImGui frame (Render must follow NewFrame).
            gui.end_frame();
            backend->draw_overlay(gui_active);

            bool end_frame_ok = false;
            {
                auto scope = core::maybe_scope(stage_submit, profile_sample);
                end_frame_ok = backend->end_frame();
            }
            if (!end_frame_ok) {
                SM2_ERROR("frame submission failed");
                exit_code = 1;
                break;
            }
            outputs.poll();

            // Unload the game and return to the picker, after the frame is
            // submitted: save NVRAM, wait for the GPU, drop the machine.
            if (return_to_picker_requested) {
                return_to_picker_requested = false;
                async_soft.forget_machine();
                if (machine_any != nullptr) {
                    machine_any->save_nvram();
                }
                backend->wait_idle();

                loaded.reset();
                machine_iface = nullptr;
                machine       = nullptr;
                machine_2b    = nullptr;
                machine_2c    = nullptr;
                machine_orig  = nullptr;
                machine_m1    = nullptr;
                machine_m3    = nullptr;
                machine_any   = nullptr;
                main_cpu      = nullptr;
                sound_board   = nullptr;
                sound_link    = nullptr;

                audio.set_paused(true);
                audio_paused_state = true;
                input.release_force_feedback();
                show_picker();
                window.set_title(build_title());
                pacer.resync();
                continue;
            }

            // The picker chose a game. Handled after the frame is submitted so
            // there is no half-drawn frame; join the scraper and free its
            // textures before the machine takes over.
            if (std::optional<std::string> pick = gui.take_pending_launch()) {
                scraper.stop();
                gui.release_picker_textures();
                backend->wait_idle();

                std::optional<LoadedMachine> chosen;
                rom::LoadError               load_error;
                if (const std::optional<std::filesystem::path> archive =
                        find_set_archive(database, options.config, *pick)) {
                    chosen = load_game(database, archive->string(), *pick,
                                       options.config.nvram_dir, options.log_unmapped,
                                       &load_error);
                } else {
                    load_error.message = "Its archive is no longer in the ROM folder.";
                }
                if (!chosen.has_value()) {
                    SM2_ERROR("picker: could not load '%s' from its ROM folder", pick->c_str());
                    show_picker();
                    const rom::GameSpec* spec = database.find(*pick);
                    gui.show_load_error(spec != nullptr ? spec->title : *pick,
                                        std::move(load_error.message),
                                        std::move(load_error.missing));
                } else {
                    adopt_game(std::move(*chosen));
                    SM2_INFO("picker: launched '%s'", loaded->game.name.c_str());
                }
                pacer.resync();
                continue;
            }

            // A ROM archive dropped on the window. The running game keeps going
            // if the new one does not load.
            if (!dropped_path.empty()) {
                const std::filesystem::path path(dropped_path);
                dropped_path.clear();
                std::string extension = path.extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension != ".zip" && extension != ".7z") {
                    gui.show_load_error(path.filename().string(),
                                        i18n::tr("Drop a .zip or .7z ROM archive to play it."), {});
                    continue;
                }
                if (machine_any != nullptr) {
                    machine_any->save_nvram();
                }
                rom::LoadError               load_error;
                std::optional<LoadedMachine> dropped =
                    load_game(database, path.string(), {}, options.config.nvram_dir,
                              options.log_unmapped, &load_error);
                if (!dropped.has_value()) {
                    const rom::GameSpec* spec =
                        load_error.game.empty() ? nullptr : database.find(load_error.game);
                    gui.show_load_error(spec != nullptr ? spec->title : path.filename().string(),
                                        std::move(load_error.message),
                                        std::move(load_error.missing));
                    continue;
                }
                input.release_force_feedback();
                adopt_game(std::move(*dropped));
                SM2_INFO("launched '%s' from a dropped file", loaded->game.name.c_str());
                continue;
            }

            // A series is written as it goes, and the readback is complete only
            // once the submission is, so this stalls the pipeline.
            if (capture_diagnostic && numbered_series) {
                backend->wait_idle();
                if (!backend->save_capture(numbered_path(options.screenshot, shown_frame))) {
                    exit_code = 1;
                    break;
                }
            }

            if (!dump_frame.empty()) {
                backend->wait_idle();
                (void)backend->save_capture(dump_frame);
            }

            // Write a timestamped PNG into the screenshots directory.
            if (screenshot_requested) {
                screenshot_requested = false;
                backend->wait_idle();
                const std::string shot = screenshot_path(options.config.screenshot_dir,
                                                          loaded.has_value()
                                                              ? loaded->game.name
                                                              : "sm2");
                if (backend->save_capture(shot)) {
                    SM2_INFO("screenshot written to %s", shot.c_str());
                } else {
                    SM2_WARN("could not write screenshot to %s", shot.c_str());
                }
            }

            // Save or load a state between frames, so the machine is never
            // mid-run_frame.
            if (state_action.has_value() && machine_any != nullptr && loaded.has_value()) {
                const std::string path = hw::state_slot_path(
                    options.config.states_dir, loaded->game.name, state_action->slot);
                // The file's base name, e.g. "topskatr.quick.kstate".
                const std::string file =
                    std::filesystem::path(path).filename().string();
                if (state_action->op == StateOp::Save) {
                    if (machine_any->save_state(path)) {
                        SM2_INFO("saved state to slot '%s'", state_action->slot.c_str());
                        gui.notify(i18n::trf("State saved: %s", file.c_str()));
                    } else {
                        SM2_WARN("could not save state to slot '%s'",
                                 state_action->slot.c_str());
                        gui.notify(i18n::trf("Save failed: %s", file.c_str()));
                    }
                } else if (state_action->op == StateOp::Load) {
                    if (machine_any->load_state(path)) {
                        SM2_INFO("loaded state from slot '%s'", state_action->slot.c_str());
                        pacer.resync();  // the frame clock jumped; do not chase it
                        gui.notify(i18n::trf("State loaded: %s", file.c_str()));
                    } else {
                        SM2_WARN("could not load state from slot '%s' (empty or "
                                 "incompatible)", state_action->slot.c_str());
                        gui.notify(i18n::trf("Load failed: %s (empty or incompatible)", file.c_str()));
                    }
                } else {  // StateOp::Delete
                    if (hw::delete_state_slot(options.config.states_dir,
                                              loaded->game.name, state_action->slot)) {
                        SM2_INFO("deleted state slot '%s'", state_action->slot.c_str());
                        gui.notify(i18n::trf("State deleted: %s", file.c_str()));
                    } else {
                        gui.notify(i18n::trf("Delete failed: %s", file.c_str()));
                    }
                }
            }
            state_action.reset();

            ++frames_presented;

            if (options.run_frames != 0 && frames_presented >= options.run_frames + m3_lag) {
                running = false;
            }

            // Measured from this iteration's own start, so a skipped iteration
            // cannot fold its idle time into the next sample. Recorded before
            // pacer.wait(), which would otherwise measure the pacer's idle time.
            if (run_deadline_ns != 0) {
                const u64 now_ns = SDL_GetTicksNS();
                // Same --profile-after gate as the per-stage samples.
                if (options.profile_after == 0 || frames_presented > options.profile_after) {
                    frame_times_ms.push_back(
                        static_cast<double>(now_ns - frame_start_ns) / 1'000'000.0);
                }
                if (now_ns >= run_deadline_ns) {
                    running = false;
                }
            }

            // Last thing in the loop, so the wait absorbs the whole frame's cost.
            frames_written_off += pacer.wait();

            const u64 now = SDL_GetTicksNS();
            if (now - last_title_ns >= 1'000'000'000ULL) {
                last_title_ns = now;
                SM2_DEBUG("%.1f of %.2f Hz, %u polygon(s) in %u triangle(s), %u blank,"
                          " %u frame(s) written off, %u ms of audio queued,"
                          " %u voice(s)",
                          pacer.measured_hz(), pacer.target_hz(),
                          backend->drawn_polygons(), backend->triangles(),
                          backend->blank_polygons(), frames_written_off,
                          audio.queued_milliseconds(),
                          sound_board != nullptr ? sound_board->active_voices() : 0u);

                window.set_title(build_title());
            }
        }

        if (texture_dumper) {
            async_soft.wait();
            texture_dumper->finish(machine_iface);
            texture_dumper.reset();
        }
        backend->set_model3_texture_dumper(nullptr);
        model3_dumper.reset();
        backend->set_texture_replacements(nullptr);

        if (frames_written_off != 0) {
            SM2_INFO("%u frame(s) were written off after falling behind",
                     frames_written_off);
        }

        if (!frame_times_ms.empty()) {
            const double total_ms =
                static_cast<double>(SDL_GetTicksNS() - run_start_ns) / 1'000'000.0;
            double sum_ms = 0.0;
            for (const double ms : frame_times_ms) {
                sum_ms += ms;
            }
            const double mean_ms = sum_ms / static_cast<double>(frame_times_ms.size());
            const double p50 = percentile(frame_times_ms, 0.50);
            const double p95 = percentile(frame_times_ms, 0.95);
            const double p99 = percentile(frame_times_ms, 0.99);

            std::printf("\n=== %s renderer, %s, %.1fs ===\n",
                        use_software_renderer ? "software" : gpu_backend_name,
                        loaded.has_value() ? loaded->game.name.c_str() : "no game",
                        total_ms / 1000.0);
            std::printf("frames presented  : %u\n", frames_presented);
            std::printf("average fps        : %.2f (%.3f ms/frame mean)\n",
                        1000.0 / mean_ms, mean_ms);
            if (async_software) {
                std::printf("texture snapshots  : %u of %u frames copied texture RAM\n",
                            async_soft.texture_copies(), async_soft.frames_begun());
            }
            std::printf("p50 / p95 / p99 ms : %.3f / %.3f / %.3f (%.2f / %.2f / %.2f fps)\n",
                        p50, p95, p99, 1000.0 / p50, 1000.0 / p95, 1000.0 / p99);
            std::printf("throttled          : %s\n", options.config.throttle ? "yes" : "no");
            SM2_INFO("%s: %u frame(s) over %.1fs, average %.2f fps (p50 %.3f / p95 %.3f / "
                     "p99 %.3f ms), throttle %s",
                     use_software_renderer ? "software" : gpu_backend_name, frames_presented,
                     total_ms / 1000.0, 1000.0 / mean_ms, p50, p95, p99,
                     options.config.throttle ? "on" : "off");

            if (options.profile) {
                // Labelled with the run's conditions.
                std::printf("\n--- per-stage CPU (ms), %s, --duration %u, --coin-at %u, "
                            "--nvram %s ---\n",
                            loaded.has_value() ? loaded->game.name.c_str() : "no game",
                            options.duration_seconds, options.coin_at,
                            options.config.nvram_dir.c_str());
                std::printf("%-24s %8s %10s %10s %10s %10s\n", "stage", "samples", "mean",
                            "p50", "p95", "p99");

                // Written alongside the printed table.
                std::FILE* csv = nullptr;
                if (!options.profile_csv.empty()) {
                    csv = std::fopen(options.profile_csv.c_str(), "w");
                    if (csv == nullptr) {
                        SM2_ERROR("could not open --profile-csv '%s'",
                                 options.profile_csv.c_str());
                    } else {
                        std::fprintf(csv, "kind,stage,samples,mean_ms,p50_ms,p95_ms,p99_ms\n");
                    }
                }

                for (const core::StageTimer* timer :
                    {&stage_run_frame, &stage_cpu_i960, &stage_cpu_copro,
                     &stage_cpu_sound, &stage_geometry, &stage_compose,
                     &stage_tilemap_upload, &stage_build, &stage_record, &stage_submit,
                     &stage_present_wait, &stage_software, &stage_software_wait,
                     &stage_software_snapshot, &stage_software_async, &stage_m3_wait,
                     &stage_m3_build, &stage_m3_textures, &stage_m3_tilegen, &stage_m3_scene,
                     &stage_m3_plan, &stage_m3_record}) {
                    const core::StageStats stats = core::summarise(*timer);
                    if (stats.samples == 0) {
                        continue;
                    }
                    std::printf("%-24s %8zu %10.4f %10.4f %10.4f %10.4f\n",
                                stats.name.c_str(), stats.samples, stats.mean_ms, stats.p50_ms,
                                stats.p95_ms, stats.p99_ms);
                    if (csv != nullptr) {
                        std::fprintf(csv, "cpu,%s,%zu,%.4f,%.4f,%.4f,%.4f\n",
                                     stats.name.c_str(), stats.samples, stats.mean_ms,
                                     stats.p50_ms, stats.p95_ms, stats.p99_ms);
                    }
                }

                std::printf("\n--- per-stage GPU (ms) ---\n");
                if (!backend->supports_gpu_timing()) {
                    std::printf("(this device does not report GPU timestamps)\n");
                } else {
                    std::printf("%-24s %8s %10s %10s %10s %10s\n", "stage", "samples", "mean",
                                "p50", "p95", "p99");
                    static constexpr const char* kGpuStageNames[] = {
                        "gpu_texture_decode", "gpu_poly3d", "gpu_composite", "gpu_present",
                        "gpu_tilemap_compose"};
                    for (usize stage = 0; stage < gpu_stage_samples.size(); ++stage) {
                        const std::vector<double>& samples = gpu_stage_samples[stage];
                        if (samples.empty()) {
                            std::printf("%-24s %8s %10s %10s %10s %10s  (never ran)\n",
                                        kGpuStageNames[stage], "0", "-", "-", "-", "-");
                            if (csv != nullptr) {
                                std::fprintf(csv, "gpu,%s,0,,,,\n", kGpuStageNames[stage]);
                            }
                            continue;
                        }
                        double sum = 0.0;
                        for (const double ms : samples) {
                            sum += ms;
                        }
                        const double gpu_mean = sum / static_cast<double>(samples.size());
                        const double gpu_p50  = core::percentile_ms(samples, 0.50);
                        const double gpu_p95  = core::percentile_ms(samples, 0.95);
                        const double gpu_p99  = core::percentile_ms(samples, 0.99);
                        std::printf("%-24s %8zu %10.4f %10.4f %10.4f %10.4f\n",
                                    kGpuStageNames[stage], samples.size(), gpu_mean, gpu_p50,
                                    gpu_p95, gpu_p99);
                        if (csv != nullptr) {
                            std::fprintf(csv, "gpu,%s,%zu,%.4f,%.4f,%.4f,%.4f\n",
                                         kGpuStageNames[stage], samples.size(), gpu_mean,
                                         gpu_p50, gpu_p95, gpu_p99);
                        }
                    }
                }

                if (csv != nullptr) {
                    std::fclose(csv);
                    SM2_INFO("wrote --profile-csv to '%s'", options.profile_csv.c_str());
                }

                std::printf("\ndevice             : %s\n", backend->device_name());
            }
        }

        if (machine_any) {
            machine_any->save_nvram();
        }
        if (machine_iface) {
            machine_iface->log_unmapped_summary();
            machine_iface->log_burst_summary();
        }

        // Nothing may be destroyed while a submitted command buffer still
        // refers to it, and up to kFramesInFlight frames are outstanding here.
        backend->wait_idle();

        // After wait_idle, so the readback buffer holds a completed copy. A series
        // has already written each of its frames as it went.
        if (!options.screenshot.empty() && options.screenshot_interval == 0
            && options.screenshot_frames.empty() && exit_code == 0
            && !backend->save_capture(options.screenshot)) {
            exit_code = 1;
        }

        if (!options.dump_audio.empty() && sound_board != nullptr
            && !hw::write_wav(options.dump_audio, recorded_audio,
                              sound_board->sample_rate())) {
            exit_code = 1;
        }

        // Persist settings on exit, so overlay changes stick without the Save
        // button.
        if (!save_config(config_path, to_save(options.config))) {
            SM2_WARN("could not save settings to '%s'", config_path.c_str());
        }

        audio.shutdown();
        input.shutdown();
        // Before gui.shutdown(): the backend's ImGui renderer shutdown reads
        // ImGui's global context.
        backend->shutdown_overlay();
        gui.shutdown();
        backend->shutdown();
    }

    SDL_Quit();
    log::close_log_file();
    return exit_code;
}
