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
#include "osd/i18n.h"
#include "core/log.h"
#include "core/types.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace sm2::i18n {

namespace {

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
using StringMap = std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

// ---------------------------------------------------------------------------
// Plural rules
// ---------------------------------------------------------------------------
// The Plural-Forms header holds a C expression in n, e.g.
//   nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);
// compiled here to a small tree.

struct PluralExpr {
    enum class Op { N, Num, Not, Mul, Div, Mod, Add, Sub, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Cond };
    Op                                         op    = Op::Num;
    unsigned long                              value = 0;
    std::array<std::unique_ptr<PluralExpr>, 3> arg;

    [[nodiscard]] unsigned long eval(unsigned long n) const
    {
        const auto a = [&](int i) { return arg[static_cast<size_t>(i)]->eval(n); };
        switch (op) {
            case Op::N: return n;
            case Op::Num: return value;
            case Op::Not: return a(0) == 0 ? 1 : 0;
            case Op::Mul: return a(0) * a(1);
            case Op::Div: { const unsigned long d = a(1); return d == 0 ? 0 : a(0) / d; }
            case Op::Mod: { const unsigned long d = a(1); return d == 0 ? 0 : a(0) % d; }
            case Op::Add: return a(0) + a(1);
            case Op::Sub: return a(0) - a(1);
            case Op::Lt: return a(0) < a(1) ? 1 : 0;
            case Op::Le: return a(0) <= a(1) ? 1 : 0;
            case Op::Gt: return a(0) > a(1) ? 1 : 0;
            case Op::Ge: return a(0) >= a(1) ? 1 : 0;
            case Op::Eq: return a(0) == a(1) ? 1 : 0;
            case Op::Ne: return a(0) != a(1) ? 1 : 0;
            case Op::And: return (a(0) != 0 && a(1) != 0) ? 1 : 0;
            case Op::Or: return (a(0) != 0 || a(1) != 0) ? 1 : 0;
            case Op::Cond: return a(0) != 0 ? a(1) : a(2);
        }
        return 0;
    }
};

class PluralParser {
public:
    explicit PluralParser(std::string_view text) : m_text(text) {}

    std::unique_ptr<PluralExpr> parse()
    {
        auto expr = conditional();
        skip_space();
        return (expr && m_pos == m_text.size()) ? std::move(expr) : nullptr;
    }

private:
    using Ptr = std::unique_ptr<PluralExpr>;
    using Op  = PluralExpr::Op;

    void skip_space()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) {
            ++m_pos;
        }
    }

    bool take(std::string_view token)
    {
        skip_space();
        if (m_text.substr(m_pos, token.size()) == token) {
            m_pos += token.size();
            return true;
        }
        return false;
    }

    static Ptr make(Op op, Ptr a, Ptr b = nullptr, Ptr c = nullptr)
    {
        if (!a || (op != Op::Not && !b) || (op == Op::Cond && !c)) {
            return nullptr;
        }
        auto node    = std::make_unique<PluralExpr>();
        node->op     = op;
        node->arg[0] = std::move(a);
        node->arg[1] = std::move(b);
        node->arg[2] = std::move(c);
        return node;
    }

    Ptr conditional()
    {
        Ptr cond = logical_or();
        if (!take("?")) {
            return cond;
        }
        Ptr yes = conditional();
        if (!take(":")) {
            return nullptr;
        }
        Ptr no = conditional();
        return make(Op::Cond, std::move(cond), std::move(yes), std::move(no));
    }

    Ptr logical_or()
    {
        Ptr left = logical_and();
        while (left && take("||")) {
            left = make(Op::Or, std::move(left), logical_and());
        }
        return left;
    }

    Ptr logical_and()
    {
        Ptr left = equality();
        while (left && take("&&")) {
            left = make(Op::And, std::move(left), equality());
        }
        return left;
    }

    Ptr equality()
    {
        Ptr left = relational();
        while (left) {
            if (take("==")) left = make(Op::Eq, std::move(left), relational());
            else if (take("!=")) left = make(Op::Ne, std::move(left), relational());
            else break;
        }
        return left;
    }

    Ptr relational()
    {
        Ptr left = additive();
        while (left) {
            if (take("<=")) left = make(Op::Le, std::move(left), additive());
            else if (take(">=")) left = make(Op::Ge, std::move(left), additive());
            else if (take("<")) left = make(Op::Lt, std::move(left), additive());
            else if (take(">")) left = make(Op::Gt, std::move(left), additive());
            else break;
        }
        return left;
    }

    Ptr additive()
    {
        Ptr left = multiplicative();
        while (left) {
            if (take("+")) left = make(Op::Add, std::move(left), multiplicative());
            else if (take("-")) left = make(Op::Sub, std::move(left), multiplicative());
            else break;
        }
        return left;
    }

    Ptr multiplicative()
    {
        Ptr left = unary();
        while (left) {
            if (take("*")) left = make(Op::Mul, std::move(left), unary());
            else if (take("/")) left = make(Op::Div, std::move(left), unary());
            else if (take("%")) left = make(Op::Mod, std::move(left), unary());
            else break;
        }
        return left;
    }

    Ptr unary()
    {
        if (take("!")) {
            return make(Op::Not, unary());
        }
        if (take("(")) {
            Ptr inner = conditional();
            return take(")") ? std::move(inner) : nullptr;
        }
        skip_space();
        if (m_pos < m_text.size() && m_text[m_pos] == 'n') {
            ++m_pos;
            auto node = std::make_unique<PluralExpr>();
            node->op  = Op::N;
            return node;
        }
        if (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
            auto node = std::make_unique<PluralExpr>();
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                node->value = node->value * 10 + static_cast<unsigned long>(m_text[m_pos] - '0');
                ++m_pos;
            }
            return node;
        }
        return nullptr;
    }

    std::string_view m_text;
    size_t           m_pos = 0;
};

