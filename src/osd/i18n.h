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
#pragma once

#include <string>
#include <string_view>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define SM2_TR_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define SM2_TR_FORMAT(fmt, args)
#endif

/// Marks a string for extraction without translating it, for tables that are
/// translated where they are drawn (with tr()).
#define N_(text) text

/// Overlay translations, read from gettext .po catalogs.
///
/// English is the source language and has no catalog. Every lookup returns the
/// string in display form: lines in scripts that ImGui cannot draw as-is
/// (Arabic, Hebrew, the Indic family, ...) have already been through the shaper
/// installed with set_shaper(). Returned pointers stay valid until the language
/// changes.
namespace sm2::i18n {

struct Language {
    std::string code;          ///< catalog name: "de", "pt_BR", "zh_TW"
    std::string name;          ///< the language's own name for itself
    std::string english_name;  ///< its English name
};

/// Find the catalogs: one <code>.po per language in a lang/ directory beside
/// games.xml (the source tree's data/, ../share/sm2-emu, the app bundle's
/// Resources, or the executable's own directory).
void init();

/// The catalogs found by init(), sorted by English name.
[[nodiscard]] const std::vector<Language>& available();

/// Switch to `code`: a catalog name, "en", or "auto" for the first of the
/// system's preferred languages that has a catalog. Returns the code in effect,
/// "en" when nothing matched.
std::string set_language(const std::string& code);

/// The language in effect, "en" when untranslated.
[[nodiscard]] const std::string& current();

/// The language in effect as a BCP 47 tag ("pt-BR"), for font and shaping lookups.
[[nodiscard]] std::string current_tag();

/// Whether the language in effect is written right to left.
[[nodiscard]] bool right_to_left();

/// Every character the current catalog uses, for choosing fonts.
[[nodiscard]] const std::u32string& catalog_characters();

/// Whether any string in the current catalog needs the shaper.
[[nodiscard]] bool catalog_needs_shaping();

/// Turns logical-order text into display form. Set by the UI once it has fonts.
using Shaper = std::string (*)(std::string_view text);
void set_shaper(Shaper shaper);

/// Whether a line contains anything the shaper has to handle.
[[nodiscard]] bool needs_shaping(std::u32string_view text);

/// UTF-8 to code points; malformed bytes become U+FFFD.
[[nodiscard]] std::u32string to_utf32(std::string_view text);

void append_utf8(std::string& out, char32_t c);

/// `msgid` translated, in display form.
[[nodiscard]] const char* tr(const char* msgid);

/// tr(msgid) with "###msgid" appended: a widget label whose ImGui ID is the
/// English text, so it neither changes nor collides between languages.
[[nodiscard]] const char* tr_id(const char* msgid);

/// `msgid` translated, in logical order (not shaped), for laying out by hand.
[[nodiscard]] const char* translate(const char* msgid);

/// Display form of text that is already in the target language.
[[nodiscard]] std::string shape(std::string_view text);

/// Translate `fmt`, format it like printf, and return the display form.
[[nodiscard]] std::string trf(const char* fmt, ...) SM2_TR_FORMAT(1, 2);

/// As trf(), choosing the singular or a plural form for `n` by the catalog's
/// plural rule. The arguments must match `plural`.
[[nodiscard]] std::string trnf(const char* singular, const char* plural, unsigned long n, ...)
    SM2_TR_FORMAT(2, 4);

}  // namespace sm2::i18n
