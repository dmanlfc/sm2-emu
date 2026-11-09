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
#pragma once

#include "core/types.h"

#include <deque>
#include <string>
#include <type_traits>
#include <vector>

// Binary save-state archive.
//
// One `serialize(Archive&)` per class both reads and writes, so the two
// directions cannot drift out of sync (the classic bug where a field is added
// to save but forgotten in load). The archive's direction is fixed at
// construction: Save appends host bytes to an output buffer, Load consumes them
// from an input span.
//
// The file is host-endian and NOT portable across endianness; every target
// this ships to is little-endian and a static_assert enforces it. See
// .kiro/specs/model2-save-states/design.md §1.

namespace sm2 {

class Archive {
public:
    enum class Mode { Save, Load };

    /// Save mode: writes go into `buffer`, growing it.
    explicit Archive(std::vector<u8>& buffer);

    /// Load mode: reads come from `data`/`size`, bounds-checked. A read past
    /// the end sets `failed()` and yields zeroes rather than reading OOB.
    Archive(const u8* data, usize size);

    [[nodiscard]] bool saving() const { return m_mode == Mode::Save; }
    [[nodiscard]] bool loading() const { return m_mode == Mode::Load; }

    /// A trivially-copyable scalar or aggregate: raw little-endian bytes.
    template <class T>
    void raw(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Archive::raw requires a trivially-copyable type");
        bytes(&value, 1);
    }

    /// A contiguous block of `count` trivially-copyable elements
    /// (std::array data, a fixed C array, a std::vector's data pointer).
    template <class T>
    void bytes(T* data, usize count)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Archive::bytes requires a trivially-copyable element");
        transfer(data, count * sizeof(T));
    }

    /// A length-prefixed byte vector. On load the vector is resized to the
    /// stored length before its contents are read.
    void vector_pod(std::vector<u8>& v);

    /// A length-prefixed std::deque<u32> (the copro FIFOs, the comm loopback
    /// queue lengths). Stored as a u32 count followed by the elements.
    void deque_u32(std::deque<u32>& d);

    /// Set once on Load when a read runs past the end of the input. Sticky:
    /// every subsequent read is a no-op yielding zeroes, so the top level can
    /// check it once after the whole payload rather than per field.
    [[nodiscard]] bool failed() const { return m_failed; }

    /// Marks a load as failed when a value is out of range, so the loader rolls
    /// the state back.
    void mark_failed() { m_failed = true; }

    /// Bytes written so far (Save) or consumed so far (Load).
    [[nodiscard]] usize position() const { return m_pos; }

    /// The format version being read (always the current one when saving).
    [[nodiscard]] u32 version() const { return m_version; }
    void set_version(u32 version) { m_version = version; }

private:
    void transfer(void* data, usize num_bytes);

    Mode m_mode;

    // Save: the growable output. Load: leave null.
    std::vector<u8>* m_out = nullptr;

    // Load: the input span. Save: leave null.
    const u8* m_in      = nullptr;
    usize     m_in_size = 0;

    usize m_pos     = 0;
    bool  m_failed  = false;
    u32   m_version = 3;
};

// ---------------------------------------------------------------------------
// File format
// ---------------------------------------------------------------------------
// A save-state file is a small header followed by the machine's serialize()
// payload:
//
//   magic     8 bytes  "SM2STATE"
//   version   u32      bumped on any serialize-layout change (req 4.2)
//   game_len  u32
//   game      game_len bytes  (GameSpec::name)
//   board     u32      (rom::Board, cast by the caller — the primitive stays
//                       ignorant of the rom layer so sm2_core keeps no
//                       dependency on sm2_rom)
//   payload   the machine's serialize() output
//
// Load reads and validates the header BEFORE the payload, so a wrong magic,
// version, game or board is rejected without touching the running machine
// (req 3.2 / 4.1).

namespace state {

inline constexpr char     kMagic[8]      = {'S', 'M', '2', 'S', 'T', 'A', 'T', 'E'};
inline constexpr u32      kFormatVersion = 3;
/// Oldest version still read; 3 added the link frames still in flight.
inline constexpr u32      kOldestFormatVersion = 2;

/// Header fields, in file order. The game name and board are supplied by the
/// caller so this stays free of the rom layer.
struct Header {
    std::string game;
    u32         board   = 0;
    u32         version = kFormatVersion;
};

/// Write the header into `ar` (must be Save mode). Follows with the caller's
/// payload writes.
void write_header(Archive& ar, const Header& header);

/// Read and validate a header from a raw file buffer. Returns true and fills
/// `out` and `payload_offset` (the byte index where the payload begins) only
/// when the magic and version match; otherwise logs a reason and returns false.
/// Game/board matching against the running machine is the caller's job (it
/// alone knows them) — this only guarantees the file is a well-formed state of
/// the current format version.
[[nodiscard]] bool read_header(const u8* data, usize size, Header& out,
                               usize& payload_offset);

}  // namespace state

}  // namespace sm2
