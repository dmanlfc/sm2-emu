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

#include <cstdio>
#include <string_view>

// Minimal levelled logging.
//
// printf-style rather than std::format: the hardware layer logs hex addresses
// and register values constantly, where "%08x" is the natural spelling, and
// the compiler can check the arguments. It also avoids depending on <format>,
// which is still uneven across the GCC and libc++ versions we care about.

namespace sm2::log {

enum class Level {
    Trace,    ///< Per-access memory tracing. Very loud.
    Debug,    ///< Development detail.
    Info,     ///< Normal progress: ROM loaded, device selected.
    Warning,  ///< Recoverable, but the user should know.
    Error,    ///< Operation failed.
};

/// Messages below this level are discarded. Defaults to Info.
void set_level(Level level);

[[nodiscard]] Level level();

/// Mirror all output to `path` in addition to stderr. Empty disables.
bool set_log_file(std::string_view path);

void close_log_file();

/// Emit one line. Prefer the macros below, which skip the call when filtered.
void write(Level level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace sm2::log

// The level check happens before argument evaluation so that disabled trace
// logging in a memory handler costs one comparison.
#define SM2_LOG(lvl, ...)                                        \
    do {                                                         \
        if (::sm2::log::Level::lvl >= ::sm2::log::level()) {      \
            ::sm2::log::write(::sm2::log::Level::lvl, __VA_ARGS__); \
        }                                                        \
    } while (0)

#define SM2_TRACE(...) SM2_LOG(Trace, __VA_ARGS__)
#define SM2_DEBUG(...) SM2_LOG(Debug, __VA_ARGS__)
#define SM2_INFO(...)  SM2_LOG(Info, __VA_ARGS__)
#define SM2_WARN(...)  SM2_LOG(Warning, __VA_ARGS__)
#define SM2_ERROR(...) SM2_LOG(Error, __VA_ARGS__)
