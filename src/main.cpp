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
//
// Loads a ROM set, runs the machine, and presents its output. Settings come from
// the configuration file first and the command line second, so a flag always wins
// over a file.

#include "core/config.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/types.h"
#include "hw/m1audio.h"
#include "hw/machine_factory.h"
#include "hw/model2.h"
#include "hw/model2_original.h"
#include "hw/sound_board.h"
#include "hw/model2b.h"
#include "hw/model2c.h"
#include "hw/model2_debug.h"
#include "hw/model2_softrender.h"
#include "osd/audio.h"
#include "osd/frame_pacer.h"
#include "osd/gui.h"
#include "osd/input.h"
#include "osd/window.h"
#include "render/backend.h"
#include "rom/game_db.h"
#include "rom/rom_loader.h"

#include <memory>

#include <SDL3/SDL.h>

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#if !defined(SM2_HAVE_VULKAN)
// enumerate_render_devices() is Vulkan-only (it lists VkPhysicalDevices);
// a Vulkan-less build has no device list, so provide an empty fallback.
namespace sm2::render {
std::vector<std::string> enumerate_render_devices() { return {}; }
}  // namespace sm2::render
#endif

namespace {

/// --graphics-backend's three values. Which of the two GL flavours `Opengl`
/// resolves to is a build-time fact (SM2_BUILD_OPENGL_DESKTOP vs
/// SM2_BUILD_OPENGL_ES), not a fourth value here -- design.md §2.
enum class GraphicsBackendChoice {
    Software,
    Vulkan,
    Opengl,
};

/// Parses --graphics-backend's argument. Returns false, naming the valid
/// choices, on anything else -- design.md §2's "reject rather than silently
/// default".
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

/// Build the input layer's wheel settings from the persisted config.
[[nodiscard]] sm2::osd::Input::WheelSettings wheel_settings_from(const sm2::Config& c)
{
    sm2::osd::Input::WheelSettings w;
    w.ffb             = c.wheel_ffb;
    w.strength        = c.wheel_ffb_strength;
    w.steer_degrees   = c.wheel_steer_degrees;
    w.lock_degrees    = c.wheel_lock_degrees;
    w.rumble          = c.wheel_rumble;
    w.rumble_strength = c.wheel_rumble_strength;
    w.buttons       = c.wheel_buttons;
    w.steer_axis    = c.wheel_steer_axis;
    w.accel_axis    = c.wheel_accel_axis;
    w.brake_axis    = c.wheel_brake_axis;
    w.accel_invert  = c.wheel_accel_invert;
    w.brake_invert  = c.wheel_brake_invert;
    return w;
}

/// Default when --graphics-backend is not given: whichever GPU backend was
/// compiled in, else software.
///
/// GLES is preferred over Vulkan where built: GLES is the ARM target (the only
/// config that builds it) and its native driver beat V3DV Vulkan on every set
/// on a Pi 5. A desktop build never enables GLES, so it keeps Vulkan/desktop-GL.
/// Only governs a bare invocation; the Batocera frontend passes the flag anyway.
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

    /// Which of those the command line actually set, so a flag can win over the
    /// file without every flag having to carry its own default.
    struct Given {
        bool vsync      = false;
        bool throttle   = false;
        bool fullscreen = false;
        bool lightgun   = false;
        bool validation = false;
        bool log_level  = false;
        bool gpu        = false;
        bool nvram_dir  = false;
        bool games_xml  = false;
    } given;

    bool        verbose    = false;
    bool        show_help  = false;
    bool        list_gpus  = false;
    bool        list_games = false;
    bool        list_gamepads = false;
    bool        write_config  = false;
    std::string config_path;
    std::string game;
    std::string dump_roms;
    std::string dump_tilemap;
    std::string screenshot;
    std::string dump_audio;
    std::string rom_path;

    /// Capture a frame every this many frames instead of only the last one. Each
    /// goes to the screenshot path with the frame number appended.
    sm2::u32 screenshot_interval = 0;

    /// Capture exactly these frames. A comparison against another emulator has to
    /// search a window of frames around each sample to find the one that lines up,
    /// and an interval cannot express "these three windows and nothing else"
    /// without writing hundreds of frames nobody looks at.
    std::set<sm2::u32> screenshot_frames;

    /// Insert coins and press start around this frame. Zero leaves the panel
    /// alone.
    sm2::u32 coin_at = 0;

    /// Write one line per frame recording what the geometry engine produced, so a
    /// whole run can be compared against MAME's own count rather than a single
    /// instant. Comparing curves rather than instants is what makes the comparison
    /// survive the two emulators sitting on different attract pages.
    std::string poly_log;

    /// Also render each captured frame on the CPU, through the port of MAME's own
    /// rasteriser, and write it beside the screenshot. Both come from the same
    /// machine state, so a difference between them is the renderer and nothing
    /// else.
    bool soft_render = false;

    /// Which renderer actually draws the window: Vulkan (the GPU path) or
    /// Software (the same CPU rasteriser --soft-render uses for comparison,
    /// drawn here instead of alongside). F2 toggles this live so the two can be
    /// compared without restarting.
    bool start_in_software_renderer = false;

    /// Which GPU backend is constructed underneath that toggle --
    /// independent of start_in_software_renderer, which only decides whether
    /// the CPU rasteriser is drawing *this frame*
    /// (.kiro/specs/model2-gl-backends/design.md §2). `software` is a pure
    /// alias for --software: it does not change which GPU backend is built
    /// (Vulkan still presents underneath), it only also sets
    /// start_in_software_renderer.
    GraphicsBackendChoice graphics_backend = kDefaultGraphicsBackend;

    /// Run this many frames headless, report, and exit. Zero means run normally.
    sm2::u32 boot_test    = 0;

    /// Quit after this many presented frames. Zero means run until asked to stop.
    sm2::u32 run_frames   = 0;

    /// Quit after this many wall-clock seconds, in the windowed path. Unlike
    /// --run-frames, this bounds real time rather than emulated frames, which is
    /// what a throughput comparison between renderers needs to hold fixed: with
    /// --no-throttle the frame *count* is exactly the thing being measured, so it
    /// cannot also be the stopping condition.
    sm2::u32 duration_seconds = 0;

    /// Report where a frame's CPU and GPU time actually goes -- design.md
    /// requirement 1's per-stage benchmark -- rather than only the whole-frame
    /// rate --duration alone reports. Implies --duration's stopping behaviour if
    /// duration_seconds is otherwise zero, since a stage breakdown needs the same
    /// fixed window a throughput figure does.
    bool profile = false;

