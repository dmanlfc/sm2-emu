//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
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
#include "hw/save_state_io.h"

#include "core/archive.h"
#include "core/log.h"

#include <miniz.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <utility>
#include <vector>

namespace sm2::hw {
namespace {

/// "HOST", the tag of the host's section after the machine's payload.
constexpr u32 kHostTag = 0x54534f48u;

// A state file is the raw state deflated behind this magic and the raw size.
// Files without it are read as they are.
constexpr char  kPackedMagic[8] = {'S', 'M', '2', 'S', 'T', 'A', 'T', 'Z'};
constexpr usize kPackedHeader   = sizeof(kPackedMagic) + sizeof(u64);
constexpr u64   kMaxStateBytes  = u64{1} << 30;

/// The host's serialize walk, when set.
SerializeFn& host_state()
{
    static SerializeFn serialize;
    return serialize;
}

/// A host section loaded while no host state was set, for when one is.
std::vector<u8>& pending_host_section()
{
    static std::vector<u8> section;
    return section;
}

/// Apply a host section; a section that does not read leaves the host state
/// as it was.
void apply_host_section(const std::vector<u8>& section)
{
    std::vector<u8> rollback;
    {
        Archive save_ar(rollback);
        host_state()(save_ar);
    }
    Archive load_ar(section.data(), section.size());
    host_state()(load_ar);
    if (load_ar.failed()) {
        SM2_WARN("save-state: the drive board's state does not read; it keeps its own");
        Archive restore_ar(rollback.data(), rollback.size());
        host_state()(restore_ar);
        return;
    }
    SM2_DEBUG("save-state: the drive board's state restored (%zu bytes)", section.size());
}

}  // namespace

void set_host_state(SerializeFn serialize)
{
    host_state() = std::move(serialize);
    if (host_state() && !pending_host_section().empty()) {
        apply_host_section(pending_host_section());
    }
    pending_host_section().clear();
}

bool save_state_to_buffer(std::vector<u8>& out, const std::string& game, u32 board,
                          bool in_frame, const SerializeFn& serialize)
{
    if (in_frame) {
        SM2_ERROR("save_state called mid-frame; only allowed between frames");
        return false;
    }

    out.clear();
    Archive       ar(out);
    state::Header header;
    header.game  = game;
    header.board = board;
    state::write_header(ar, header);
    serialize(ar);
    if (host_state()) {
        std::vector<u8> section;
        {
            Archive host_ar(section);
            host_state()(host_ar);
        }
        u32 tag = kHostTag;
        ar.raw(tag);
        ar.vector_pod(section);
    }
    return true;
}

bool load_state_from_buffer(std::span<const u8> data, const std::string& game, u32 board,
                            bool in_frame, const SerializeFn& serialize)
{
    if (in_frame) {
        SM2_ERROR("load_state called mid-frame; only allowed between frames");
        return false;
    }

    state::Header parsed;
    usize         payload_offset = 0;
    if (!state::read_header(data.data(), data.size(), parsed, payload_offset)) {
        return false;  // read_header logged the reason
    }
    if (parsed.game != game) {
        SM2_ERROR("save-state: state is for '%s', running '%s' — refused",
                  parsed.game.c_str(), game.c_str());
        return false;
    }
    if (parsed.board != board) {
        SM2_ERROR("save-state: board mismatch — refused");
        return false;
    }

    // Reject-before-commit (design §5(4), requirement 3.2). A scratch machine is
    // not viable (create_machine consumes the RomSet), so snapshot the current
    // state to a rollback buffer, apply the file, and if the read ran past the
    // end restore from the rollback. The Archive zero-fills rather than reading
    // OOB, so the rollback is always applicable.
    std::vector<u8> rollback;
    {
        Archive save_ar(rollback);
        serialize(save_ar);
    }

    Archive load_ar(data.data() + payload_offset, data.size() - payload_offset);
    serialize(load_ar);
    if (load_ar.failed()) {
        SM2_ERROR("save-state: state is truncated or corrupt — rolling back");
        Archive restore_ar(rollback.data(), rollback.size());
        serialize(restore_ar);
        return false;
    }

    // The host's section, when the state has one.
    pending_host_section().clear();
    const usize payload_size = data.size() - payload_offset;
    if (payload_size - load_ar.position() >= sizeof(u32)) {
        u32 tag = 0;
        load_ar.raw(tag);
        std::vector<u8> section;
        if (tag == kHostTag) {
            load_ar.vector_pod(section);
        }
        if (tag != kHostTag || load_ar.failed()) {
            SM2_WARN("save-state: state ends with bytes that are no host section, ignored");
        } else if (host_state()) {
            apply_host_section(section);
        } else {
            pending_host_section() = std::move(section);
        }
    }
    return true;
}

bool write_state_file(const std::string& path, std::span<const u8> data)
{
    // Create the parent directory (e.g. the states dir) on first use, the way
    // the screenshot path does; fopen("wb") will not make it.
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    mz_ulong        packed_size = mz_compressBound(static_cast<mz_ulong>(data.size()));
    std::vector<u8> packed(kPackedHeader + packed_size);
    if (mz_compress2(packed.data() + kPackedHeader, &packed_size, data.data(),
                     static_cast<mz_ulong>(data.size()), MZ_BEST_SPEED)
        != MZ_OK) {
        SM2_ERROR("save-state: could not compress the state");
        return false;
    }
    const u64 raw_size = data.size();
    std::memcpy(packed.data(), kPackedMagic, sizeof(kPackedMagic));
    std::memcpy(packed.data() + sizeof(kPackedMagic), &raw_size, sizeof(raw_size));
    packed.resize(kPackedHeader + packed_size);

    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("save-state: could not open '%s' for writing", path.c_str());
        return false;
    }
    const usize written = std::fwrite(packed.data(), 1, packed.size(), handle);
    std::fclose(handle);
    if (written != packed.size()) {
        SM2_ERROR("save-state: short write to '%s'", path.c_str());
        return false;
    }
    SM2_INFO("save-state: wrote %zu bytes (%zu raw) to '%s'", packed.size(), data.size(),
             path.c_str());
    return true;
}

