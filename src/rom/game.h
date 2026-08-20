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
#pragma once

#include "core/types.h"

#include <array>
#include <string>
#include <vector>

// The ROM database data model.
//
// Games are described in data/games.xml rather than in code, so adding one is a
// data edit. The schema mirrors how the ROM chips are physically wired: a
// region is a flat byte array, and each file contributes `chunk` bytes every
// `stride` bytes starting at `offset`. That single mechanism expresses every
// interleaving Model 2 uses.

namespace sm2::rom {

/// Which Model 2 board variant, which decides the coprocessor and sound
/// hardware. The three CRX boards (2A, 2B and 2C) are implemented; the original
/// Model 2 is here so the database can describe it and the loader can refuse it
/// with a clear message.
enum class Board {
    Model2,   ///< Original. TGP + Model 1 sound board.
    Model2A,  ///< 2A-CRX. TGP + 68000/SCSP.
    Model2B,  ///< 2B-CRX. ADSP-21062 SHARC + 68000/SCSP.
    Model2C,  ///< 2C-CRX. MB86235 TGPx4 + 68000/SCSP.
};

[[nodiscard]] const char* board_name(Board board);

/// True for board variants sm2-emu can currently construct a working machine
/// for. Both `RomLoader::load` (which decides whether a game's archive may
/// even be assembled) and `hw::create_machine` (which decides whether a
/// machine can be built for it) consult this single function, so the two can
/// never drift out of sync as later waves add board support -- there is
/// exactly one place that knows "which boards are implemented".
[[nodiscard]] bool board_implemented(Board board);

/// Logical input groups a game uses, so the input layer can bind only what is
/// relevant and the UI can hide the rest.
enum class InputFlags : u32 {
    None      = 0,
    Common    = 1U << 0,  ///< Coins, service, test, start.
    Joystick1 = 1U << 1,
    Joystick2 = 1U << 2,
    Buttons3  = 1U << 3,  ///< Three attack buttons per player (fighting games).
    Vehicle   = 1U << 4,  ///< Steering, pedals, gear shift.
    Gun1      = 1U << 5,
    Gun2      = 1U << 6,
};

[[nodiscard]] constexpr InputFlags operator|(InputFlags a, InputFlags b)
{
    return static_cast<InputFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has_input(InputFlags set, InputFlags wanted)
{
    return (static_cast<u32>(set) & static_cast<u32>(wanted)) != 0;
}

/// A control wired to one channel of the I/O controller's analogue mux.
///
/// These are MAME's ioport names for the Model 2 analogue inputs. They are kept
/// as distinct logical controls rather than collapsed to "axis 0/1/2" because
/// the host binding differs per control -- a pedal rests at one end of its
/// travel and a wheel rests in the middle -- and because the channel a control
/// sits on is not consistent between titles. Manx TT and Motor Raid put the
/// handlebars on channel 2 with throttle on 0, where every other racer has
/// steering on 0.
enum class AnalogControl : u8 {
    None,
    Steer,      ///< Wheel or paddle, rests centred.
    Accel,      ///< Pedal, rests released.
    Brake,      ///< Pedal, rests released.
    Throttle,   ///< Pedal (Manx TT) or lever (Wave Runner).
    Bank,       ///< Manx TT / Motor Raid handlebars.
    StickX,
    StickY,
    Gun1X,      ///< Analogue stick standing in for a gun: Gunblade, BEL,
    Gun1Y,      ///< Rail Chase 2. Not the serial lightgun interface.
    Gun2X,
    Gun2Y,
    Handle,     ///< Wave Runner handlebars.
    Roll,
    Pitch,
    Slide,
    Curving,
    Swing,
    Inclining,
    Bat1,       ///< Dynamite Baseball bat swing.
    Bat2,
};

/// One analogue mux channel, mirroring the fields of MAME's PORT_BIT for an
/// analogue input: the value at rest, the travel limits from PORT_MINMAX, and
/// PORT_REVERSE.
struct AnalogChannel {
    AnalogControl control = AnalogControl::None;
    u8            minimum = 0x00;
    u8            maximum = 0xff;
    u8            rest    = 0x80;
    bool          reverse = false;
};

/// One axis of the lightgun interface board.
///
/// The guns are 10-bit and reach the program over RS-422 channel 2 rather than
/// through the analogue mux, and each title calibrates its own travel, so the
/// raw range is part of the game's data rather than a property of the hardware.
struct LightgunAxis {
    u16 minimum = 0;
    u16 maximum = 0x3ff;
    u16 rest    = 0x200;
};

/// The four axes of a two-gun cabinet, in the order the interface board's mux
/// presents them: P1 Y, P1 X, P2 Y, P2 X.
struct LightgunSpec {
    bool         present = false;
    LightgunAxis p1y;
    LightgunAxis p1x;
    LightgunAxis p2y;
    LightgunAxis p2x;
};

/// A protection chip a title's main board needs. One value per chip/mode
/// combination rather than a child element, since so far every title needing
/// one needs exactly one fixed configuration of it apart from the 315-5881's
/// per-title key, which rides along in `GameSpec::protection_key`.
enum class Protection {
    None,
    Sega315_5838_Doa,  ///< 315-5838/317-0229 compression chip, DOA hack mode.
    Sega315_5881,      ///< 315-5881 stream cipher. Needs `protection_key`.
};

/// One ROM chip's contribution to a region.
struct FileSpec {
    std::string name;             ///< Filename inside the archive.
    u32         offset  = 0;      ///< Destination byte offset within the region.
    u32         crc32   = 0;      ///< Expected CRC32.
    bool        has_crc = false;  ///< False means identify by name alone.
};

/// A word of an assembled region overwritten after loading.
///
/// This is MAME's per-game `init_` ROM patches, which exist because a couple of
/// programs contain outright bugs the hardware happened to survive and an
/// emulator does not. Zero Gunner and Pilot Kids overwrite their own interrupt
/// table and never repair it; MAME rewrites one word of the vector so the
/// program keeps running, and without it the i960 executes zeros.
///
/// This is a workaround, not emulation. Every patch names the MAME `init_`
/// function it comes from, so none of them can quietly become folklore.
struct RegionPatch {
    u32 offset = 0;  ///< Byte offset within the assembled region.
    u32 value  = 0;  ///< Replacement 32-bit little-endian word.
};

/// A block of an assembled region mirrored to another offset within it.
///
/// This is MAME's `ROM_COPY`, and it is not cosmetic: the Model 2 ROM boards
/// alias their last populated bank across the rest of the address window, and
/// games read the aliases. Without it those addresses read as `fill` and the
/// program walks off a table of zeros -- Sky Target, Virtua Cop 2, Motor Raid,
/// Manx TT and Dead or Alive all depend on one.
struct RegionCopy {
    u32 from = 0;  ///< Source offset within the assembled region.
    u32 to   = 0;  ///< Destination offset within the same region.
    u32 size = 0;
};

/// A flat byte array assembled from one or more ROM chips.
struct RegionSpec {
    std::string name;

    /// Declared size. Zero means "derive it from the files", which is the usual
    /// case; an explicit size matters when the hardware sees a region larger
    /// than the chips populating it.
    u32 size = 0;

    /// Interleaving. `stride == chunk` is a plain contiguous copy; Model 2's
    /// 32-bit regions built from pairs of 16-bit chips use stride 4, chunk 2,
    /// with the two files at offsets 0 and 2.
    u32 stride = 1;
    u32 chunk  = 1;

    /// Swap every pair of bytes after assembly. This is what MAME's
    /// ROM_LOAD16_WORD_SWAP does, and it is how the big-endian 68000 sound
    /// program and its samples are stored.
    bool byte_swap = false;

    /// Value for bytes no chip covers. MAME's ROMREGION_ERASEFF regions, such
    /// as the texture ROMs, need 0xFF rather than zero.
    u8 fill = 0x00;

    /// A region that may legitimately be absent. Also covers regions that exist
    /// but hold no chips on this board, such as copro_data on Virtua Fighter 2.
    bool required = true;

    std::vector<FileSpec> files;

    /// Applied after every file has been placed, in declaration order, so a
    /// copy may read bytes an earlier copy wrote -- matching MAME, where
    /// ROM_COPY runs in the order it appears in the ROM_START block.
    std::vector<RegionCopy> copies;

    /// Applied last, after the copies, so a patch is never overwritten by a
    /// mirror. MAME's driver init functions run after the whole ROM set is
    /// loaded, which is the same ordering.
    std::vector<RegionPatch> patches;
};

/// Everything the database knows about one game.
struct GameSpec {
    std::string name;    ///< Set identifier, e.g. "vf2".
    std::string parent;  ///< Set this is a clone of, empty if a parent.

    std::string title;
    std::string version;
    std::string manufacturer;
    u32         year = 0;

    Board      board  = Board::Model2A;
    InputFlags inputs = InputFlags::None;
    Protection protection = Protection::None;

    /// Key for a keyed protection chip, matching MAME's ROM_PARAMETER. Only the
    /// 315-5881 uses one; roughly 30 of its bits have been recovered, which is
    /// why it fits in 32.
    u32 protection_key = 0;

    /// Bit of the operator port (IN0) that IPT_START1 sits on. Most Model 2A
    /// titles use MAME's default of 0x10, but several PORT_MODIFY their IN0
    /// layout and move it to 0x40 instead (Sky Target, Manx TT and everything
    /// that inherits from it, Indy 500, Wave Runner, Top Skater). Coin1 is
    /// always 0x01: no title in this database's scope remaps it.
    u8 start1_bit = 0x10;

    /// How this title's machine config wires the analogue mux. Index is the
    /// channel number; an entry with control None is an unconnected channel.
    std::array<AnalogChannel, 8> analog{};

    /// The serial lightgun interface board, when the cabinet has one.
    LightgunSpec lightgun;

    /// True when IN1 bits 0x70 read a gear selector rather than buttons, as
    /// Sega Rally's and Daytona's do through MAME's daytona_gearbox_r.
    bool gearbox = false;

    /// True when the title latches bytes to a force-feedback drive board on the
    /// I/O controller's port E.
    bool drive_board = false;

    /// True when the set is known not to run yet, so the loader can warn.
    bool preliminary = false;

    std::vector<RegionSpec> regions;

    [[nodiscard]] const RegionSpec* region(const std::string& region_name) const;
};

}  // namespace sm2::rom
