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
#include "osd/ui_text.h"
#include "core/log.h"
#include "core/types.h"
#include "osd/i18n.h"
#include "osd/sysfont.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imstb_truetype.h>

#include <SheenBidi/SheenBidi.h>
#include <hb.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace sm2::osd::ui_text {

namespace {

/// ProggyClean's size; the overlay scales from it.
constexpr float kBaseSize = 13.0f;

/// System fonts are sized so their em is this share of ProggyClean's line,
/// rather than fitting their (often much taller) line height into it.
constexpr float kEmScale = 0.95f;

/// A system font loaded into the atlas. The bytes belong to the atlas.
struct Face {
    std::string          name;
    const unsigned char* data  = nullptr;
    int                  size  = 0;
    int                  index = 0;
    stbtt_fontinfo       info{};
    float                em_scale = 0.0f;  ///< font units to pixels at size 1
    hb_face_t*           hb_face  = nullptr;
    hb_font_t*           hb_font  = nullptr;
};

/// One positioned glyph from the shaper, drawn as a private-use code point.
struct ShapedGlyph {
    u32 face;
    u32 glyph;
    s32 advance;
    s32 x_offset;
    s32 y_offset;

    bool operator==(const ShapedGlyph&) const = default;
};

struct ShapedGlyphHash {
    size_t operator()(const ShapedGlyph& g) const
    {
        size_t h = g.face;
        for (const s32 v : {static_cast<s32>(g.glyph), g.advance, g.x_offset, g.y_offset}) {
            h = h * 1000003u ^ static_cast<size_t>(static_cast<u32>(v));
        }
        return h;
    }
};

// Private-use code points for shaped glyphs: plane 15 when ImGui is built with
// 32-bit ImWchar, else the BMP's private-use area.
constexpr char32_t kFirstShaped = sizeof(ImWchar) == 4 ? 0xF0000 : 0xE000;
constexpr char32_t kLastShaped  = sizeof(ImWchar) == 4 ? 0xFFFFD : 0xF8FF;

struct State {
    ImFont*                                                    font = nullptr;
    std::vector<Face>                                          faces;
    std::vector<ShapedGlyph>                                   glyphs;
    std::unordered_map<ShapedGlyph, char32_t, ShapedGlyphHash> glyph_codes;
    hb_buffer_t*                                               buffer   = nullptr;
    hb_language_t                                              language = HB_LANGUAGE_INVALID;
    std::vector<unsigned char>                                 bitmap;
};

State& state()
{
    static State s;
    return s;
}

// ---------------------------------------------------------------------------
// ImGui font source for shaped glyphs
// ---------------------------------------------------------------------------

bool shaped_contains(ImFontAtlas*, ImFontConfig*, ImWchar codepoint)
{
    const char32_t c = codepoint;
    return c >= kFirstShaped && c - kFirstShaped < state().glyphs.size();
}

bool shaped_load(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*,
                 ImWchar codepoint, ImFontGlyph* out)
{
    State& s = state();
    if (!shaped_contains(atlas, src, codepoint)) {
        return false;
    }
    const ShapedGlyph& g    = s.glyphs[static_cast<char32_t>(codepoint) - kFirstShaped];
    const Face&        face = s.faces[g.face];
    const auto         id   = static_cast<int>(g.glyph);

    const float scale   = face.em_scale * baked->Size;
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float raster  = scale * density;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&face.info, id, raster, raster, &x0, &y0, &x1, &y1);
    out->Codepoint = codepoint;
    out->AdvanceX  = static_cast<float>(g.advance) * scale;
    if (x0 == x1 || y0 == y1) {
        return true;
    }

    const int               w       = x1 - x0;
    const int               h       = y1 - y0;
    const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack_id == ImFontAtlasRectId_Invalid) {
        return false;
    }
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack_id);
    s.bitmap.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    stbtt_MakeGlyphBitmap(&face.info, s.bitmap.data(), w, h, w, raster, raster, id);

    const float off_x = static_cast<float>(g.x_offset) * scale;
    const float off_y = -static_cast<float>(g.y_offset) * scale + IM_ROUND(baked->Ascent);
    out->X0           = static_cast<float>(x0) / density + off_x;
    out->Y0           = static_cast<float>(y0) / density + off_y;
    out->X1           = static_cast<float>(x0 + r->w) / density + off_x;
    out->Y1           = static_cast<float>(y0 + r->h) / density + off_y;
    out->Visible      = true;
    out->PackId       = pack_id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, s.bitmap.data(),
                                       ImTextureFormat_Alpha8, w);
    return true;
}