bool read_state_file(const std::string& path, std::vector<u8>& out)
{
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        SM2_ERROR("save-state: could not open '%s' for reading", path.c_str());
        return false;
    }
    std::fseek(handle, 0, SEEK_END);
    const long size = std::ftell(handle);
    std::fseek(handle, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(handle);
        SM2_ERROR("save-state: '%s' is empty or unreadable", path.c_str());
        return false;
    }
    out.resize(static_cast<usize>(size));
    const usize read = std::fread(out.data(), 1, out.size(), handle);
    std::fclose(handle);
    if (read != out.size()) {
        SM2_ERROR("save-state: short read from '%s'", path.c_str());
        return false;
    }
    if (out.size() < kPackedHeader || std::memcmp(out.data(), kPackedMagic, sizeof(kPackedMagic)) != 0) {
        return true;
    }
    u64 raw_size = 0;
    std::memcpy(&raw_size, out.data() + sizeof(kPackedMagic), sizeof(raw_size));
    if (raw_size == 0 || raw_size > kMaxStateBytes) {
        SM2_ERROR("save-state: '%s' is corrupt (bad size)", path.c_str());
        return false;
    }
    std::vector<u8> raw(static_cast<usize>(raw_size));
    mz_ulong        raw_len = static_cast<mz_ulong>(raw_size);
    if (mz_uncompress(raw.data(), &raw_len, out.data() + kPackedHeader,
                      static_cast<mz_ulong>(out.size() - kPackedHeader))
            != MZ_OK
        || raw_len != raw_size) {
        SM2_ERROR("save-state: '%s' is corrupt or truncated", path.c_str());
        return false;
    }
    out = std::move(raw);
    return true;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

std::string state_slot_path(const std::string& dir, const std::string& game,
                            const std::string& slot)
{
    return (std::filesystem::path(dir) / (game + "." + slot + ".sm2state")).string();
}

std::vector<SlotInfo> list_state_slots(const std::string& dir, const std::string& game)
{
    std::vector<SlotInfo> slots;
    slots.reserve(1 + kNumberedSlots);

    auto add = [&](const std::string& name) {
        SlotInfo info;
        info.slot = name;
        std::error_code ec;
        const std::filesystem::path p = state_slot_path(dir, game, name);
        if (std::filesystem::exists(p, ec)) {
            info.occupied = true;
            const auto ftime = std::filesystem::last_write_time(p, ec);
            if (!ec) {
                // file_clock -> system_clock -> time_t, portably enough for a
                // display stamp (exact epoch alignment does not matter here).
                const auto sctime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now()
                    + std::chrono::system_clock::now());
                const std::time_t t = std::chrono::system_clock::to_time_t(sctime);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
                info.timestamp = stamp;
            }
        }
        slots.push_back(std::move(info));
    };

    add(kQuickSlot);
    for (int i = 1; i <= kNumberedSlots; ++i) {
        add(std::to_string(i));
    }
    return slots;
}

bool delete_state_slot(const std::string& dir, const std::string& game,
                       const std::string& slot)
{
    const std::filesystem::path p = state_slot_path(dir, game, slot);
    std::error_code ec;
    std::filesystem::remove(p, ec);  // false + no error means it was already gone
    if (ec) {
        SM2_ERROR("save-state: could not delete '%s': %s", p.string().c_str(),
                  ec.message().c_str());
        return false;
    }
    return true;
}

}  // namespace sm2::hw
