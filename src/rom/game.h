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

#include <algorithm>
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
/// hardware. All four are implemented.
enum class Board {
    Model2,   ///< Original. TGP + Model 1 sound board.
    Model2A,  ///< 2A-CRX. TGP + 68000/SCSP.
    Model2B,  ///< 2B-CRX. ADSP-21062 SHARC + 68000/SCSP.
    Model2C,  ///< 2C-CRX. MB86235 TGPx4 + 68000/SCSP.
    Model1,   ///< V60 + TGP + Model 1 sound board.
};

[[nodiscard]] const char* board_name(Board board);

/// True for board variants that can construct a working machine. Both
/// `RomLoader::load` and `hw::create_machine` consult this one function so the
/// two cannot drift as board support is added.
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
    Stick2X,    ///< A second player's stick: the Star Wars gunner.
    Stick2Y,
};

/// The raw value a positional gun sends when it aims at one place across the
/// picture (0 = left or top, 1 = right or bottom).
struct AnalogPoint {
    float at    = 0.0f;
    u8    value = 0;
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

    /// Optional, for a positional gun: its value at places across the picture,
    /// in increasing `at` order. When present they replace the straight line
    /// from minimum to maximum.
    std::array<AnalogPoint, 16> points{};
    u8                          point_count = 0;
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

/// An X axis for widescreen: the travel the screen's `width` pixels span,
/// extended by `columns` pixels' worth on each side within the board's 10 bits,
/// so an aim beside the screen reads as a position past its edge rather than
/// as off screen.
[[nodiscard]] inline LightgunAxis widened_axis(const LightgunAxis& axis, u32 columns, u32 width)
{
    const u32    span  = axis.maximum > axis.minimum ? axis.maximum - axis.minimum : 0;
    const u32    extra = (span * columns + width / 2) / width;
    LightgunAxis out   = axis;
    out.minimum = static_cast<u16>(axis.minimum > extra ? axis.minimum - extra : 0);
    out.maximum = static_cast<u16>(std::min<u32>(0x3ff, axis.maximum + extra));
    return out;
}

/// The four axes of a two-gun cabinet, in the order the interface board's mux
/// presents them: P1 Y, P1 X, P2 Y, P2 X.
struct LightgunSpec {
    bool         present = false;
    LightgunAxis p1y;
    LightgunAxis p1x;
    LightgunAxis p2y;
    LightgunAxis p2x;
};

/// One piece of game state published as a cabinet output (see osd/outputs.h).
///
/// The lamp latch says what the cabinet lit. It does not say that a shot was
/// fired or a life lost, which is what a recoil solenoid or a rumble motor
/// wants: a Model 2 gun cabinet has no recoil of its own to latch. Those live
/// in the program's own variables, so a title lists where they are and the
/// names to publish them under -- the ones DemulShooter established for these
/// games, which the game files of MAMEHooker and Hook Of The Reaper expect.
///
/// The addresses are the program's, inside work RAM, and belong to one
/// revision: a clone does not inherit its parent's outputs.
struct OutputSpec {
    enum class Kind : u8 {
        Lamp,   ///< One bit of the lamp latch, under another name.
        Value,  ///< A number read from work RAM.
        Flag,   ///< 1 while `source` is above zero.
        Pulse,  ///< 1 for `frames` frames each time `source` falls (or rises).
    };

    enum class Type : u8 { U8, U16, S16, U32, Float };

    std::string name;
    Kind        kind = Kind::Value;

    u8 bit = 0;  ///< Lamp: the latch bit.

    /// Value: where it is and how wide. Little-endian, as the i960 stores it.
    /// A signed or floating-point value below zero is published as zero.
    u32  address = 0;
    Type type    = Type::U8;

    /// A condition on a Value, for a variable that only means something while
    /// its player is in the game. The byte at `address`, masked, must equal
    /// `value` -- or, with `any`, be anything but zero.
    struct When {
        u32  address = 0;
        u32  mask    = 0xff;
        u32  value   = 0;
        bool any     = false;
    };

