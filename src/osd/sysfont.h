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

#include <filesystem>
#include <string>
#include <vector>

namespace sm2::osd {

/// One face in a font file on disk.
struct FontFile {
    std::filesystem::path path;
    int                   index = 0;  ///< face within a .ttc; -1 = find by `postscript_name`
    std::string           postscript_name;
};

/// The fonts the OS would draw UI text in language `tag` (BCP 47, e.g. "ja",
/// "pt-BR") with, best first: the platform's UI face for the language, then
/// fallbacks that between them cover as much of `needed` as the system can.
/// Fontconfig on Linux, CoreText on macOS, DirectWrite on Windows; empty
/// where none is available.
[[nodiscard]] std::vector<FontFile> find_system_fonts(const std::string& tag,
                                                      const std::u32string& needed);

}  // namespace sm2::osd