// ---------------------------------------------------------------------------
// .po parsing
// ---------------------------------------------------------------------------

struct PoEntry {
    std::string              context;
    std::string              id;
    std::string              id_plural;
    std::vector<std::string> str;
    bool                     fuzzy = false;
};

/// The C escapes gettext writes.
std::string unescape(std::string_view quoted)
{
    std::string out;
    out.reserve(quoted.size());
    for (size_t i = 0; i < quoted.size(); ++i) {
        const char c = quoted[i];
        if (c != '\\' || i + 1 >= quoted.size()) {
            out += c;
            continue;
        }
        switch (const char e = quoted[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'a': out += '\a'; break;
            default: out += e; break;
        }
    }
    return out;
}

/// The text between the first and last double quote on a line.
bool quoted_text(std::string_view line, std::string* out)
{
    const size_t open  = line.find('"');
    const size_t close = line.rfind('"');
    if (open == std::string_view::npos || close <= open) {
        return false;
    }
    *out = unescape(line.substr(open + 1, close - open - 1));
    return true;
}

/// Calls `visit` for each entry in order; it returns false to stop early.
void parse_po(std::istream& in, const std::function<bool(PoEntry&)>& visit)
{
    PoEntry      entry;
    bool         have   = false;
    std::string* target = nullptr;
    bool         go_on  = true;

    const auto flush = [&] {
        if (have && go_on) {
            go_on = visit(entry);
        }
        entry  = PoEntry{};
        have   = false;
        target = nullptr;
    };

    std::string raw;
    while (go_on && std::getline(in, raw)) {
        std::string_view line(raw);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.front() == '#') {
            if (line.rfind("#,", 0) == 0 && line.find("fuzzy") != std::string_view::npos) {
                if (have) flush();
                entry.fuzzy = true;
            }
            continue;
        }
        std::string text;
        if (line.front() == '"') {
            if (target != nullptr && quoted_text(line, &text)) {
                *target += text;
            }
            continue;
        }
        if (!quoted_text(line, &text)) {
            continue;
        }
        if (line.rfind("msgctxt", 0) == 0) {
            if (have) {
                const bool fuzzy = entry.fuzzy;
                flush();
                entry.fuzzy = fuzzy;
            }
            have          = true;
            entry.context = std::move(text);
            target        = &entry.context;
        } else if (line.rfind("msgid_plural", 0) == 0) {
            entry.id_plural = std::move(text);
            target          = &entry.id_plural;
        } else if (line.rfind("msgid", 0) == 0) {
            if (have && !entry.str.empty()) {
                const bool fuzzy = entry.fuzzy;
                flush();
                entry.fuzzy = fuzzy;
            }
            have     = true;
            entry.id = std::move(text);
            target   = &entry.id;
        } else if (line.rfind("msgstr[", 0) == 0) {
            const size_t index = static_cast<size_t>(std::strtoul(line.data() + 7, nullptr, 10));
            if (index < 16) {
                if (entry.str.size() <= index) {
                    entry.str.resize(index + 1);
                }
                entry.str[index] = std::move(text);
                target           = &entry.str[index];
            }
        } else if (line.rfind("msgstr", 0) == 0) {
            entry.str.assign(1, std::move(text));
            target = &entry.str[0];
        }
    }
    flush();
}

