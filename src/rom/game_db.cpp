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
#include "rom/game_db.h"

#include "core/log.h"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace sm2::rom {
namespace {

/// Parse a decimal or 0x-prefixed hexadecimal integer.
[[nodiscard]] bool parse_integer(std::string_view text, u32* out)
{
    // Trim, because hand-edited XML attributes pick up stray whitespace.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }

    u32         value  = 0;
    const char* begin  = text.data();
    const char* end    = text.data() + text.size();
    const auto  result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    *out = value;
    return true;
}

[[nodiscard]] bool attribute_integer(const pugi::xml_node& node,
                                    const char*           name,
                                    u32                   fallback,
                                    u32*                  out,
                                    const char*           context)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) {
        *out = fallback;
        return true;
    }
    if (!parse_integer(attribute.value(), out)) {
        SM2_ERROR("%s: attribute %s=\"%s\" is not a valid integer", context, name,
                  attribute.value());
        return false;
    }
    return true;
}

[[nodiscard]] bool attribute_bool(const pugi::xml_node& node,
                                  const char*           name,
                                  bool                  fallback,
                                  bool*                 out,
                                  const char*           context)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) {
        *out = fallback;
        return true;
    }
    const std::string_view value = attribute.value();
    if (value == "true" || value == "1" || value == "yes") {
        *out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        *out = false;
        return true;
    }
    SM2_ERROR("%s: attribute %s=\"%s\" is not a boolean", context, name, attribute.value());
    return false;
}

[[nodiscard]] bool parse_board(std::string_view text, Board* out)
{
    if (text == "model2")  { *out = Board::Model2;  return true; }
    if (text == "model2a") { *out = Board::Model2A; return true; }
    if (text == "model2b") { *out = Board::Model2B; return true; }
    if (text == "model2c") { *out = Board::Model2C; return true; }
    return false;
}

[[nodiscard]] bool parse_input_type(std::string_view text, InputFlags* out)
{
    if (text == "common")    { *out = InputFlags::Common;    return true; }
    if (text == "joystick1") { *out = InputFlags::Joystick1; return true; }
    if (text == "joystick2") { *out = InputFlags::Joystick2; return true; }
    if (text == "buttons3")  { *out = InputFlags::Buttons3;  return true; }
    if (text == "vehicle")   { *out = InputFlags::Vehicle;   return true; }
    if (text == "gun1")      { *out = InputFlags::Gun1;      return true; }
    if (text == "gun2")      { *out = InputFlags::Gun2;      return true; }
    return false;
}

/// Directory holding the running executable, empty if it cannot be determined.
[[nodiscard]] std::filesystem::path executable_directory()
{
    std::error_code error;

#if defined(__APPLE__)
    u32 size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    const std::filesystem::path resolved = std::filesystem::canonical(buffer.c_str(), error);
#elif defined(_WIN32)
    char  buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return {};
    }
    const std::filesystem::path resolved = std::filesystem::canonical(buffer, error);
#else
    const std::filesystem::path resolved =
        std::filesystem::canonical("/proc/self/exe", error);
#endif

    if (error) {
        return {};
    }
    return resolved.parent_path();
}

}  // namespace

// ---------------------------------------------------------------------------
// Locating the database
// ---------------------------------------------------------------------------

std::optional<std::string> GameDatabase::locate(const std::string& override_path)
{
    std::vector<std::filesystem::path> candidates;

    if (!override_path.empty()) {
        // An explicit path is honoured or reported, never silently replaced by a
        // fallback that would make the failure confusing.
        if (std::filesystem::exists(override_path)) {
            return override_path;
        }
        SM2_ERROR("games.xml was not found at '%s'", override_path.c_str());
        return std::nullopt;
    }

    candidates.emplace_back("games.xml");
    candidates.emplace_back("data/games.xml");

    const std::filesystem::path exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "games.xml");
        candidates.push_back(exe_dir / ".." / "share" / "sm2-emu" / "games.xml");
        candidates.push_back(exe_dir / ".." / ".." / "data" / "games.xml");
    }

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return std::filesystem::weakly_canonical(candidate, error).string();
        }
    }

    SM2_ERROR("games.xml could not be found. Looked in the current directory, "
              "beside the executable, and in ../share/sm2-emu. Pass "
              "--games-xml <path> to name it explicitly.");
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

