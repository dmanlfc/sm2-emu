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
#include "core/types.h"
#include "hw/machine_factory.h"
#include "hw/model2.h"
#include "hw/model2_debug.h"
#include "osd/audio.h"
#include "osd/frame_pacer.h"
#include "osd/gui.h"
#include "osd/input.h"
#include "osd/window.h"
#include "render/vk/context.h"
#include "render/vk/frame_capture.h"
#include "render/vk/poly3d_pass.h"
#include "render/vk/present_pass.h"
#include "render/vk/tilemap_pass.h"
#include "rom/game_db.h"
#include "rom/rom_loader.h"

#include <memory>

#include <SDL3/SDL.h>

#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct Options {
    /// Settings that persist. Loaded from the file, then overridden by flags.
    sm2::Config config;

    /// Which of those the command line actually set, so a flag can win over the
    /// file without every flag having to carry its own default.
    struct Given {
        bool vsync      = false;
        bool throttle   = false;
        bool fullscreen = false;
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

    /// Insert coins and press start around this frame. Zero leaves the panel
    /// alone.
    sm2::u32 coin_at = 0;

    /// Run this many frames headless, report, and exit. Zero means run normally.
    sm2::u32 boot_test    = 0;

    /// Quit after this many presented frames. Zero means run until asked to stop.
    sm2::u32 run_frames   = 0;
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
        "      --screenshot <f>  Write the last presented frame to this PPM file\n"
        "      --screenshot-interval <n>  Capture every n frames instead, numbering\n"
        "                      each file after the frame it came from\n"
        "      --coin-at <n>   Insert two coins and press start around frame n, so\n"
        "                      an unattended run reaches the game itself\n"
        "      --dump-audio <f>  Write everything the sound board produced to this\n"
        "                      WAV file, so it can be listened to or compared\n"
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
        "      --config <p>    Read settings from this file instead of the usual\n"
        "                      places\n"
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
        } else if (std::strcmp(arg, "--no-vsync") == 0) {
            out->config.vsync = false;
            out->given.vsync  = true;
        } else if (std::strcmp(arg, "--no-throttle") == 0) {
            out->config.throttle = false;
            out->given.throttle  = true;
        } else if (std::strcmp(arg, "--fullscreen") == 0) {
            out->config.fullscreen = true;
            out->given.fullscreen  = true;
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
        } else if (takes_value("--screenshot", &out->screenshot)) {
            // handled
        } else if (takes_value("--nvram", &out->config.nvram_dir)) {
            out->given.nvram_dir = true;
        } else if (takes_value("--dump-audio", &out->dump_audio)) {
            // handled
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

    // Command line first, then the file for everything the command line did not
    // mention. Doing it this way round rather than loading first and overwriting
    // means a flag is always the last word, whatever the file says.
    Config defaults;
#if defined(SM2_ENABLE_VALIDATION)
    // A build with validation compiled in enables it by default, which is still a
    // default: a file that turns it off is obeyed.
    defaults.validation = true;
#endif

    const std::string config_path =
        options.config_path.empty() ? default_config_path() : options.config_path;

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

    if (options.list_gpus) {
        const std::vector<std::string> names = render::vk::Context::enumerate_device_names();
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
    }

    if (options.list_gamepads) {
        if (!SDL_Init(0)) {
            SM2_ERROR("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }
        osd::Input input;
        const bool started = input.init();
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
    // hw::Model2. Only Model2A is implemented today, so this always hands
    // back a concrete hw::Model2 on success; downcasting here keeps every
    // accessor below (cpu(), copro(), sound(), uart(), and the debug/render
    // call sites) written against the concrete class unchanged, matching
    // hw::Model2MachineBase's own documented split between the shared
    // interface and board-specific accessors.
    std::unique_ptr<hw::Model2MachineBase> machine_iface;
    hw::Model2* machine = nullptr;
    if (loaded.has_value()) {
        machine_iface = hw::create_machine(loaded->game, std::move(loaded->roms));
        if (!machine_iface) {
            return 1;
        }
        machine = dynamic_cast<hw::Model2*>(machine_iface.get());
        if (!machine) {
            SM2_ERROR("internal error: create_machine returned an unexpected "
                      "machine type for a Model 2A game");
            return 1;
        }
        machine->set_nvram_directory(options.config.nvram_dir);
        machine->set_log_unmapped(options.log_unmapped);
        machine->load_nvram();
        // init() resets before the NVRAM is in place, so reset again to let the
        // program read the settings it saved last time.
        machine->reset();
    }

    // -- headless boot test ------------------------------------------------
    // No window, no Vulkan: just run the machine and report where it got to.
    // This is the fastest way to see whether a change moved the boot forward.
    if (options.boot_test != 0) {
        if (!machine) {
            SM2_ERROR("--boot-test needs a ROM");
            return 1;
        }

        int exit_code_boot_test = 0;
        std::vector<s16> recorded;
        if (!options.dump_audio.empty()) {
            // About 767 stereo frames per video frame.
            recorded.reserve(static_cast<usize>(options.boot_test) * 800 * 2);
        }
        SM2_INFO("running %u frame(s) headless", options.boot_test);
        const u64 start = SDL_GetPerformanceCounter();
        for (u32 frame = 0; frame < options.boot_test; ++frame) {
            if (options.coin_at != 0) {
                const osd::Input::ScriptedPress press =
                    osd::Input::scripted_press(frame, options.coin_at);
                machine->inputs().in0 = static_cast<u8>(0xff & ~press.in0);
                machine->inputs().in1 = static_cast<u8>(0xff & ~press.in1);
            }
            machine->run_frame();

            // Draining every frame whether or not it is being recorded: the sound
            // board drops samples nothing collects, and leaving that to happen
            // would put gaps in a recording.
            const std::span<const s16> produced = machine->sound().pending_samples();
            if (!options.dump_audio.empty()) {
                recorded.insert(recorded.end(), produced.begin(), produced.end());
            }
            machine->sound().clear_pending_samples();

            if (machine->cpu().faulted()) {
                SM2_ERROR("the CPU faulted on frame %u", frame);
                break;
            }
        }
        const double seconds = static_cast<double>(SDL_GetPerformanceCounter() - start)
                             / static_cast<double>(SDL_GetPerformanceFrequency());

        const double emulated = static_cast<double>(machine->frames())
                              / (static_cast<double>(hw::Model2::kCpuClock)
                                 / static_cast<double>(hw::Model2::kCyclesPerFrame));

        std::printf("\n=== boot test ===\n");
        std::printf("frames run        : %llu\n",
                    (unsigned long long)machine->frames());
        std::printf("instructions      : %llu\n",
                    (unsigned long long)machine->cpu().instructions());
        std::printf("master cycles     : %llu\n",
                    (unsigned long long)machine->cycles());
        std::printf("faulted           : %s\n",
                    machine->cpu().faulted() ? machine->cpu().fault_message().c_str()
                                             : "no");
        std::printf("cpu state         : %s%s\n", machine->cpu().state_string().c_str(),
                    machine->cpu().halted() ? " HALTED" : "");
        std::printf("interrupts        : intena %03x intreq %03x\n",
                    machine->intena(), machine->intreq());

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
        const hw::Model2Sound& sound = machine->sound();
        if (sound.present()) {
            const hw::Model2Sound::Counters& snd = sound.counters();
            std::printf("sound 68000       : %s\n", sound.cpu().state_string().c_str());
            std::printf("sound cycles      : %llu\n",
                        (unsigned long long)sound.cpu().cycles());
            std::printf("sound scsp        : %llu write(s), %llu read(s)\n",
                        (unsigned long long)snd.scsp_writes,
                        (unsigned long long)snd.scsp_reads);
            std::printf("sound samples     : %llu read(s), banking %llu write(s)\n",
                        (unsigned long long)snd.sample_reads,
                        (unsigned long long)snd.snd_ctrl_writes);
            std::printf("sound unmapped    : %llu read(s), %llu write(s)\n",
                        (unsigned long long)snd.unmapped_reads,
                        (unsigned long long)snd.unmapped_writes);

            const hw::I8251::Counters& uart = machine->uart().counters();
            std::printf("sound link        : %llu byte(s) to the board, %llu back, "
                        "%llu overrun(s), %llu status reads\n",
                        (unsigned long long)uart.bytes_sent,
                        (unsigned long long)uart.bytes_received,
                        (unsigned long long)uart.overruns,
                        (unsigned long long)uart.status_reads);

            const hw::Scsp::Stats& scsp = sound.scsp().stats();
            std::printf("scsp audio        : %llu sample(s) at %u Hz, peak %d/32767, "
                        "%llu dropped\n",
                        (unsigned long long)scsp.samples, sound.sample_rate(),
                        scsp.peak_output, (unsigned long long)snd.samples_dropped);
            std::printf("scsp slots        : %llu key-on(s), %u sounding now\n",
                        (unsigned long long)scsp.slot_starts,
                        sound.scsp().active_slots());
            std::printf("scsp events       : %llu timer irq(s), %llu DMA(s), "
                        "MIDI %llu in / %llu out\n",
                        (unsigned long long)scsp.timer_interrupts,
                        (unsigned long long)scsp.dma_transfers,
                        (unsigned long long)scsp.midi_in_bytes,
                        (unsigned long long)scsp.midi_out_bytes);
        } else {
            std::printf("sound 68000       : no program ROM\n");
        }

        std::printf("wall time         : %.2f s for %.2f s emulated (%.2fx)\n",
                    seconds, emulated, seconds > 0.0 ? emulated / seconds : 0.0);
        std::printf("\n");
        machine->log_unmapped_summary();
        machine->log_burst_summary();

        if (!hw::run_copro_selftest(*machine)) {
            SM2_ERROR("the coprocessor's mathematical units are out of tolerance");
            exit_code_boot_test = 1;
        }

        hw::print_render_list_summary(*machine);
        hw::print_tilemap_summary(*machine);

        // Compose once, at the end. This exercises the real colour chain and the
        // real compositor, which is what the display uses, so a discrepancy
        // between this and the raw layer dump points at one or the other.
        machine->compose_video();
        if (!options.dump_tilemap.empty()) {
            hw::dump_tilemaps(*machine, options.dump_tilemap);
            hw::dump_composed_frame(*machine, options.dump_tilemap);
            hw::dump_render_list_wireframe(*machine, options.dump_tilemap);
        }

        if (!options.dump_audio.empty()
            && !hw::write_wav(options.dump_audio, recorded,
                              machine->sound().sample_rate())) {
            exit_code_boot_test = 1;
        }

        machine->save_nvram();
        return machine->cpu().faulted() ? 1 : exit_code_boot_test;
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

        osd::Window window;
        if (!window.create(window_config)) {
            SDL_Quit();
            return 1;
        }

        render::vk::ContextConfig context_config;
        context_config.enable_validation = options.config.validation;
        context_config.vsync             = options.config.vsync;
        context_config.preferred_device  = options.config.gpu;

        render::vk::Context context;
        if (!context.init(window, context_config)) {
            SM2_ERROR("Vulkan initialisation failed");
            SDL_Quit();
            return 1;
        }

        render::vk::TilemapPass tilemaps;
        if (!tilemaps.init(context)) {
            SM2_ERROR("could not create the 2D pipeline");
            SDL_Quit();
            return 1;
        }

        render::vk::Poly3DPass polygons;
        if (!polygons.init(context)) {
            SM2_ERROR("could not create the 3D pipeline");
            SDL_Quit();
            return 1;
        }

        // Owns the native-resolution frame the two passes above draw into, and
        // scales it to the window once at the end.
        render::vk::PresentPass present;
        if (!present.init(context)) {
            SM2_ERROR("could not create the presentation pipeline");
            SDL_Quit();
            return 1;
        }

        render::vk::FrameCapture capture;
        if (!options.screenshot.empty() && !capture.init(context)) {
            SM2_ERROR("could not set up frame capture");
            SDL_Quit();
            return 1;
        }

        // The settings overlay, drawn on top of the emulator's output.
        osd::Gui gui;
        if (!gui.init(window.handle(), context.instance(), context.physical_device(),
                      context.device(), context.graphics_family(),
                      context.graphics_queue(), context.swapchain_format(),
                      render::vk::Context::kFramesInFlight)) {
            SM2_ERROR("could not initialise the GUI overlay");
            SDL_Quit();
            return 1;
        }

        // Show the overlay when launched without a ROM so there is something to
        // interact with.
        if (!machine) {
            gui.show();
        }

        // GPU names for the settings dropdown.
        const std::vector<std::string> gpu_names =
            render::vk::Context::enumerate_device_names();

        // A machine with no gamepad is not an error, so a failure here is worth
        // reporting but not worth refusing to run over: the keyboard covers
        // everything the cabinet has.
        osd::Input input;
        if (!input.init()) {
            SM2_WARN("gamepads are unavailable; the keyboard still works");
        }

        // A machine with no audio device still has to run, so a failure here is
        // reported by Audio::init and otherwise ignored. Opened at the machine's
        // own 44100 Hz and left to SDL to resample.
        osd::Audio audio;
        if (machine) {
            static_cast<void>(audio.init(machine->sound().sample_rate()));
        }

        // With no machine there is nothing to draw, so present the two empty
        // surfaces over a recognisable background rather than a blank window.
        hw::Model2Video idle_video;

        if (machine) {
            window.set_title(std::string("sm2-emu — ") + loaded->game.title);
        }

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

        SM2_INFO("entering main loop; Escape quits, P pauses, Tab fast-forwards");

        /// Everything the sound board produced, when --dump-audio was given.
        std::vector<s16> recorded_audio;

        u32  frames_presented = 0;   ///< Since the loop started.
        u32  frames_written_off = 0; ///< Whole frames given up after a stall.
        bool paused             = false;
        bool fast_forward       = false;
        bool running            = true;
        u64  last_title_ns      = SDL_GetTicksNS();

        while (running) {
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
                        } else if (!gui.visible()) {
                            // Only process game keys when the overlay is hidden.
                            if (event.key.key == SDLK_P && !event.key.repeat) {
                                paused = !paused;
                                audio.set_paused(paused);
                                pacer.resync();
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

            if (machine && !paused) {
                // Inputs are levels, sampled whenever the program polls the I/O
                // controller during the frame, so they have to be set before the
                // frame runs rather than after.
                input.poll(&machine->inputs(), loaded->game.inputs);
                if (options.coin_at != 0) {
                    // Scripted coin, start and character confirmation, so an
                    // unattended capture can reach the game itself rather than
                    // only the attract mode.
                    const osd::Input::ScriptedPress press =
                        osd::Input::scripted_press(frames_presented, options.coin_at);
                    machine->inputs().in0 &= static_cast<u8>(~press.in0);
                    machine->inputs().in1 &= static_cast<u8>(~press.in1);
                }

                machine->run_frame();

                // The sound board produced about 767 stereo frames while that ran.
                // Handed over every frame rather than buffered here, so the only
                // buffering is SDL's.
                const std::span<const s16> produced = machine->sound().pending_samples();
                audio.submit(produced);
                if (!options.dump_audio.empty()) {
                    recorded_audio.insert(recorded_audio.end(), produced.begin(),
                                          produced.end());
                }
                machine->sound().clear_pending_samples();

                if (machine->cpu().faulted()) {
                    SM2_ERROR("stopping: %s", machine->cpu().fault_message().c_str());
                    exit_code = 1;
                    break;
                }
            }

            if (!context.begin_frame()) {
                // Minimised or the swapchain was rebuilt. Yield rather than
                // spinning on a window we cannot draw into, and abandon the pacing
                // deadline because an unknown amount of time is about to pass.
                SDL_Delay(16);
                pacer.resync();
                continue;
            }

            const hw::Model2Video& video = machine ? machine->video() : idle_video;
            if (machine) {
                machine->compose_video();
            }

            // Uploads and the 3D pass first. Both need a command buffer that is
            // not inside a rendering scope: a transfer cannot be issued inside
            // one, and the 3D pass opens a scope of its own on its offscreen
            // target.
            tilemaps.upload(video.below(), video.above());
            polygons.build(machine, video);
            polygons.render();

            // The hardware's three-way composite, all of it at the native 496x384:
            // tilemap layers of priority category zero, then the 3D output, then
            // category one. Nothing is magnified until present.record() below, so
            // the two blends run on the hardware's own pixels rather than on
            // colours a magnifying filter has already mixed with their neighbours.
            const VkImageView native = present.begin_frame();
            tilemaps.record_below(native, video.background());
            polygons.composite();
            tilemaps.record_above();

            // Read back the finished native frame, before it is scaled, so a
            // screenshot is the frame the hardware produced at the size it
            // produced it.
            const bool last_frame =
                options.run_frames != 0 && frames_presented + 1 >= options.run_frames;
            const bool capture_this_frame =
                !options.screenshot.empty()
                && (options.screenshot_interval != 0
                        ? (frames_presented % options.screenshot_interval) == 0 || last_frame
                        : last_frame || options.run_frames == 0);
            if (capture_this_frame
                && !capture.record(present.native_image(),
                                   render::vk::PresentPass::native_extent(),
                                   render::vk::PresentPass::native_format())) {
                SM2_ERROR("frame capture failed");
                exit_code = 1;
                break;
            }

            // Then the one magnification, into the swapchain.
            render::vk::record_image_barrier(
                context.cmd(), context.swapchain_image(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            present.record();

            // Draw the ImGui overlay on top of the presented frame, while the
            // swapchain image is still in COLOR_ATTACHMENT_OPTIMAL layout.
            gui.new_frame();
            const bool gui_active = gui.draw(options.config, gpu_names,
                                             pacer.measured_hz());
            // Always finalise the ImGui frame (Render must follow NewFrame).
            if (gui_active) {
                // Open a dynamic rendering scope on the swapchain for ImGui.
                VkRenderingAttachmentInfo colour_att{};
                colour_att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colour_att.imageView   = context.swapchain_view();
                colour_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colour_att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
                colour_att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo rendering{};
                rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent    = context.swapchain_extent();
                rendering.layerCount           = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments    = &colour_att;

                vkCmdBeginRendering(context.cmd(), &rendering);
                gui.render(context.cmd());
                vkCmdEndRendering(context.cmd());
            } else {
                gui.render(VK_NULL_HANDLE);
            }

            render::vk::record_image_barrier(
                context.cmd(), context.swapchain_image(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

            if (!context.end_frame()) {
                SM2_ERROR("frame submission failed");
                exit_code = 1;
                break;
            }

            // A series has to be written as it goes, and the readback is only
            // complete once the submission is. Waiting for the device here stalls
            // the pipeline, which is acceptable in a diagnostic mode and is why
            // this is not the default path.
            if (capture_this_frame && options.screenshot_interval != 0) {
                context.wait_idle();
                if (!capture.save(numbered_path(options.screenshot, frames_presented))) {
                    exit_code = 1;
                    break;
                }
            }

            ++frames_presented;

            if (options.run_frames != 0 && frames_presented >= options.run_frames) {
                running = false;
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
                          polygons.drawn_polygons(), polygons.triangles(),
                          polygons.blank_polygons(), frames_written_off,
                          audio.queued_milliseconds(),
                          machine ? machine->sound().scsp().active_slots() : 0u);

                std::string title = std::string("sm2-emu — ")
                                  + (loaded.has_value() ? loaded->game.title : "no game");
                if (paused) {
                    title += " — paused";
                } else {
                    char rate[32];
                    std::snprintf(rate, sizeof(rate), " — %.1f Hz", pacer.measured_hz());
                    title += rate;
                    if (!pacer.throttled()) {
                        title += " (unthrottled)";
                    }
                }
                window.set_title(title);
            }
        }

        if (frames_written_off != 0) {
            SM2_INFO("%u frame(s) were written off after falling behind",
                     frames_written_off);
        }

        if (machine) {
            machine->save_nvram();
            machine->log_unmapped_summary();
            machine->log_burst_summary();
        }

        // Nothing may be destroyed while a submitted command buffer still
        // refers to it, and up to kFramesInFlight frames are outstanding here.
        context.wait_idle();

        // After wait_idle, so the readback buffer holds a completed copy. A series
        // has already written each of its frames as it went.
        if (!options.screenshot.empty() && options.screenshot_interval == 0
            && exit_code == 0 && !capture.save(options.screenshot)) {
            exit_code = 1;
        }

        if (!options.dump_audio.empty() && machine
            && !hw::write_wav(options.dump_audio, recorded_audio,
                              machine->sound().sample_rate())) {
            exit_code = 1;
        }

        audio.shutdown();
        input.shutdown();
        gui.shutdown();
        capture.shutdown();
        present.shutdown();
        polygons.shutdown();
        tilemaps.shutdown();
        context.shutdown();
    }

    SDL_Quit();
    log::close_log_file();
    return exit_code;
}
