//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "core/archive.h"

#include "core/log.h"

#include <bit>
#include <cstring>

// The file is written as raw host bytes, so it is only readable on a host of
// the same endianness. Every target this ships to is little-endian; a
// big-endian port must fail loud here rather than silently produce garbage.
static_assert(std::endian::native == std::endian::little,
              "the save-state format is little-endian; port the archive before "
              "building for a big-endian host");

namespace sm2 {

Archive::Archive(std::vector<u8>& buffer) : m_mode(Mode::Save), m_out(&buffer) {}

Archive::Archive(const u8* data, usize size)
    : m_mode(Mode::Load), m_in(data), m_in_size(size)
{
}

void Archive::transfer(void* data, usize num_bytes)
{
    if (num_bytes == 0) {
        return;
    }
    if (m_mode == Mode::Save) {
        const auto* src = static_cast<const u8*>(data);
        m_out->insert(m_out->end(), src, src + num_bytes);
        m_pos += num_bytes;
        return;
    }

    // Load. A read past the end is a truncated or malformed file: set the
    // sticky failure flag and zero-fill so the caller sees deterministic
    // (empty) values rather than reading out of bounds.
    if (m_failed || m_pos + num_bytes > m_in_size) {
        m_failed = true;
        std::memset(data, 0, num_bytes);
        return;
    }
    std::memcpy(data, m_in + m_pos, num_bytes);
    m_pos += num_bytes;
}

void Archive::vector_pod(std::vector<u8>& v)
{
    u32 len = static_cast<u32>(v.size());
    raw(len);
    if (loading()) {
        if (m_failed) {
            v.clear();
            return;
        }
        v.resize(len);
    }
    bytes(v.data(), v.size());
}

void Archive::deque_u32(std::deque<u32>& d)
{
    u32 len = static_cast<u32>(d.size());
    raw(len);
    if (saving()) {
        for (u32 value : d) {
            raw(value);
        }
        return;
    }

    d.clear();
    if (m_failed) {
        return;
    }
    for (u32 i = 0; i < len; ++i) {
        u32 value = 0;
        raw(value);
        d.push_back(value);
    }
}

namespace state {

void write_header(Archive& ar, const Header& header)
{
    ar.bytes(const_cast<char*>(kMagic), sizeof(kMagic));
    u32 version = kFormatVersion;
    ar.raw(version);
    u32 game_len = static_cast<u32>(header.game.size());
    ar.raw(game_len);
    if (game_len != 0) {
        ar.bytes(const_cast<char*>(header.game.data()), game_len);
    }
    u32 board = header.board;
    ar.raw(board);
}

bool read_header(const u8* data, usize size, Header& out, usize& payload_offset)
{
    Archive ar(data, size);

    char magic[sizeof(kMagic)] = {};
    ar.bytes(magic, sizeof(magic));
    if (ar.failed() || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        SM2_ERROR("save-state: bad magic (not an SM2STATE file)");
        return false;
    }

    u32 version = 0;
    ar.raw(version);
    if (ar.failed()) {
        SM2_ERROR("save-state: truncated header (no version)");
        return false;
    }
    if (version != kFormatVersion) {
        SM2_ERROR("save-state: format version %u, this build reads %u; the file "
                  "is from a different version and cannot be loaded",
                  version, kFormatVersion);
        return false;
    }

    u32 game_len = 0;
    ar.raw(game_len);
    if (ar.failed()) {
        SM2_ERROR("save-state: truncated header (no game length)");
        return false;
    }
    out.game.assign(game_len, '\0');
    if (game_len != 0) {
        ar.bytes(out.game.data(), game_len);
    }
    ar.raw(out.board);
    if (ar.failed()) {
        SM2_ERROR("save-state: truncated header (game name / board)");
        return false;
    }

    payload_offset = ar.position();
    return true;
}

}  // namespace state

}  // namespace sm2