/// "Key: value" lines from a catalog's header entry.
std::string header_field(std::string_view header, std::string_view key)
{
    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find('\n', pos);
        if (end == std::string_view::npos) {
            end = header.size();
        }
        std::string_view line = header.substr(pos, end - pos);
        if (line.size() > key.size() && line.substr(0, key.size()) == key
            && line[key.size()] == ':') {
            line.remove_prefix(key.size() + 1);
            while (!line.empty() && line.front() == ' ') {
                line.remove_prefix(1);
            }
            return std::string(line);
        }
        pos = end + 1;
    }
    return {};
}

/// The printf conversions in `fmt` (length modifier + conversion), in order. A
/// translated format is only used when its conversions match the original's,
/// so a catalog cannot make printf read arguments that are not there.
std::string conversions(std::string_view fmt)
{
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        ++i;
        if (i < fmt.size() && fmt[i] == '%') {
            continue;
        }
        while (i < fmt.size() && std::strchr("-+ #0'", fmt[i]) != nullptr) ++i;
        while (i < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[i])) || fmt[i] == '*')) {
            if (fmt[i] == '*') out += '*';
            ++i;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            while (i < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[i])) || fmt[i] == '*')) {
                if (fmt[i] == '*') out += '*';
                ++i;
            }
        }
        while (i < fmt.size() && std::strchr("hlzjtL", fmt[i]) != nullptr) {
            out += fmt[i++];
        }
        if (i < fmt.size()) {
            out += fmt[i];
        }
        out += ' ';
    }
    return out;
}

