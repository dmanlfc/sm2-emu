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
#include "osd/sysfont.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>

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

/// Whether the font's first face has outlines stb_truetype draws (glyf or CFF);
/// PingFang's are hvgl, which it cannot.
bool has_outlines(const FontFile& file)
{
    std::ifstream in(file.path, std::ios::binary);
    const auto    be32 = [&](std::streamoff at) {
        unsigned char b[4] = {};
        in.seekg(at);
        in.read(reinterpret_cast<char*>(b), 4);
        return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
    };
    std::streamoff face = 0;
    if (be32(0) == 0x74746366) {  // 'ttcf'
        face = be32(12);
    }
    const std::uint32_t tables = be32(face + 4) >> 16;
    for (std::uint32_t i = 0; i < tables && in; ++i) {
        const std::uint32_t tag = be32(face + 12 + 16 * static_cast<std::streamoff>(i));
        if (tag == 0x676C7966 || tag == 0x43464620) {  // 'glyf', 'CFF '
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<FontFile> find_system_fonts(const std::string& tag, const std::u32string& needed)
{
    std::vector<FontFile> out;
    // CoreText knows Chinese by script, not region.
    std::string script = tag;
    if (tag == "zh-CN" || tag == "zh-SG") {
        script = "zh-Hans";
    } else if (tag == "zh-TW" || tag == "zh-HK" || tag == "zh-MO") {
        script = "zh-Hant";
    }
    CFStringRef language =
        CFStringCreateWithCString(kCFAllocatorDefault, script.c_str(), kCFStringEncodingUTF8);
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
        if (!file_of(font, &file) || !has_outlines(file)) {
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

    // The cascade's CJK entries can be virtual fonts with no file behind them:
    // ask for the real font that draws each character still missing.
    std::u32string pending = missing;
    for (int tries = 0; tries < 16 && !pending.empty() && !missing.empty() && out.size() < 8; ++tries) {
        const char32_t c        = pending.front();
        const bool     pair     = c >= 0x10000;
        const UniChar  units[2] = {static_cast<UniChar>(pair ? 0xD800 + ((c - 0x10000) >> 10) : c),
                                   static_cast<UniChar>(pair ? 0xDC00 + ((c - 0x10000) & 0x3FF) : 0)};
        CFStringRef    text     = CFStringCreateWithCharacters(kCFAllocatorDefault, units, pair ? 2 : 1);
        CTFontRef      font     = CTFontCreateForStringWithLanguage(ui, text, CFRangeMake(0, CFStringGetLength(text)),
                                                                    language);
        if (font != nullptr) {
            consider(font);
            CFRelease(font);
        }
        CFRelease(text);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](char32_t p) { return p == c || missing.find(p) == std::u32string::npos; }),
                      pending.end());
    }

    // That font may be one stb_truetype cannot draw (PingFang): try every font
    // made for the language.
    if (!missing.empty() && out.size() < 8) {
        const void*     keys[]   = {kCTFontLanguagesAttribute};
        const void*     values[] = {languages};
        CFDictionaryRef attrs    = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef wanted  = CTFontDescriptorCreateWithAttributes(attrs);
        CFArrayRef          matches = CTFontDescriptorCreateMatchingFontDescriptors(wanted, nullptr);
        if (matches != nullptr) {
            const CFIndex count = CFArrayGetCount(matches);
            for (CFIndex i = 0; i < count && !missing.empty() && out.size() < 8; ++i) {
                CTFontRef font = CTFontCreateWithFontDescriptor(
                    static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(matches, i)), 13.0, nullptr);
                if (font != nullptr) {
                    consider(font);
                    CFRelease(font);
                }
            }
            CFRelease(matches);
        }
        CFRelease(wanted);
        CFRelease(attrs);
    }
    CFRelease(languages);
    CFRelease(ui);
    CFRelease(language);
    return out;
}

}  // namespace sm2::osd