bool GameDatabase::load(const std::string& path)
{
    pugi::xml_document        document;
    const pugi::xml_parse_result result = document.load_file(path.c_str());
    if (!result) {
        SM2_ERROR("%s: %s at byte %lld", path.c_str(), result.description(),
                  static_cast<long long>(result.offset));
        return false;
    }

    m_games.clear();

    const pugi::xml_node root = document.child("games");
    if (!root) {
        SM2_ERROR("%s: no <games> root element", path.c_str());
        return false;
    }

    // Clones that named no board of their own; filled in from the parent by
    // merge_clones.
    std::set<std::string> board_inherited;

    for (const pugi::xml_node game_node : root.children("game")) {
        GameSpec game;

        game.name = game_node.attribute("name").value();
        if (game.name.empty()) {
            SM2_ERROR("%s: a <game> element has no name attribute", path.c_str());
            return false;
        }
        const std::string context = path + ": game '" + game.name + "'";

        game.parent = game_node.attribute("parent").value();

        const pugi::xml_attribute board_attribute = game_node.attribute("board");
        if (!board_attribute) {
            // A clone is the same hardware as its parent unless it says otherwise,
            // which is the usual case: a revision respins the program EPROMs and
            // nothing else.
            if (game.parent.empty()) {
                SM2_ERROR("%s: no board attribute", context.c_str());
                return false;
            }
            board_inherited.insert(game.name);
        } else if (!parse_board(board_attribute.value(), &game.board)) {
            SM2_ERROR("%s: unrecognised board '%s' (expected model2, model2a, "
                      "model2b or model2c)", context.c_str(), board_attribute.value());
            return false;
        }

        game.title        = game_node.child_value("title");
        game.version      = game_node.child_value("version");
        game.manufacturer = game_node.child_value("manufacturer");

        const pugi::xml_node year_node = game_node.child("year");
        if (year_node && !parse_integer(year_node.child_value(), &game.year)) {
            SM2_ERROR("%s: year '%s' is not a number", context.c_str(),
                      year_node.child_value());
            return false;
        }

        if (!attribute_bool(game_node, "preliminary", false, &game.preliminary,
                            context.c_str())) {
            return false;
        }

        for (const pugi::xml_node input_node : game_node.child("inputs").children("input")) {
            const pugi::xml_attribute type = input_node.attribute("type");
            InputFlags                flag = InputFlags::None;
            if (!type || !parse_input_type(type.value(), &flag)) {
                // Refuse rather than defaulting to zero: a typo here silently
                // unbinds a control, which is a miserable thing to debug.
                SM2_ERROR("%s: unrecognised input type '%s'", context.c_str(), type.value());
                return false;
            }
            game.inputs = game.inputs | flag;
        }

        for (const pugi::xml_node region_node : game_node.child("roms").children("region")) {
            RegionSpec region;
            region.name = region_node.attribute("name").value();
            if (region.name.empty()) {
                SM2_ERROR("%s: a <region> element has no name attribute", context.c_str());
                return false;
            }
            const std::string region_context = context + ", region '" + region.name + "'";

            u32 fill = 0;
            if (!attribute_integer(region_node, "size",   0, &region.size,   region_context.c_str())
                || !attribute_integer(region_node, "stride", 1, &region.stride, region_context.c_str())
                || !attribute_integer(region_node, "chunk",  1, &region.chunk,  region_context.c_str())
                || !attribute_integer(region_node, "fill",   0, &fill,          region_context.c_str())
                || !attribute_bool(region_node, "byte_swap", false, &region.byte_swap, region_context.c_str())
                || !attribute_bool(region_node, "required",  true,  &region.required,  region_context.c_str())) {
                return false;
            }
            if (fill > 0xFFU) {
                SM2_ERROR("%s: fill must be a byte value", region_context.c_str());
                return false;
            }
            region.fill = static_cast<u8>(fill);

            if (region.chunk == 0 || region.stride == 0) {
                SM2_ERROR("%s: stride and chunk must both be non-zero",
                          region_context.c_str());
                return false;
            }
            if (region.chunk > region.stride) {
                SM2_ERROR("%s: chunk (%u) exceeds stride (%u), so files would "
                          "overlap", region_context.c_str(), region.chunk, region.stride);
                return false;
            }
            if (region.byte_swap && (region.stride % 2) != 0) {
                SM2_ERROR("%s: byte_swap needs an even stride", region_context.c_str());
                return false;
            }

            for (const pugi::xml_node file_node : region_node.children("file")) {
                FileSpec file;
                file.name = file_node.attribute("name").value();
                if (file.name.empty()) {
                    SM2_ERROR("%s: a <file> element has no name attribute",
                              region_context.c_str());
                    return false;
                }
                if (!attribute_integer(file_node, "offset", 0, &file.offset,
                                       region_context.c_str())) {
                    return false;
                }

                const pugi::xml_attribute crc = file_node.attribute("crc");
                if (crc) {
                    if (!parse_integer(crc.value(), &file.crc32)) {
                        SM2_ERROR("%s: file '%s' has an invalid crc '%s'",
                                  region_context.c_str(), file.name.c_str(), crc.value());
                        return false;
                    }
                    file.has_crc = true;
                }

                region.files.push_back(std::move(file));
            }

            game.regions.push_back(std::move(region));
        }

        m_games.push_back(std::move(game));
    }

    if (m_games.empty()) {
        SM2_ERROR("%s: contains no games", path.c_str());
        return false;
    }

    if (!merge_clones(board_inherited)) {
        return false;
    }

    SM2_INFO("loaded %zu game definition(s) from %s", m_games.size(), path.c_str());
    return true;
}

