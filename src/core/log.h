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
#pragma once

#include <atomic>
#include <cstdio>
#include <string_view>

// Minimal levelled logging. printf-style, so "%08x" stays natural, the compiler
// checks arguments and there is no dependency on <format>.

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

namespace detail {
// Exposed so level() inlines. Relaxed is enough: set_level() only runs at startup.
extern std::atomic<Level> g_level;
}  // namespace detail

[[nodiscard]] inline Level level()
{
    return detail::g_level.load(std::memory_order_relaxed);
}

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

// The level check precedes argument evaluation, so disabled trace logging in a
// memory handler costs one load and one comparison.
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
