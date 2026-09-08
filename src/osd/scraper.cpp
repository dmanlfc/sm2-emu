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
#include "osd/scraper.h"

#include "core/log.h"
#include "core/types.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(SM2_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace sm2::osd {
namespace {

// A hand-rolled reader for the handful of named fields we need, not a general
// JSON parser: ArcadeDB's reply and our own cache file are both flat objects of
// string values, so this is simpler to audit than vendoring a parser.

/// Decode a JSON string body, resolving \" \\ \/ \n \r \t \b \f and \uXXXX
/// (BMP only, to UTF-8).
[[nodiscard]] std::string decode_json_string(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (usize i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        const char esc = raw[++i];
        switch (esc) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'u': {
                if (i + 4 >= raw.size()) { out.push_back('?'); break; }
                unsigned code = 0;
                bool     ok   = true;
                for (int k = 0; k < 4; ++k) {
                    const char h = raw[i + 1 + static_cast<usize>(k)];
                    code <<= 4;
                    if (h >= '0' && h <= '9')      code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else { ok = false; break; }
                }
                if (!ok) { out.push_back('?'); break; }
                i += 4;
                // BMP -> UTF-8; surrogate pairs are not handled (ArcadeDB's text
                // stays in the BMP).
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default: out.push_back(esc); break;
        }
    }
    return out;
}

/// String value of `"key"` in a flat JSON object, or empty. Escapes in the
/// value are honoured so an embedded \" does not end it early.
[[nodiscard]] std::string json_string_field(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    usize pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        usize i = pos + needle.size();
        // Require a colon, so "title" does not match inside "short_title".
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != ':') { pos += needle.size(); continue; }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != '"') { pos += needle.size(); continue; }
        ++i;
        const usize start = i;
        while (i < json.size()) {
            if (json[i] == '\\') { i += 2; continue; }
            if (json[i] == '"') break;
            ++i;
        }
        return decode_json_string(json.substr(start, i - start));
    }
    return {};
}

/// Whether `"result"` is an empty array -- ArcadeDB's "not found" reply.
[[nodiscard]] bool result_is_empty(const std::string& json)
{
    const usize r = json.find("\"result\"");
    if (r == std::string::npos) return true;
    usize i = r + 8;
    while (i < json.size() && json[i] != '[') ++i;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n'
                               || json[i] == '\r')) ++i;
    return i < json.size() && json[i] == ']';
}

[[nodiscard]] std::string encode_json_string(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

[[nodiscard]] std::string read_file_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

#if defined(SM2_HAVE_CURL)
namespace {

constexpr const char* kUserAgent =
    "sm2-emu/1.0 (Sega Model 2 emulator; game picker artwork)";
constexpr long kConnectTimeoutSeconds = 8;
constexpr long kTotalTimeoutSeconds   = 15;

size_t append_to_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

/// GET `url` into `out`; true on a 2xx with a body. Time-bounded and non-fatal.
[[nodiscard]] bool http_get(const std::string& url, std::string* out)
{
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return false;

    out->clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // thread-safe timeouts
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        SM2_DEBUG("scraper: GET %s failed: %s", url.c_str(), curl_easy_strerror(rc));
        return false;
    }
    return !out->empty();
}

}  // namespace
#endif  // SM2_HAVE_CURL

Scraper::~Scraper()
{
    stop();
}

void Scraper::start(std::string artwork_dir, std::vector<Entry> entries, bool enabled)
{
    m_artwork_dir = std::move(artwork_dir);
    m_entries     = std::move(entries);

#if defined(SM2_HAVE_CURL)
    m_scraping = enabled;
#else
    m_scraping = false;
    if (enabled) {
        SM2_INFO("scraper: built without libcurl; artwork scraping unavailable, "
                 "picker runs offline");
    }
#endif

    if (!m_scraping) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(m_artwork_dir, error);
    if (error) {
        SM2_WARN("scraper: could not create artwork dir '%s'; running offline",
                 m_artwork_dir.c_str());
        m_scraping = false;
        return;
    }

    m_stop.store(false);
    m_thread = std::thread(&Scraper::run, this);
}