const ImFontLoader* shaped_loader()
{
    static ImFontLoader loader = [] {
        ImFontLoader l;
        l.Name                 = "sm2 shaped text";
        l.FontSrcContainsGlyph = shaped_contains;
        l.FontBakedLoadGlyph   = shaped_load;
        return l;
    }();
    return &loader;
}

// ---------------------------------------------------------------------------
// Shaping
// ---------------------------------------------------------------------------

/// The private-use code point for a glyph, or 0 when they have run out.
char32_t code_for(const ShapedGlyph& g)
{
    State& s = state();
    if (const auto it = s.glyph_codes.find(g); it != s.glyph_codes.end()) {
        return it->second;
    }
    const char32_t code = kFirstShaped + static_cast<char32_t>(s.glyphs.size());
    if (code > kLastShaped) {
        return 0;
    }
    s.glyphs.push_back(g);
    s.glyph_codes.emplace(g, code);
    return code;
}

bool is_mark_or_joiner(char32_t c)
{
    if (c == 0x200C || c == 0x200D || c == 0x25CC) {
        return true;
    }
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), c)) {
        case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
            return true;
        default:
            return false;
    }
}

/// The first face that has `c`, or -1.
int face_for(char32_t c)
{
    const auto& faces = state().faces;
    for (size_t i = 0; i < faces.size(); ++i) {
        if (stbtt_FindGlyphIndex(&faces[i].info, static_cast<int>(c)) != 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Shape text[start, start + length) with one face; false if it could not be.
bool shape_item(const std::u32string& text, size_t start, size_t length, int face_index,
                bool rtl, std::string& out)
{
    State&      s    = state();
    const Face& face = s.faces[static_cast<size_t>(face_index)];
    hb_buffer_clear_contents(s.buffer);
    hb_buffer_add_codepoints(s.buffer, reinterpret_cast<const uint32_t*>(text.data()),
                             static_cast<int>(text.size()), static_cast<unsigned>(start),
                             static_cast<int>(length));
    hb_buffer_set_direction(s.buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_language(s.buffer, s.language);
    hb_buffer_guess_segment_properties(s.buffer);
    hb_shape(face.hb_font, s.buffer, nullptr, 0);

    unsigned int               count = 0;
    const hb_glyph_info_t*     info  = hb_buffer_get_glyph_infos(s.buffer, &count);
    const hb_glyph_position_t* pos   = hb_buffer_get_glyph_positions(s.buffer, &count);
    std::string                shaped;
    for (unsigned int i = 0; i < count; ++i) {
        const char32_t code = code_for({static_cast<u32>(face_index), info[i].codepoint,
                                        pos[i].x_advance, pos[i].x_offset, pos[i].y_offset});
        if (code == 0) {
            return false;
        }
        i18n::append_utf8(shaped, code);
    }
    out += shaped;
    return true;
}

/// One directional run, already in visual position, emitted left to right.
void emit_run(const std::u32string& text, size_t start, size_t length, bool rtl, std::string& out)
{
    // Split into items: stretches one face shapes, and plain characters ImGui
    // draws itself (spaces, digits, Latin, punctuation).
    struct Item {
        size_t start;
        size_t length;
        int    face;  // -1 for plain
    };
    std::vector<Item> items;
    for (size_t i = start; i < start + length; ++i) {
        const char32_t c    = text[i];
        int            face = -1;
        if (i18n::needs_shaping(std::u32string_view(&text[i], 1))) {
            face = face_for(c);
        } else if (is_mark_or_joiner(c) && !items.empty() && items.back().face >= 0) {
            face = items.back().face;  // stays with its base
        }
        if (!items.empty() && items.back().face == face) {
            ++items.back().length;
        } else {
            items.push_back({i, 1, face});
        }
    }
    if (rtl) {
        std::reverse(items.begin(), items.end());
    }

    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    for (const Item& item : items) {
        if (item.face >= 0 && shape_item(text, item.start, item.length, item.face, rtl, out)) {
            continue;
        }
        for (size_t k = 0; k < item.length; ++k) {
            if (rtl) {
                // Reversed, with brackets and the like mirrored.
                const char32_t c = text[item.start + item.length - 1 - k];
                i18n::append_utf8(out, hb_unicode_mirroring(unicode, c));
            } else {
                i18n::append_utf8(out, text[item.start + k]);
            }
        }
    }
}

/// i18n's shaper: one line of logical-order text to display form.
std::string shape_line(std::string_view line)
{
    const std::u32string text = i18n::to_utf32(line);
    if (text.empty()) {
        return {};
    }
    SBCodepointSequence sequence{SBStringEncodingUTF32, text.data(), text.size()};
    SBAlgorithmRef      algorithm = SBAlgorithmCreate(&sequence);
    SBParagraphRef      paragraph = SBAlgorithmCreateParagraph(
        algorithm, 0, text.size(), i18n::right_to_left() ? SBLevel{1} : SBLevelDefaultLTR);
    SBLineRef bidi_line = SBParagraphCreateLine(paragraph, 0, SBParagraphGetLength(paragraph));

    std::string    out;
    const SBRun*   runs  = SBLineGetRunsPtr(bidi_line);
    const SBUInteger count = SBLineGetRunCount(bidi_line);
    for (SBUInteger i = 0; i < count; ++i) {
        emit_run(text, runs[i].offset, runs[i].length, (runs[i].level & 1) != 0, out);
    }

    SBLineRelease(bidi_line);
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
    return out;
}

void release_shaper()
{
    State& s = state();
    i18n::set_shaper(nullptr);
    for (Face& face : s.faces) {
        hb_font_destroy(face.hb_font);
        hb_face_destroy(face.hb_face);
    }
    s.faces.clear();
    s.glyphs.clear();
    s.glyph_codes.clear();
    if (s.buffer != nullptr) {
        hb_buffer_destroy(s.buffer);
        s.buffer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

/// ProggyClean covers printable ASCII and Latin-1.
bool in_proggy(char32_t c)
{
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF);
}

bool is_latin_letter(char32_t c)
{
    return (c >= 0x100 && c <= 0x24F) || (c >= 0x1E00 && c <= 0x1EFF)
        || (c >= 0x2C60 && c <= 0x2C7F) || (c >= 0xA720 && c <= 0xA7FF);
}

/// Letters, marks and digits must be drawable; a missing symbol only costs a box.
bool essential(char32_t c)
{
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), c)) {
        case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER:
            return true;
        default:
            return false;
    }
}

/// The face in `data` with this PostScript name (name ID 6), or 0.
int face_index_by_name(const unsigned char* data, const std::string& name)
{
    const int count = std::max(stbtt_GetNumberOfFonts(data), 1);
    for (int i = 0; i < count; ++i) {
        stbtt_fontinfo info{};
        if (stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, i)) == 0) {
            continue;
        }
        int length = 0;
        // Windows platform, Unicode BMP, US English: UTF-16BE.
        const char* utf16 = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                                    STBTT_MS_EID_UNICODE_BMP,
                                                    STBTT_MS_LANG_ENGLISH, 6);
        std::string ascii;
        for (int k = 0; utf16 != nullptr && k + 1 < length; k += 2) {
            ascii += utf16[k + 1];
        }
        if (ascii == name) {
            return i;
        }
    }
    return 0;
}