    /// Value: every one of these must hold. While one does not, the value is
    /// published as zero and no pulse fires from it.
    std::vector<When> when;

    /// Flag and Pulse: index of the Value they follow, earlier in the list.
    u32  source = 0;
    bool rising = false;  ///< Pulse: fire when the source goes up, not down.
    u32  frames = 3;      ///< Pulse: how long it stays at 1.
};

/// A protection chip a title's main board needs. One value per chip/mode
/// combination rather than a child element, since so far every title needing
/// one needs exactly one fixed configuration of it apart from the 315-5881's
/// per-title key, which rides along in `GameSpec::protection_key`.
enum class SideFill : u8 {
    Mirror,     ///< Mirror the screen edge where nothing better is known.
    Black,      ///< Leave those pixels black.
    Menu,       ///< Black only for a row-scroll layer's foreground tiles,
                ///< which is how some games build their menu panels.
    Pillarbox,  ///< Keep the whole picture 4:3 with black side bars.
};

/// Which command set a title's force-feedback drive board speaks.
enum class DriveProtocol : u8 {
    Daytona,  ///< Daytona's board (EPR-16488A): it works out the forces itself.
    Indy,     ///< Indy 500's board (EPR-18261), Touring Car's, Over Rev's, Super GT's.
    Rally,    ///< Sega Rally's own board program.
    Vr,       ///< Virtua Racing's, a Daytona board taking 0..15 strengths.
};

enum class Protection {
    None,
    Sega315_5838_Doa,  ///< 315-5838/317-0229 compression chip, DOA hack mode.
    Sega315_5881,      ///< 315-5881 stream cipher. Needs `protection_key`.
    DaytonaMaxxPic,    ///< Fan-hack's PIC simulation, MAME's model2o_maxx_state::maxx_r.
    DaytonaGtxBank,    ///< Fan-hack's banked data ROM, MAME's model2o_gtx_state::gtx_r.
};

/// One ROM chip's contribution to a region.
struct FileSpec {
    std::string name;             ///< Filename inside the archive.
    u32         offset  = 0;      ///< Destination byte offset within the region.
    u32         crc32   = 0;      ///< Expected CRC32.
    bool        has_crc = false;  ///< False means identify by name alone.
    u32         alt_crc = 0;      ///< An older dump the region's patches repair.
    bool        has_alt_crc = false;
    u32         stride  = 0;      ///< Overrides the region's when non-zero.
    u32         chunk   = 0;
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

    /// Where the four generic wheel action buttons (WheelRole::Button1..4, which
    /// a driving cabinet's view/VR buttons land on) sit in the input ports, as
    /// {port index 0..2, active-low bit mask}. The default is player one's four
    /// arcade buttons on IN1 (0x01..0x08); a driving title whose view buttons are
    /// wired elsewhere overrides it. Daytona, for one, puts VR1..3 on IN0 and VR4
    /// on IN1.
    std::array<std::pair<u8, u8>, 4> wheel_button_bits = {{
        {1, 0x01}, {1, 0x02}, {1, 0x04}, {1, 0x08},
    }};

    /// True when the title declared `vr_buttons`
    bool vr_buttons_declared = false;
    u8   vr_button_count      = 0;  ///< How many VR entries were declared (1..4).

    /// How this title's machine config wires the analogue mux. Index is the
    /// channel number; an entry with control None is an unconnected channel.
    std::array<AnalogChannel, 8> analog{};

    /// True when the title has a self-centring steering-type control (a wheel,
    /// a bike's lean, or a jetski's handlebar). The wheel's synthesised centring
    /// spring and rumble apply to these, independent of any force-feedback drive
    /// board -- a bike had a motion base, not a force wheel, but a wheel player
    /// still wants the feel.
    [[nodiscard]] bool has_steering() const
    {
        for (const AnalogChannel& c : analog) {
            if (c.control == AnalogControl::Steer || c.control == AnalogControl::Bank
                || c.control == AnalogControl::Handle) {
                return true;
            }
        }
        return false;
    }

