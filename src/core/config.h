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
#pragma once

#include "core/log.h"
#include "core/types.h"

#include <string>
#include <vector>

namespace sm2 {

/// Settings worth keeping between runs.
///
/// Deliberately only the persistent ones. Anything that describes a single run,
/// such as how many frames to capture, stays on the command line: writing it to a
/// file would mean the emulator behaved differently tomorrow for no visible reason.
///
/// Every field's default is the value here, so a missing or empty file behaves
/// exactly as no file at all.
struct Config {
    // -- presentation ------------------------------------------------------

    /// Wait for the display's vertical blank before presenting. Independent of
    /// pacing: this decides whether a frame can tear, not how fast the machine
    /// runs.
    bool vsync = true;

    /// Hold the machine to its own 57.5245 Hz. Turning this off runs as fast as the
    /// host manages, which is what a capture wants and nothing else does.
    bool throttle = true;

    bool fullscreen = false;
    u32  window_width  = 992;
    u32  window_height = 768;

    /// Exact device name to prefer, as `--list-gpus` prints it. Empty picks the
    /// best-scoring device.
    std::string gpu;

    // -- paths -------------------------------------------------------------

    std::string nvram_dir = "nvram";

    /// ROM database to use instead of searching the usual places.
    std::string games_xml;

    // -- diagnostics -------------------------------------------------------

    bool validation = false;

    /// One of trace, debug, info, warning, error.
    std::string log_level = "info";
};

/// Where the configuration is read from and written to when nothing says otherwise.
///
/// A file named sm2-emu.ini in the working directory wins, which is what a build
/// tree wants; otherwise the platform's preferences directory, which is what an
/// installed copy wants. Returns the working-directory path only when that file
/// exists, so a fresh install writes to the proper place.
[[nodiscard]] std::string default_config_path();

/// Read `path` into `out`, leaving fields the file does not mention alone.
///
/// A line the parser does not understand is appended to `problems` and skipped
/// rather than failing the load: a file written by a later version must not stop an
/// earlier one from starting. Returns false only when the file exists but cannot be
/// read; a missing file is success with nothing changed.
[[nodiscard]] bool load_config(const std::string&        path,
                               Config*                   out,
                               std::vector<std::string>* problems);

/// Write `config` to `path`, creating the directory if need be.
///
/// Every field is written with a comment, so the file doubles as the documentation
/// for what can be set.
[[nodiscard]] bool save_config(const std::string& path, const Config& config);

/// Parse a log level name. Returns false on an unrecognised name, leaving `out`
/// alone.
[[nodiscard]] bool parse_log_level(const std::string& name, log::Level* out_level);

}  // namespace sm2
