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

#include <algorithm>
#include <array>
#include <string>
#include <vector>

// The ROM database data model, loaded from data/games.xml. A region is a flat
// byte array, and each file contributes `chunk` bytes every `stride` bytes
// starting at `offset`, which expresses every interleaving the boards use.

namespace sm2::rom {

/// Which board, which decides the CPUs, coprocessor and sound hardware.
enum class Board {
    Model2,   ///< Original. TGP + Model 1 sound board.
    Model2A,  ///< 2A-CRX. TGP + 68000/SCSP.
    Model2B,  ///< 2B-CRX. ADSP-21062 SHARC + 68000/SCSP.
    Model2C,  ///< 2C-CRX. MB86235 TGPx4 + 68000/SCSP.
    Model1,   ///< V60 + TGP + Model 1 sound board.
    Model3,   ///< PowerPC + Real3D Pro-1000. The stepping is in GameSpec::model3.
};

[[nodiscard]] const char* board_name(Board board);

/// True for boards that can construct a working machine. Consulted by both
/// `RomLoader::load` and `hw::create_machine`.
[[nodiscard]] bool board_implemented(Board board);

/// Logical input groups a game uses. Bits 7 and up are Model 3 groups.
enum class InputFlags : u32 {
    None      = 0,
    Common    = 1U << 0,  ///< Coins, service, test, start.
    Joystick1 = 1U << 1,
    Joystick2 = 1U << 2,
    Buttons3  = 1U << 3,  ///< Three attack buttons per player (fighting games).
    Vehicle   = 1U << 4,  ///< Steering, pedals, gear shift.
    Gun1      = 1U << 5,  ///< Light gun, player 1 ("gun1").
    Gun2      = 1U << 6,  ///< Light gun, player 2 ("gun2").