bool GameDatabase::load_from_string(std::string_view xml, const char* origin)
{
    // Round-trip through a temporary file so there is exactly one parser
    // implementation to keep correct. The database is small and this path is
    // only used by tests.
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() / "sm2-emu-games-test.xml";
    {
        std::FILE* handle = std::fopen(temp.string().c_str(), "wb");
        if (handle == nullptr) {
            SM2_ERROR("%s: could not create a temporary file", origin);
            return false;
        }
        std::fwrite(xml.data(), 1, xml.size(), handle);
        std::fclose(handle);
    }
    const bool ok = load(temp.string());
    std::error_code error;
    std::filesystem::remove(temp, error);
    return ok;
}

// ---------------------------------------------------------------------------
// Clone merging
// ---------------------------------------------------------------------------

bool GameDatabase::merge_clones(const std::set<std::string>& board_inherited)
{
    std::unordered_map<std::string, usize> index_by_name;
    for (usize index = 0; index < m_games.size(); ++index) {
        const auto [_, inserted] = index_by_name.emplace(m_games[index].name, index);
        if (!inserted) {
            SM2_ERROR("duplicate game definition '%s'", m_games[index].name.c_str());
            return false;
        }
    }

    for (GameSpec& game : m_games) {
        if (game.parent.empty()) {
            continue;
        }

        const auto parent_entry = index_by_name.find(game.parent);
        if (parent_entry == index_by_name.end()) {
            SM2_ERROR("game '%s' names parent '%s', which is not defined",
                      game.name.c_str(), game.parent.c_str());
            return false;
        }

        const GameSpec& parent = m_games[parent_entry->second];
        if (!parent.parent.empty()) {
            // One level only. Chained clones would need the merge to be ordered
            // topologically, and no Model 2 set requires it.
            SM2_ERROR("game '%s' is a clone of '%s', which is itself a clone; "
                      "only one level of parenting is supported",
                      game.name.c_str(), parent.name.c_str());
            return false;
        }

        // Inherit metadata the child left blank.
        if (board_inherited.count(game.name) != 0) { game.board = parent.board; }
        if (game.title.empty())        { game.title = parent.title; }
        if (game.manufacturer.empty()) { game.manufacturer = parent.manufacturer; }
        if (game.year == 0)            { game.year = parent.year; }
        if (game.inputs == InputFlags::None) { game.inputs = parent.inputs; }
        // A clone cannot be more validated than its parent unless it has been
        // explicitly boot-tested as well. Keep preliminary status conservative
        // across revisions so --list-games does not advertise an unverified set.
        game.preliminary = game.preliminary || parent.preliminary;

        // A region the child redefines replaces the parent's entirely, which is
        // what a revision does: it respins whole chips, not individual bytes.
        for (const RegionSpec& parent_region : parent.regions) {
            if (game.region(parent_region.name) == nullptr) {
                game.regions.push_back(parent_region);
            }
        }
    }

    return true;
}

const GameSpec* GameDatabase::find(std::string_view name) const
{
    const auto match = std::find_if(m_games.begin(), m_games.end(),
                                    [name](const GameSpec& game) { return game.name == name; });
    return match != m_games.end() ? &*match : nullptr;
}

}  // namespace sm2::rom