bool is_rtl_language(std::string_view code)
{
    const std::string_view base = code.substr(0, code.find('_'));
    static constexpr std::array<std::string_view, 10> kRtl = {
        "ar", "fa", "he", "ur", "yi", "ps", "sd", "ug", "dv", "ckb"};
    return std::find(kRtl.begin(), kRtl.end(), base) != kRtl.end();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct State {
    std::filesystem::path dir;
    std::vector<Language> languages;

    std::string                                               code = "en";
    std::unordered_map<std::string, std::vector<std::string>> catalog;
    std::unique_ptr<PluralExpr>                               plural;
    unsigned long                                             nplurals = 2;
    std::u32string                                            characters;
    bool                                                      shaping = false;
    Shaper                                                    shaper  = nullptr;

    StringMap display;  ///< msgid -> display form
    StringMap ids;      ///< msgid -> display form + "###" + msgid
    StringMap shaped;   ///< logical text -> display form, for formatted strings
};

State& state()
{
    static State s;
    return s;
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The catalog code for a system locale, or empty.
std::string match_locale(std::string language, std::string country)
{
    language = lowercase(std::move(language));
    country  = lowercase(std::move(country));
    if (language == "no") {
        language = "nb";
    }
    if (language == "iw") {
        language = "he";
    }
    if (language == "zh") {
        country = (country == "tw" || country == "hk" || country == "mo") ? "tw" : "cn";
    }

    const auto& languages = state().languages;
    const auto  find      = [&](const std::string& code) -> std::string {
        for (const Language& l : languages) {
            if (lowercase(l.code) == code) {
                return l.code;
            }
        }
        return {};
    };
    if (!country.empty()) {
        if (std::string hit = find(language + "_" + country); !hit.empty()) return hit;
    }
    if (std::string hit = find(language); !hit.empty()) return hit;
    for (const Language& l : languages) {
        if (lowercase(l.code).rfind(language + "_", 0) == 0) {
            return l.code;
        }
    }
    return {};
}

std::string auto_language()
{
#if !defined(_WIN32) && !defined(__APPLE__)
    // gettext's order: LANGUAGE (a colon list) over the LC_ALL / LC_MESSAGES /
    // LANG locale, unless that locale is C. SDL ranks LANG first, which misses
    // a desktop that sets only LANGUAGE for its UI.
    const auto env = [](const char* name) -> std::string {
        const char* value = std::getenv(name);
        return value != nullptr ? value : "";
    };
    std::string locale = env("LC_ALL");
    if (locale.empty()) locale = env("LC_MESSAGES");
    if (locale.empty()) locale = env("LANG");
    if (!locale.empty() && locale != "C" && locale.rfind("C.", 0) != 0 && locale != "POSIX") {
        std::string list = env("LANGUAGE");
        list += (list.empty() ? "" : ":") + locale;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t end = list.find(':', pos);
            if (end == std::string::npos) end = list.size();
            std::string entry = list.substr(pos, end - pos);
            entry             = entry.substr(0, entry.find_first_of(".@"));
            pos               = end + 1;
            if (entry.empty()) continue;
            const size_t      sep      = entry.find('_');
            const std::string language = lowercase(entry.substr(0, sep));
            if (language == "en") {
                return {};
            }
            std::string found =
                match_locale(language, sep == std::string::npos ? "" : entry.substr(sep + 1));
            if (!found.empty()) {
                return found;
            }
        }
        return {};
    }
#endif
    int          count   = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string  found;
    for (int i = 0; locales != nullptr && i < count; ++i) {
        const SDL_Locale* l = locales[i];
        if (l == nullptr || l->language == nullptr) {
            continue;
        }
        const std::string language = lowercase(l->language);
        if (language == "en") {
            break;  // English preferred over any later choice
        }
        found = match_locale(language, l->country != nullptr ? l->country : "");
        if (!found.empty()) {
            break;
        }
    }
    SDL_free(locales);
    return found;
}

void clear_caches()
{
    State& s = state();
    s.display.clear();
    s.ids.clear();
    s.shaped.clear();
}

bool load_catalog(const std::string& code)
{
    State&        s = state();
    std::ifstream in(s.dir / (code + ".po"), std::ios::binary);
    if (!in) {
        SM2_WARN("i18n: cannot open %s.po", code.c_str());
        return false;
    }

    std::unordered_map<std::string, std::vector<std::string>> catalog;
    std::unique_ptr<PluralExpr>                               plural;
    unsigned long                                             nplurals = 2;
    parse_po(in, [&](PoEntry& e) {
        if (e.id.empty()) {
            // Header: the plural rule.
            const std::string rule = header_field(e.str.empty() ? "" : e.str[0], "Plural-Forms");
            const size_t      np   = rule.find("nplurals=");
            const size_t      p    = rule.find("plural=");
            if (np != std::string::npos && p != std::string::npos) {
                nplurals = std::strtoul(rule.c_str() + np + 9, nullptr, 10);
                std::string expr = rule.substr(p + 7);
                if (const size_t semi = expr.rfind(';'); semi != std::string::npos) {
                    expr.resize(semi);
                }
                plural = PluralParser(expr).parse();
                if (!plural) {
                    SM2_WARN("i18n: %s.po: cannot parse plural rule '%s'", code.c_str(),
                             expr.c_str());
                }
            }
            return true;
        }
        if (e.fuzzy || e.str.empty() || e.str[0].empty()) {
            return true;
        }
        std::string key = e.context.empty() ? e.id : e.context + '\x04' + e.id;
        catalog.emplace(std::move(key), std::move(e.str));
        return true;
    });

    s.catalog  = std::move(catalog);
    s.plural   = std::move(plural);
    s.nplurals = std::max(1ul, nplurals);
    s.code     = code;

    std::u32string chars;
    s.shaping = false;
    for (const auto& [id, forms] : s.catalog) {
        for (const std::string& form : forms) {
            const std::u32string text = to_utf32(form);
            chars += text;
            s.shaping = s.shaping || needs_shaping(text);
        }
    }
    std::sort(chars.begin(), chars.end());
    chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
    s.characters = std::move(chars);
    SM2_INFO("i18n: language %s, %zu strings", code.c_str(), s.catalog.size());
    return true;
}

const std::vector<std::string>* lookup(const char* msgid)
{
    const State& s = state();
    if (s.catalog.empty()) {
        return nullptr;
    }
    const auto it = s.catalog.find(msgid);
    return it == s.catalog.end() ? nullptr : &it->second;
}

std::string vformat(const char* fmt, va_list args)
{
    std::string out(256, '\0');
    va_list     copy;
    va_copy(copy, args);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    const int n = std::vsnprintf(out.data(), out.size(), fmt, copy);
    va_end(copy);
    if (n < 0) {
        return {};
    }
    if (static_cast<size_t>(n) >= out.size()) {
        out.assign(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(out.data(), out.size(), fmt, args);
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    out.resize(static_cast<size_t>(n));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::u32string to_utf32(std::string_view text)
{
    std::u32string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto c    = static_cast<unsigned char>(text[i]);
        int        more = 0;
        char32_t   cp   = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f; more = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f; more = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07; more = 3;
        } else {
            out += U'\uFFFD';
            ++i;
            continue;
        }
        ++i;
        bool ok = true;
        for (int k = 0; k < more; ++k, ++i) {
            if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        }
        out += ok ? cp : U'\uFFFD';
    }
    return out;
}

void append_utf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

bool needs_shaping(std::u32string_view text)
{
    // Right-to-left scripts and those whose glyphs join, reorder or stack.
    static constexpr std::array<std::pair<char32_t, char32_t>, 14> kRanges = {{
        {0x0590, 0x08FF},    // Hebrew, Arabic, Syriac, Thaana, NKo, ...
        {0x0900, 0x0DFF},    // Devanagari ... Sinhala
        {0x0E00, 0x0FFF},    // Thai, Lao, Tibetan
        {0x1000, 0x109F},    // Myanmar
        {0x1700, 0x1AAF},    // Philippine scripts, Khmer, Mongolian, Limbu, Tai, Buginese
        {0x1B00, 0x1C4F},    // Balinese, Sundanese, Batak, Lepcha
        {0xA800, 0xA82F},    // Syloti Nagri
        {0xA840, 0xA8FF},    // Phags-pa, Saurashtra, Devanagari Extended
        {0xA900, 0xAA7F},    // Kayah Li, Rejang, Javanese, Cham, Myanmar Extended
        {0xABC0, 0xABFF},    // Meetei Mayek
        {0xFB1D, 0xFDFF},    // Hebrew and Arabic presentation forms
        {0xFE70, 0xFEFF},    // Arabic presentation forms B
        {0x10800, 0x11FFF},  // historic right-to-left and Brahmic scripts
        {0x1E800, 0x1EFFF},  // Adlam and other right-to-left scripts
    }};
    for (const char32_t c : text) {
        for (const auto& [lo, hi] : kRanges) {
            if (c >= lo && c <= hi) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

void init()
{
    State& s = state();
    s.languages.clear();
    s.dir.clear();

    std::vector<std::filesystem::path> candidates = {"lang", "data/lang"};
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const std::filesystem::path exe_dir(base);
        candidates.push_back(exe_dir / "lang");
        candidates.push_back(exe_dir / ".." / "share" / "sm2-emu" / "lang");
        candidates.push_back(exe_dir / ".." / ".." / "data" / "lang");
        candidates.push_back(exe_dir / ".." / "Resources" / "lang");
    }
    std::error_code ec;
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::is_directory(candidate, ec)) {
            s.dir = candidate;
            break;
        }
    }
    if (s.dir.empty()) {
        SM2_WARN("i18n: no lang directory found; the overlay stays in English");
        return;
    }
    const std::filesystem::path& dir = s.dir;

    for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
        if (file.path().extension() != ".po") {
            continue;
        }
        std::ifstream in(file.path(), std::ios::binary);
        Language      language;
        language.code = file.path().stem().string();
        parse_po(in, [&](PoEntry& e) {
            if (e.id.empty() && !e.str.empty()) {
                language.name         = header_field(e.str[0], "X-Language-Name");
                language.english_name = header_field(e.str[0], "X-Language-Name-English");
            }
            return false;  // the header is the first entry
        });
        if (language.english_name.empty()) {
            language.english_name = language.code;
        }
        if (language.name.empty()) {
            language.name = language.english_name;
        }
        s.languages.push_back(std::move(language));
    }
    std::sort(s.languages.begin(), s.languages.end(),
              [](const Language& a, const Language& b) { return a.english_name < b.english_name; });
    SM2_INFO("i18n: %zu languages in %s", s.languages.size(), dir.string().c_str());
}

const std::vector<Language>& available()
{
    return state().languages;
}

std::string set_language(const std::string& code)
{
    State& s = state();
    clear_caches();

    std::string want = code;
    if (want.empty() || want == "auto") {
        want = auto_language();
    }
    const bool known = std::any_of(s.languages.begin(), s.languages.end(),
                                   [&](const Language& l) { return l.code == want; });
    if (want.empty() || want == "en" || !known || !load_catalog(want)) {
        if (!want.empty() && want != "en" && !known) {
            SM2_WARN("i18n: no catalog for language '%s'", want.c_str());
        }
        s.catalog.clear();
        s.plural.reset();
        s.characters.clear();
        s.shaping = false;
        s.code    = "en";
    }
    return s.code;
}

const std::string& current()
{
    return state().code;
}

std::string current_tag()
{
    std::string tag = state().code;
    std::replace(tag.begin(), tag.end(), '_', '-');
    return tag;
}

bool right_to_left()
{
    return is_rtl_language(state().code);
}

const std::u32string& catalog_characters()
{
    return state().characters;
}

bool catalog_needs_shaping()
{
    return state().shaping;
}

void set_shaper(Shaper shaper)
{
    state().shaper = shaper;
    clear_caches();
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const char* translate(const char* msgid)
{
    const std::vector<std::string>* forms = lookup(msgid);
    return forms != nullptr ? (*forms)[0].c_str() : msgid;
}

std::string shape(std::string_view text)
{
    State& s = state();
    if (s.shaper == nullptr) {
        return std::string(text);
    }
    if (const auto it = s.shaped.find(text); it != s.shaped.end()) {
        return it->second;
    }

    std::string out;
    size_t      pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(pos, end - pos);
        out += needs_shaping(to_utf32(line)) ? s.shaper(line) : std::string(line);
        if (end < text.size()) {
            out += '\n';
        }
        pos = end + 1;
    }

    if (s.shaped.size() >= 1024) {
        s.shaped.clear();  // formatted text such as a counter keeps changing
    }
    s.shaped.emplace(std::string(text), out);
    return out;
}

const char* tr(const char* msgid)
{
    State& s = state();
    if (s.catalog.empty()) {
        return msgid;
    }
    auto it = s.display.find(std::string_view(msgid));
    if (it == s.display.end()) {
        it = s.display.emplace(msgid, shape(translate(msgid))).first;
    }
    return it->second.c_str();
}

const char* tr_id(const char* msgid)
{
    State& s  = state();
    auto   it = s.ids.find(std::string_view(msgid));
    if (it == s.ids.end()) {
        it = s.ids.emplace(msgid, std::string(tr(msgid)) + "###" + msgid).first;
    }
    return it->second.c_str();
}

std::string trf(const char* fmt, ...)
{
    const char* translated = translate(fmt);
    if (translated != fmt && conversions(translated) != conversions(fmt)) {
        translated = fmt;
    }
    va_list args;
    va_start(args, fmt);
    std::string text = vformat(translated, args);
    va_end(args);
    return state().catalog.empty() ? text : shape(text);
}

std::string trnf(const char* singular, const char* plural, unsigned long n, ...)
{
    const State&                    s     = state();
    const std::vector<std::string>* forms = lookup(singular);
    const char*                     fmt   = n == 1 ? singular : plural;
    if (forms != nullptr) {
        const unsigned long index = s.plural ? s.plural->eval(n) : (n == 1 ? 0 : 1);
        if (index < forms->size() && !(*forms)[index].empty()
            && (conversions((*forms)[index]) == conversions(plural)
                || conversions((*forms)[index]).empty())) {
            fmt = (*forms)[index].c_str();
        }
    }
    va_list args;
    va_start(args, n);
    std::string text = vformat(fmt, args);
    va_end(args);
    return s.catalog.empty() ? text : shape(text);
}

}  // namespace sm2::i18n