void Scraper::stop()
{
    m_stop.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

std::vector<std::string> Scraper::take_ready()
{
    std::vector<std::string> ready;
    std::lock_guard<std::mutex> lock(m_ready_mutex);
    while (!m_ready.empty()) {
        ready.push_back(std::move(m_ready.front()));
        m_ready.pop();
    }
    return ready;
}

Scraper::Metadata Scraper::load_metadata(const std::string& name,
                                         const std::string& fallback_title) const
{
    Metadata meta;
    meta.title = fallback_title;

    const std::filesystem::path json_path =
        std::filesystem::path(m_artwork_dir) / (name + ".json");
    const std::string json = read_file_text(json_path);
    if (json.empty()) {
        return meta;  // nothing cached yet
    }

    meta.found = json_string_field(json, "found") == "true";
    const std::string title = json_string_field(json, "title");
    if (!title.empty()) {
        meta.title = title;
    }
    meta.year         = json_string_field(json, "year");
    meta.manufacturer = json_string_field(json, "manufacturer");
    meta.genre        = json_string_field(json, "genre");
    meta.description  = json_string_field(json, "description");

    // "images" is a comma-separated filename list; keep those still on disk.
    const std::string images = json_string_field(json, "images");
    usize             start  = 0;
    while (start <= images.size()) {
        const usize comma = images.find(',', start);
        const std::string file =
            images.substr(start, comma == std::string::npos ? std::string::npos
                                                            : comma - start);
        if (!file.empty()) {
            const std::filesystem::path path = std::filesystem::path(m_artwork_dir) / file;
            std::error_code             error;
            if (std::filesystem::exists(path, error) && !error) {
                meta.image_paths.push_back(path.string());
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return meta;
}

#if defined(SM2_HAVE_CURL)

void Scraper::run()
{
    // Safe unguarded because this is the only thread that ever touches curl.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        SM2_WARN("scraper: curl_global_init failed; running offline");
        return;
    }

    for (const Entry& entry : m_entries) {
        if (m_stop.load()) break;

        // A cached marker (done or recorded-missing) means leave it alone.
        const std::filesystem::path json_path =
            std::filesystem::path(m_artwork_dir) / (entry.name + ".json");
        std::error_code error;
        if (std::filesystem::exists(json_path, error) && !error) {
            continue;
        }

        const std::string meta_url =
            "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&lang=en"
            "&game_name=" + entry.name;
        std::string body;
        if (!http_get(meta_url, &body)) {
            continue;  // transient failure: no marker, retry next run
        }

        std::string              title, year, manufacturer, genre, description;
        std::vector<std::string> image_files;
        bool                     found = false;

        if (!result_is_empty(body)) {
            found        = true;
            title        = json_string_field(body, "short_title");
            if (title.empty()) title = json_string_field(body, "title");
            year         = json_string_field(body, "year");
            manufacturer = json_string_field(body, "manufacturer");
            genre        = json_string_field(body, "genre");
            description  = json_string_field(body, "history");

            // Every artwork type the server has, in display order. The URLs
            // carry resize=0 (full size, large and slow); rewrite to a bounded
            // size so each fetch is quick and the cache stays small.
            const struct { const char* field; const char* suffix; } kinds[] = {
                {"url_image_flyer",   "flyer"},
                {"url_image_title",   "title"},
                {"url_image_ingame",  "ingame"},
                {"url_image_marquee", "marquee"},
            };
            for (const auto& kind : kinds) {
                if (m_stop.load()) break;
                std::string url = json_string_field(body, kind.field);
                if (url.empty()) continue;
                const usize r = url.find("resize=0");
                if (r != std::string::npos) {
                    url.replace(r, 8, "resize=400");
                }
                std::string image_bytes;
                if (http_get(url, &image_bytes) && image_bytes.size() > 64) {
                    const std::string file =
                        entry.name + "." + kind.suffix + ".png";
                    std::ofstream img(std::filesystem::path(m_artwork_dir) / file,
                                      std::ios::binary | std::ios::trunc);
                    img.write(image_bytes.data(),
                              static_cast<std::streamsize>(image_bytes.size()));
                    image_files.push_back(file);
                }
            }
        }

        // The cache file, written even for a not-found set so it is not
        // re-fetched every launch.
        {
            std::string image_list;
            for (usize k = 0; k < image_files.size(); ++k) {
                if (k != 0) image_list += ",";
                image_list += image_files[k];
            }
            std::ofstream out(json_path, std::ios::trunc);
            out << "{\n"
                << "  \"found\": \"" << (found ? "true" : "false") << "\",\n"
                << "  \"title\": \"" << encode_json_string(title) << "\",\n"
                << "  \"year\": \"" << encode_json_string(year) << "\",\n"
                << "  \"manufacturer\": \"" << encode_json_string(manufacturer) << "\",\n"
                << "  \"genre\": \"" << encode_json_string(genre) << "\",\n"
                << "  \"images\": \"" << encode_json_string(image_list) << "\",\n"
                << "  \"description\": \"" << encode_json_string(description) << "\"\n"
                << "}\n";
        }

        {
            std::lock_guard<std::mutex> lock(m_ready_mutex);
            m_ready.push(entry.name);
        }

        // A small gap between sets so a full sweep is not a burst; stop-flag
        // breaks it early.
        for (int i = 0; i < 10 && !m_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    curl_global_cleanup();
}

#else  // no libcurl

void Scraper::run()
{
    // Never reached (m_scraping stays false without libcurl); defined so the
    // thread ctor's address-of resolves.
}

#endif  // SM2_HAVE_CURL

}  // namespace sm2::osd
