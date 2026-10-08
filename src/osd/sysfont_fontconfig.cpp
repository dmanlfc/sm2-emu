//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#include "osd/sysfont.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace sm2::osd {

namespace {

/// stb_truetype reads TrueType and CFF outlines only; skip bitmap formats.
bool readable(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

/// The regional suffix CJK collections (Noto, Source Han) give each face, so
/// Japanese is not drawn with Chinese or Korean forms of the shared characters
/// when the system's fontconfig has no per-language preference.
std::string cjk_region(const std::string& tag)
{
    const std::string lang = tag.substr(0, 2);
    if (lang == "ja") return " JP";
    if (lang == "ko") return " KR";
    if (lang != "zh") return {};
    if (tag.find("TW") != std::string::npos || tag.find("Hant") != std::string::npos) return " TC";
    if (tag.find("HK") != std::string::npos || tag.find("MO") != std::string::npos) return " HK";
    return " SC";
}

}  // namespace

std::vector<FontFile> find_system_fonts(const std::string& tag, const std::u32string& needed)
{
    std::vector<FontFile> out;
    if (FcInit() == FcFalse) {
        return out;
    }

    // fontconfig's own fallback order for a sans-serif UI in this language.
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans-serif"));
    FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8*>(tag.c_str()));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult   result = FcResultNoMatch;
    FcFontSet* set    = FcFontSort(nullptr, pattern, FcFalse, nullptr, &result);
    FcPatternDestroy(pattern);
    if (set == nullptr) {
        return out;
    }

    std::u32string missing = needed;
    for (int i = 0; i < set->nfont && out.size() < 8; ++i) {
        FcPattern* font = set->fonts[i];
        FcChar8*   file = nullptr;
        int        index = 0;
        FcBool     color = FcFalse;
        FcCharSet* chars = nullptr;
        if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch
            || FcPatternGetCharSet(font, FC_CHARSET, 0, &chars) != FcResultMatch
            || !readable(reinterpret_cast<const char*>(file))) {
            continue;
        }
        FcPatternGetInteger(font, FC_INDEX, 0, &index);
        if (FcPatternGetBool(font, FC_COLOR, 0, &color) == FcResultMatch && color == FcTrue) {
            continue;  // colour emoji: bitmaps stb_truetype cannot draw
        }

        // The best match is the UI face; after it, keep a font only for what
        // the ones before it lack.
        const auto covered = [&](char32_t c) { return FcCharSetHasChar(chars, c) == FcTrue; };
        const bool helps   = std::any_of(missing.begin(), missing.end(), covered);
        if (!out.empty() && !helps) {
            continue;
        }
        missing.erase(std::remove_if(missing.begin(), missing.end(), covered), missing.end());
        if (const std::string region = cjk_region(tag); !region.empty()) {
            // Another face of the same collection named for this language.
            for (int k = 0; k < set->nfont; ++k) {
                FcChar8* other_file = nullptr;
                FcChar8* family     = nullptr;
                int      other      = 0;
                if (FcPatternGetString(set->fonts[k], FC_FILE, 0, &other_file) == FcResultMatch
                    && FcStrCmp(other_file, file) == 0
                    && FcPatternGetString(set->fonts[k], FC_FAMILY, 0, &family) == FcResultMatch
                    && FcPatternGetInteger(set->fonts[k], FC_INDEX, 0, &other) == FcResultMatch) {
                    const std::string name = reinterpret_cast<const char*>(family);
                    if (name.size() > region.size()
                        && name.compare(name.size() - region.size(), region.size(), region) == 0) {
                        index = other;
                        break;
                    }
                }
            }
        }
        out.push_back({reinterpret_cast<const char*>(file), index & 0xffff, {}});
        if (missing.empty()) {
            break;
        }
    }
    FcFontSetDestroy(set);
    return out;
}

}  // namespace sm2::osd