    /// Discard every profiler and frame-time sample before this presented-frame
    /// number, so attract/title/select screens (which draw almost nothing) do
    /// not inflate the average. Set past the coin/start sequence (coin_at +
    /// ~350). Zero measures from the first frame.
    sm2::u32 profile_after = 0;

    /// Also write --profile's per-stage table to this CSV file, one row per
    /// stage, so a before-and-after comparison is scriptable rather than
    /// needing to parse the stdout table -- design.md requirement 1.4.
    std::string profile_csv;

    bool     log_unmapped = false;
};

void print_usage()
{
    std::printf(
        "sm2-emu " SM2_VERSION " — Sega Model 2 arcade emulator\n"
        "\n"
        "Usage: sm2-emu [options] [rom.zip]\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this message\n"
        "      --list-games    List the games in the ROM database\n"
        "      --list-gpus     List the Vulkan devices that could be used\n"
        "      --list-gamepads Show which gamepads were recognised and which\n"
        "                      player each would drive\n"
        "      --game <name>   Load this set specifically, for archives that\n"
        "                      hold several revisions\n"
        "      --games-xml <p> Use this ROM database instead of searching\n"
        "      --dump-roms <d> Write the assembled regions to this directory\n"
        "                      and exit, for comparison against a reference\n"
        "      --nvram <dir>   Directory for NVRAM and EEPROM images\n"
        "                      (default: nvram)\n"
        "      --boot-test <n> Run n frames without a window, report where the\n"
        "                      program got to, and exit\n"
        "      --run-frames <n>  Quit after presenting n frames\n"
        "      --duration <n>  Quit after n wall-clock seconds, and print the\n"
        "                      average and p50/p95/p99 frame rate over the run.\n"
        "                      Combine with --no-throttle for a throughput\n"
        "                      comparison between --software and Vulkan\n"
        "      --profile [n]   As --duration, but also break the frame down by\n"
        "                      stage: the geometry engine, tilemap composition,\n"
        "                      the 3D pass build, and each GPU pass via timestamp\n"
        "                      queries. n defaults to 30 seconds if omitted\n"
        "      --profile-csv <f>  With --profile, also write the per-stage table\n"
        "                      to this CSV file, one row per stage\n"
        "      --profile-after <n>  Discard profiler and fps samples before frame\n"
        "                      n, so attract/title/select screens do not skew the\n"
        "                      in-game average. Set past coin-at + ~350\n"
        "      --screenshot <f>  Write the last presented frame to this PPM file\n"
        "      --soft-render   Also render each captured frame on the CPU with a\n"
        "                      port of MAME's own rasteriser, beside the screenshot,\n"
        "                      so the renderer can be compared against it\n"
        "      --software      Start with the CPU rasteriser driving the window,\n"
        "                      instead of Vulkan. F2 switches at runtime either way\n"
        "      --graphics-backend <software|vulkan|opengl>\n"
        "                      Which GPU backend to build the window/present path\n"
        "                      on. 'opengl' means whichever GL flavour this binary\n"
        "                      was built with. 'software' is an alias for\n"
        "                      --software; --software still wins if both are given\n"
        "      --screenshot-interval <n>  Capture every n frames instead, numbering\n"
        "                      each file after the frame it came from\n"
        "      --screenshot-frames <list>  Capture exactly these frames, comma\n"
        "                      separated, numbering each file after its frame\n"
        "      --coin-at <n>   Insert two coins and press start around frame n, so\n"
        "                      an unattended run reaches the game itself\n"
        "      --dump-audio <f>  Write everything the sound board produced to this\n"
        "                      WAV file, so it can be listened to or compared\n"
        "      --poly-log <f>  With --boot-test, append one line per frame with\n"
        "                      what the geometry engine produced, for comparison\n"
        "                      against MAME's own polygon count\n"
        "      --dump-tilemap <d>  With --boot-test, write the decoded tilemap\n"
        "                      layers, character RAM and the composed frame to\n"
        "                      this directory\n"
        "      --log-unmapped  Log every access outside a mapped region\n"
        "      --gpu <name>    Use the device with this exact name\n"
        "      --validation    Enable Vulkan validation layers\n"
        "      --no-vsync      Present without waiting for vertical blank\n"
        "      --no-throttle   Run as fast as this computer manages instead of at\n"
        "                      the machine's own 57.52 Hz\n"
        "      --fullscreen    Start filling the screen\n"
        "      --lightgun      Light-gun mode: show the aiming crosshair and\n"
        "                      hide the mouse cursor (for the gun titles)\n"
        "      --config <dir>  Directory holding sm2-emu.ini, used for both\n"
        "                      reading and saving settings\n"
        "      --write-config  Write the settings this run would use, then exit,\n"
        "                      so there is a file to edit\n"
        "      --log-level <l> trace, debug, info, warning or error\n"
        "  -v, --verbose       Same as --log-level debug\n"
        "\n");
    sm2::osd::Input::print_bindings();
    std::printf("\nNo ROM data is distributed with this software.\n");
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
        } else if (std::strcmp(arg, "--software") == 0) {
            out->start_in_software_renderer = true;
        } else if (std::strcmp(arg, "--graphics-backend") == 0) {
            if (index + 1 >= argc) {
                SM2_ERROR("--graphics-backend requires a value (software, vulkan, opengl)");
                return false;
            }
            if (!parse_graphics_backend(argv[++index], &out->graphics_backend)) {
                return false;
            }
            if (out->graphics_backend == GraphicsBackendChoice::Software) {
                out->start_in_software_renderer = true;
            }
        } else if (std::strcmp(arg, "--no-vsync") == 0) {
            out->config.vsync = false;
            out->given.vsync  = true;
        } else if (std::strcmp(arg, "--no-throttle") == 0) {
            out->config.throttle = false;
            out->given.throttle  = true;
        } else if (std::strcmp(arg, "--fullscreen") == 0) {
            out->config.fullscreen = true;
            out->given.fullscreen  = true;
        } else if (std::strcmp(arg, "--lightgun") == 0) {
            out->config.lightgun = true;
            out->given.lightgun  = true;
        } else if (std::strcmp(arg, "--write-config") == 0) {
            out->write_config = true;
        } else if (std::strcmp(arg, "-v") == 0 || std::strcmp(arg, "--verbose") == 0) {
            out->verbose = true;
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
        } else if (takes_value("--nvram", &out->config.nvram_dir)) {
            out->given.nvram_dir = true;
        } else if (takes_value("--dump-audio", &out->dump_audio)) {
            // handled
        } else if (takes_value("--poly-log", &out->poly_log)) {
        } else if (takes_value("--dump-tilemap", &out->dump_tilemap)) {
            // handled
        } else if (takes_value("--gpu", &out->config.gpu)) {
            out->given.gpu = true;
        } else if (takes_value("--games-xml", &out->config.games_xml)) {
            out->given.games_xml = true;
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

}  // namespace

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

    // --graphics-backend opengl needs a GL backend compiled in (either
    // desktop GL 4.3 core or GLES 3.1 -- they are mutually exclusive build
    // options that both produce the same "opengl" runtime name).
