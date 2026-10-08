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

#include <windows.h>
#include <dwrite_2.h>

#include <algorithm>

namespace sm2::osd {

namespace {

template <typename T>
struct Com {
    T* ptr = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com()
    {
        if (ptr != nullptr) ptr->Release();
    }
    T** put() { return &ptr; }
    T*  operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

/// The text DirectWrite's fallback is asked to map, tagged with the language.
class TextSource final : public IDWriteTextAnalysisSource {
public:
    TextSource(const std::wstring& text, const std::wstring& locale)
        : m_text(text), m_locale(locale)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSource)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Lives on the stack for one lookup, so no reference counting.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR** text,
                                                UINT32* length) override
    {
        const auto size = static_cast<UINT32>(m_text.size());
        *text           = pos < size ? m_text.c_str() + pos : nullptr;
        *length         = pos < size ? size - pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR** text,
                                                    UINT32* length) override
    {
        const auto size = static_cast<UINT32>(m_text.size());
        *text           = pos > 0 && pos <= size ? m_text.c_str() : nullptr;
        *length         = pos > 0 && pos <= size ? pos : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* length,
                                            const WCHAR** locale) override
    {
        const auto size = static_cast<UINT32>(m_text.size());
        *length         = pos < size ? size - pos : 0;
        *locale         = m_locale.c_str();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* length,
                                                    IDWriteNumberSubstitution** sub) override
    {
        const auto size = static_cast<UINT32>(m_text.size());
        *length         = pos < size ? size - pos : 0;
        *sub            = nullptr;
        return S_OK;
    }

private:
    const std::wstring& m_text;
    const std::wstring& m_locale;
};

std::wstring widen(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    out.resize(wcslen(out.c_str()));
    return out;
}

bool file_of(IDWriteFont* font, FontFile* out)
{
    Com<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(face.put()))) {
        return false;
    }
    UINT32              count = 1;
    Com<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, file.put())) || !file) {
        return false;
    }
    const void* key      = nullptr;
    UINT32      key_size = 0;
    Com<IDWriteFontFileLoader> loader;
    Com<IDWriteLocalFontFileLoader> local;
    if (FAILED(file->GetReferenceKey(&key, &key_size)) || FAILED(file->GetLoader(loader.put()))
        || FAILED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                         reinterpret_cast<void**>(local.put())))) {
        return false;  // not a file on disk
    }
    UINT32 length = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, key_size, &length))) {
        return false;
    }
    std::wstring path(length + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, key_size, path.data(), length + 1))) {
        return false;
    }
    path.resize(length);
    *out = FontFile{std::filesystem::path(path), static_cast<int>(face->GetIndex()), {}};
    return true;
}

}  // namespace

std::vector<FontFile> find_system_fonts(const std::string& tag, const std::u32string& needed)
{
    std::vector<FontFile> out;
    Com<IDWriteFactory2>  factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                   reinterpret_cast<IUnknown**>(factory.put())))) {
        return out;
    }
    Com<IDWriteFontCollection> collection;
    Com<IDWriteFontFallback>   fallback;
    if (FAILED(factory->GetSystemFontCollection(collection.put()))
        || FAILED(factory->GetSystemFontFallback(fallback.put()))) {
        return out;
    }

    const auto add = [&](IDWriteFont* font) {
        FontFile file;
        if (font != nullptr && file_of(font, &file)
            && std::none_of(out.begin(), out.end(), [&](const FontFile& f) {
                   return f.path == file.path && f.index == file.index;
               })) {
            out.push_back(std::move(file));
        }
    };

    // Segoe UI is the Windows UI face.
    UINT32 family_index = 0;
    BOOL   exists       = FALSE;
    if (SUCCEEDED(collection->FindFamilyName(L"Segoe UI", &family_index, &exists)) && exists) {
        Com<IDWriteFontFamily> family;
        Com<IDWriteFont>       font;
        if (SUCCEEDED(collection->GetFontFamily(family_index, family.put()))
            && SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL, font.put()))) {
            add(font.ptr);
        }
    }

    // Then whatever the system falls back to for the characters it lacks,
    // chosen for this language (which settles Chinese against Japanese glyphs).
    std::wstring text;
    for (const char32_t c : needed) {
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            text += static_cast<wchar_t>(0xD800 + (v >> 10));
            text += static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        } else {
            text += static_cast<wchar_t>(c);
        }
    }
    const std::wstring locale = widen(tag);
    TextSource         source(text, locale);
    const auto         length = static_cast<UINT32>(text.size());
    for (UINT32 pos = 0; pos < length && out.size() < 8;) {
        UINT32           mapped = 0;
        FLOAT            scale  = 1.0f;
        Com<IDWriteFont> font;
        if (FAILED(fallback->MapCharacters(&source, pos, length - pos, collection.ptr, L"Segoe UI",
                                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, &mapped, font.put(),
                                           &scale))
            || mapped == 0) {
            break;
        }
        add(font.ptr);
        pos += mapped;
    }
    return out;
}

}  // namespace sm2::osd
