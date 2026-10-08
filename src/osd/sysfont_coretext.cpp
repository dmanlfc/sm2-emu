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

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace sm2::osd {

namespace {

std::string to_string(CFStringRef text)
{
    if (text == nullptr) {
        return {};
    }
    const CFIndex size =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(size), '\0');
    if (!CFStringGetCString(text, out.data(), size, kCFStringEncodingUTF8)) {
        return {};
    }
    out.resize(std::strlen(out.c_str()));
    return out;
}

bool covers(CTFontRef font, char32_t c)
{
    UniChar units[2];
    CGGlyph glyphs[2];
    CFIndex count = 1;
    if (c >= 0x10000) {
        const char32_t v = c - 0x10000;
        units[0]         = static_cast<UniChar>(0xD800 + (v >> 10));
        units[1]         = static_cast<UniChar>(0xDC00 + (v & 0x3FF));
        count            = 2;
    } else {
        units[0] = static_cast<UniChar>(c);
    }
    return CTFontGetGlyphsForCharacters(font, units, glyphs, count);
}

/// The file behind a font. CoreText does not say which face of a collection it
/// is, so the PostScript name is kept to find it.
bool file_of(CTFontRef font, FontFile* out)
{
    const auto url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
    if (url == nullptr) {
        return false;
    }
    char       path[PATH_MAX];
    const bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path),
                                                     sizeof path);
    CFRelease(url);
    if (!ok) {
        return false;
    }
    CFStringRef name = CTFontCopyPostScriptName(font);
    *out             = FontFile{path, -1, to_string(name)};
    if (name != nullptr) {
        CFRelease(name);
    }
    return true;
}

}  // namespace

std::vector<FontFile> find_system_fonts(const std::string& tag, const std::u32string& needed)
{
    std::vector<FontFile> out;
    CFStringRef language =
        CFStringCreateWithCString(kCFAllocatorDefault, tag.c_str(), kCFStringEncodingUTF8);
    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13.0, language);
    if (ui == nullptr) {
        CFRelease(language);
        return out;
    }

    std::u32string missing = needed;
    const auto     consider = [&](CTFontRef font) {
        const auto covered = [&](char32_t c) { return covers(font, c); };
        if (!out.empty() && std::none_of(missing.begin(), missing.end(), covered)) {
            return;
        }
        FontFile file;
        if (!file_of(font, &file)) {
            return;
        }
        missing.erase(std::remove_if(missing.begin(), missing.end(), covered), missing.end());
        out.push_back(std::move(file));
    };

    // The UI font first, then the system's fallback list for this language.
    consider(ui);
    const void* languages_raw[] = {language};
    CFArrayRef  languages = CFArrayCreate(kCFAllocatorDefault, languages_raw, 1, &kCFTypeArrayCallBacks);
    CFArrayRef  cascade   = CTFontCopyDefaultCascadeListForLanguages(ui, languages);
    if (cascade != nullptr) {
        const CFIndex count = CFArrayGetCount(cascade);
        for (CFIndex i = 0; i < count && !missing.empty() && out.size() < 8; ++i) {
            const auto descriptor =
                static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(cascade, i));
            CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, 13.0, nullptr);
            if (font != nullptr) {
                consider(font);
                CFRelease(font);
            }
        }
        CFRelease(cascade);
    }
    CFRelease(languages);
    CFRelease(ui);
    CFRelease(language);
    return out;
}

}  // namespace sm2::osd