#if !defined(SM2_HAVE_OPENGL_DESKTOP) && !defined(SM2_HAVE_OPENGL_ES)
    if (options.graphics_backend == GraphicsBackendChoice::Opengl) {
        SM2_ERROR("--graphics-backend opengl was requested, but this binary was "
                  "built without an OpenGL backend. Rebuild with "
                  "-DSM2_BUILD_OPENGL_DESKTOP=ON or -DSM2_BUILD_OPENGL_ES=ON.");
        return 1;
    }
#endif

    // Likewise the Vulkan backend is opt-in at build time (SM2_BUILD_VULKAN).
#if !defined(SM2_HAVE_VULKAN)
    if (options.graphics_backend == GraphicsBackendChoice::Vulkan) {
        SM2_ERROR("--graphics-backend vulkan was requested, but this binary was "
                  "built without the Vulkan backend. Rebuild with "
                  "-DSM2_BUILD_VULKAN=ON.");
        return 1;
    }
#endif

    // Command line first, then the file for everything the command line did not
    // mention. Doing it this way round rather than loading first and overwriting
    // means a flag is always the last word, whatever the file says.
    Config defaults;
#if defined(SM2_ENABLE_VALIDATION)
    // A build with validation compiled in enables it by default, which is still a
    // default: a file that turns it off is obeyed.
    defaults.validation = true;