/// Read a font file into memory owned by ImGui's allocator, ready for the atlas.
bool load_face(const FontFile& file, Face* out)
{
    std::ifstream in(file.path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > (std::streamoff{1} << 30)) {
        return false;
    }
    auto* data = static_cast<unsigned char*>(IM_ALLOC(static_cast<size_t>(size)));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(data), size)) {
        IM_FREE(data);
        return false;
    }
    const int index  = file.index >= 0 ? file.index : face_index_by_name(data, file.postscript_name);
    const int offset = stbtt_GetFontOffsetForIndex(data, index);
    if (offset < 0 || stbtt_InitFont(&out->info, data, offset) == 0 || index > 127) {
        IM_FREE(data);
        return false;
    }
    out->name     = file.path.filename().string();
    out->data     = data;
    out->size     = static_cast<int>(size);
    out->index    = index;
    out->em_scale = stbtt_ScaleForMappingEmToPixels(&out->info, 1.0f) * kEmScale;
    return true;
}

}  // namespace

bool build_font()
{
    State&    s  = state();
    ImGuiIO&  io = ImGui::GetIO();
    release_shaper();
    if (s.font != nullptr) {
        io.Fonts->RemoveFont(s.font);
        s.font = nullptr;
    }

    // What ProggyClean cannot draw. If that includes Latin letters, the system
    // font draws all the text, so a word is never split between two fonts.
    const std::u32string& chars = i18n::catalog_characters();
    std::u32string        needed;
    const bool            latin = std::any_of(chars.begin(), chars.end(), is_latin_letter);
    for (const char32_t c : chars) {
        if (c > 0x20 && (latin || !in_proggy(c))) {
            needed += c;
        }
    }

    std::vector<Face> faces;
    std::u32string    missing = needed;
    if (!needed.empty()) {
        for (const FontFile& file : find_system_fonts(i18n::current_tag(), needed)) {
            Face face;
            if (!load_face(file, &face)) {
                SM2_WARN("gui: cannot load font %s", file.path.string().c_str());
                continue;
            }
            const auto covered = [&](char32_t c) {
                return stbtt_FindGlyphIndex(&face.info, static_cast<int>(c)) != 0;
            };
            if (std::none_of(missing.begin(), missing.end(), covered)) {
                IM_FREE(const_cast<unsigned char*>(face.data));
                continue;
            }
            missing.erase(std::remove_if(missing.begin(), missing.end(), covered), missing.end());
            SM2_INFO("gui: font %s (face %d)", face.name.c_str(), face.index);
            faces.push_back(std::move(face));
        }
    }

    const usize undrawable = static_cast<usize>(std::count_if(
        missing.begin(), missing.end(), [](char32_t c) { return !in_proggy(c) && essential(c); }));
    const bool ok = undrawable == 0;
    if (!ok) {
        SM2_WARN("gui: no system font draws %zu characters of language %s", undrawable,
                 i18n::current().c_str());
        for (const Face& face : faces) {
            IM_FREE(const_cast<unsigned char*>(face.data));
        }
        faces.clear();
    }

    // ProggyClean first: it sets the size and line metrics either way.
    static const ImWchar kAllOfProggy[] = {0x20, 0xFF, 0};
    ImFontConfig         base;
    if (ok && latin) {
        base.GlyphExcludeRanges = kAllOfProggy;
    }
    s.font = io.Fonts->AddFontDefault(&base);

    // Shaped glyphs next, ahead of any system font's own private-use glyphs.
    const bool shaping = ok && i18n::catalog_needs_shaping();
    if (shaping) {
        ImFontConfig shaped;
        shaped.MergeMode  = true;
        shaped.FontLoader = shaped_loader();
        std::snprintf(shaped.Name, sizeof shaped.Name, "shaped text");
        io.Fonts->AddFont(&shaped);
    }

    for (Face& face : faces) {
        ImFontConfig cfg;
        cfg.MergeMode            = true;
        cfg.FontData             = const_cast<unsigned char*>(face.data);
        cfg.FontDataSize         = face.size;
        cfg.FontDataOwnedByAtlas = true;
        cfg.FontNo               = static_cast<ImS8>(face.index);
        // Pixel-height sizing relative to ProggyClean, set so the em comes out
        // at kEmScale of the line.
        cfg.SizePixels = kBaseSize * face.em_scale / stbtt_ScaleForPixelHeight(&face.info, 1.0f);
        std::snprintf(cfg.Name, sizeof cfg.Name, "%s", face.name.c_str());
        io.Fonts->AddFont(&cfg);
    }
    io.FontDefault = s.font;

    if (shaping) {
        for (Face& face : faces) {
            hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(face.data),
                                             static_cast<unsigned>(face.size),
                                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
            face.hb_face = hb_face_create(blob, static_cast<unsigned>(face.index));
            face.hb_font = hb_font_create(face.hb_face);
            hb_blob_destroy(blob);
        }
        s.faces    = std::move(faces);
        s.buffer   = hb_buffer_create();
        s.language = hb_language_from_string(i18n::current_tag().c_str(), -1);
        i18n::set_shaper(shape_line);
    }
    return ok;
}

void shutdown()
{
    State& s = state();
    release_shaper();
    s.font = nullptr;
}

}  // namespace sm2::osd::ui_text