    /// The serial lightgun interface board, when the cabinet has one.
    LightgunSpec lightgun;

    /// Game state published as outputs next to the lamps; empty for most titles.
    std::vector<OutputSpec> outputs;

    /// True when IN1 bits 0x70 read a gear selector rather than buttons, as
    /// Sega Rally's and Daytona's do through MAME's daytona_gearbox_r.
    bool gearbox = false;

    /// Gate position a paddle shifter starts in, 0 = neutral and 1..4 the gears.
    /// Daytona's rolling start hands over a car already at speed and wants a high
    /// gear; Sega Rally counts down from a standstill and wants 1st.
    u32 start_gear = 1;

    /// Wait cycles per texture RAM access. Virtual On's stage briefing lasts as
    /// long as its texture upload, which is slower on the cabinet. 0 = none.
    u32 texture_wait = 0;

    /// In widescreen, which side-column pixels stay black rather than mirror
    /// the screen edge: none, all of them, or only menu panels; or keep the
    /// game 4:3 altogether (a 2D cockpit frame around the 3D).
    SideFill side_fill = SideFill::Mirror;

    /// In widescreen, the screen columns the HUD splits at: what lies left of
    /// the first moves to the left edge, right of the second to the right
    /// edge. Zero for a HUD that stays on the 4:3 screen. `hud_layers` is a
    /// bit per tile layer the HUD is drawn on.
    u16 hud_spread_left  = 0;
    u16 hud_spread_right = 0;
    u8  hud_layers       = 0;

    /// The spread HUD's 2D bounds per band of 48 rows, two bands to a word:
    /// the column the left-hand HUD reaches to and the one the right-hand HUD
    /// starts at, both halved, 8 bits each; 0 where that side has none.
    std::array<u32, 4> hud_bands{};

    /// True when shifting is two momentary buttons (shift up / shift down) on
    /// IN1 bits 0x10 and 0x20 rather than the 0x70 sequential gate. Indy 500 and
    /// Manx TT and everything inheriting from them wire it this way. The GearUp/
    /// GearDown wheel roles drive those bits directly, no gear-position state.
    bool shift_buttons = false;

    /// True when the title latches bytes to a force-feedback drive board on the
    /// I/O controller's port E.
    bool drive_board = false;

    /// How to read the bytes latched to the drive board.
    DriveProtocol drive_protocol = DriveProtocol::Daytona;

    /// True when the positional-gun cabinet has a separate Missile button
    /// (Behind Enemy Lines: P1 on IN1 bit 0x10, P2 on 0x20) rather than an
    /// off-screen reload. The second mouse button fires the missile instead of
    /// acting as a reload. From MAME's bel input ports.
    bool gun_missile = false;

    /// True when the cabinet's motion base answers on the I/O controller's ports
    /// D and E and the title refuses to start until it does.
    ///
    /// Rail Chase 2's is the only one in scope. MAME does not emulate its board
    /// either: `rchase2_drive_board_r`/`_w` acknowledge four commands and no
    /// more, with the comment "simulate this so that it passes the initial
    /// checks". Without the acknowledgement the title stops on a diagram of the
    /// main and drive boards reading NET_BAD.
    bool motion_base = false;

    /// True when the set is known not to run yet, so the loader can warn.
    bool preliminary = false;

    /// Names of MAME device ROM sets this title needs alongside its own chips.
    ///
    /// Some boards carry firmware that belongs to a device rather than to a
    /// game, so MAME ships it as its own set and every title using that device
    /// shares it: the original Model 2's I/O board is `model1io` (Desert Tank,
    /// Daytona) or `model1io2` (Virtua Cop), each holding a Z80 program. The
    /// loader looks for `<name>.zip` or `<name>.7z` beside the game's archive,
    /// the same way it finds a clone's parent.
    std::vector<std::string> device_sets;

    std::vector<RegionSpec> regions;

    [[nodiscard]] const RegionSpec* region(const std::string& region_name) const;
};

}  // namespace sm2::rom