#endif

    // --config names a directory; the file within it is always sm2-emu.ini.
    const std::string config_path =
        options.config_path.empty()
            ? default_config_path()
            : (std::filesystem::path(options.config_path) / "sm2-emu.ini").string();

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
    if (!options.given.lightgun) {
        options.config.lightgun = from_file.lightgun;
    }
    if (!options.given.validation) {
        options.config.validation = from_file.validation;
    }
    if (!options.given.gpu) {
        options.config.gpu = from_file.gpu;
    }
    if (!options.given.nvram_dir) {
        options.config.nvram_dir = from_file.nvram_dir;
    }
    if (!options.given.games_xml) {
        options.config.games_xml = from_file.games_xml;
    }
    if (!options.given.log_level) {
        options.config.log_level = from_file.log_level;
    }
    options.config.window_width  = from_file.window_width;
    options.config.window_height = from_file.window_height;

    // Settings with no command-line flag come straight from the file, so the
    // GUI shows and round-trips what was saved. (These were being dropped, which
    // made the wheel bindings and other GUI-only settings appear as defaults.)
    options.config.show_fps            = from_file.show_fps;
    options.config.wheel_ffb           = from_file.wheel_ffb;
    options.config.wheel_ffb_strength  = from_file.wheel_ffb_strength;
    options.config.wheel_steer_degrees = from_file.wheel_steer_degrees;
    options.config.wheel_lock_degrees  = from_file.wheel_lock_degrees;
    options.config.wheel_rumble          = from_file.wheel_rumble;
    options.config.wheel_rumble_strength = from_file.wheel_rumble_strength;
    options.config.wheel_buttons       = from_file.wheel_buttons;
    options.config.wheel_steer_axis    = from_file.wheel_steer_axis;
    options.config.wheel_accel_axis    = from_file.wheel_accel_axis;
    options.config.wheel_brake_axis    = from_file.wheel_brake_axis;
    options.config.wheel_accel_invert  = from_file.wheel_accel_invert;
    options.config.wheel_brake_invert  = from_file.wheel_brake_invert;

    options.config.lightgun_crosshair       = from_file.lightgun_crosshair;
    options.config.lightgun_recoil          = from_file.lightgun_recoil;
    options.config.lightgun_recoil_strength = from_file.lightgun_recoil_strength;
    options.config.sinden_border            = from_file.sinden_border;
    options.config.sinden_border_colour     = from_file.sinden_border_colour;
    options.config.sinden_border_thickness  = from_file.sinden_border_thickness;
    options.config.gun_buttons              = from_file.gun_buttons;

    // --verbose is shorthand, so an explicit level beats it.
    log::Level level = log::Level::Info;
    if (options.given.log_level || !options.verbose) {
        (void)parse_log_level(options.config.log_level, &level);
    } else {
        level = log::Level::Debug;
    }
    log::set_level(level);

    SM2_INFO("sm2-emu %s", SM2_VERSION);
    if (!readable) {
        SM2_WARN("could not read '%s'; using defaults", config_path.c_str());
    }
    for (const std::string& problem : problems) {
        SM2_WARN("%s", problem.c_str());
    }

    if (options.write_config) {
        if (!save_config(config_path, options.config)) {
            SM2_ERROR("could not write '%s'", config_path.c_str());
            return 1;
        }
        std::printf("Wrote %s\n", config_path.c_str());
        return 0;
    }

    // Make sure the settings and NVRAM locations exist. If there is no ini yet,
    // write one so there is a file to edit and later saves have somewhere to go;
    // an existing one is left untouched. save_config creates the parent dir.
    {
        std::error_code error;
        if (!std::filesystem::exists(config_path, error) || error) {
            save_config(config_path, options.config);
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

    // -- ROM database ------------------------------------------------------
    // Loaded before anything graphical, so a bad ROM path fails immediately
    // instead of after a window has appeared.
    rom::GameDatabase database;
    if (options.list_games || !options.rom_path.empty()) {
        const std::optional<std::string> database_path =
            rom::GameDatabase::locate(options.config.games_xml);
        if (!database_path.has_value() || !database.load(*database_path)) {
            return 1;
        }
    }

    if (options.list_games) {
        std::printf("%-12s %-14s %-34s %-6s %s\n",
                    "SET", "BOARD", "TITLE", "YEAR", "NOTES");
        for (const rom::GameSpec& game : database.games()) {
            // The version is printed as written rather than with a "v" prefix,
            // because these strings are Sega's own words: "2.1", but also
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

    std::optional<rom::LoadResult> loaded;
    if (!options.rom_path.empty()) {
        loaded = rom::RomLoader::load(database, options.rom_path, options.game);
        if (!loaded.has_value()) {
            return 1;
        }
        if (!options.dump_roms.empty()) {
            return rom::RomLoader::dump_regions(loaded->roms, options.dump_roms) ? 0 : 1;
        }
    } else {
        SM2_INFO("no ROM given; starting with the bring-up display only");
    }

    // -- the machine -------------------------------------------------------
    // hw::create_machine dispatches on loaded->game.board and does the
    // construction and init() that main.cpp used to do directly against
    // hw::Model2. All four boards are implemented, so this hands back one of
    // four concrete classes on success; downcasting here keeps every accessor
    // below (cpu(), copro(), sound(), uart(), and the debug/render call sites)
    // written against the concrete class unchanged, matching
    // hw::Model2MachineBase's own documented split between the shared interface
    // and board-specific accessors.
    //
    // The main CPU and the sound link are the same types on all four, so they
    // are resolved once here into pointers rather than re-tested at every use.
    // The coprocessor genuinely differs, and so does the sound board: the
    // original Model 2 carries the Model 1 audio board (68000 + YM3438 + two
    // MultiPCMs) rather than the 68000/SCSP one the CRX family shares. Both are
    // emulated, and both present hw::SoundBoard, so the audio path below is
    // written once against that interface; the two places that need a specific
    // board's registers say so with a cast.
    std::unique_ptr<hw::Model2MachineBase> machine_iface;
    hw::Model2*         machine      = nullptr;
    hw::Model2B*        machine_2b   = nullptr;
    hw::Model2C*        machine_2c   = nullptr;
    hw::Model2Original* machine_orig = nullptr;

    cpu::i960::I960* main_cpu    = nullptr;
    hw::SoundBoard*  sound_board = nullptr;
    const hw::I8251* sound_link  = nullptr;

    if (loaded.has_value()) {
        machine_iface = hw::create_machine(loaded->game, std::move(loaded->roms));
        if (!machine_iface) {
            return 1;
        }
        machine      = dynamic_cast<hw::Model2*>(machine_iface.get());
        machine_2b   = dynamic_cast<hw::Model2B*>(machine_iface.get());
        machine_2c   = dynamic_cast<hw::Model2C*>(machine_iface.get());
        machine_orig = dynamic_cast<hw::Model2Original*>(machine_iface.get());
        if (machine != nullptr) {
            main_cpu    = &machine->cpu();
            sound_board = &machine->sound();
            sound_link  = &machine->uart();
        } else if (machine_2b != nullptr) {
            main_cpu    = &machine_2b->cpu();
            sound_board = &machine_2b->sound();
            sound_link  = &machine_2b->uart();
        } else if (machine_2c != nullptr) {
            main_cpu    = &machine_2c->cpu();
            sound_board = &machine_2c->sound();
            sound_link  = &machine_2c->uart();
        } else if (machine_orig != nullptr) {
            main_cpu    = &machine_orig->cpu();
            sound_board = &machine_orig->sound();
            sound_link  = &machine_orig->uart();
        } else {
            SM2_ERROR("internal error: create_machine returned an unexpected "
                      "machine type");
            return 1;
        }
        machine_iface->set_nvram_directory(options.config.nvram_dir);
        machine_iface->set_log_unmapped(options.log_unmapped);
        machine_iface->load_nvram();
        // init() resets before the NVRAM is in place, so reset again to let the
        // program read the settings it saved last time.
        machine_iface->reset();
    }

    // -- headless boot test ------------------------------------------------
    // No window, no Vulkan: just run the machine and report where it got to.
    // This is the fastest way to see whether a change moved the boot forward.
    if (options.boot_test != 0) {
        if (main_cpu == nullptr) {
            SM2_ERROR("--boot-test needs a ROM");
            return 1;
        }

        int exit_code_boot_test = 0;
        std::vector<s16> recorded;
        if (!options.dump_audio.empty()) {
            // About 767 stereo frames per video frame.
            recorded.reserve(static_cast<usize>(options.boot_test) * 800 * 2);
        }

        // Unified accessors: every implemented board shares these types. The
        // sound board is a pointer rather than a reference because a synthetic
        // machine in the tests may have none -- see the note where it is
        // resolved.
        auto&            the_cpu   = *main_cpu;
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
            machine_iface->run_frame();

            // A numbered software frame every so often, so one headless run can be
            // searched for the instant that lines up with a MAME capture instead of
            // guessing the time.
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

            // Draining every frame whether or not it is being recorded: the sound
            // board drops samples nothing collects, and leaving that to happen
            // would put gaps in a recording.
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
            // The original Model 2 carries the same MB86234 as Model 2A, wired the
            // same way, so the same figures apply. What is board-specific here is
            // the I/O board: it is a whole second computer, and if its Z80 is not
            // executing sensibly the program sees no inputs at all, so its state
            // belongs next to the coprocessor's rather than buried in a log.
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
        } else {
            const hw::CoproTgpx4& copro = machine_2c->copro();
            // Two host writes make one 64-bit program word, so both figures are
            // reported: a program that uploaded an odd number of halves has left
            // its last word half-written, which is worth seeing.
            std::printf("copro uploaded    : %u host write(s), %u program word(s)\n",
                        copro.uploaded_words(), copro.uploaded_words() / 2);
            std::printf("copro instructions: %llu\n",
                        (unsigned long long)copro.cpu().instructions());
            std::printf("copro state       : %s%s\n", copro.cpu().state_string().c_str(),
                        copro.cpu().halted() ? " HALTED" : "");
            // The MB86235 records an unimplemented opcode and stops rather than
            // aborting, so without printing this a fault looks like a
            // coprocessor that simply did nothing.
            std::printf("copro faulted     : %s\n",
                        copro.cpu().faulted() ? copro.cpu().fault_message().c_str()
                                              : "no");
            std::printf("copro fifos       : in %zu (peak overflow %zu), out %zu "
                        "(peak overflow %zu)\n",
                        copro.fifo_in().size(), copro.fifo_in().peak_overflow(),
                        copro.fifo_out().size(), copro.fifo_out().peak_overflow());
        }

        // The link is reported whether or not there is a board behind it, because
        // on the original Model 2 the link is all there is: the M1 audio board is
        // not emulated, so "bytes sent" is the only evidence that the program's
        // sound handshake completed rather than stalled.
        {
            const hw::I8251::Counters& uart = the_uart.counters();
            std::printf("sound link        : %llu byte(s) to the board, %llu back, "
                        "%llu overrun(s), %llu status reads\n",
                        (unsigned long long)uart.bytes_sent,
                        (unsigned long long)uart.bytes_received,
                        (unsigned long long)uart.overruns,
                        (unsigned long long)uart.status_reads);
        }

        // Past this point the report is board-specific: the two boards share no
        // registers, so there is nothing to say about both at once beyond whether
        // a program ROM turned up.
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

        // Both TGP boards, since both read the same table ROM through the same
        // coprocessor.
        hw::CoproTgp* tgp = machine != nullptr        ? &machine->copro()
                          : machine_orig != nullptr   ? &machine_orig->copro()
                                                      : nullptr;
        if (tgp != nullptr && !hw::run_copro_selftest(*tgp)) {
            SM2_ERROR("the coprocessor's mathematical units are out of tolerance");
            exit_code_boot_test = 1;
        }

        hw::print_render_list_summary(*machine_iface);
        hw::print_tilemap_summary(*machine_iface);

        // Compose once, at the end. This exercises the real colour chain and the
        // real compositor, which is what the display uses, so a discrepancy
        // between this and the raw layer dump points at one or the other.
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

        machine_iface->save_nvram();
        return the_cpu.faulted() ? 1 : exit_code_boot_test;
    }

    if (!SDL_Init(0)) {
        SM2_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    int exit_code = 0;
    {
        osd::WindowConfig window_config;
        window_config.title      = std::string("sm2-emu ") + SM2_VERSION;
        window_config.width      = options.config.window_width;
        window_config.height     = options.config.window_height;
        window_config.fullscreen = options.config.fullscreen;

        // Which GPU backend presents. `opengl` names it; `vulkan` and
        // `software` present through Vulkan, or OpenGL if Vulkan is not built
        // (`software` only replaces the drawing, not the presentation path).
        bool present_with_opengl = options.graphics_backend == GraphicsBackendChoice::Opengl;
#if !defined(SM2_HAVE_VULKAN)
        present_with_opengl = true;
#endif
        window_config.graphics_api = present_with_opengl ? osd::GraphicsApi::OpenGl
                                                          : osd::GraphicsApi::Vulkan;

#if defined(SM2_HAVE_OPENGL_ES)
        // GLES on X11 needs EGL, not GLX; force it before the GL library loads.
        if (window_config.graphics_api == osd::GraphicsApi::OpenGl) {
            SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
        }
#endif

        osd::Window window;
        if (!window.create(window_config)) {
            SDL_Quit();
            return 1;
        }

        render::BackendConfig backend_config;
        backend_config.enable_validation = options.config.validation;
        backend_config.vsync             = options.config.vsync;
        backend_config.preferred_device  = options.config.gpu;

        // Each factory is only defined when its SM2_BUILD_* option was on, so
        // the #if guards keep an absent backend from being an undefined symbol.
        // The mismatched --graphics-backend cases already errored out above.
        std::unique_ptr<render::Backend> backend;
        if (present_with_opengl) {
#if defined(SM2_HAVE_OPENGL_DESKTOP) || defined(SM2_HAVE_OPENGL_ES)
            backend = render::create_opengl_backend();
#endif
        } else {
#if defined(SM2_HAVE_VULKAN)
            backend = render::create_vulkan_backend();
#endif
        }
        if (!backend) {
            SM2_ERROR("no GPU backend is available to present with. Build with "
                      "-DSM2_BUILD_VULKAN=ON and/or -DSM2_BUILD_OPENGL_DESKTOP=ON "
                      "(or run headless with --boot-test).");
            SDL_Quit();
            return 1;
        }
        if (!backend->init(window, backend_config)) {
            SM2_ERROR("render backend initialisation failed");
            SDL_Quit();
            return 1;
        }

        // Name of the constructed GPU backend, for every place that used to
        // hardcode "Vulkan" as "whichever GPU backend isn't the software
        // one" -- true while Vulkan was the only choice, not once
        // --graphics-backend opengl can select a real second one.
        const char* gpu_backend_name = present_with_opengl ? "OpenGL" : "Vulkan";

        // The settings overlay, drawn on top of the emulator's output. Owns
        // only ImGui's context and its SDL3 platform backend; the backend
        // above owns whichever GPU API actually draws what it builds.
        osd::Gui gui;
        gui.set_config_path(config_path);
        if (!gui.init(window.handle())) {
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

        // Show the overlay when launched without a ROM so there is something to
        // interact with.
        if (!machine_iface) {
            gui.show();
        }

        // GPU names for the settings dropdown.
        const std::vector<std::string> gpu_names = render::enumerate_render_devices();

        // A machine with no gamepad is not an error, so a failure here is worth
        // reporting but not worth refusing to run over: the keyboard covers
        // everything the cabinet has.
        osd::Input input;
        if (!input.init(wheel_settings_from(options.config))) {
            SM2_WARN("gamepads are unavailable; the keyboard still works");
        }

        // A machine with no audio device still has to run, so a failure here is
        // reported by Audio::init and otherwise ignored. Opened at the machine's
        // own 44100 Hz and left to SDL to resample.
        osd::Audio audio;
        if (sound_board != nullptr) {
            static_cast<void>(audio.init(sound_board->sample_rate()));
        }

        // With no machine there is nothing to draw, so present the two empty
        // surfaces over a recognisable background rather than a blank window.
        hw::Model2Video idle_video;

        // The CPU rasteriser, driven live instead of only alongside a capture, so
        // the two renderers can be A/B'd without restarting. Only used when
        // use_software_renderer is set and a machine is loaded; see F2 below.
        hw::SoftRenderer soft_renderer;
        std::vector<u32> soft_frame(static_cast<usize>(backend->native_width())
                                        * backend->native_height(),
                                    0);

        // Paced against real time rather than against the display, because the
        // machine's 57.5245 Hz divides into no monitor's refresh rate.
        //
        // Vsync and pacing compose rather than conflict: whichever wants the longer
        // frame wins, so on a 60 Hz display the pacer's 17.384 ms is the limit and
        // the rate is right. On a display slower than 57.5 Hz vsync would win and the
        // game would run slow, which is what --no-vsync is for.
        osd::FramePacer pacer;
        pacer.set_throttled(options.config.throttle);
        pacer.start(hw::Model2::kFrameNanoseconds);

        SM2_INFO("entering main loop; Escape quits, P pauses, Tab fast-forwards, "
                 "F2 switches renderer");

        /// Everything the sound board produced, when --dump-audio was given.
        std::vector<s16> recorded_audio;

        // Per-stage CPU timers for --profile (design.md requirement 1.1). Built
        // regardless of options.profile -- maybe_scope() below makes recording
        // into them a no-op when profiling is off, which costs one branch per
        // stage per frame rather than needing every call site to be conditional.
        core::StageTimer stage_run_frame("run_frame");
        core::StageTimer stage_geometry("geometry_engine");
        core::StageTimer stage_compose("tilemap_compose");
        // build() both triangulates and memcpys into the mapped host buffers, and
        // -- when texture_generation changed -- also records the decode
        // dispatch's vkCmdDispatch, so this one CPU scope covers design.md's
        // "render-list build" and "host-buffer memcpys" together, plus a small,
        // usually-once amount of command recording it was not practical to pull
        // apart without splitting build() itself.
        core::StageTimer stage_build("poly3d_build_and_memcpy");
        core::StageTimer stage_tilemap_upload("tilemap_upload_memcpy");
        // Everything else that issues vkCmd* for this frame: the 3D pass's own
        // draw calls, both tilemap draws, and the present blit. Named plainly as
        // "command recording" because build()'s occasional dispatch aside, this
        // is the whole of it.
        core::StageTimer stage_record("command_recording");
        core::StageTimer stage_submit("submit_and_present");
        core::StageTimer stage_software("software_renderer");
        // The wait the emulation thread spends blocked on the GPU/present,
        // inside begin_frame() (Vulkan fence+acquire, GL swap back-pressure). It
        // was outside every stage before, which is why the idle stall was
        // invisible in earlier profiles. design.md §1.
        core::StageTimer stage_present_wait("present_wait");
        // The three cores run_frame() interleaves, split out so a CPU-bound
        // result can be told from one hot core. Filled inside run_frame() and
        // read back here the way stage_geometry already is. design.md §1/§4.
        core::StageTimer stage_cpu_i960("cpu_i960");
        core::StageTimer stage_cpu_copro("cpu_copro");
        core::StageTimer stage_cpu_sound("cpu_sound");
        if (machine_iface) {
            machine_iface->set_core_profiling(options.profile);
        }
        if (options.profile) {
            const usize expected = static_cast<usize>(options.duration_seconds) * 120;
            for (core::StageTimer* timer :
                {&stage_run_frame, &stage_geometry, &stage_compose, &stage_build,
                 &stage_tilemap_upload, &stage_record, &stage_submit, &stage_software,
                 &stage_present_wait, &stage_cpu_i960, &stage_cpu_copro,
                 &stage_cpu_sound}) {
                timer->reserve(expected);
            }
        }
        // GPU stage samples, read back once per frame from the backend
        // (design.md requirement 1.2). Indexed by render::GpuStage.
        std::array<std::vector<double>, static_cast<usize>(render::GpuStage::kCount)>
            gpu_stage_samples;
        bool gpu_timing_unavailable_warned = false;

        u32  frames_presented = 0;   ///< Since the loop started.
        u32  frames_written_off = 0; ///< Whole frames given up after a stall.
        bool paused             = false;
        bool audio_paused_state = false; ///< tracks effective pause to drive audio/pacer on change.
        bool fast_forward       = false;
        bool running            = true;
        bool use_software_renderer = options.start_in_software_renderer;
        u64  last_title_ns      = SDL_GetTicksNS();

        // --duration's deadline, and the per-frame wall-clock cost recorded for
        // its summary. Frame time here is measured start-to-start rather than
        // just the render, so it includes run_frame(), the pacer's wait() and
        // everything else one iteration of this loop does -- the whole-loop cost
        // is what a player experiences as the frame rate, which is the same
        // reasoning FramePacer::measured_hz() already uses.
        const u64 run_start_ns = SDL_GetTicksNS();
        const u64 run_deadline_ns =
            options.duration_seconds != 0
                ? run_start_ns + static_cast<u64>(options.duration_seconds) * 1'000'000'000ULL
                : 0;
        std::vector<double> frame_times_ms;
        if (run_deadline_ns != 0) {
            // 57.52 Hz unthrottled on reasonable hardware comfortably exceeds
            // this; reserving avoids reallocation skewing the very timings being
            // measured.
            frame_times_ms.reserve(static_cast<usize>(options.duration_seconds) * 2000);
        }

        // "sm2-emu — <game or 'no game'> [Vulkan|OpenGL|Software]", plus the
        // rate and pause state once the loop is running. Shared by the
        // initial title, the once-a-second refresh and the immediate
        // refresh F2 does, so the three can never drift into different
        // formats.
        const auto build_title = [&]() {
            std::string title = std::string("sm2-emu — ")
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

        while (running) {
            const u64 frame_start_ns = SDL_GetTicksNS();

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // Let ImGui see every event so it can capture mouse/keyboard
                // when the overlay is active.
                ImGui_ImplSDL3_ProcessEvent(&event);

                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        running = false;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        if (event.key.key == SDLK_ESCAPE) {
                            running = false;
                        } else if (event.key.key == SDLK_F1 && !event.key.repeat) {
                            gui.toggle();
                        } else if (event.key.key == SDLK_F2 && !event.key.repeat) {
                            use_software_renderer = !use_software_renderer;
                            SM2_INFO("switched to the %s renderer",
                                     use_software_renderer ? "software" : gpu_backend_name);
                            window.set_title(build_title());
                        } else if (!gui.visible()) {
                            // Only process game keys when the overlay is hidden.
                            if (event.key.key == SDLK_P && !event.key.repeat) {
                                paused = !paused;
                            } else if (event.key.key == SDLK_TAB && !event.key.repeat) {
                                fast_forward = true;
                            }
                        }
                        break;
                    case SDL_EVENT_KEY_UP:
                        if (event.key.key == SDLK_TAB) {
                            fast_forward = false;
                        }
                        break;
                    default:
                        if (!gui.visible()) {
                            input.handle_event(event);
                        }
                        break;
                }
            }
            if (!running) {
                break;
            }

            // Fast-forward is a held key rather than a toggle, so letting go always
            // returns to real time.
            pacer.set_throttled(options.config.throttle && !fast_forward);

            // Audio clock pull: nudge the frame rate toward the audio device's
            // true rate so the queue does not drift. Too full -> slower, too
            // empty -> faster; deadband stops it fighting the per-frame sawtooth.
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

            // Warm-up gate: don't sample until --profile-after's frame (see its
            // declaration). Zero means always true.
            const bool profile_sample =
                options.profile
                && (options.profile_after == 0 || frames_presented >= options.profile_after);

            // The wheel's Menu-bound button toggles the overlay, same as F1.
            // Polled every frame (not only while running) so it can also close
            // the overlay once it has paused the game.
            if (input.menu_button_pressed()) {
                gui.toggle();
            }

            // Opening the overlay pauses the game, so settings can be changed
            // without the car driving off. The P key's manual pause still holds
            // independently. Audio and the pacer follow on each transition.
            const bool effective_pause = paused || gui.visible();
            if (effective_pause != audio_paused_state) {
                audio_paused_state = effective_pause;
                audio.set_paused(effective_pause);
                pacer.resync();
            }

            if (machine_iface && !effective_pause) {
                // Inputs are levels, sampled whenever the program polls the I/O
                // controller during the frame, so they have to be set before the
                // frame runs rather than after.
                input.poll(&machine_iface->inputs(), loaded->game);
                // Push the live settings (the GUI changes these) before computing
                // this frame's force, so slider and calibration take effect now.
                input.set_wheel_settings(wheel_settings_from(options.config));
                input.set_recoil(options.config.lightgun_recoil,
                                 options.config.lightgun_recoil_strength);
                input.set_gun_buttons(options.config.gun_buttons);
                input.update_force_feedback(loaded->game, machine_iface->drive_board_force());
                if (options.coin_at != 0) {
                    // Scripted coin, start and character confirmation, so an
                    // unattended capture can reach the game itself rather than
                    // only the attract mode.
                    const osd::Input::ScriptedPress press = osd::Input::scripted_press(
                        frames_presented, options.coin_at, loaded->game.start1_bit);
                    machine_iface->inputs().in0 &= static_cast<u8>(~press.in0);
                    machine_iface->inputs().in1 &= static_cast<u8>(~press.in1);
                }

                {
                    auto scope = core::maybe_scope(stage_run_frame, profile_sample);
                    machine_iface->run_frame();
                }
                if (profile_sample) {
                    // Read straight back out rather than timed separately: the
                    // geometry engine runs inside run_frame() (at vblank), not as
                    // a call main.cpp makes itself, so there is no separate scope
                    // to wrap -- see Geometrizer::last_run_nanoseconds()'s own
                    // documentation of why this figure exists.
                    stage_geometry.record_ms(
                        static_cast<double>(machine_iface->geometry_stage_nanoseconds())
                        / 1'000'000.0);
                    // The per-core split, filled inside run_frame() for the same
                    // reason: main.cpp sees one interleaved call, not three.
                    stage_cpu_i960.record_ms(
                        static_cast<double>(machine_iface->i960_stage_nanoseconds())
                        / 1'000'000.0);
                    stage_cpu_copro.record_ms(
                        static_cast<double>(machine_iface->copro_stage_nanoseconds())
                        / 1'000'000.0);
                    stage_cpu_sound.record_ms(
                        static_cast<double>(machine_iface->sound_stage_nanoseconds())
                        / 1'000'000.0);
                }

                // The sound board produced about 767 stereo frames while that ran.
                // Handed over every frame rather than buffered here, so the only
                // buffering is SDL's. Null on the original Model 2, whose sound
                // board is not emulated.
                if (sound_board != nullptr) {
                    const std::span<const s16> produced = sound_board->pending_samples();
                    audio.submit(produced);
                    if (!options.dump_audio.empty()) {
                        recorded_audio.insert(recorded_audio.end(), produced.begin(),
                                              produced.end());
                    }
                    sound_board->clear_pending_samples();
                }

                auto& cpu = *main_cpu;
                if (cpu.faulted()) {
                    SM2_ERROR("stopping: %s", cpu.fault_message().c_str());
                    exit_code = 1;
                    break;
                }
            }

            // Timed as present_wait (see its declaration). The scope closes
            // before the continue below so a skipped frame's SDL_Delay is not
            // folded into the figure.
            bool begin_ok;
            {
                auto scope = core::maybe_scope(stage_present_wait, profile_sample);
                begin_ok = backend->begin_frame();
            }
            if (!begin_ok) {
                // Minimised or the swapchain was rebuilt. Yield rather than
                // spinning on a window we cannot draw into, and abandon the pacing
                // deadline because an unknown amount of time is about to pass.
                SDL_Delay(16);
                pacer.resync();
                // Still honour a wall-clock run deadline (--duration/--profile):
                // without this, a window that stays non-drawable spins here
                // forever and the profile/summary never prints. The frame-time
                // sample is deliberately NOT recorded for a skipped frame.
                if (run_deadline_ns != 0 && SDL_GetTicksNS() >= run_deadline_ns) {
                    running = false;
                }
                continue;
            }

            const hw::Model2Video& video =
                machine_iface ? machine_iface->video() : idle_video;

            // Only meaningful with a machine loaded: SoftRenderer::render() takes
            // a Model2MachineBase, so there is nothing for it to draw against the
            // idle placeholder and the Vulkan path below covers that case anyway.
            const bool draw_with_software = use_software_renderer && machine_iface;

            // Render test mode's framebuffer overlay (Model2Video::
            // draw_framebuffer) overwrites `below` after the tilemap composite
            // and has no GPU equivalent, so that mode always takes the CPU path.
            const bool render_test = machine_iface && machine_iface->render_test_mode();

            // Whether this frame's Vulkan draw uses tilemaps.compute() in place
            // of the CPU-composited upload. Independent of need_cpu_compose
            // below: a --soft-render comparison run wants both the GPU path
            // exercised (this) and a fresh CPU oracle (that), in the same frame.
            const bool use_gpu_tilemap = machine_iface && !use_software_renderer && !render_test;

            // Whether Model2Video::compose(), the CPU tilemap composite, must
            // run this frame: whenever something reads video.below()/above()
            // directly -- the software renderer's live draw, a --soft-render
            // comparison capture later in this same frame (hw::dump_software_frame,
            // below), or render test mode's compose()-then-overlay sequence.
            const bool need_cpu_compose =
                machine_iface && (use_software_renderer || options.soft_render || render_test);

            if (machine_iface) {
                if (need_cpu_compose) {
                    auto scope = core::maybe_scope(stage_compose, profile_sample);
                    machine_iface->compose_video();
                } else if (use_gpu_tilemap) {
                    // compose_video()'s other job, done here directly since its
                    // own compose() call -- the expensive part -- is what
                    // tilemaps.compute() below replaces.
                    if (machine_iface->palette_dirty()) {
                        machine_iface->video().refresh_pens();
                        machine_iface->clear_palette_dirty();
                    }
                }
            }

            if (!draw_with_software) {
                // Uploads and the 3D pass first. Both need a command buffer that is
                // not inside a rendering scope: a transfer cannot be issued inside
                // one, and the 3D pass opens a scope of its own on its offscreen
                // target.
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
                    backend->submit_polygons(machine_iface.get(), video);
                }
            }

            // Command recording proper starts here: polygons.render() below, and
            // everything up to and including present.record() further down
            // (marked at its own call site) is vkCmd* issuance for this frame,
            // with a capture's readback recording folded in between since it is
            // also just a command, and nothing else interleaved in the
            // non-software path. Not used when draw_with_software, since there
            // is no Vulkan drawing to time in that path -- present.upload_from_host()
            // is a transfer, not a draw, and stays out of this figure.
            std::optional<core::StageTimer::Scope> record_scope;
            if (profile_sample && !draw_with_software) {
                record_scope.emplace(stage_record);
            }

            if (!draw_with_software) {
                backend->render_polygons();
            }

            // The hardware's three-way composite, all of it at the native 496x384:
            // tilemap layers of priority category zero, then the 3D output, then
            // category one. Nothing is magnified until blit_to_swapchain() below,
            // so the two blends run on the hardware's own pixels rather than on
            // colours a magnifying filter has already mixed with their neighbours.
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
                // The CPU rasteriser performs this whole three-way composite
                // itself -- background, below, 3D (or the framebuffer in render
                // test mode), above -- so the tilemap and 3D passes above are
                // skipped entirely; its result lands in the same native target
                // either renderer presents from, which is what lets a screenshot
                // and the present/letterbox path stay renderer-agnostic.
                {
                    auto scope = core::maybe_scope(stage_software, profile_sample);
                    soft_renderer.render(*machine_iface, machine_iface->render_list(),
                                         soft_frame);
                }
                backend->submit_native_frame(soft_frame);
            } else {
                // Render test mode cuts the DSP out: the framebuffer bank the host has
                // been drawing into is shown instead of the 3D pass, and has already
                // been composed into the layers below.
                const bool skip_3d = machine_iface && machine_iface->render_test_mode();
                backend->composite_native_frame(video.background(), skip_3d);
            }

            // Read back the finished native frame, before it is scaled, so a
            // screenshot is the frame the hardware produced at the size it
            // produced it.
            const bool last_frame =
                options.run_frames != 0 && frames_presented + 1 >= options.run_frames;
            const bool numbered_series =
                options.screenshot_interval != 0 || !options.screenshot_frames.empty();
            const bool capture_this_frame =
                !options.screenshot.empty()
                && (!options.screenshot_frames.empty()
                        ? options.screenshot_frames.count(frames_presented) != 0
                        : options.screenshot_interval != 0
                              ? (frames_presented % options.screenshot_interval) == 0
                                    || last_frame
                              : last_frame || options.run_frames == 0);
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

            // Then the one magnification, into the swapchain.
            backend->blit_to_swapchain();
            // Command recording, as this figure means it, ends here: the GUI
            // overlay below is a separate concern (ImGui's own draw-list build
            // plus its own submission) and left out on purpose.
            record_scope.reset();

            // Draw the ImGui overlay on top of the presented frame, while the
            // swapchain image is still in a colour-attachment layout. The FPS
            // counter is always on now, so this always has something to draw and
            // gui_active is unconditionally true; the return value stays a bool
            // for symmetry with draw_overlay()'s inactive path below.
            backend->begin_overlay_frame();
            gui.new_frame();
            const bool gui_active =
                gui.draw(options.config, gpu_names, pacer.measured_hz(),
                        use_software_renderer ? "Software" : gpu_backend_name, &input);
            // Apply a fullscreen toggle from the Settings menu the moment it
            // changes, rather than only at the next launch.
            if (options.config.fullscreen != window.fullscreen()) {
                window.set_fullscreen(options.config.fullscreen);
            }
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

            // A series has to be written as it goes, and the readback is only
            // complete once the submission is. Waiting for the device here stalls
            // the pipeline, which is acceptable in a diagnostic mode and is why
            // this is not the default path.
            if (capture_this_frame && numbered_series) {
                backend->wait_idle();
                if (!backend->save_capture(numbered_path(options.screenshot, frames_presented))) {
                    exit_code = 1;
                    break;
                }
            }

            ++frames_presented;

            if (options.run_frames != 0 && frames_presented >= options.run_frames) {
                running = false;
            }

            // Measured from this iteration's own frame_start_ns rather than an
            // external "last frame's end" mark, so a skipped iteration (the
            // begin_frame() continue above, on a minimised window or a swapchain
            // rebuild) cannot fold its idle time into the next sample as a fake
            // outlier.
            //
            // Recorded before pacer.wait(): with throttling on, wait() is where
            // the pacer deliberately spends idle time to hold the target rate, and
            // including it would measure the pacer instead of the renderer. With
            // --no-throttle, which is what a throughput comparison wants, wait()
            // returns immediately and this is the whole frame cost either way --
            // see FramePacer::wait()'s own account_for_frame() call for the same
            // reasoning applied to measured_hz().
            if (run_deadline_ns != 0) {
                const u64 now_ns = SDL_GetTicksNS();
                // Same --profile-after gate as the per-stage samples, so the fps
                // average is in-game only. The deadline check below stays
                // unconditional.
                if (options.profile_after == 0 || frames_presented > options.profile_after) {
                    frame_times_ms.push_back(
                        static_cast<double>(now_ns - frame_start_ns) / 1'000'000.0);
                }
                if (now_ns >= run_deadline_ns) {
                    running = false;
                }
            }

            // Last thing in the loop, so the wait absorbs everything the frame cost
            // rather than only part of it.
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
            std::printf("p50 / p95 / p99 ms : %.3f / %.3f / %.3f (%.2f / %.2f / %.2f fps)\n",
                        p50, p95, p99, 1000.0 / p50, 1000.0 / p95, 1000.0 / p99);
            std::printf("throttled          : %s\n", options.config.throttle ? "yes" : "no");
            SM2_INFO("%s: %u frame(s) over %.1fs, average %.2f fps (p50 %.3f / p95 %.3f / "
                     "p99 %.3f ms), throttle %s",
                     use_software_renderer ? "software" : gpu_backend_name, frames_presented,
                     total_ms / 1000.0, 1000.0 / mean_ms, p50, p95, p99,
                     options.config.throttle ? "on" : "off");

            if (options.profile) {
                // Named per design.md requirement 1.5: set, frame range, coin-at
                // and NVRAM directory, so two reports that used different
                // conditions cannot be mistaken for a comparable pair.
                std::printf("\n--- per-stage CPU (ms), %s, --duration %u, --coin-at %u, "
                            "--nvram %s ---\n",
                            loaded.has_value() ? loaded->game.name.c_str() : "no game",
                            options.duration_seconds, options.coin_at,
                            options.config.nvram_dir.c_str());
                std::printf("%-24s %8s %10s %10s %10s %10s\n", "stage", "samples", "mean",
                            "p50", "p95", "p99");

                // Collected alongside the printed table, for --profile-csv below,
                // rather than reopening the file and re-deriving the same
                // StageStats/GpuStageTime figures a second time.
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
                     &stage_present_wait, &stage_software}) {
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

        if (machine_iface) {
            machine_iface->save_nvram();
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

        audio.shutdown();
        input.shutdown();
        // Before gui.shutdown(): the backend's own ImGui renderer backend
        // shutdown also reads ImGui's global context, which must not have
        // been destroyed yet.
        backend->shutdown_overlay();
        gui.shutdown();
        backend->shutdown();
    }

    SDL_Quit();
    log::close_log_file();
    return exit_code;
}
