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
#include "core/log.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace sm2::log {
namespace {

Level       g_level = Level::Info;
std::FILE*  g_file  = nullptr;
std::mutex  g_mutex;

const char* level_tag(Level level)
{
    switch (level) {
        case Level::Trace:   return "trace";
        case Level::Debug:   return "debug";
        case Level::Info:    return "info ";
        case Level::Warning: return "WARN ";
        case Level::Error:   return "ERROR";
    }
    return "?????";
}

}  // namespace

void set_level(Level level)
{
    std::lock_guard lock(g_mutex);
    g_level = level;
}

Level level()
{
    // Read without the lock: a torn read of an enum is not possible on any
    // platform we target, and taking a mutex on the trace fast path would
    // dominate the cost of the logging it is meant to elide.
    return g_level;
}

bool set_log_file(std::string_view path)
{
    std::lock_guard lock(g_mutex);

    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path.empty()) {
        return true;
    }

    const std::string owned(path);
    g_file = std::fopen(owned.c_str(), "w");
    if (g_file == nullptr) {
        std::fprintf(stderr, "[ERROR] could not open log file '%s': %s\n",
                     owned.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

void close_log_file()
{
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void write(Level level, const char* fmt, ...)
{
    // Format once into a stack buffer, then fan out. Truncation is preferable
    // to allocating on a path that may be called from a memory handler.
    char    buffer[2048];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written < 0) {
        std::snprintf(buffer, sizeof(buffer), "<malformed log message>");
    }

    std::lock_guard lock(g_mutex);

    std::fprintf(stderr, "[%s] %s\n", level_tag(level), buffer);
    if (g_file != nullptr) {
        std::fprintf(g_file, "[%s] %s\n", level_tag(level), buffer);
        if (level >= Level::Warning) {
            std::fflush(g_file);
        }
    }
}

}  // namespace sm2::log