    Fighting       = 1U << 7,   ///< "fighting": punch, kick, guard, escape per player
                                ///< (Virtua Fighter 3, Fighting Vipers 2).
    Shift4         = 1U << 8,   ///< "shift4": four-speed H-gate shifter.
    ShiftUpDown    = 1U << 9,   ///< "shiftupdown": sequential shift up / down.
    Vr4            = 1U << 10,  ///< "vr4": four VR view buttons (Daytona USA 2, Scud Race).
    ViewChange     = 1U << 11,  ///< "viewchange": a single view-change button.
    Handbrake      = 1U << 12,  ///< "handbrake": Sega Rally 2's handbrake.
    Soccer         = 1U << 13,  ///< "soccer": short pass, long pass, shoot (Virtua Striker 2).
    TwinJoysticks  = 1U << 14,  ///< "twin_joysticks": Virtual On's two sticks, each
                                ///< with its turbo and shot buttons.
    AnalogJoystick = 1U << 15,  ///< "analog_joystick": analogue flight stick with
                                ///< trigger and event buttons (Star Wars Trilogy).
    AnalogGun1     = 1U << 16,  ///< "analog_gun1": a positional gun read through the
                                ///< analogue inputs, player 1 (Lost World, Ocean Hunter).
    AnalogGun2     = 1U << 17,  ///< "analog_gun2": the same for player 2.
    Fishing        = 1U << 18,  ///< "fishing": reel, rod lean and cast (Sega Bass Fishing).
    Ski            = 1U << 19,  ///< "ski": Ski Champ's platform lean and swing, pole buttons.
    MagTruck       = 1U << 20,  ///< "magtruck": Magical Truck Adventure's handles.
    Harley         = 1U << 21,  ///< "harley": Harley-Davidson's handlebar, throttle,
                                ///< brake, shifter and music selector.
    SpikeOut       = 1U << 22,  ///< "spikeout": shift, beat, charge, jump per player.
};

[[nodiscard]] constexpr InputFlags operator|(InputFlags a, InputFlags b)
{
    return static_cast<InputFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has_input(InputFlags set, InputFlags wanted)
{
    return (static_cast<u32>(set) & static_cast<u32>(wanted)) != 0;
}

/// A control wired to one channel of the I/O controller's analogue mux, after
/// MAME's ioport names. Kept distinct rather than as numbered axes because
/// controls rest differently and their channel varies between titles.
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

/// One 10-bit axis of the lightgun interface board (RS-422 channel 2). Each
/// title calibrates its own travel, so the range is per game.
struct LightgunAxis {
    u16 minimum = 0;
    u16 maximum = 0x3ff;
    u16 rest    = 0x200;
};

/// An X axis for widescreen: the travel extended by `columns` pixels' worth on
/// each side, so an aim beside the 4:3 screen reads as a position, not off screen.
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

/// One piece of game state published as a cabinet output (see osd/outputs.h),
/// such as a shot fired, read from the program's variables. The addresses
/// belong to one revision: a clone does not inherit its parent's outputs.
struct OutputSpec {
    enum class Kind : u8 {
        Lamp,   ///< One bit of the lamp latch, under another name.
        Value,  ///< A number read from work RAM.
        Flag,   ///< 1 while `source` is above zero.
        Pulse,  ///< 1 for `frames` frames each time `source` falls (or rises).
        Input,  ///< 1 while an input bit is pressed (active low).
    };

    enum class Type : u8 { U8, U16, S16, U32, Float };

    std::string name;
    Kind        kind = Kind::Value;

    u8 bit  = 0;  ///< Lamp: the latch bit. Input: the port bit.
    u8 port = 0;  ///< Input: 0 for IN0, 1 for IN1, 2 for IN2.

    /// Value: where it is and how wide. Little-endian, as the i960 stores it.
    /// A signed or floating-point value below zero is published as zero.
    u32  address = 0;
    Type type    = Type::U8;

    /// A condition on a Value: the byte at `address`, masked, must equal
    /// `value`, or with `any` be non-zero.
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

/// How widescreen fills the side columns.
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

/// A protection chip a title's main board needs, one value per chip and mode.
enum class Protection {
    None,
    Sega315_5838_Doa,  ///< 315-5838/317-0229 compression chip, DOA hack mode.
    Sega315_5881,      ///< 315-5881 stream cipher. Needs `protection_key`.
    DaytonaMaxxPic,    ///< Fan-hack's PIC simulation, MAME's model2o_maxx_state::maxx_r.
    DaytonaGtxBank,    ///< Fan-hack's banked data ROM, MAME's model2o_gtx_state::gtx_r.
};

/// Sega Model 3 board stepping. Each step is a different CPU board: the
/// PowerPC's clock, the Real3D chip revision and parts of the memory map differ.
enum class Model3Step : u8 {
    Step10,  ///< PPC603e, 66 MHz.
    Step15,  ///< PPC603e, 100 MHz.
    Step20,  ///< PPC603ev, 166 MHz.
    Step21,  ///< PPC603ev, 166 MHz, revised security and Real3D.
};

/// The MPEG music board a Model 3 cabinet carries, if any.
enum class Model3MpegBoard : u8 {
    None,
    Dsb1,  ///< Z80 Digital Sound Board.
    Dsb2,  ///< 68000 Digital Sound Board.
};

/// The cabinet's drive board, which the game talks to over the I/O board.
enum class Model3DriveBoard : u8 {
    None,
    Wheel,      ///< Force-feedback steering wheel.
    Joystick,   ///< Force-feedback flight stick (Star Wars Trilogy).
    Billboard,  ///< Lamp and display board (Virtua Striker 2, Spikeout...).
    Ski,        ///< Ski Champ's motion platform.
};

/// How the sound board's outputs reach the cabinet's speakers.
enum class Model3Audio : u8 {
    Stereo,
    QuadFrontRear,  ///< Front pair from SCSP 1, rear pair from SCSP 2.
    QuadMix,        ///< Four channels mixed down to stereo.
};

/// The <model3> element of games.xml. A clone without one takes its parent's.
struct Model3Spec {
    Model3Step       step        = Model3Step::Step10;
    Model3MpegBoard  mpeg_board  = Model3MpegBoard::None;
    Model3DriveBoard drive_board = Model3DriveBoard::None;
    Model3Audio      audio       = Model3Audio::Stereo;

    /// True when the cabinet has a network board for linked play.
    bool netboard = false;

    /// Centre a letterboxed 3D window when nothing is drawn around it.
    bool centre_letterbox = false;

    /// The PowerPC's clock for this step, in Hz.
    [[nodiscard]] u32 cpu_hz() const
    {
        switch (step) {
            case Model3Step::Step10: return 66'000'000;
            case Model3Step::Step15: return 100'000'000;
            case Model3Step::Step20: return 166'000'000;
            case Model3Step::Step21: return 166'000'000;
        }
        return 66'000'000;
    }
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

/// A word of an assembled region overwritten after loading: MAME's per-game
/// `init_` ROM patches for program bugs the hardware survived. Each patch names
/// its MAME `init_` function.
///
/// Big-endian patches (Model 3) may be any width and are applied after the
/// region's byte swap; little-endian ones are applied before it.
struct RegionPatch {
    u32  offset     = 0;      ///< Byte offset within the assembled region.
    u64  value      = 0;      ///< Replacement value, `bits` wide.
    u8   bits       = 32;     ///< 8, 16, 32 or 64.
    bool big_endian = false;  ///< Most significant byte at `offset`.
};

/// A block of an assembled region mirrored to another offset within it, as
/// MAME's `ROM_COPY`. The Model 2 ROM boards alias their last bank across the
/// window, and games read the aliases.
struct RegionCopy {
    u32 from = 0;  ///< Source offset within the assembled region.
    u32 to   = 0;  ///< Destination offset within the same region.
    u32 size = 0;
};

/// A flat byte array assembled from one or more ROM chips.
struct RegionSpec {
    std::string name;

    /// Declared size; zero derives it from the files.
    u32 size = 0;

    /// Interleaving. `stride == chunk` is a plain contiguous copy; Model 2's
    /// 32-bit regions built from pairs of 16-bit chips use stride 4, chunk 2,
    /// with the two files at offsets 0 and 2.
    u32 stride = 1;
    u32 chunk  = 1;

    /// Swap every pair of bytes after assembly, as MAME's ROM_LOAD16_WORD_SWAP.
    bool byte_swap = false;

    /// Value for bytes no chip covers. MAME's ROMREGION_ERASEFF regions, such
    /// as the texture ROMs, need 0xFF rather than zero.
    u8 fill = 0x00;

    /// A region that may legitimately be absent. Also covers regions that exist
    /// but hold no chips on this board, such as copro_data on Virtua Fighter 2.
    bool required = true;

    std::vector<FileSpec> files;

    /// Applied after every file, in declaration order as in MAME, so a copy may
    /// read bytes an earlier copy wrote.
    std::vector<RegionCopy> copies;

    /// Applied last, after the copies, as MAME's driver init runs.
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

    /// Key for the 315-5881, matching MAME's ROM_PARAMETER.
    u32 protection_key = 0;

    /// Bit of IN0 that IPT_START1 sits on: 0x10 by default, 0x40 for titles that
    /// remap it (Sky Target, Manx TT, Indy 500, ...). Coin1 is always 0x01.
    u8 start1_bit = 0x10;

    /// Where WheelRole::Button1..4 sit in the input ports, as {port 0..2,
    /// active-low mask}. Default: player one's buttons on IN1; Daytona puts
    /// VR1..3 on IN0.
    std::array<std::pair<u8, u8>, 4> wheel_button_bits = {{
        {1, 0x01}, {1, 0x02}, {1, 0x04}, {1, 0x08},
    }};

    /// True when the title declared `vr_buttons`.
    bool vr_buttons_declared = false;
    u8   vr_button_count      = 0;  ///< How many VR entries were declared (1..4).

    /// How this title's machine config wires the analogue mux. Index is the
    /// channel number; an entry with control None is an unconnected channel.
    std::array<AnalogChannel, 8> analog{};

    /// True when the title has a self-centring steering-type control (a wheel,
    /// a bike's lean, or a jetski's handlebar).
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
    u32 start_gear = 1;

    /// Wait cycles per texture RAM access. Virtual On's stage briefing lasts as
    /// long as its texture upload, which is slower on the cabinet. 0 = none.
    u32 texture_wait = 0;

    /// In widescreen, which side-column pixels stay black rather than mirror
    /// the screen edge: none, all of them, or only menu panels; or keep the
    /// game 4:3 altogether (a 2D cockpit frame around the 3D). Model 3:
    /// anything but mirror keeps the 2D layers 4:3, pillarbox the 3D too.
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

    /// True when shifting is two momentary buttons on IN1 bits 0x10 and 0x20
    /// rather than the 0x70 gate (Indy 500, Manx TT).
    bool shift_buttons = false;

    /// True when the title latches bytes to a force-feedback drive board on the
    /// I/O controller's port E.
    bool drive_board = false;

    /// How to read the bytes latched to the drive board.
    DriveProtocol drive_protocol = DriveProtocol::Daytona;

    /// True when the positional-gun cabinet has a Missile button (Behind Enemy
    /// Lines: IN1 bits 0x10/0x20) rather than an off-screen reload.
    bool gun_missile = false;

    /// True when the title will not start until its motion base answers on the
    /// I/O controller's ports D and E (Rail Chase 2). Acknowledged as MAME does.
    bool motion_base = false;

    /// Only meaningful when board == Board::Model3.
    Model3Spec model3;

    /// True when the set is known not to run yet, so the loader can warn.
    bool preliminary = false;

    /// MAME device ROM sets this title needs, such as `model1io`, found beside
    /// the game's archive like a clone's parent.
    std::vector<std::string> device_sets;

    std::vector<RegionSpec> regions;

    [[nodiscard]] const RegionSpec* region(const std::string& region_name) const;
};

}  // namespace sm2::rom
