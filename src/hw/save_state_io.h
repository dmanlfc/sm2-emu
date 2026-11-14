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
#pragma once

#include "core/types.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace sm2 {
class Archive;
}

// Shared save-state file I/O for all four boards.
//
// The envelope around each board's serialize(Archive&): a header and payload on
// save; on load the header (magic/version/game/board) is validated and the
// payload applied with a snapshot rollback, so a truncated or corrupt file leaves
// the running machine untouched.

namespace sm2::hw {

/// A board's serialize walk. Called for both save and load; the Archive's mode
/// decides the direction.
using SerializeFn = std::function<void(Archive&)>;

/// Write `serialize`'s payload, behind a state header keyed by `game` /
/// `board`, into `out`. `in_frame` must be false (save only between frames).
[[nodiscard]] bool save_state_to_buffer(std::vector<u8>& out, const std::string& game,
                                        u32 board, bool in_frame,
                                        const SerializeFn& serialize);

/// Validate and apply a state in `data` to the live machine via `serialize`.
/// Rejects (logged, machine untouched) on header mismatch against
/// `game`/`board`, on `in_frame`, or on a truncated payload (rolled back).
/// On success the caller still runs its own fix-ups (generation bumps etc.).
[[nodiscard]] bool load_state_from_buffer(std::span<const u8> data, const std::string& game,
                                          u32 board, bool in_frame,
                                          const SerializeFn& serialize);

/// A state file's bytes to and from disk, deflated on write; an uncompressed
/// file still reads. False (logged) on an I/O error or a corrupt file.
[[nodiscard]] bool write_state_file(const std::string& path, std::span<const u8> data);
[[nodiscard]] bool read_state_file(const std::string& path, std::vector<u8>& out);

/// Host state (the drive board model), saved in a tagged section after the
/// machine's payload. A section loaded before this is set is applied once it
/// is. An empty function unsets it.
void set_host_state(SerializeFn serialize);

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
// A game gets one reserved quick-save slot plus a few numbered slots. Files are
// named `<game>.<slot>.kstate` under the states directory (data dir / states).

/// The reserved quick-save slot name.
inline constexpr const char* kQuickSlot = "quick";

/// How many numbered slots ("1".."kNumberedSlots") a game has.
inline constexpr int kNumberedSlots = 4;

/// Absolute path of one slot's file: `<dir>/<game>.<slot>.kstate`. `dir` is
/// the states directory (resolve_default_paths derives it as data dir/states).
/// An sm2-emu `.sm2state` for the slot is converted to it first.
[[nodiscard]] std::string state_slot_path(const std::string& dir, const std::string& game,
                                          const std::string& slot);

/// One slot's status for the overlay.
struct SlotInfo {
    std::string slot;                ///< "quick", "1", ...
    bool        occupied = false;
    std::string timestamp;           ///< local "YYYY-MM-DD HH:MM" when occupied, else empty
};

/// The quick slot followed by the numbered slots, each with its occupied state
/// and (where occupied) a local-time modification stamp, for `game` under `dir`.
[[nodiscard]] std::vector<SlotInfo> list_state_slots(const std::string& dir,
                                                     const std::string& game);

/// Delete one slot's file. False (logged) on an I/O error; a missing file is success.
[[nodiscard]] bool delete_state_slot(const std::string& dir, const std::string& game,
                                     const std::string& slot);

}  // namespace sm2::hw
