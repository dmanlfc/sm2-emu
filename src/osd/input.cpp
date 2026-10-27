//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "osd/input.h"

#include "core/archive.h"
#include "core/log.h"
#include "hw/model2.h"
#include "osd/drive_command.h"
#include "osd/wheel_ffb.h"

#ifdef SM2_HAVE_LIGHTGUNS
#include "osd/light_guns.h"
#endif

#include "render/geometry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace sm2::osd {
namespace {

constexpr u8 kCoin1   = Input::kCoin1;
constexpr u8 kCoin2   = Input::kCoin2;
constexpr u8 kTest    = Input::kTest;
constexpr u8 kService = Input::kService;
constexpr u8 kStart1  = Input::kStart1;
constexpr u8 kStart2  = Input::kStart2;

constexpr u8 kButton1 = Input::kButton1;
constexpr u8 kButton2 = Input::kButton2;
constexpr u8 kButton3 = Input::kButton3;
constexpr u8 kButton4 = Input::kButton4;
constexpr u8 kDown    = Input::kDown;
constexpr u8 kUp      = Input::kUp;
constexpr u8 kRight   = Input::kRight;
constexpr u8 kLeft    = Input::kLeft;

/// Which way a push turns the wheel: +1 left, -1 right. A kick pushes away
/// from the centre. Like the board, it counts the wheel as right of centre
/// once it is two steps of the 8-bit pot over, and pushes left anywhere else.
int push_direction(const DriveCommand& command, int deflection)
{
    constexpr int kRightOfCentre = 2 * 256;
    switch (command.effect) {
        case DriveCommand::Effect::PushLeft:  return 1;
        case DriveCommand::Effect::PushRight: return -1;
        case DriveCommand::Effect::Kick:      return deflection > kRightOfCentre ? -1 : 1;
        default:                              return 0;
    }
}

// Which host axis each logical control reads. The channel a control occupies is
// the game's business (rom::GameSpec::analog); this table is only about how a
// gamepad stands in for the cabinet's own hardware.
//
// Two shapes cover everything Model 2 uses: a self-centring axis (wheel,
// handlebars, stick, motion platform) and a pedal, which rests at one end of its
// travel. Pedals go on the triggers so both can be held at once, which a driving
// game expects.
enum class HostAxis : u8 {
    None,
    PadLeftX,       ///< Player 1's left stick, horizontal.
    PadLeftY,
    Pad2LeftX,      ///< Player 2's pad, for the twin-stick cabinets.
    Pad2LeftY,
    RightTrigger,   ///< Pedal.
    LeftTrigger,    ///< Pedal.
};

struct ControlBinding {
    rom::AnalogControl control;
    HostAxis           axis;
};

constexpr ControlBinding kControlBindings[] = {
    {rom::AnalogControl::Steer,     HostAxis::PadLeftX},
    {rom::AnalogControl::Bank,      HostAxis::PadLeftX},
    {rom::AnalogControl::Handle,    HostAxis::PadLeftX},
    {rom::AnalogControl::StickX,    HostAxis::PadLeftX},
    {rom::AnalogControl::Gun1X,     HostAxis::PadLeftX},
    {rom::AnalogControl::Curving,   HostAxis::PadLeftX},
    {rom::AnalogControl::Slide,     HostAxis::PadLeftX},
    {rom::AnalogControl::Roll,      HostAxis::PadLeftX},
    {rom::AnalogControl::Inclining, HostAxis::PadLeftX},

    {rom::AnalogControl::StickY,    HostAxis::PadLeftY},
    {rom::AnalogControl::Gun1Y,     HostAxis::PadLeftY},
    {rom::AnalogControl::Pitch,     HostAxis::PadLeftY},
    {rom::AnalogControl::Swing,     HostAxis::PadLeftY},

    {rom::AnalogControl::Gun2X,     HostAxis::Pad2LeftX},
    {rom::AnalogControl::Gun2Y,     HostAxis::Pad2LeftY},

    {rom::AnalogControl::Accel,     HostAxis::RightTrigger},
    {rom::AnalogControl::Throttle,  HostAxis::RightTrigger},
    {rom::AnalogControl::Bat1,      HostAxis::RightTrigger},
    {rom::AnalogControl::Brake,     HostAxis::LeftTrigger},
    {rom::AnalogControl::Bat2,      HostAxis::LeftTrigger},
};

// A released stick can settle off centre, so read the deadzone as centre and
// rescale beyond it. curve 1 is linear; response bends the middle of the
// travel without moving its ends, so full deflection is always full lock.
[[nodiscard]] float centred_fraction(s16 raw, float centre_dead, float curve, float response)
{
    const float value = std::max(static_cast<float>(raw), -32767.0f);
    const float past  = std::abs(value) - centre_dead;
    if (past <= 0.0f) {
        return 0.5f;
    }
    const float travel = std::pow(past / (32767.0f - centre_dead), curve / response);
    return 0.5f + std::copysign(travel, value) * 0.5f;
}

[[nodiscard]] HostAxis host_axis_for(rom::AnalogControl control)
{
    for (const ControlBinding& binding : kControlBindings) {
        if (binding.control == control) {
            return binding.axis;
        }
    }
    return HostAxis::None;
}

/// True for an axis that rests at one end of its travel rather than centred.
[[nodiscard]] bool is_pedal(HostAxis axis)
{
    return axis == HostAxis::RightTrigger || axis == HostAxis::LeftTrigger;
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

struct KeyBinding {
    SDL_Scancode key;
    u8           bit;
};

constexpr KeyBinding kOperatorKeys[] = {
    {SDL_SCANCODE_5, kCoin1},   {SDL_SCANCODE_6, kCoin2},
    {SDL_SCANCODE_1, kStart1},  {SDL_SCANCODE_2, kStart2},
    {SDL_SCANCODE_9, kService}, {SDL_SCANCODE_0, kTest},
};

constexpr KeyBinding kPlayerOneKeys[] = {
    {SDL_SCANCODE_LEFT, kLeft}, {SDL_SCANCODE_RIGHT, kRight},
    {SDL_SCANCODE_UP, kUp},     {SDL_SCANCODE_DOWN, kDown},
    {SDL_SCANCODE_Z, kButton1}, {SDL_SCANCODE_X, kButton2},
    {SDL_SCANCODE_C, kButton3}, {SDL_SCANCODE_V, kButton4},
};

constexpr KeyBinding kPlayerTwoKeys[] = {
    {SDL_SCANCODE_A, kLeft},    {SDL_SCANCODE_D, kRight},
    {SDL_SCANCODE_W, kUp},      {SDL_SCANCODE_S, kDown},
    {SDL_SCANCODE_G, kButton1}, {SDL_SCANCODE_H, kButton2},
    {SDL_SCANCODE_J, kButton3}, {SDL_SCANCODE_K, kButton4},
};

/// Pull a port's bits low for every held key.
template <usize Count>
void gather_keys(u8* port, const bool* keys, int key_count,
                 const KeyBinding (&bindings)[Count])
{
    for (const KeyBinding& binding : bindings) {
        if (static_cast<int>(binding.key) < key_count && keys[binding.key]) {
            *port &= static_cast<u8>(~binding.bit);
        }
    }
}

// ---------------------------------------------------------------------------
// Gamepad
// ---------------------------------------------------------------------------

/// Half travel. Wide enough that a resting stick stays quiet and the diagonal band
/// is comfortable to hold.
constexpr int kStickThreshold = 16384;

/// A pedal rests at zero, so it needs no centre band, only a floor under the noise.
constexpr int kPedalFloor = 1024;

/// How far a trigger is pulled before it fires a gun.
constexpr int kTriggerPress = 8000;

/// Lets a wheel that also carries a gamepad mapping win the device over the gamepad path.
bool is_wheel_typed(SDL_JoystickID id)
{
    return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL;
}

/// Light guns also expose a joystick interface, its axes the aim.
bool is_gun_named(SDL_JoystickID id)
{
    const char* name = SDL_GetJoystickNameForID(id);
    if (name == nullptr) {
        return false;
    }
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("gun") != std::string::npos || lower.find("aimtrak") != std::string::npos;
}

/// A simulated panel spring: zero in a small deadzone, rising to `full` part
/// way to lock. Signed like the deflection.
int centring_spring(int deflection, int full)
{
    constexpr int kDeadzone = 1500;
    constexpr int kSpan     = 14000;
    const int     m         = std::abs(deflection);
    if (m <= kDeadzone) {
        return 0;
    }
    const int force = full * std::min(m - kDeadzone, kSpan) / kSpan;
    return deflection < 0 ? -force : force;
}

// The Sega Rally / Daytona board's interrupt rate. It has not been measured,
// so it is taken as the 8 MHz crystal divided by 8192.
constexpr double kDriveBoardIrqHz = 1000.0;
constexpr double kMachineHz       = 57.5245;

/// Sega Rally's start-up calibration finds the power that holds the wheel this
/// many counts (out of 128) off centre each way, against the panel spring.
constexpr int kRallyCalibCountsRight = 30;
constexpr int kRallyCalibCountsLeft  = 32;

/// Daytona's calibration does the same at 16 counts each way, and keeps the
/// mean of the two powers minus one.
constexpr int kDaytonaCalibCounts = 16;

/// The drive boards' full motor power, 6 bits: at a gain of 100 % it gives the
/// wheel's full force.
constexpr int kBoardFullRange = 63;

/// Sega Rally's strongest torque: the power of a command at full strength.
constexpr int kBoardFullPower = 32;

/// Converts the wheel's axis to board ADC counts off centre, 128 a side.
int board_counts(int deflection, u32 steer_degrees, u32 lock_degrees)
{
    const long long steer = std::max(1u, steer_degrees);
    const long long lock  = std::max(1u, lock_degrees);
    return static_cast<int>(std::clamp(deflection * 128LL * steer / lock / 32767, -128LL, 128LL));
}

/// Converts board ADC counts back to the wheel's axis: the inverse of
/// board_counts.
int board_deflection(int counts, u32 steer_degrees, u32 lock_degrees)
{
    const long long steer = std::max(1u, steer_degrees);
    const long long lock  = std::max(1u, lock_degrees);
    return static_cast<int>(std::min(32767LL * counts / 128 * lock / steer, 32767LL));
}

}  // namespace

u8 Input::stick_bits(s16 x, s16 y)
{
    u8 bits = 0;
    if (x <= -kStickThreshold) {
        bits |= kLeft;
    }
    if (x >= kStickThreshold) {
        bits |= kRight;
    }
    // SDL's y axis grows downwards, as the screen's does, and the cabinet's "up" is
    // the stick pushed away from the player.
    if (y <= -kStickThreshold) {
        bits |= kUp;
    }
    if (y >= kStickThreshold) {
        bits |= kDown;
    }
    return bits;
}

u8 Input::axis_to_centred(s16 value)
{
    return static_cast<u8>((static_cast<int>(value) + 32768) >> 8);
}

u8 Input::axis_to_pedal(s16 value)
{
    const int travelled = std::max(0, static_cast<int>(value) - kPedalFloor);
    const int scaled    = (travelled * 255) / (32767 - kPedalFloor);
    return static_cast<u8>(std::min(scaled, 255));
}

/// Linux KEY_CONFIG, written out so the Windows build has it too.
constexpr u16 kKeyConfig = 0x171;

/// Scale a screen fraction into one lightgun axis's calibrated travel.
///
/// The gun board is 10-bit and each title calibrates its own travel, so an edge
/// fraction must land on that title's own min/max rather than 0 or 0x3ff, or the
/// crosshair is misplaced and the offscreen test fires early. The axis ends mark
/// the gun as off screen, so the picture maps one step inside them and only an
/// aim past its edge reaches them. Both the mouse pointer and an evdev gun feed
/// through here so the two sources agree.
[[nodiscard]] u16 fraction_to_gun(float fraction, const rom::LightgunAxis& axis)
{
    if (fraction < 0.0f) {
        return axis.minimum;
    }
    if (fraction > 1.0f) {
        return axis.maximum;
    }
    const float span = static_cast<float>(axis.maximum - axis.minimum - 2);
    return static_cast<u16>(static_cast<float>(axis.minimum + 1) + fraction * span + 0.5f);
}

[[nodiscard]] u16 mouse_to_gun(float position, int extent, const rom::LightgunAxis& axis)
{
    if (extent <= 1) {
        return axis.rest;
    }
    const float maximum = static_cast<float>(extent - 1);
    return fraction_to_gun(position / maximum, axis);
}

/// Mouse position in the focused window, that window's size, and its buttons.
struct PointerState {
    float x       = 0.0f;
    float y       = 0.0f;
    int   width   = 0;
    int   height  = 0;
    bool  left    = false;  ///< left button held (gun trigger in mouse mode).
    bool  right   = false;  ///< right button held (reload / off-screen shot).
};

[[nodiscard]] PointerState pointer_state()
{
    PointerState     state;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&state.x, &state.y);
    state.left  = (buttons & SDL_BUTTON_LMASK) != 0;
    state.right = (buttons & SDL_BUTTON_RMASK) != 0;
    if (SDL_Window* focus = SDL_GetMouseFocus(); focus != nullptr) {
        // The renderer computes its letterbox from the window's pixel size, and
        // the gun coordinate is mapped against that same letterbox, so the
        // pointer and its extent must be in pixels too. SDL_GetMouseState reports
        // logical units; on a HiDPI or fullscreen surface those differ from
        // pixels, which otherwise scales the aim to the wrong width. Convert the
        // pointer to pixels and take the pixel size for the extent.
        int logical_w = 0;
        int logical_h = 0;
        SDL_GetWindowSize(focus, &logical_w, &logical_h);
        SDL_GetWindowSizeInPixels(focus, &state.width, &state.height);
        if (logical_w > 0 && logical_h > 0) {
            state.x *= static_cast<float>(state.width) / static_cast<float>(logical_w);
            state.y *= static_cast<float>(state.height) / static_cast<float>(logical_h);
        }
    }
    return state;
}

Input::Input() = default;

Input::~Input()
{
    shutdown();
}

bool Input::init(const WheelSettings& wheel)
{
    m_wheel_settings = wheel;

    // GAMEPAD implies JOYSTICK; HAPTIC is separate and only needed for a wheel's
    // force feedback, so a failure there is not fatal.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SM2_ERROR("SDL_InitSubSystem(GAMEPAD) failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        SM2_WARN("SDL_InitSubSystem(HAPTIC) failed, no force feedback: %s", SDL_GetError());
    }
    m_started = true;

    // Devices already plugged in do not generate connection events, so they have
    // to be collected once at startup; the two lists are walked separately, same
    // routing as handle_event()'s SDL_EVENT_GAMEPAD_ADDED/JOYSTICK_ADDED cases.
    int             count = 0;
    SDL_JoystickID* ids   = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        // The pads asked for by path go first, so the others cannot take their slots.
        const auto asked_for = [this](SDL_JoystickID id) {
            const char* path = SDL_GetJoystickPathForID(id);
            return path != nullptr
                && std::find(m_pad_devices.begin(), m_pad_devices.end(), path) != m_pad_devices.end();
        };
        std::stable_partition(ids, ids + count, asked_for);
        for (int index = 0; index < count; ++index) {
            add_gamepad(ids[index]);
        }
        SDL_free(ids);
    }

    ids = SDL_GetJoysticks(&count);
    if (ids != nullptr) {
        for (int index = 0; index < count; ++index) {
            if (is_wheel_typed(ids[index]) || !SDL_IsGamepad(ids[index])) {
                add_wheel(ids[index]);
            }
        }
        SDL_free(ids);
    }

    if (m_pads.empty() && m_wheel.handle == nullptr) {
        SM2_INFO("no gamepad found; the keyboard covers both players");
    }

#ifdef SM2_HAVE_LIGHTGUNS
    // Open any per-device light guns. If none are present the pointer stays the
    // gun source, so this failing to find anything is not an error.
    m_guns = std::make_unique<LightGuns>();
    m_guns->set_preferred_devices(m_gun_devices);
    if (!m_guns->init() || m_guns->count() == 0) {
        m_guns.reset();
    }
#endif

    return true;
}

void Input::shutdown()
{
    for (Pad& pad : m_pads) {
        if (pad.handle != nullptr) {
            // Silence the motors, or a pad left mid-effect keeps buzzing after exit.
            SDL_RumbleGamepad(pad.handle, 0, 0, 0);
            SDL_CloseGamepad(pad.handle);
            pad.handle = nullptr;
        }
    }
    m_pads.clear();
    remove_wheel(m_wheel.id);
    if (m_started) {
        SDL_QuitSubSystem(SDL_INIT_HAPTIC);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        m_started = false;
    }
}

void Input::handle_event(const SDL_Event& event)
{
    switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            add_gamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            remove_gamepad(event.gdevice.which);
            break;
        case SDL_EVENT_JOYSTICK_ADDED:
            if (is_wheel_typed(event.jdevice.which) || !SDL_IsGamepad(event.jdevice.which)) {
                add_wheel(event.jdevice.which);
            }
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            remove_wheel(event.jdevice.which);
            break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            if (event.jaxis.which == m_wheel.id && event.jaxis.axis < Wheel::kMaxAxes) {
                m_wheel.axes_moved |= 1u << event.jaxis.axis;
            }
            break;
        default:
            break;
    }
}

bool Input::player_taken(u32 player) const
{
    return std::any_of(m_pads.begin(), m_pads.end(),
                       [player](const Pad& pad) { return pad.player == player; });
}

u32 Input::player_for(SDL_JoystickID id) const
{
    const auto path_is = [](SDL_JoystickID joystick, const std::string& want) {
        const char* path = SDL_GetJoystickPathForID(joystick);
        return !want.empty() && path != nullptr && want == path;
    };
    for (u32 player = 0; player < kPlayers; ++player) {
        if (path_is(id, m_pad_devices[player]) && !player_taken(player)) {
            return player;
        }
    }
    // Otherwise skip the slots kept for a pad that is plugged in.
    int             count = 0;
    SDL_JoystickID* ids   = SDL_GetJoysticks(&count);
    u32             found = kPlayers;
    for (u32 player = 0; player < kPlayers && found == kPlayers; ++player) {
        const bool kept = ids != nullptr && std::any_of(ids, ids + count, [&](SDL_JoystickID other) {
            return other != id && path_is(other, m_pad_devices[player]);
        });
        if (!player_taken(player) && !kept) {
            found = player;
        }
    }
    SDL_free(ids);
    return found;
}

void Input::add_gamepad(SDL_JoystickID id)
{
    if (std::any_of(m_pads.begin(), m_pads.end(),
                    [id](const Pad& pad) { return pad.id == id; })) {
        return;
    }
    if (is_wheel_typed(id)) {
        SM2_DEBUG("joystick %u type=%d treated as a wheel, not a gamepad",
                  static_cast<unsigned>(id), static_cast<int>(SDL_GetJoystickTypeForID(id)));
        return;
    }
    if (!SDL_IsGamepad(id)) {
        // A joystick SDL has no mapping for. Reporting it is worth doing, because
        // the user's device is plugged in and doing nothing.
        SM2_INFO("joystick %u has no gamepad mapping and is ignored",
                 static_cast<unsigned>(id));
        return;
    }

    const u32 player = player_for(id);
    if (player >= kPlayers) {
        SM2_INFO("both player positions are taken; the extra gamepad is idle");
        return;
    }

    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (handle == nullptr) {
        SM2_WARN("could not open gamepad %u: %s", static_cast<unsigned>(id),
                 SDL_GetError());
        return;
    }

    const char* name = SDL_GetGamepadName(handle);
    SM2_INFO("player %u: %s", player + 1, name != nullptr ? name : "gamepad");
    m_pads.push_back(Pad{handle, id, player});
}

void Input::remove_gamepad(SDL_JoystickID id)
{
    const auto found = std::find_if(m_pads.begin(), m_pads.end(),
                                    [id](const Pad& pad) { return pad.id == id; });
    if (found == m_pads.end()) {
        return;
    }

    SM2_INFO("player %u's gamepad was disconnected", found->player + 1);
    if (found->handle != nullptr) {
        SDL_RumbleGamepad(found->handle, 0, 0, 0);
        SDL_CloseGamepad(found->handle);
    }
    // The remaining pads keep their positions. Compacting would move a player
    // mid-game, and a pad that comes back finds its old slot free anyway.
    m_pads.erase(found);
}

SDL_Gamepad* Input::pad_for(u32 player) const
{
    const auto match = std::find_if(m_pads.begin(), m_pads.end(),
                                    [player](const Pad& pad) {
                                        return pad.player == player && plays(pad);
                                    });
    return match != m_pads.end() ? match->handle : nullptr;
}

void Input::add_wheel(SDL_JoystickID id)
{
    if (m_wheel.handle != nullptr) {
        return;  // One wheel, driving player one, is all a Model 2 cabinet wires.
    }

    // UNKNOWN passes: it only reaches here when nothing else claimed the device.
    // A named device skips the check, having whatever type uinput gave it.
    const SDL_JoystickType type = SDL_GetJoystickTypeForID(id);
    if (type != SDL_JOYSTICK_TYPE_WHEEL && type != SDL_JOYSTICK_TYPE_UNKNOWN) {
        return;
    }
    if (type != SDL_JOYSTICK_TYPE_WHEEL && is_gun_named(id)) {
        SM2_DEBUG("joystick %s is a light gun, not taken as a wheel",
                  SDL_GetJoystickNameForID(id));
        return;
    }

    SDL_Joystick* handle = SDL_OpenJoystick(id);
    if (handle == nullptr) {
        SM2_WARN("could not open joystick %u: %s", static_cast<unsigned>(id),
                 SDL_GetError());
        return;
    }

    // An untyped device needs steering plus a pedal axis. Keyboard and mouse
    // receivers expose a one-axis "System Control" joystick that would otherwise
    // take the driving controls from the gamepad.
    if (type != SDL_JOYSTICK_TYPE_WHEEL && SDL_GetNumJoystickAxes(handle) < 2) {
        const char* name = SDL_GetJoystickName(handle);
        SM2_DEBUG("joystick %s has %d axis, not taken as a wheel",
                  name != nullptr ? name : "?", SDL_GetNumJoystickAxes(handle));
        SDL_CloseJoystick(handle);
        return;
    }

    m_wheel            = Wheel{};
    m_wheel.handle     = handle;
    m_wheel.id         = id;
    m_wheel.can_rumble = SDL_GetBooleanProperty(SDL_GetJoystickProperties(handle),
                                                SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);

    const int axes = SDL_GetNumJoystickAxes(handle);
    for (int axis = 0; axis < axes && axis < Wheel::kMaxAxes; ++axis) {
        m_wheel.axis_rest[static_cast<usize>(axis)] = SDL_GetJoystickAxis(handle, axis);
    }
    assign_wheel_axes();

    // Force feedback is a constant force we aim ourselves each frame (see
    // update_force_feedback): the wheel's driver ignores FF_SPRING but honours
    // FF_CONSTANT, so the centring pull is computed from the wheel angle rather
    // than programmed as a spring. Started at zero level and left running.
    if (m_wheel_settings.ffb) {
        auto ffb = std::make_unique<WheelForce>();
        if (ffb->open(handle)) {
            SM2_INFO("wheel force feedback: %s%s", ffb->backend(),
                     ffb->has_rumble() ? " (with rumble)" : "");
            // Centres the wheel until our spring has a position to work from.
            ffb->set_autocenter(50);
            m_wheel.autocenter = true;
            m_wheel.ffb        = std::move(ffb);
        } else {
            SM2_INFO("wheel has no constant-force effect; using plain rumble");
        }
    }
}

void Input::set_wheel_settings(const WheelSettings& wheel)
{
    const WheelSettings& old = m_wheel_settings;
    const bool axes_changed =
        wheel.steer_axis != old.steer_axis || wheel.accel_axis != old.accel_axis
        || wheel.brake_axis != old.brake_axis || wheel.accel_invert != old.accel_invert
        || wheel.brake_invert != old.brake_invert || wheel.accel_half != old.accel_half
        || wheel.brake_half != old.brake_half;
    m_wheel_settings = wheel;
    // A calibration takes effect at once, not at the wheel's next connection.
    if (axes_changed && m_wheel.handle != nullptr) {
        assign_wheel_axes();
    }
}

void Input::assign_wheel_axes()
{
    // Each role takes its calibrated axis, else auto-detects: steering is axis 0,
    // pedals are the other axes resting at an extreme, accel then brake.
    const int axes = SDL_GetNumJoystickAxes(m_wheel.handle);
    const WheelSettings& cal = m_wheel_settings;
    m_wheel.steer_axis   = cal.steer_axis;
    m_wheel.accel_axis   = cal.accel_axis;
    m_wheel.brake_axis   = cal.brake_axis;
    m_wheel.accel_invert = cal.accel_axis >= 0 && cal.accel_invert;
    m_wheel.brake_invert = cal.brake_axis >= 0 && cal.brake_invert;
    m_wheel.accel_half   = cal.accel_axis >= 0 && cal.accel_half;
    m_wheel.brake_half   = cal.brake_axis >= 0 && cal.brake_half;

    const auto taken = [&](int axis) {
        return axis == cal.steer_axis || axis == cal.accel_axis || axis == cal.brake_axis;
    };
    if (m_wheel.steer_axis < 0 && axes > 0 && !taken(0)) {
        m_wheel.steer_axis = 0;
    }

    // The pedals that are not calibrated, in order.
    struct Pedal { int* axis; bool* invert; };
    Pedal pedals[2];
    int   wanted = 0;
    if (m_wheel.accel_axis < 0) {
        pedals[wanted++] = {&m_wheel.accel_axis, &m_wheel.accel_invert};
    }
    if (m_wheel.brake_axis < 0) {
        pedals[wanted++] = {&m_wheel.brake_axis, &m_wheel.brake_invert};
    }
    int found = 0;
    for (int axis = 1; axis < axes && found < wanted; ++axis) {
        const int rest = static_cast<int>(wheel_axis(axis));
        if (taken(axis) || axis == m_wheel.steer_axis || (rest >= -16384 && rest <= 16384)) {
            continue;
        }
        *pedals[found].axis   = axis;
        *pedals[found].invert = rest > 0;
        ++found;
    }
    if (found == 0) {
        for (int axis = 1; axis < axes && axis <= 2 && found < wanted; ++axis) {
            if (!taken(axis) && axis != m_wheel.steer_axis) {
                *pedals[found++].axis = axis;
            }
        }
    }

    const char* name = SDL_GetJoystickName(m_wheel.handle);
    SM2_INFO("wheel: %s (%d axes; steer %d accel %d(inv %d half %d) brake %d(inv %d half %d))",
             name != nullptr ? name : "steering wheel", axes,
             m_wheel.steer_axis, m_wheel.accel_axis, m_wheel.accel_invert, m_wheel.accel_half,
             m_wheel.brake_axis, m_wheel.brake_invert, m_wheel.brake_half);
}

void Input::remove_wheel(SDL_JoystickID id)
{
    if (m_wheel.handle == nullptr || m_wheel.id != id) {
        return;
    }
    if (m_wheel.ffb) {
        m_wheel.ffb->close();
    }
    SDL_RumbleJoystick(m_wheel.handle, 0, 0, 0);
    SDL_CloseJoystick(m_wheel.handle);
    m_wheel = Wheel{};
}

s16 Input::wheel_axis(int axis) const
{
    if (axis >= 0 && axis < Wheel::kMaxAxes && (m_wheel.axes_moved & (1u << axis)) == 0) {
        return m_wheel.axis_rest[static_cast<usize>(axis)];
    }
    return SDL_GetJoystickAxis(m_wheel.handle, axis);
}

float Input::pedal_travel(int axis, bool invert, bool half) const
{
    const float raw = static_cast<float>(wheel_axis(axis));
    float travel;
    if (half) {
        // Released at the centre, pressed towards one end.
        travel = invert ? -raw / 32768.0f : raw / 32767.0f;
    } else {
        // SDL normalises every axis to -32768..32767: a pedal over the whole
        // axis rests at one end and is pressed at the other.
        travel = (raw + 32768.0f) / 65535.0f;
        if (invert) {
            travel = 1.0f - travel;
        }
    }
    return std::clamp(travel, 0.0f, 1.0f);
}

bool Input::sample_wheel_channel(const rom::AnalogChannel& channel, u8* out) const
{
    if (m_wheel.handle == nullptr) {
        return false;
    }

    // Which of the wheel's axes, if any, this control reads. Only the driving
    // controls come off the wheel; anything else falls through to the gamepad.
    int axis = -1;
    switch (channel.control) {
        // Steer, and the bike's lean (Bank) and jetski's handlebar (Handle), are
        // all the self-centring "which way am I pointing" control -- the wheel
        // drives them the same way.
        case rom::AnalogControl::Steer:
        case rom::AnalogControl::Bank:
        case rom::AnalogControl::Handle:   axis = m_wheel.steer_axis; break;
        case rom::AnalogControl::Accel:
        case rom::AnalogControl::Throttle: axis = m_wheel.accel_axis; break;
        case rom::AnalogControl::Brake:    axis = m_wheel.brake_axis; break;
        default: return false;
    }
    if (axis < 0) {
        return false;
    }

    const s16 raw = wheel_axis(axis);

    const auto scaled = [&channel](float fraction) {
        const float span  = static_cast<float>(channel.maximum - channel.minimum);
        const float value = static_cast<float>(channel.minimum)
                          + std::clamp(fraction, 0.0f, 1.0f) * span;
        return static_cast<u8>(value + 0.5f);
    };

    // SDL normalises every joystick axis to -32768..32767. A wheel's steering
    // rests at 0 (centre); its pedals rest at -32768 (released) and reach 32767
    // fully pressed. So both are the same 0..1 map from the raw value -- the
    // difference is only that a pedal at rest reads 0 and a wheel at rest 0.5.
    float fraction = static_cast<float>(static_cast<int>(raw) + 32768) / 65535.0f;

    const bool is_steering = channel.control == rom::AnalogControl::Steer
                          || channel.control == rom::AnalogControl::Bank
                          || channel.control == rom::AnalogControl::Handle;
    if (is_steering) {
        // Centre the wheel
        if (axis < Wheel::kMaxAxes && (m_wheel.axes_moved & (1u << axis)) == 0) {
            fraction = 0.5f;
        }

        // The user's deadzone: read its edge as centre and rescale beyond it.
        const float dead = 0.5f * static_cast<float>(m_wheel_settings.steer_deadzone) / 100.0f;
        if (dead > 0.0f) {
            const float off  = fraction - 0.5f;
            const float past = std::abs(off) - dead;
            fraction = past <= 0.0f ? 0.5f
                                    : 0.5f + std::copysign(past / (0.5f - dead), off) * 0.5f;
        }

        // steer_degrees is the wheel's own physical rotation range; lock_degrees
        // is the physical rotation (total) at which the game reaches full lock.
        // Mapping one onto the other makes full lock arrive after turning about
        // lock_degrees, regardless of how far the wheel can spin. Scale the
        // deflection about centre and clamp; past the mapped angle the game is
        // already at full lock.
        const float scale = static_cast<float>(std::max(1u, m_wheel_settings.steer_degrees))
                          / static_cast<float>(std::max(1u, m_wheel_settings.lock_degrees));
        fraction = 0.5f + (fraction - 0.5f) * scale;
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        const u8 value = scaled(fraction);
        *out = channel.reverse
                   ? static_cast<u8>(channel.maximum - (value - channel.minimum))
                   : value;
        return true;
    }

    // A pedal: 0 is released, 1 fully pressed.
    const bool brake = channel.control == rom::AnalogControl::Brake;
    fraction = pedal_travel(axis, brake ? m_wheel.brake_invert : m_wheel.accel_invert,
                            brake ? m_wheel.brake_half : m_wheel.accel_half);

    // Map the pedal the way MAME's PORT_BIT does. A PORT_REVERSE pedal (Over
    // Rev, Super GT) reads inverted -- released at the maximum, pressed at the
    // minimum -- because the game treats the resting ADC value as idle; feeding
    // it non-reversed makes the game read a released pedal (0x00) as full
    // throttle, which is the "accelerates on its own" fault.
    const float span = static_cast<float>(channel.maximum - channel.minimum);
    const float value = channel.reverse
                            ? static_cast<float>(channel.maximum) - fraction * span
                            : static_cast<float>(channel.minimum) + fraction * span;
    *out = static_cast<u8>(value + 0.5f);
    return true;
}

u8 Input::sample_channel(const rom::AnalogChannel& channel, float curve) const
{
    // A wheel, when present, owns the driving controls; everything else, and any
    // control the wheel has no axis for, falls through to the gamepad.
    if (u8 wheel_value = 0; sample_wheel_channel(channel, &wheel_value)) {
        return wheel_value;
    }

    const HostAxis axis = host_axis_for(channel.control);
    if (axis == HostAxis::None) {
        return channel.rest;
    }

    SDL_Gamepad* pad = pad_for(axis == HostAxis::Pad2LeftX || axis == HostAxis::Pad2LeftY
                                   ? 1u
                                   : 0u);
    if (pad == nullptr) {
        // No pad for this control: hand back the value the hardware reads with
        // nobody touching it. A wheel at rest is centred and a pedal is
        // released, and the difference matters -- a game that sees full lock or
        // full throttle at boot can refuse to leave its self-test.
        return channel.rest;
    }

    // Scale into the channel's own calibrated travel rather than the full byte:
    // several titles declare a narrow PORT_MINMAX and treat anything outside it
    // as a fault or as an off-screen gun.
    const auto scaled = [&channel](float fraction) {
        const float span  = static_cast<float>(channel.maximum - channel.minimum);
        const float value = static_cast<float>(channel.minimum)
                          + std::clamp(fraction, 0.0f, 1.0f) * span;
        return static_cast<u8>(value + 0.5f);
    };

    const auto centred = [dead = m_pad_stick_dead, curve, response = m_pad_stick_response](s16 raw) {
        return centred_fraction(raw, dead, curve, response);
    };

    // The axis a role reads: the user's override, else the default. Player
    // two's aim reads its own pad.
    const u32 which_player = (axis == HostAxis::Pad2LeftX || axis == HostAxis::Pad2LeftY)
                                 ? 1u
                                 : 0u;
    const auto role_axis = [&](Config::PadAxisRole role, SDL_GamepadAxis fallback,
                               bool* inverted) {
        const s32 override_axis = m_pad_axes[which_player][static_cast<u32>(role)];
        *inverted = m_pad_axis_invert[which_player][static_cast<u32>(role)];
        return override_axis >= 0 ? static_cast<SDL_GamepadAxis>(override_axis) : fallback;
    };

    float fraction = 0.0f;
    switch (axis) {
        case HostAxis::PadLeftX:
        case HostAxis::Pad2LeftX: {
            bool inv = false;
            const SDL_GamepadAxis sdl =
                role_axis(Config::PadAxisRole::AimX, SDL_GAMEPAD_AXIS_LEFTX, &inv);
            s16 raw = SDL_GetGamepadAxis(pad, sdl);
            if (inv) raw = static_cast<s16>(-static_cast<int>(raw));
            fraction = centred(raw);
            break;
        }
        case HostAxis::PadLeftY:
        case HostAxis::Pad2LeftY: {
            bool inv = false;
            const SDL_GamepadAxis sdl =
                role_axis(Config::PadAxisRole::AimY, SDL_GAMEPAD_AXIS_LEFTY, &inv);
            s16 raw = SDL_GetGamepadAxis(pad, sdl);
            if (inv) raw = static_cast<s16>(-static_cast<int>(raw));
            fraction = centred(raw);
            break;
        }
        case HostAxis::RightTrigger:
        case HostAxis::LeftTrigger: {
            bool inv = false;
            const SDL_GamepadAxis which =
                axis == HostAxis::RightTrigger
                    ? role_axis(Config::PadAxisRole::Accel, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, &inv)
                    : role_axis(Config::PadAxisRole::Brake, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, &inv);
            int reading = static_cast<int>(SDL_GetGamepadAxis(pad, which));
            // A stick bound to a pedal rests centred, not low, so its full
            // swing is the pedal.
            const bool is_stick_axis = which == SDL_GAMEPAD_AXIS_LEFTX
                                    || which == SDL_GAMEPAD_AXIS_LEFTY
                                    || which == SDL_GAMEPAD_AXIS_RIGHTX
                                    || which == SDL_GAMEPAD_AXIS_RIGHTY;
            if (inv) {
                reading = is_stick_axis ? -reading : (32767 - reading);
            }
            const int floor = is_stick_axis ? -32768 : kPedalFloor;
            const int range = 32767 - floor;
            const int travelled = std::max(0, reading - floor);
            fraction = range > 0 ? static_cast<float>(travelled) / static_cast<float>(range)
                                 : 0.0f;
            // A button bound to the pedal floors it while held.
            const auto role   = axis == HostAxis::RightTrigger ? Config::PadAxisRole::Accel
                                                               : Config::PadAxisRole::Brake;
            if (binding_held(pad, m_pad_axis_buttons[which_player][static_cast<u32>(role)])) {
                fraction = 1.0f;
            }
            break;
        }
        case HostAxis::None:
            return channel.rest;
    }

    // A pedal's rest value is its minimum, so an untouched trigger has to read
    // as rest rather than as the bottom of the scaled range; they coincide for
    // every title in the database, but not by construction.
    if (is_pedal(axis) && fraction <= 0.0f) {
        return channel.reverse ? static_cast<u8>(channel.maximum - (channel.rest - channel.minimum))
                               : channel.rest;
    }

    const u8 value = scaled(fraction);
    if (!channel.reverse) {
        return value;
    }
    // PORT_REVERSE mirrors within the declared travel, not within the byte.
    return static_cast<u8>(channel.maximum - (value - channel.minimum));
}

bool Input::binding_held(SDL_Gamepad* pad, s32 binding)
{
    constexpr int kHalfTravel = 16384;
    if (pad == nullptr || binding < 0) {
        return false;
    }
    if (binding >= Config::kPadAxisMinus) {
        const auto axis = static_cast<SDL_GamepadAxis>(binding - Config::kPadAxisMinus);
        return SDL_GetGamepadAxis(pad, axis) < -kHalfTravel;
    }
    if (binding >= Config::kPadAxisPlus) {
        const auto axis = static_cast<SDL_GamepadAxis>(binding - Config::kPadAxisPlus);
        return SDL_GetGamepadAxis(pad, axis) > kHalfTravel;
    }
    return SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(binding));
}

bool Input::pad_button_held(const Pad& pad, SDL_GamepadButton physical) const
{
    static const auto kDefaults = Config::PadLayout{}.buttons[0];
    s32 button = physical;
    for (usize role = 0; role < kDefaults.size(); ++role) {
        if (kDefaults[role] == physical) {
            button = m_pad_buttons[pad.player][role];
            break;
        }
    }
    return binding_held(pad.handle, button);
}

bool Input::pad_role_held(u32 player, Config::PadRole role) const
{
    for (const Pad& pad : m_pads) {
        if (plays(pad) && pad.player == player) {
            return binding_held(pad.handle, m_pad_buttons[player][static_cast<u32>(role)]);
        }
    }
    return false;
}

bool Input::pad_pedal_pressed(SDL_Gamepad* pad, u32 player, Config::PadAxisRole role) const
{
    const auto r = static_cast<u32>(role);
    if (const s32 button = m_pad_axis_buttons[player][r]; button >= 0) {
        return binding_held(pad, button);
    }
    const s32  bound = m_pad_axes[player][r];
    const auto axis  = bound >= 0 ? static_cast<SDL_GamepadAxis>(bound)
                     : role == Config::PadAxisRole::Brake ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                          : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    const bool stick = axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    int reading = SDL_GetGamepadAxis(pad, axis);
    if (m_pad_axis_invert[player][r]) {
        reading = stick ? -reading : 32767 - reading;
    }
    return reading > (stick ? kStickThreshold : kTriggerPress);
}

bool Input::pad_physical_down(u32 player, s32 button) const
{
    SDL_Gamepad* pad = pad_for(player);
    return pad != nullptr && button >= 0
        && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(button));
}

s32 Input::pressed_pad_button(u32 player) const
{
    SDL_Gamepad* pad = pad_for(player);
    if (pad == nullptr) {
        return -1;
    }
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(button))) {
            return button;
        }
    }
    return -1;
}

void Input::pad_axis_baseline(u32 player, s16* out) const
{
    SDL_Gamepad* pad = pad_for(player);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        out[a] = pad != nullptr
                     ? SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a))
                     : 0;
    }
}

s16 Input::pad_steer_raw(u32 player) const
{
    SDL_Gamepad* pad = pad_for(player);
    if (pad == nullptr || player > 1) {
        return 0;
    }
    const auto role  = static_cast<u32>(Config::PadAxisRole::AimX);
    const s32  bound = m_pad_axes[player][role];
    const auto axis  = bound >= 0 ? static_cast<SDL_GamepadAxis>(bound) : SDL_GAMEPAD_AXIS_LEFTX;
    const int  raw   = SDL_GetGamepadAxis(pad, axis);
    return static_cast<s16>(std::clamp(m_pad_axis_invert[player][role] ? -raw : raw, -32768, 32767));
}

s16 Input::wheel_steer_raw() const
{
    if (m_wheel.handle == nullptr || m_wheel.steer_axis < 0) {
        return 0;
    }
    return wheel_axis(m_wheel.steer_axis);
}

s32 Input::captured_pad_axis(u32 player, const s16* baseline, bool* positive) const
{
    SDL_Gamepad* pad = pad_for(player);
    if (pad == nullptr) {
        return -1;
    }
    // The axis that has moved furthest from rest, once clearly past the noise.
    constexpr int kMoveThreshold = 12000;
    int  best_axis  = -1;
    int  best_delta = kMoveThreshold;
    bool best_pos   = true;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const int delta = static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)))
                        - static_cast<int>(baseline[a]);
        if (std::abs(delta) > best_delta) {
            best_delta = std::abs(delta);
            best_axis  = a;
            best_pos   = delta > 0;
        }
    }
    if (best_axis >= 0 && positive != nullptr) {
        *positive = best_pos;
    }
    return best_axis;
}

std::string Input::pad_axis_name(s32 axis)
{
    switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:         return "Left Stick X";
        case SDL_GAMEPAD_AXIS_LEFTY:         return "Left Stick Y";
        case SDL_GAMEPAD_AXIS_RIGHTX:        return "Right Stick X";
        case SDL_GAMEPAD_AXIS_RIGHTY:        return "Right Stick Y";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return "Left Trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
        default:                             return "";
    }
}

std::string Input::pad_button_name(s32 button)
{
    if (button >= Config::kPadAxisPlus) {
        const bool minus = button >= Config::kPadAxisMinus;
        const s32  axis  = button - (minus ? Config::kPadAxisMinus : Config::kPadAxisPlus);
        switch (static_cast<SDL_GamepadAxis>(axis)) {
            case SDL_GAMEPAD_AXIS_LEFTX:         return minus ? "Left Stick Left" : "Left Stick Right";
            case SDL_GAMEPAD_AXIS_LEFTY:         return minus ? "Left Stick Up" : "Left Stick Down";
            case SDL_GAMEPAD_AXIS_RIGHTX:        return minus ? "Right Stick Left" : "Right Stick Right";
            case SDL_GAMEPAD_AXIS_RIGHTY:        return minus ? "Right Stick Up" : "Right Stick Down";
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return "Left Trigger";
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
            default:                             return "";
        }
    }
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return "A (South)";
        case SDL_GAMEPAD_BUTTON_EAST:           return "B (East)";
        case SDL_GAMEPAD_BUTTON_WEST:           return "X (West)";
        case SDL_GAMEPAD_BUTTON_NORTH:          return "Y (North)";
        case SDL_GAMEPAD_BUTTON_BACK:           return "Back";
        case SDL_GAMEPAD_BUTTON_GUIDE:          return "Guide";
        case SDL_GAMEPAD_BUTTON_START:          return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return "L Stick";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return "R Stick";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return "D-Pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return "D-Pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return "D-Pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return "D-Pad Right";
        default:                                return "";
    }
}

s32 Input::pressed_wheel_button() const
{
    if (m_wheel.handle == nullptr) {
        return -1;
    }
    const int count = SDL_GetNumJoystickButtons(m_wheel.handle);
    for (int button = 0; button < count; ++button) {
        if (SDL_GetJoystickButton(m_wheel.handle, button)) {
            return button;
        }
    }
    return -1;
}

bool Input::menu_button_pressed()
{
    const s32 button = m_wheel_settings.buttons[static_cast<usize>(Config::WheelRole::Menu)];
    const bool held  = m_wheel.handle != nullptr && button >= 0
                    && button < SDL_GetNumJoystickButtons(m_wheel.handle)
                    && SDL_GetJoystickButton(m_wheel.handle, button);
    const bool edge = held && !m_menu_held;
    m_menu_held     = held;
    return edge;
}

Input::PadShortcuts Input::update_pad_shortcuts(bool enabled)
{
    enum : u8 { kA = 1, kX = 2, kY = 4, kStart = 8, kLB = 16, kDpadLeft = 32, kDpadRight = 64 };

    PadShortcuts out;
    for (Pad& pad : m_pads) {
        if (pad.handle == nullptr) {
            continue;
        }
        if (!enabled) {
            pad.shortcut_down    = 0;
            pad.shortcut_claimed = 0;
            continue;
        }
        const auto button = [&](SDL_GamepadButton b) {
            return SDL_GetGamepadButton(pad.handle, b);
        };
        u8 down = 0;
        down |= button(SDL_GAMEPAD_BUTTON_SOUTH) ? kA : 0;
        down |= button(SDL_GAMEPAD_BUTTON_WEST) ? kX : 0;
        down |= button(SDL_GAMEPAD_BUTTON_NORTH) ? kY : 0;
        down |= button(SDL_GAMEPAD_BUTTON_START) ? kStart : 0;
        down |= button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? kLB : 0;
        down |= button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? kDpadLeft : 0;
        down |= button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? kDpadRight : 0;

        // Only a control pressed while Guide is down starts a shortcut, so a
        // direction already held for the game does not rewind on Guide.
        const u8 pressed = down & static_cast<u8>(~pad.shortcut_down);
        const u8 started = button(SDL_GAMEPAD_BUTTON_GUIDE) ? pressed : 0;
        pad.shortcut_claimed = static_cast<u8>((pad.shortcut_claimed | started) & down);
        pad.shortcut_down    = down;

        out.menu         |= (started & kA) != 0;
        out.save         |= (started & kX) != 0;
        out.load         |= (started & kY) != 0;
        out.exit         |= (started & kStart) != 0;
        out.screenshot   |= (started & kLB) != 0;
        out.rewind       |= (pad.shortcut_claimed & kDpadLeft) != 0;
        out.fast_forward |= (pad.shortcut_claimed & kDpadRight) != 0;
    }
    return out;
}

bool Input::operator_chord(const Pad& pad, u8* port0)
{
    if (!SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_GUIDE)) {
        return false;
    }
    if (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_LEFT_STICK)) {
        *port0 &= static_cast<u8>(~kService);
    }
    if (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
        *port0 &= static_cast<u8>(~kTest);
    }
    return true;
}

int Input::wheel_axis_count() const
{
    return m_wheel.handle != nullptr ? SDL_GetNumJoystickAxes(m_wheel.handle) : 0;
}

void Input::wheel_axis_baseline(s16* out, int count) const
{
    for (int axis = 0; axis < count; ++axis) {
        out[axis] = m_wheel.handle != nullptr ? wheel_axis(axis) : 0;
    }
}

s32 Input::captured_axis(const s16* baseline, int count, bool* positive) const
{
    if (m_wheel.handle == nullptr) {
        return -1;
    }
    // The axis that has travelled furthest from where it rested, once clearly
    // past the noise: a deliberate turn or full pedal press swamps everything
    // else, so the largest delta is the control the user is operating.
    constexpr int kMoveThreshold = 12000;
    int best_axis  = -1;
    int best_delta = kMoveThreshold;
    bool best_pos  = true;
    for (int axis = 0; axis < count; ++axis) {
        const int delta = static_cast<int>(wheel_axis(axis))
                        - static_cast<int>(baseline[axis]);
        if (std::abs(delta) > best_delta) {
            best_delta = std::abs(delta);
            best_axis  = axis;
            best_pos   = delta > 0;
        }
    }
    if (best_axis >= 0 && positive != nullptr) {
        *positive = best_pos;
    }
    return best_axis;
}

void Input::update_drive_board(const rom::GameSpec& game, std::span<const u8> writes)
{
    for (const u8 value : writes) {
        SM2_TRACE("drive board: %02X", value);
        if (game.drive_protocol == rom::DriveProtocol::Daytona) {
            m_daytona.command(value);
        } else if (game.drive_protocol == rom::DriveProtocol::Indy) {
            m_indy.command(value);
        }
        const DriveCommand command = decode_drive_command(game.drive_protocol, value);
        if (command.pulse >= 0) {
            m_drive_pulse = command.pulse;
        }
        if (command.effect == DriveCommand::Effect::Other) {
            continue;
        }
        m_drive_command = command;
    }
}

void Input::serialize_drive_board(Archive& ar)
{
    // The layout of this section, for a later one to read this one.
    u8 version = 1;
    ar.raw(version);
    if (ar.loading() && version != 1) {
        ar.mark_failed();
        return;
    }

    DriveCommand& command = m_drive_command;
    u8 effect = static_cast<u8>(command.effect);
    ar.raw(effect);
    ar.raw(command.strength);
    ar.raw(command.held);
    ar.raw(command.deadzone);
    ar.raw(command.ramp_from);
    ar.raw(command.full_at);
    ar.raw(command.pulse);
    if (ar.loading()) {
        if (effect > static_cast<u8>(DriveCommand::Effect::Kick)) {
            ar.mark_failed();
            return;
        }
        command.effect = static_cast<DriveCommand::Effect>(effect);
    }
    ar.raw(m_drive_pulse);
    ar.raw(m_drive_ticks);
    m_daytona.serialize(ar);
    m_indy.serialize(ar);
}

bool Input::wheel_ffb_active(const rom::GameSpec& game) const
{
    return m_wheel.handle != nullptr && m_wheel.ffb && m_wheel_settings.ffb
        && game.drive_board && m_wheel.steer_axis >= 0;
}

void Input::update_force_feedback(const rom::GameSpec& game)
{
    if (m_wheel.handle == nullptr) {
        return;
    }

    // The game's own force, from its drive board.
    const DriveCommand& command = m_drive_command;
    int level  = 0;
    int rumble = 0;   // sine magnitude, felt as vibration rather than a push
    // Sega Rally's chopping when it flips too fast for the frames: its swing,
    // played as a sine on a wheel delivering force feedback, and its period.
    int chop_swing     = 0;
    u16 chop_period_ms = WheelForce::kRumblePeriod;
    const double board_ticks = kDriveBoardIrqHz / kMachineHz;
    m_drive_ticks += board_ticks;
    if (wheel_ffb_active(game)) {
        // The gain: at 100 % the board's full power gives the wheel's full
        // force, and a command at full strength Sega Rally's strongest torque.
        const int board_full = static_cast<int>(
            std::clamp(m_wheel_settings.strength, 0u, 200u) * 32767 / 100);
        const int command_full = board_full * kBoardFullPower / kBoardFullRange;
        const int mag = command.strength * command_full / kDriveFull;
        // The board's motor stiction level, added to every force it outputs.
        constexpr int kStiction = 5 * kDriveFull / 26;
        const int     stiction  = kStiction * command_full / kDriveFull;

        // An axis that has not reported yet says nothing about where the wheel is.
        const bool steer_known =
            m_wheel.steer_axis >= Wheel::kMaxAxes
            || (m_wheel.axes_moved & (1u << m_wheel.steer_axis)) != 0;
        if (steer_known && m_wheel.autocenter && m_wheel.ffb) {
            m_wheel.ffb->set_autocenter(0);
            m_wheel.autocenter = false;
        }
        const int deflection =
            steer_known ? static_cast<int>(wheel_axis(m_wheel.steer_axis)) : 0;
        const int velocity = deflection - m_wheel.last_deflection;
        m_wheel.last_deflection = deflection;

        // The board reads the pot the game reads: wheel travel mapped onto the lock.
        const int pot = std::clamp(
            deflection * static_cast<int>(std::max(1u, m_wheel_settings.steer_degrees))
                / static_cast<int>(std::max(1u, m_wheel_settings.lock_degrees)),
            -32767, 32767);

        // A spring of the given strength about the wheel's position.
        const auto spring = [deflection](int full) { return centring_spring(deflection, full); };

        // The board's own spring. Outside the deadzone: a ramp of two units a pot
        // step from ramp_from, clipped at the step's strength, plus one unit per
        // 16 steps of distance and the stiction level. Inside it the board keeps
        // the stiction level in its last direction until the pot reads centre,
        // then holds it with no direction, which brakes the motor. 26 is kDriveFull.
        const auto board_spring = [this, pot, velocity, command_full, stiction](const DriveCommand& c) {
            constexpr int kPotStep = 256;
            const int     m        = std::abs(pot);
            if (m > c.deadzone) {
                m_wheel.board_dir = pot < 0 ? -1 : 1;
                const int ramp    = 2 * (m - c.ramp_from) * kDriveFull / (26 * kPotStep);
                const int reach   = m * kDriveFull / (26 * 16 * kPotStep);
                const int held    = m > c.full_at ? c.strength : std::min(c.strength, ramp);
                const int force   = (held + reach + kStiction) * command_full / kDriveFull;
                return pot < 0 ? -force : force;
            }
            if (m < kPotStep) {
                m_wheel.board_dir = 0;
            }
            if (m_wheel.board_dir != 0) {
                return m_wheel.board_dir * stiction;
            }
            constexpr int kFullSpeed = 1024;
            return stiction * std::clamp(velocity, -kFullSpeed, kFullSpeed) / kFullSpeed;
        };

        // The Sega Rally and Daytona panel has a light spring, the user's share
        // of the strength. The EPR-18261 panels are direct drive and have none.
        const bool panel_spring = game.drive_protocol == rom::DriveProtocol::Rally
                               || game.drive_protocol == rom::DriveProtocol::Daytona;
        const int  panel_full   = command_full * static_cast<int>(m_wheel_settings.panel_spring) / 100;
        const int  baseline     = panel_spring ? spring(panel_full) : 0;

        // Positive levels push the wheel left, as WheelForce::set_force takes them.
        int game_force = 0;
        m_daytona.set_dip(static_cast<int>(m_wheel_settings.drive_dip));
        m_indy.set_dip(static_cast<int>(m_wheel_settings.drive_dip));
        if (game.drive_protocol == rom::DriveProtocol::Daytona) {
            // The board's start-up calibration is run against the simulated
            // spring.
            const u32 steer       = m_wheel_settings.steer_degrees;
            const u32 lock        = m_wheel_settings.lock_degrees;
            const int unit        = std::max(1, board_full / kBoardFullRange);
            const int calib_force = centring_spring(
                board_deflection(kDaytonaCalibCounts, steer, lock), panel_full);
            const int breakaway = std::max(0, (calib_force + unit - 1) / unit - 1);
            const DaytonaBoard::Output out =
                m_daytona.step(board_counts(deflection, steer, lock), board_ticks, breakaway);
            const int force = out.power * board_full / kBoardFullRange;
            switch (out.way) {
                case DaytonaBoard::Way::Down: game_force = force; break;   // left
                case DaytonaBoard::Way::Up:   game_force = -force; break;  // right
                case DaytonaBoard::Way::None: {
                    // No way set: the clutch brakes against the turn.
                    constexpr int kFullSpeed = 1024;
                    game_force = force * std::clamp(velocity, -kFullSpeed, kFullSpeed) / kFullSpeed;
                    break;
                }
            }
            SM2_DEBUG("ffb daytona: on=%d effect=%02X osc=%d way=%d power=%d breakaway=%d",
                      m_daytona.forces_on() ? 1 : 0, m_daytona.effect(),
                      m_daytona.oscillating() ? 1 : 0, static_cast<int>(out.way), out.power,
                      breakaway);
        } else if (game.drive_protocol == rom::DriveProtocol::Indy) {
            // The board sends two powers, one each way, and what they have in
            // common brakes the motor. It reads the game's own steering pot, so
            // "up" follows the channel's direction.
            int  position = 0x80 + board_counts(deflection, m_wheel_settings.steer_degrees,
                                                m_wheel_settings.lock_degrees);
            bool up_is_left = false;
            for (const rom::AnalogChannel& channel : game.analog) {
                if (u8 value = 0; channel.control == rom::AnalogControl::Steer
                                  && sample_wheel_channel(channel, &value)) {
                    position   = value;
                    up_is_left = channel.reverse;
                    break;
                }
            }
            const IndyBoard::Output out = m_indy.step(position, IndyBoard::kNmiHz / kMachineHz);
            const int dead   = static_cast<int>(m_wheel_settings.indy_deadband);
            const int full   = static_cast<int>(m_wheel_settings.indy_full_power);
            const auto torque = [dead, full, board_full](int power) {
                const int span = std::max(1, full - dead);
                return std::clamp(power - dead, 0, span) * board_full / span;
            };
            constexpr int kFullSpeed = 1024;
            const int up    = torque(out.up);
            const int down  = torque(out.down);
            const int brake = std::min(up, down);
            const int push  = down - up;  // down is left
            game_force = (up_is_left ? -push : push)
                       + brake * std::clamp(velocity, -kFullSpeed, kFullSpeed) / kFullSpeed;
            SM2_DEBUG("ffb indy: effect=%02X base=%d centre=%02X position=%d up=%d down=%d",
                      m_indy.effect(), m_indy.base(), m_indy.centre(), position, out.up,
                      out.down);
        } else if (command.is_push() && command.held) {
            // A streamed torque is the game's own steering feel, centring
            // included; apply it as sent.
            m_wheel.constant_hold = 0;
            m_wheel.constant_dir  = 0;
            const int dir   = push_direction(command, pot);
            int       force = mag;
            if (game.drive_protocol == rom::DriveProtocol::Rally) {
                // The operator's strength, DIP switches 1-3: 0..7 more power.
                force += static_cast<int>(m_wheel_settings.drive_dip) * board_full / kBoardFullRange;
                // An outward push also gets the calibration power, capped at the
                // spring's force at that point, since a PC wheel has no gear drag.
                const bool outward = dir > 0 ? deflection < 0 : deflection >= 0;
                if (outward) {
                    const int counts = dir > 0 ? kRallyCalibCountsLeft : kRallyCalibCountsRight;
                    const int calib  = centring_spring(
                        board_deflection(counts, m_wheel_settings.steer_degrees,
                                         m_wheel_settings.lock_degrees),
                        panel_full);
                    force += std::min(std::abs(spring(panel_full)), calib);
                }

                // $11..$17 chop the torque: off half the time, and a quarter
                // stronger the other half.
                if (m_drive_pulse != 0) {
                    const double flip = static_cast<double>(2u << m_drive_pulse);
                    if (flip >= 2.0 * board_ticks) {
                        const bool off = (static_cast<u64>(m_drive_ticks / flip) & 1u) != 0;
                        force          = off ? 0 : force * 5 / 4;
                    } else {
                        // Faster than the frames can show: its mean as the
                        // torque, its swing as a vibration where there is one.
                        force = force * 5 / 8;
                        if (m_wheel.ffb->has_rumble()) {
                            chop_swing     = std::min(force, 32767);
                            chop_period_ms = static_cast<u16>(
                                std::max(1.0, 2.0 * flip * 1000.0 / kDriveBoardIrqHz));
                        }
                    }
                }
            }
            m_wheel.board_dir = dir;
            game_force        = dir * force;
        } else if (command.is_push()) {
            const int dir = push_direction(command, pot);
            // A push is a jolt from the road, so it also drives the vibration; a
            // change of direction is the sharpest. A sustained push is held for a
            // brief kick then decayed, as a free PC wheel would otherwise spin to
            // the stop where the cabinet's heavy geared wheel barely moved.
            rumble = mag;
            if (dir == m_wheel.constant_dir) {
                m_wheel.constant_hold++;
            } else {
                rumble = mag * 3 / 2;
                m_wheel.constant_hold = 0;
                m_wheel.constant_dir  = dir;
            }
            constexpr int kFullFrames = 8;   // ~0.14 s of full kick
            int scaled = mag;
            if (m_wheel.constant_hold > kFullFrames) {
                const int over = std::min(m_wheel.constant_hold - kFullFrames, 18);
                scaled = mag - (mag * 9 / 10) * over / 18;
            }
            m_wheel.board_dir = dir;
            game_force        = dir * (scaled + stiction);
        } else {
            m_wheel.constant_hold = 0;
            m_wheel.constant_dir  = 0;
            switch (command.effect) {
                case DriveCommand::Effect::Spring:
                    game_force = command.full_at != 0 ? board_spring(command) : spring(mag);
                    break;
                case DriveCommand::Effect::Friction: {
                    // Opposes the turn, full strength from this speed (axis units per frame).
                    constexpr int kFullSpeed = 1024;
                    m_wheel.board_dir = 0;
                    int brake = mag;
                    if (game.drive_protocol == rom::DriveProtocol::Rally) {
                        // Sega Rally's board adds its DIP strength to the brake too.
                        brake += static_cast<int>(m_wheel_settings.drive_dip) * board_full / kBoardFullRange;
                    }
                    game_force = brake * std::clamp(velocity, -kFullSpeed, kFullSpeed) / kFullSpeed;
                    break;
                }
                default:
                    m_wheel.board_dir = 0;
                    break;
            }
        }
        // The wheel's full force: above a gain of 100 %, the strongest saturate.
        level = std::clamp(baseline + game_force, -32767, 32767);
    }

    // Without force feedback, the game's impacts come through as rumble instead.
    const bool ffb_active = wheel_ffb_active(game);
    if (!ffb_active && game.has_steering() && m_wheel_settings.rumble && command.is_push()) {
        const int ceiling = static_cast<int>(
            std::clamp(m_wheel_settings.rumble_strength, 0u, 100u) * 16383 / 100);
        rumble = std::max(rumble, command.strength * ceiling / kDriveFull);
    }

    // Boost the impact rumble so a hit is clearly felt.
    rumble *= 2;

    // Daytona streams no continuous road/engine buzz -- its command stream is
    // centring plus directional jolts -- so a light engine rumble is synthesised
    // from the throttle, giving constant feel while driving that rises with the
    // gas. This is a feel, not replayed game data, with its own on/off + strength.
    // Kept deliberately subtle: even at full strength it is a fraction of the
    // device maximum, so it reads as an engine hum rather than a jackhammer.
    if (!ffb_active && game.has_steering() && m_wheel_settings.rumble
        && m_wheel_settings.rumble_engine && m_wheel.accel_axis >= 0) {
        // Full strength maps to ~12% of the device max at full throttle; the
        // G923's motor is strong, so even a small sine magnitude is plenty.
        const int rmax = static_cast<int>(
            std::clamp(m_wheel_settings.rumble_strength, 0u, 100u) * 4000 / 100);
        const float throttle =
            pedal_travel(m_wheel.accel_axis, m_wheel.accel_invert, m_wheel.accel_half);
        // A faint idle hum (a fifth of the range) rising to the full engine level.
        const int engine = rmax / 5 + static_cast<int>(static_cast<float>(rmax * 4 / 5) * throttle);
        rumble = std::max(rumble, engine);
    }

    // Smooth it: a trigger is a single-frame spike, so decay the running
    // magnitude and hold it a few frames into a sustained felt vibration.
    // A wheel delivering force feedback rumbles only with the board's chopping.
    int rumble_now = std::max(rumble, (m_wheel.rumble_mag < 0 ? 0 : m_wheel.rumble_mag) * 4 / 5);
    rumble_now     = ffb_active ? chop_swing : std::clamp(rumble_now, 0, 32767);
    const u16 rumble_period = chop_swing != 0 ? chop_period_ms : WheelForce::kRumblePeriod;

    constexpr int kFeltChange = 256;
    const auto worth_sending = [](int want, int sent) {
        return (want == 0) != (sent == 0) || std::abs(want - sent) >= kFeltChange;
    };

    // Positive left, like the deflection's spring: a wheel turned right is
    // pulled back left. Reversed for a driver that has it the other way.
    const int force_out = m_wheel_settings.ffb_invert ? -level : level;
    if (m_wheel.ffb && worth_sending(force_out, m_wheel.force_level)) {
        m_wheel.force_level = force_out;
        m_wheel.ffb->set_force(static_cast<s16>(force_out));
    }

    if (m_wheel.ffb && m_wheel.ffb->has_rumble()) {
        const bool new_period = rumble_now != 0 && rumble_period != m_wheel.rumble_period;
        if (worth_sending(rumble_now, m_wheel.rumble_mag) || new_period) {
            m_wheel.rumble_mag    = rumble_now;
            m_wheel.rumble_period = rumble_period;
            m_wheel.ffb->set_rumble(static_cast<s16>(rumble_now), rumble_period);
        }
    } else if (m_wheel.can_rumble) {
        // A wheel has no rumble motors; its driver swings the steering instead.
        const auto low  = static_cast<u16>(std::clamp(rumble_now * 2, 0, 65535));
        const auto high = static_cast<u16>(std::clamp(rumble_now, 0, 65535));
        const bool changed = low != m_wheel.rumble_low || high != m_wheel.rumble_high;
        const bool stale   = (low != 0 || high != 0) && ++m_wheel.rumble_age >= 4;
        if (changed || stale) {
            SDL_RumbleJoystick(m_wheel.handle, low, high, 250);
            m_wheel.rumble_low  = low;
            m_wheel.rumble_high = high;
            m_wheel.rumble_age  = 0;
        }
        m_wheel.rumble_mag = rumble_now;
    }

    SM2_DEBUG("ffb: effect=%d strength=%d chop=%d steer=%d level=%d rumble=%d",
              static_cast<int>(m_drive_command.effect), m_drive_command.strength,
              m_drive_pulse,
              m_wheel.steer_axis >= 0 ? static_cast<int>(wheel_axis(m_wheel.steer_axis)) : 0,
              level, rumble_now);
}

void Input::release_force_feedback()
{
    if (!m_wheel.ffb) {
        return;
    }
    if (m_wheel.force_level != 0) {
        m_wheel.force_level = 0;
        m_wheel.ffb->set_force(0);
    }
    if (m_wheel.rumble_mag > 0) {
        m_wheel.rumble_mag = 0;
        m_wheel.ffb->set_rumble(0);
    }
}

void Input::update_pad_rumble(const rom::GameSpec& game)
{
    if (m_pads.empty()) {
        return;
    }

    const int ceiling = static_cast<int>(
        std::clamp(m_pad_rumble_strength, 0u, 100u) * 65535 / 100);
    const bool active = m_pad_rumble_enabled && game.has_steering() && !wheel_ffb_active(game);
    if (!active) {
        m_pad_rumble_hold = 0;
        m_pad_rumble_level = 0;
    }

    int steer = 0;
    if (active) {
        if (SDL_Gamepad* pad = pad_for(0)) {
            const u32 aim_x = static_cast<u32>(Config::PadAxisRole::AimX);
            const s32 bound = m_pad_axes[0][aim_x];
            steer = SDL_GetGamepadAxis(
                pad, bound >= 0 ? static_cast<SDL_GamepadAxis>(bound) : SDL_GAMEPAD_AXIS_LEFTX);
            if (m_pad_axis_invert[0][aim_x]) {
                steer = -steer;
            }
        }
    }

    // Jolts: the drive board's pushes, which are short bursts, not a held force.
    const DriveCommand& command = m_drive_command;
    int impact = 0;
    if (active && command.strength > 0 && !command.held && command.is_push()) {
        // Start at a floor; a pad motor cannot render the smallest levels.
        const int min_felt = ceiling / 4;
        impact = min_felt + (ceiling - min_felt) * command.strength / kDriveFull;

        const int dir = push_direction(command, steer);
        if (dir != m_pad_rumble_dir) {
            impact = std::min(impact * 3 / 2, 65535);  // a flip is the sharper hit
            m_pad_rumble_dir = dir;
        }
    }

    // Cornering load: the board's answer is a centring spring, so synthesise a buzz instead.
    int cornering = 0;
    if (active && m_pad_rumble_cornering) {
        const int deflection = std::abs(steer);
        const int deadzone   = static_cast<int>(m_pad_stick_dead);
        if (deflection > deadzone) {
            const int span = std::max(1, 32767 - deadzone);
            const int over = std::min(deflection - deadzone, span);
            cornering = (ceiling * 2 / 5) * over / span;  // kept under the jolts
        }
    }

    const int target = std::max(impact, cornering);
    SM2_DEBUG("pad rumble: effect=%d impact=%d cornering=%d",
              static_cast<int>(command.effect), impact, cornering);

    // Hold the level for a burst rather than tracking frame by frame, so a pad does not drone.
    constexpr int kHoldFrames = 12;   // ~200 ms at 57.5 Hz
    if (target > 0) {
        m_pad_rumble_level = target;
        m_pad_rumble_hold  = kHoldFrames;
    } else if (m_pad_rumble_hold > 0 && --m_pad_rumble_hold == 0) {
        m_pad_rumble_level = 0;
        m_pad_rumble_dir   = 0;
    }

    // The big motor carries the body, the small one a lighter edge.
    const auto low  = static_cast<u16>(std::clamp(m_pad_rumble_level, 0, 65535));
    const auto high = static_cast<u16>(std::clamp(m_pad_rumble_level / 2, 0, 65535));

    for (Pad& pad : m_pads) {
        if (pad.handle == nullptr) {
            continue;
        }
        // A rumble effect lapses, so re-arm on a change or every few frames.
        const bool changed = low != pad.rumble_low || high != pad.rumble_high;
        const bool stale   = (low != 0 || high != 0) && ++pad.rumble_age >= 4;
        if (!changed && !stale) {
            continue;
        }
        SDL_RumbleGamepad(pad.handle, low, high, 250);
        pad.rumble_low  = low;
        pad.rumble_high = high;
        pad.rumble_age  = 0;
    }
}

void Input::gather_lightguns(hw::Inputs* inputs, const rom::GameSpec& game) const
{
    const rom::LightgunSpec& spec = game.lightgun;

    // Two kinds of gun cabinet. The RS-422 lightgun titles (Virtua Cop, House of
    // the Dead) declare a <lightgun> spec and take their aim through the serial
    // gun board (inputs->gun_p1x/y). The positional-gun titles (Gunblade NY,
    // Behind Enemy Lines, Rail Chase 2) have no such board: their aim is an
    // analogue stick, wired to analog[] channels carrying the Gun1X/Y, Gun2X/Y
    // controls. Both aim with the mouse here, but write to different places.
    int pos_gun_ch[4] = {-1, -1, -1, -1};  // p1x, p1y, p2x, p2y -> analog channel
    for (usize ch = 0; ch < game.analog.size(); ++ch) {
        switch (game.analog[ch].control) {
            case rom::AnalogControl::Gun1X: pos_gun_ch[0] = static_cast<int>(ch); break;
            case rom::AnalogControl::Gun1Y: pos_gun_ch[1] = static_cast<int>(ch); break;
            case rom::AnalogControl::Gun2X: pos_gun_ch[2] = static_cast<int>(ch); break;
            case rom::AnalogControl::Gun2Y: pos_gun_ch[3] = static_cast<int>(ch); break;
            default: break;
        }
    }
    const bool positional = pos_gun_ch[0] >= 0 || pos_gun_ch[1] >= 0;

    if (!spec.present && !positional) {
        m_gun_aims[0].active = false;
        m_gun_aims[1].active = false;
        return;
    }

    // The single system pointer, used for any player without a dedicated gun.
    // The finished frame is letterboxed to 4:3 inside the window, so the pointer
    // has to be mapped onto that game-image rectangle, not the whole window, or
    // the shot lands offset from the cursor by the size of the bars.
    const PointerState pointer = pointer_state();
    float ptr_fx = 0.5f;
    float ptr_fy = 0.5f;
    if (pointer.width > 1 && pointer.height > 1) {
        const render::Letterbox box = render::compute_game_rect(
            static_cast<u32>(pointer.width), static_cast<u32>(pointer.height),
            m_present_aspect, m_present_method, m_present_area);
        if (box.width > 0.0f && box.height > 0.0f) {
            ptr_fx = (pointer.x - box.x) / box.width;
            ptr_fy = (pointer.y - box.y) / box.height;
        }
    }

    // Resolve each player's aim, trigger and reload from either a dedicated
    // evdev gun or the shared mouse pointer.
    //
    // These titles have no reload button: the gun reloads when fired while aimed
    // off screen (`lightgun_offscreen_read` treats a coordinate near the edge of
    // the calibrated travel as off-screen). So a reload -- the mouse's right
    // button, or a gun's reload button -- forces the aim to the corner and pulls
    // the trigger.
    struct GunInput {
        float x           = 0.5f;  ///< Fraction of the game image; outside 0..1 is off screen.
        float y           = 0.5f;
        float win_x       = 0.5f;  ///< Fraction of the window, for the calibration marker.
        float win_y       = 0.5f;
        bool  calibrating = false;
        bool  trigger     = false;
        bool  reload      = false;
        bool  missile     = false;
        bool  coin        = false;
        bool  start       = false;
        bool  up          = false;
        bool  down        = false;
        bool  left        = false;
        bool  right       = false;
    };
    // With a Missile button (bel), right = missile; otherwise right = reload.
    const bool has_missile = game.gun_missile;
    const auto from_mouse = [&]() {
        GunInput gi;
        gi.x = ptr_fx;
        gi.y = ptr_fy;
        if (pointer.width > 0 && pointer.height > 0) {
            gi.win_x = pointer.x / static_cast<float>(pointer.width);
            gi.win_y = pointer.y / static_cast<float>(pointer.height);
        }
        if (has_missile) {
            gi.trigger = pointer.left;
            gi.missile = pointer.right;
        } else {
            gi.trigger = pointer.left || pointer.right;
            gi.reload  = pointer.right;
        }
        return gi;
    };
    GunInput p1 = from_mouse();
    GunInput p2 = from_mouse();

    // Moving the mouse hands the aim back to it from the pad/keyboard cursor.
    if (pointer.x != m_gun_last_ptr_x || pointer.y != m_gun_last_ptr_y) {
        m_gun_cursor_owns = {false, false};
        m_gun_last_ptr_x  = pointer.x;
        m_gun_last_ptr_y  = pointer.y;
    }
    std::array<bool, kPlayers> on_mouse = {true, true};

#ifdef SM2_HAVE_LIGHTGUNS
    // Gun 0 drives player 1, gun 1 player 2. A gun also moves the system
    // pointer, so with one gun player 2 leaves the mouse alone.
    if (m_guns) {
        m_guns->poll();

        // A gun reports where it points on the whole screen, so map that through
        // the window onto the letterboxed image; aiming at a side bar then reads
        // as off screen. A fullscreen window is the screen; only a windowed one
        // needs the display geometry. A gun aiming against the Sinden border
        // already reports positions on the image.
        float screen_x = 0.0f;
        float screen_y = 0.0f;
        float screen_w = 1.0f;
        float screen_h = 1.0f;
        // A Sinden reports positions on the picture its border frames, which in
        // widescreen is wider than the game's own screen.
        const float wide_edge = static_cast<float>(m_wide_columns)
                              / static_cast<float>(render::frame_width(m_wide_columns));
        render::Letterbox gun_box{wide_edge, 0.0f, 1.0f - 2.0f * wide_edge, 1.0f};
        SDL_Window* window = SDL_GetKeyboardFocus();
        if (window == nullptr) {
            window = SDL_GetMouseFocus();
        }
        SDL_Rect display{};
        int win_x = 0;
        int win_y = 0;
        int win_w = 0;
        int win_h = 0;
        int pix_w = 0;
        int pix_h = 0;
        if (!m_sinden_border && window != nullptr
            && SDL_GetWindowSizeInPixels(window, &pix_w, &pix_h) && pix_w > 0 && pix_h > 0) {
            const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            const bool placed = fullscreen
                || (SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &display)
                    && SDL_GetWindowPosition(window, &win_x, &win_y)
                    && SDL_GetWindowSize(window, &win_w, &win_h)
                    && win_w > 0 && win_h > 0 && display.w > 0 && display.h > 0);
            const render::Letterbox box = render::compute_game_rect(
                static_cast<u32>(pix_w), static_cast<u32>(pix_h), m_present_aspect,
                m_present_method, m_present_area);
            if (placed && box.width > 0.0f && box.height > 0.0f) {
                if (!fullscreen) {
                    screen_x = static_cast<float>(display.x - win_x) / static_cast<float>(win_w);
                    screen_y = static_cast<float>(display.y - win_y) / static_cast<float>(win_h);
                    screen_w = static_cast<float>(display.w) / static_cast<float>(win_w);
                    screen_h = static_cast<float>(display.h) / static_cast<float>(win_h);
                }
                gun_box = {box.x / static_cast<float>(pix_w), box.y / static_cast<float>(pix_h),
                           box.width / static_cast<float>(pix_w),
                           box.height / static_cast<float>(pix_h)};
            }
        }

        const auto from_gun = [&](const LightGuns::Gun& g, usize player) {
            const auto& bind = m_gun_buttons[player];
            const auto held  = [&](usize role) {
                const u32 code = bind[role];
                return code != 0 && g.held(static_cast<u16>(code));
            };
            GunInput gi;
            gi.win_x = screen_x + g.x * screen_w;  // 0..1 across the window
            gi.win_y = screen_y + g.y * screen_h;
            gi.x = (gi.win_x - gun_box.x) / gun_box.width;
            gi.y = (gi.win_y - gun_box.y) / gun_box.height;
            // The backends clamp the raw aim to 0..1, so a value at either end
            // means the gun is pointing off the screen (or past the Sinden border).
            if (g.x <= 0.0f) gi.x = -1.0f; else if (g.x >= 1.0f) gi.x = 2.0f;
            if (g.y <= 0.0f) gi.y = -1.0f; else if (g.y >= 1.0f) gi.y = 2.0f;
            // Batocera's gun calibrator holds KEY_CONFIG while it walks the aim
            // through its targets, which sit on the screen, not on the image.
            gi.calibrating = g.held(kKeyConfig);
            const bool reload = held(GrReload);
            if (has_missile) {
                gi.trigger = held(GrTrigger);
                gi.missile = reload;
            } else {
                gi.trigger = held(GrTrigger) || reload;
                gi.reload  = reload;
            }
            gi.coin  = held(GrCoin);
            gi.start = held(GrStart);
            gi.up    = held(GrHatUp);
            gi.down  = held(GrHatDown);
            gi.left  = held(GrHatLeft);
            gi.right = held(GrHatRight);
            return gi;
        };
        if (m_guns->count() >= 1) {
            p1          = from_gun(m_guns->gun(0), 0);
            on_mouse[0] = false;
        }
        if (m_guns->count() >= 2) {
            p2 = from_gun(m_guns->gun(1), 1);
        } else {
            p2 = GunInput{};
        }
        on_mouse[1] = false;

        // Recoil: pulse a gun's motor once on the trigger's press edge, keyed on
        // the bound trigger code (not reload) so racking off-screen does not kick.
        for (usize i = 0; i < m_guns->count() && i < kMaxGuns; ++i) {
            const u32  code = m_gun_buttons[i < 2 ? i : 1][GrTrigger];
            const bool down = code != 0
                              && m_guns->gun(i).held(static_cast<u16>(code));
            if (m_recoil_enabled && down && !m_trigger_was_down[i]
                && m_guns->has_recoil(i)) {
                m_guns->fire_recoil(i, m_recoil_strength);
            }
            m_trigger_was_down[i] = down;
        }
    }
#endif

    // Keyboard/pad aiming fallback (mouse and dedicated guns stay preferred): a
    // per-player cursor nudged by the pad's sticks or the keyboard arrows, fired
    // with the pad's Accelerator slot / Left Ctrl, reload-or-missile on its Brake
    // slot / Right Alt. It
    // overrides the mouse aim only while actually driven, so the mouse is
    // untouched when the pad/keys are idle.
    {
        const auto aim_from = [&](usize player, GunInput& gi) {
            bool active = false;
            float dx = 0.0f;
            float dy = 0.0f;

            bool  centred = false;
            float cx      = 0.0f;
            float cy      = 0.0f;
            if (SDL_Gamepad* pad = pad_for(static_cast<u32>(player)); pad != nullptr) {
                // Aim on either stick.
                for (const auto [ax, ay] : {std::pair{SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY},
                                            std::pair{SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY}}) {
                    const s16 sx = SDL_GetGamepadAxis(pad, ax);
                    const s16 sy = SDL_GetGamepadAxis(pad, ay);
                    if (m_pad_aim_centred) {
                        cx += centred_fraction(sx, m_pad_stick_dead, 1.0f, 1.0f) * 2.0f - 1.0f;
                        cy += centred_fraction(sy, m_pad_stick_dead, 1.0f, 1.0f) * 2.0f - 1.0f;
                    } else if (std::abs(sx) > kStickThreshold / 2 || std::abs(sy) > kStickThreshold / 2) {
                        dx += static_cast<float>(sx) / 32767.0f;
                        dy += static_cast<float>(sy) / 32767.0f;
                        active = true;
                    }
                }
                dx      = std::clamp(dx, -1.0f, 1.0f);
                dy      = std::clamp(dy, -1.0f, 1.0f);
                cx      = std::clamp(cx, -1.0f, 1.0f);
                cy      = std::clamp(cy, -1.0f, 1.0f);
                centred = m_pad_aim_centred;
                const u32 p = static_cast<u32>(player);
                const auto& binds = m_pad_buttons[p];
                if (binding_held(pad, binds[static_cast<u32>(Config::PadRole::Button1)])) {
                    gi.trigger = true;
                }
                if (binding_held(pad, binds[static_cast<u32>(Config::PadRole::Button2)])) {
                    if (has_missile) gi.missile = true;
                    else { gi.trigger = true; gi.reload = true; }
                }
                if (pad_pedal_pressed(pad, p, Config::PadAxisRole::Accel)) {
                    gi.trigger = true;
                    active     = true;
                }
                if (pad_pedal_pressed(pad, p, Config::PadAxisRole::Brake)) {
                    active = true;
                    if (has_missile) gi.missile = true;
                    else { gi.trigger = true; gi.reload = true; }
                }
            }
            // Keyboard aim only for player one (arrows), so it does not fight P2.
            int         key_count = 0;
            const bool* keys      = SDL_GetKeyboardState(&key_count);
            if (player == 0 && keys != nullptr) {
                const auto kd = [&](SDL_Scancode sc) {
                    return static_cast<int>(sc) < key_count && keys[sc];
                };
                if (kd(SDL_SCANCODE_LEFT))  { dx -= 1.0f; active = true; }
                if (kd(SDL_SCANCODE_RIGHT)) { dx += 1.0f; active = true; }
                if (kd(SDL_SCANCODE_UP))    { dy -= 1.0f; active = true; }
                if (kd(SDL_SCANCODE_DOWN))  { dy += 1.0f; active = true; }
                if (kd(SDL_SCANCODE_LCTRL)) { gi.trigger = true; active = true; }
                if (kd(SDL_SCANCODE_RALT)) {
                    active = true;
                    if (has_missile) gi.missile = true; else { gi.trigger = true; gi.reload = true; }
                }
            }

            const bool keys_aim = dx != 0.0f || dy != 0.0f;
            if (centred && (cx != 0.0f || cy != 0.0f || (m_gun_cursor_owns[player] && !keys_aim))) {
                m_gun_cursor_owns[player] = true;
                m_gun_cursor_x[player]    = 0.5f + cx * 0.5f;
                m_gun_cursor_y[player]    = 0.5f + cy * 0.5f;
            } else if (active) {
                if (!m_gun_cursor_owns[player]) {
                    // Take over from wherever the mouse or gun was aiming.
                    m_gun_cursor_x[player]   = std::clamp(gi.x, 0.0f, 1.0f);
                    m_gun_cursor_y[player]   = std::clamp(gi.y, 0.0f, 1.0f);
                    m_gun_cursor_owns[player] = true;
                }
                // ~1.5%/frame at full deflection is a controllable sweep at 57.5 Hz.
                constexpr float kSpeed = 0.015f;
                m_gun_cursor_x[player] = std::clamp(m_gun_cursor_x[player] + dx * kSpeed, 0.0f, 1.0f);
                m_gun_cursor_y[player] = std::clamp(m_gun_cursor_y[player] + dy * kSpeed, 0.0f, 1.0f);
            }
            if (!m_gun_cursor_owns[player]) {
                return;  // the mouse or gun keeps the aim.
            }
            gi.x             = m_gun_cursor_x[player];
            gi.y             = m_gun_cursor_y[player];
            on_mouse[player] = false;
        };
        aim_from(0, p1);
        aim_from(1, p2);
    }

    // Record the aim (before the reload snap) for the crosshair overlay. Player
    // 2's crosshair only shows when a second gun is actually aiming it, so a
    // single-mouse session does not paint two overlapping crosshairs.
    bool p2_active = pad_for(1) != nullptr;  // a 2nd pad aims player 2
#ifdef SM2_HAVE_LIGHTGUNS
    p2_active = p2_active || (m_guns && m_guns->count() >= 2);
#endif
    // Positional-gun titles draw their own in-game crosshair, so suppress ours
    // to avoid two overlapping reticles; the RS-422 lightgun titles do not.
    // Widescreen: how far past the screen's edges, as a fraction of its width,
    // the picture reaches.
    const float side = static_cast<float>(m_wide_columns) / static_cast<float>(render::kNativeWidth);
    const auto record = [side](bool active, bool mouse, const GunInput& g) {
        return GunAim{active, mouse, std::clamp(g.x, -side, 1.0f + side),
                      std::clamp(g.y, 0.0f, 1.0f), g.calibrating, g.win_x, g.win_y};
    };
    // A gun nobody is holding goes still, so its crosshair hides after a few seconds
    constexpr u64 kIdleMs = 3000;
    const u64     now     = SDL_GetTicks();
    const auto    idle    = [&](usize player, const GunInput& g) {
        const bool moved = m_gun_sampled[player]
                        && (std::fabs(g.x - m_gun_prev_x[player]) > 0.002f
                            || std::fabs(g.y - m_gun_prev_y[player]) > 0.002f);
        m_gun_sampled[player] = true;
        m_gun_prev_x[player]  = g.x;
        m_gun_prev_y[player]  = g.y;
        if (moved || g.trigger || g.reload || g.missile || on_mouse[player]) {
            m_gun_used[player]        = true;
            m_gun_last_use_ms[player] = now;
        }
        return !m_gun_used[player] || now - m_gun_last_use_ms[player] > kIdleMs;
    };
    m_gun_aims[0] = record(!positional, on_mouse[0], p1);
    m_gun_aims[1] = record(p2_active && !positional, on_mouse[1], p2);
    m_gun_aims[0].idle = idle(0, p1);
    m_gun_aims[1].idle = idle(1, p2);

    if (positional) {
        // Positional gun: the aim is an analogue axis. Scale the mouse fraction
        // into each channel's calibrated travel and overwrite the value the
        // stick sampled. No off-screen reload here, so the reload snap is not
        // applied; these titles use an explicit missile/reload button instead.
        // X reaches into the widescreen columns: the end segments carry on
        // past the screen's edges, as far as the 8-bit value allows.
        const auto write_channel = [&](int idx, float fraction, float reach) {
            if (idx < 0) {
                return;
            }
            const rom::AnalogChannel& c = game.analog[static_cast<usize>(idx)];
            fraction         = std::clamp(fraction, -reach, 1.0f + reach);
            float value      = 0.0f;
            if (c.point_count >= 2) {
                usize next = 1;
                while (next + 1 < c.point_count && fraction > c.points[next].at) {
                    ++next;
                }
                const rom::AnalogPoint& a = c.points[next - 1];
                const rom::AnalogPoint& b = c.points[next];
                float t = (fraction - a.at) / (b.at - a.at);
                if ((next > 1 && t < 0.0f) || (next + 1 < c.point_count && t > 1.0f)) {
                    t = std::clamp(t, 0.0f, 1.0f);
                }
                value = std::lerp(static_cast<float>(a.value), static_cast<float>(b.value), t);
            } else {
                const float f    = c.reverse ? 1.0f - fraction : fraction;
                const float span = static_cast<float>(c.maximum - c.minimum);
                value            = static_cast<float>(c.minimum) + f * span;
            }
            inputs->analog[static_cast<usize>(idx)] =
                static_cast<u8>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
        };
        write_channel(pos_gun_ch[0], p1.x, side);
        write_channel(pos_gun_ch[1], p1.y, 0.0f);
        write_channel(pos_gun_ch[2], p2.x, side);
        write_channel(pos_gun_ch[3], p2.y, 0.0f);
    } else {
        // X across the whole picture, which in widescreen reaches past the
        // screen; the X axes' travel grows to match.
        const auto across = [side](float& aim) { aim = (aim + side) / (1.0f + 2.0f * side); };
        across(p1.x);
        across(p2.x);
        const rom::LightgunAxis p1x =
            rom::widened_axis(spec.p1x, m_wide_columns, render::kNativeWidth);
        const rom::LightgunAxis p2x =
            rom::widened_axis(spec.p2x, m_wide_columns, render::kNativeWidth);

        // The picture's very edge counts as off screen, so a gun that cannot
        // aim past it can still reload.
        constexpr float kReloadZone = 0.01f;
        const auto reload_zone = [](float& aim) {
            if (aim < kReloadZone) {
                aim = -1.0f;
            } else if (aim > 1.0f - kReloadZone) {
                aim = 2.0f;
            }
        };
        reload_zone(p1.x);
        reload_zone(p1.y);
        reload_zone(p2.x);
        reload_zone(p2.y);

        // RS-422 lightgun: off-screen reload snaps the aim past the corner.
        if (p1.reload) { p1.x = -1.0f; p1.y = -1.0f; }
        if (p2.reload) { p2.x = -1.0f; p2.y = -1.0f; }
        inputs->gun_p1x = fraction_to_gun(p1.x, p1x);
        inputs->gun_p1y = fraction_to_gun(p1.y, spec.p1y);
        inputs->gun_p2x = fraction_to_gun(p2.x, p2x);
        inputs->gun_p2y = fraction_to_gun(p2.y, spec.p2y);
    }

    // Triggers, active low: player 1 on IN1 bit 0, player 2 on IN1 bit 1.
    if (p1.trigger) inputs->in1 &= static_cast<u8>(~kButton1);
    if (p2.trigger) inputs->in1 &= static_cast<u8>(~kButton2);

    // Missile (bel): P1 IN1 0x10, P2 IN1 0x20.
    if (p1.missile) inputs->in1 &= static_cast<u8>(~0x10);
    if (p2.missile) inputs->in1 &= static_cast<u8>(~0x20);

    // Coin and Start from the gun buttons, on the operator port. start1_bit
    // carries the title's start bit, which some sets move off 0x10.
    const u8 start1 = game.start1_bit != 0 ? game.start1_bit : kStart1;
    if (p1.coin)  inputs->in0 &= static_cast<u8>(~kCoin1);
    if (p1.start) inputs->in0 &= static_cast<u8>(~start1);
    if (p2.coin)  inputs->in0 &= static_cast<u8>(~kCoin2);
    if (p2.start) inputs->in0 &= static_cast<u8>(~kStart2);

    // Gun hat -> the player port's direction bits (P1 in1, P2 in2).
    const auto hat = [](u8& port, const GunInput& g) {
        if (g.up)    port &= static_cast<u8>(~kUp);
        if (g.down)  port &= static_cast<u8>(~kDown);
        if (g.left)  port &= static_cast<u8>(~kLeft);
        if (g.right) port &= static_cast<u8>(~kRight);
    };
    hat(inputs->in1, p1);
    hat(inputs->in2, p2);
}

void Input::poll(hw::Inputs* inputs) const
{
    poll(inputs, rom::GameSpec{});
}

void Input::poll(hw::Inputs* inputs, const rom::GameSpec& game) const
{
    inputs->in3 = 0xff;
    const rom::InputFlags game_inputs = game.inputs;
    (void)game_inputs;

    u8 ports[1 + kPlayers] = {0xff, 0xff, 0xff};

    // Sega Ski Super G drives itself entirely from three Select buttons and two
    // Zoom buttons, spread across IN0 (0x10/0x20/0x40/0x80) and IN1 (0x01) --
    // not the generic IN1 button nibble. It gets its own key and pad map below
    // and is excluded from the generic ones so those bits are not driven twice.
    const bool is_ski = game.name == "skisuprg" || game.parent == "skisuprg";

    // Sega Water Ski: six buttons on their own IN0/IN1 bits (two Pitch/trick,
    // Select Up/Down, Set), not the generic nibble. Own map below, excluded
    // from the generic ones.
    const bool is_wski = game.name == "segawski" || game.parent == "segawski";

    // Sky Target: fire buttons are Machine Gun (IN1 0x10) and Missile (IN1
    // 0x20), not the generic button nibble, so a pad never fired. Own map below.
    const bool is_skytgt = game.name == "skytargt" || game.parent == "skytargt";

    int         key_count = 0;
    const bool* keys      = SDL_GetKeyboardState(&key_count);
    if (keys != nullptr) {
        gather_keys(&ports[0], keys, key_count, kOperatorKeys);
        if (!is_ski && !is_wski && !is_skytgt) {
            gather_keys(&ports[1], keys, key_count, kPlayerOneKeys);
        }
        gather_keys(&ports[2], keys, key_count, kPlayerTwoKeys);

        // kOperatorKeys pressed the default start1 bit (kStart1 = 0x10); some
        // titles (Indy 500, Sky Target, Manx TT and Top Skater family) move
        // START1 to another IN0 bit. Redirect the press to the real bit so the
        // keyboard start key works on those sets, matching the scripted and
        // gun/wheel paths which already honour start1_bit.
        const u8 start1 = game.start1_bit != 0 ? game.start1_bit : kStart1;
        if (start1 != kStart1 && (ports[0] & kStart1) == 0) {
            ports[0] |= kStart1;                        // release the wrong bit
            ports[0] &= static_cast<u8>(~start1);       // press the real one
        }

        // VR/view buttons on the keyboard: B N M , = VR1..VR4
        if (game.vr_buttons_declared) {
            static constexpr SDL_Scancode kVrKeys[4] = {
                SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA,
            };
            for (u8 i = 0; i < game.vr_button_count && i < 4; ++i) {
                const SDL_Scancode sc = kVrKeys[i];
                if (static_cast<int>(sc) < key_count && keys[sc]) {
                    const auto [port, bit] = game.wheel_button_bits[i];
                    if (port < 3) {
                        ports[port] &= static_cast<u8>(~bit);
                    }
                }
            }
        }
    }

    // Virtual On is a single-cabinet twin-stick title: both 8-way sticks and all
    // four buttons are player one's, split across IN1 and IN2.
    const bool is_von = game.name == "von" || game.parent == "von";

    // Desert Tank is a single-cabinet vehicle title whose buttons do not follow
    // the generic per-player layout.
    const bool is_desert = game.name == "desert" || game.parent == "desert";

    const bool is_gun = game.lightgun.present
                     || std::any_of(game.analog.begin(), game.analog.end(),
                                    [](const rom::AnalogChannel& c) {
                                        return c.control == rom::AnalogControl::Gun1X;
                                    });

    for (const Pad& pad : m_pads) {
        if (!plays(pad) || pad.player >= kPlayers || is_von || is_desert
            || is_ski || is_wski || is_skytgt) {
            continue;
        }
        u8& port = ports[1 + pad.player];

        // Button 1-4 and the four directions read their bindings; Start and
        // Coin are handled with the Guide chord below.
        static constexpr u8 kPadRoleBit[] = {
            kButton1, kButton2, kButton3, kButton4, kUp, kDown, kLeft, kRight,
        };
        const auto& binds = m_pad_buttons[pad.player];
        bool lb_bound = false;
        bool rb_bound = false;
        for (usize role = 0; role < std::size(kPadRoleBit); ++role) {
            const bool gun_role = is_gun && role <= static_cast<usize>(Config::PadRole::Button2);
            if (!gun_role && binding_held(pad.handle, binds[role])) {
                port &= static_cast<u8>(~kPadRoleBit[role]);
            }
            lb_bound |= binds[role] == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
            rb_bound |= binds[role] == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        }

        // The shoulders also act as Button 3/4 while those keep their default
        // binding and the shoulder isn't bound to one of the roles above.
        static const auto kDefaults = Config::PadLayout{}.buttons[0];
        constexpr auto kB3 = static_cast<u32>(Config::PadRole::Button3);
        constexpr auto kB4 = static_cast<u32>(Config::PadRole::Button4);
        lb_bound |= binds[kB3] != kDefaults[kB3];
        rb_bound |= binds[kB4] != kDefaults[kB4];
        if (!lb_bound && SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
            port &= static_cast<u8>(~kButton3);
        }
        if (!rb_bound && SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            port &= static_cast<u8>(~kButton4);
        }

        // Pad VR buttons: A/B/X/Y -> VR1-4 through the declared port/bit, for
        // titles that wire them off the default IN1 nibble (Daytona) where the
        // generic face-button mapping above cannot reach them. Player one only.
        if (game.vr_buttons_declared && pad.player == 0) {
            static constexpr SDL_GamepadButton kVrPadButtons[4] = {
                SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
            };
            for (u8 i = 0; i < game.vr_button_count && i < 4; ++i) {
                if (pad_button_held(pad, kVrPadButtons[i])) {
                    const auto [vport, vbit] = game.wheel_button_bits[i];
                    if (vport < 3) {
                        ports[vport] &= static_cast<u8>(~vbit);
                    }
                }
            }
        }

        // The left stick drives the same four switches as the d-pad, so either works
        // and holding both is harmless.
        port &= static_cast<u8>(
            ~stick_bits(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX),
                        SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY)));

        // Start/coin; coin 2 is player 2's slot.
        const auto held = [&](Config::PadRole role) {
            return binding_held(pad.handle, binds[static_cast<u32>(role)]);
        };
        const bool start = held(Config::PadRole::Start);
        const bool back  = held(Config::PadRole::Coin);
        if (!operator_chord(pad, &ports[0])) {
            if (start) {
                const u8 start1 = game.start1_bit != 0 ? game.start1_bit : kStart1;
                ports[0] &= static_cast<u8>(~(pad.player == 0 ? start1 : kStart2));
            }
            if (back) {
                ports[0] &= static_cast<u8>(~(pad.player == 0 ? kCoin1 : kCoin2));
            }
        }
    }

    // Sega Ski Super G: three Selects and two Zooms on their own IN0/IN1 bits,
    // not the generic button nibble. It has no start button -- Test is what
    // advances the game -- so plain Start presses Test.
    if (is_ski) {
        struct SkiBit { u8 port; u8 bit; };
        constexpr SkiBit kSelect1{0, 0x40};
        constexpr SkiBit kSelect2{0, 0x80};
        constexpr SkiBit kSelect3{0, 0x10};
        constexpr SkiBit kZoomIn {0, 0x20};
        constexpr SkiBit kZoomOut{1, 0x01};
        const auto press = [&](SkiBit b) { ports[b.port] &= static_cast<u8>(~b.bit); };

        // Keyboard: Select 1/2/3 = Z/X/C, Zoom In/Out = V/B (off the arrows,
        // which nudge the analog).
        if (keys != nullptr) {
            const auto kdown = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            if (kdown(SDL_SCANCODE_Z)) press(kSelect1);
            if (kdown(SDL_SCANCODE_X)) press(kSelect2);
            if (kdown(SDL_SCANCODE_C)) press(kSelect3);
            if (kdown(SDL_SCANCODE_V)) press(kZoomIn);
            if (kdown(SDL_SCANCODE_B)) press(kZoomOut);
        }

        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            const auto held = [&](SDL_GamepadButton b) { return pad_button_held(pad, b); };
            if (held(SDL_GAMEPAD_BUTTON_WEST))      press(kSelect1);
            if (held(SDL_GAMEPAD_BUTTON_NORTH))     press(kSelect2);
            if (held(SDL_GAMEPAD_BUTTON_EAST))      press(kSelect3);
            if (held(SDL_GAMEPAD_BUTTON_DPAD_UP))   press(kZoomIn);
            if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) press(kZoomOut);

            const bool start = held(SDL_GAMEPAD_BUTTON_START);
            const bool back  = held(SDL_GAMEPAD_BUTTON_BACK);
            if (!operator_chord(pad, &ports[0])) {
                if (start) ports[0] &= static_cast<u8>(~kTest);   // no start button; Test advances
                if (back)  ports[0] &= static_cast<u8>(~kCoin1);
            }
        }
    }

    // Sega Water Ski: Pitch Left/Right (the jump/trick buttons) on the shoulders
    // so they work while steering; Select Up/Down on the d-pad; Set on A. Start
    // and coin keep their defaults. Keyboard -- Pitch L/R = Z/X, Select Up/Down =
    // Up/Down, Set = C.
    if (is_wski) {
        struct WsBit { u8 port; u8 bit; };
        constexpr WsBit kPitchLeft {1, 0x04};
        constexpr WsBit kPitchRight{1, 0x08};
        constexpr WsBit kSelectUp  {1, 0x02};
        constexpr WsBit kSelectDown{0, 0x40};
        constexpr WsBit kSet       {1, 0x01};
        const auto press = [&](WsBit b) { ports[b.port] &= static_cast<u8>(~b.bit); };

        if (keys != nullptr) {
            const auto kdown = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            if (kdown(SDL_SCANCODE_Z))    press(kPitchLeft);
            if (kdown(SDL_SCANCODE_X))    press(kPitchRight);
            if (kdown(SDL_SCANCODE_UP))   press(kSelectUp);
            if (kdown(SDL_SCANCODE_DOWN)) press(kSelectDown);
            if (kdown(SDL_SCANCODE_C))    press(kSet);
        }

        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            const auto held = [&](SDL_GamepadButton b) { return pad_button_held(pad, b); };
            if (held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  press(kPitchLeft);
            if (held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) press(kPitchRight);
            if (held(SDL_GAMEPAD_BUTTON_DPAD_UP))        press(kSelectUp);
            if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN))      press(kSelectDown);
            if (held(SDL_GAMEPAD_BUTTON_SOUTH))          press(kSet);

            const bool start = held(SDL_GAMEPAD_BUTTON_START);
            const bool back  = held(SDL_GAMEPAD_BUTTON_BACK);
            if (!operator_chord(pad, &ports[0])) {
                if (start) ports[0] &= static_cast<u8>(~kStart1);
                if (back)  ports[0] &= static_cast<u8>(~kCoin1);
            }
        }
    }

    // Sky Target: a flight-stick shooter. Machine Gun (IN1 0x10) on the right
    // trigger and Missile (IN1 0x20) on the left, View Change (IN0 0x20) on Y;
    // the left stick is the flight stick. Keyboard -- gun = Left Ctrl, missile =
    // X, view = keep the VR key. Start (IN0 0x40) and coin keep their defaults.
    if (is_skytgt) {
        constexpr int kTriggerPress = 8000;  // half pull counts as a press
        if (keys != nullptr) {
            const auto kdown = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            if (kdown(SDL_SCANCODE_LCTRL)) ports[1] &= static_cast<u8>(~0x10);
            if (kdown(SDL_SCANCODE_X))     ports[1] &= static_cast<u8>(~0x20);
        }
        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerPress)
                ports[1] &= static_cast<u8>(~0x10);   // Machine Gun
            if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerPress)
                ports[1] &= static_cast<u8>(~0x20);   // Missile
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_NORTH))
                ports[0] &= static_cast<u8>(~0x20);   // View Change

            const bool start = pad_button_held(pad, SDL_GAMEPAD_BUTTON_START);
            const bool back  = pad_button_held(pad, SDL_GAMEPAD_BUTTON_BACK);
            if (!operator_chord(pad, &ports[0])) {
                if (start) ports[0] &= static_cast<u8>(~0x40);  // Start1
                if (back)  ports[0] &= static_cast<u8>(~kCoin1);
            }
        }
    }

    // Wheel buttons, driving player one, each from its bound button index. The
    // four arcade buttons are also where a driving cabinet's view/VR buttons sit,
    // so binding those covers them. Gears are handled with the gearbox below.
    if (m_wheel.handle != nullptr) {
        const int count = SDL_GetNumJoystickButtons(m_wheel.handle);
        const auto role_pressed = [&](Config::WheelRole role) {
            const s32 button = m_wheel_settings.buttons[static_cast<usize>(role)];
            return button >= 0 && button < count
                && SDL_GetJoystickButton(m_wheel.handle, button);
        };

        // The four action buttons go to whichever port/bit this title wires its
        // view/VR buttons to (Daytona spreads them across IN0 and IN1), not a
        // fixed IN1 nibble.
        const Config::WheelRole action[] = {
            Config::WheelRole::Button1, Config::WheelRole::Button2,
            Config::WheelRole::Button3, Config::WheelRole::Button4,
        };
        for (int i = 0; i < 4; ++i) {
            if (role_pressed(action[i])) {
                const auto [port, bit] = game.wheel_button_bits[static_cast<usize>(i)];
                if (port < 3) {
                    ports[port] &= static_cast<u8>(~bit);
                }
            }
        }
        // Start sits on whichever IN0 bit this title uses (0x10 by default, but
        // Indy 500, Sky Target, Manx TT and family move it to 0x40).
        const u8 start_bit = game.start1_bit != 0 ? game.start1_bit : kStart1;
        if (role_pressed(Config::WheelRole::Start))   ports[0] &= static_cast<u8>(~start_bit);
        if (role_pressed(Config::WheelRole::Coin))    ports[0] &= static_cast<u8>(~kCoin1);
        if (role_pressed(Config::WheelRole::Test))    ports[0] &= static_cast<u8>(~kTest);
        if (role_pressed(Config::WheelRole::Service)) ports[0] &= static_cast<u8>(~kService);
    }

    inputs->in0 = ports[0];
    inputs->in1 = ports[1];
    inputs->in2 = ports[2];

    // Analogue channels follow the title's own machine config: which channel a
    // control sits on, how far it travels and where it rests are all per-title.
    // An unconnected channel reads zero, as an unbound an_port_callback does.
    // Top Skater wants fine carving; the rest are linear.
    const bool  fine_stick = game.name == "topskatr" || game.parent == "topskatr";
    const float curve      = fine_stick ? 2.0f : 1.0f;
    for (usize channel = 0; channel < inputs->analog.size(); ++channel) {
        const rom::AnalogChannel& wiring = game.analog[channel];
        inputs->analog[channel] = wiring.control == rom::AnalogControl::None
                                      ? 0x00
                                      : sample_channel(wiring, curve);
    }

    // Keyboard analog driver, so any analog title is playable without a pad:
    // Left/Right drive the centring X axes, Up/Down the centring Y axes, Left
    // Ctrl the gas and Left Alt the brake. Only overrides a channel while a key
    // is held, so the pad axis still works otherwise; the game-specific blocks
    // below run after and win for their own channels.
    if (keys != nullptr) {
        const auto down = [&](SDL_Scancode sc) {
            return static_cast<int>(sc) < key_count && keys[sc];
        };
        const bool kleft  = down(SDL_SCANCODE_LEFT);
        const bool kright = down(SDL_SCANCODE_RIGHT);
        const bool kup    = down(SDL_SCANCODE_UP);
        const bool kdown  = down(SDL_SCANCODE_DOWN);
        const bool kgas   = down(SDL_SCANCODE_LCTRL);
        const bool kbrake = down(SDL_SCANCODE_LALT);

        const auto drive = [&](usize ch, bool high) {
            const rom::AnalogChannel& c = game.analog[ch];
            const bool at_max = c.reverse ? !high : high;
            inputs->analog[ch] = at_max ? c.maximum : c.minimum;
        };

        for (usize ch = 0; ch < game.analog.size(); ++ch) {
            switch (game.analog[ch].control) {
                case rom::AnalogControl::Steer:
                case rom::AnalogControl::Bank:
                case rom::AnalogControl::Handle:
                case rom::AnalogControl::StickX:
                case rom::AnalogControl::Roll:
                case rom::AnalogControl::Slide:
                case rom::AnalogControl::Inclining:
                case rom::AnalogControl::Curving:
                    if (kleft != kright) drive(ch, kright);
                    break;
                case rom::AnalogControl::StickY:
                case rom::AnalogControl::Pitch:
                case rom::AnalogControl::Swing:
                    if (kup != kdown) drive(ch, kdown);
                    break;
                case rom::AnalogControl::Accel:
                case rom::AnalogControl::Throttle:
                case rom::AnalogControl::Bat1:
                    if (kgas) drive(ch, true);
                    break;
                case rom::AnalogControl::Brake:
                case rom::AnalogControl::Bat2:
                    if (kbrake) drive(ch, true);
                    break;
                default:
                    break;  // Gun1/2 X/Y: gather_lightguns.
            }
        }
    }

    // Sega Ski Super G turns hardest with inclining and swing moving together,
    // as the platform does; their raw values run in opposite directions.
    if (is_ski) {
        usize incline_ch = inputs->analog.size();
        usize swing_ch   = inputs->analog.size();
        for (usize ch = 0; ch < game.analog.size(); ++ch) {
            if (game.analog[ch].control == rom::AnalogControl::Inclining) incline_ch = ch;
            if (game.analog[ch].control == rom::AnalogControl::Swing)     swing_ch   = ch;
        }
        if (incline_ch < inputs->analog.size() && swing_ch < inputs->analog.size()) {
            // D-pad / arrow left-right snaps the steer to full lock instantly
            // for quick right-to-left flicks a stick cannot make; reverse means
            // full lock left is the channel maximum. The stick still steers
            // proportionally when the d-pad is idle.
            const rom::AnalogChannel& inc = game.analog[incline_ch];
            bool snap_left  = false;
            bool snap_right = false;
            if (keys != nullptr) {
                const auto kdown = [&](SDL_Scancode sc) {
                    return static_cast<int>(sc) < key_count && keys[sc];
                };
                snap_left  = kdown(SDL_SCANCODE_LEFT);
                snap_right = kdown(SDL_SCANCODE_RIGHT);
            }
            for (const Pad& pad : m_pads) {
                if (!plays(pad) || pad.player != 0) continue;
                if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                    snap_left = true;
                if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                    snap_right = true;
            }
            if (snap_left != snap_right) {
                const bool left_is_max = inc.reverse;
                inputs->analog[incline_ch] =
                    (snap_left == left_is_max) ? inc.maximum : inc.minimum;
            }

            const rom::AnalogChannel& swing = game.analog[swing_ch];
            inputs->analog[swing_ch] = static_cast<u8>(
                swing.maximum - (inputs->analog[incline_ch] - inc.minimum));
        }
    }

    // Top Skater special-case. The game has no digital d-pad; it steers with an
    // analog board (Curving) and navigates its menus with digital "Select
    // Left/Right" buttons on IN0 (0x80/0x10), plus Jump Front (IN0 0x20) and
    // Jump Tail (IN1 0x01). The generic keyboard/pad mapping puts buttons on the 
    // player port, so neither the menu nor keyboard steering worked.
    // Wire it directly: arrow Left/Right (and the d-pad) drive BOTH the digital
    // Select bits (menu) and the Curving analog (in-game steer), 
    // so one control is correct in both states.
    if (game.name == "topskatr" || game.parent == "topskatr") {
        constexpr u8 kSelectLeft  = 0x80;  // IN0
        constexpr u8 kSelectRight = 0x10;  // IN0
        constexpr u8 kJumpFront   = 0x20;  // IN0
        constexpr u8 kJumpTailIn1 = 0x01;  // IN1

        bool left = false, right = false, jump_front = false, jump_tail = false;
        if (keys != nullptr) {
            const auto down = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            left       = down(SDL_SCANCODE_LEFT);
            right      = down(SDL_SCANCODE_RIGHT);
            jump_front = down(SDL_SCANCODE_Z);
            jump_tail  = down(SDL_SCANCODE_X);
        }
        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            left       |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            right      |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            jump_front |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_SOUTH);
            jump_tail  |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_EAST);
        }

        // Digital menu Select and jumps, active low on their real bits.
        if (left)       inputs->in0 &= static_cast<u8>(~kSelectLeft);
        if (right)      inputs->in0 &= static_cast<u8>(~kSelectRight);
        if (jump_front) inputs->in0 &= static_cast<u8>(~kJumpFront);
        if (jump_tail)  inputs->in1 &= static_cast<u8>(~kJumpTailIn1);

        // Past its limit Slide skids the board instead of turning, so it sits on
        // the triggers rather than the steering axis: left skids left.
        float slide = 0.0f;
        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            const auto pull = [&](SDL_GamepadAxis axis) {
                const int raw = static_cast<int>(SDL_GetGamepadAxis(pad.handle, axis));
                return static_cast<float>(std::max(0, raw - kPedalFloor))
                     / static_cast<float>(32767 - kPedalFloor);
            };
            slide = pull(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) - pull(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            break;
        }
        if (keys != nullptr) {
            const bool skid_left  = SDL_SCANCODE_C < key_count && keys[SDL_SCANCODE_C];
            const bool skid_right = SDL_SCANCODE_V < key_count && keys[SDL_SCANCODE_V];
            if (skid_left != skid_right) {
                slide = skid_right ? 1.0f : -1.0f;
            }
        }
        for (usize channel = 0; channel < inputs->analog.size(); ++channel) {
            const rom::AnalogChannel& wiring = game.analog[channel];
            if (wiring.control != rom::AnalogControl::Slide) {
                continue;
            }
            const float fraction = 0.5f + slide * 0.5f;
            const float span = static_cast<float>(wiring.maximum - wiring.minimum);
            inputs->analog[channel] = static_cast<u8>(
                static_cast<float>(wiring.minimum) + fraction * span + 0.5f);
        }

        // Keyboard steer: nudge the Curving channel off its centre rest so the
        // skater turns without a pad stick. The pad left stick already drives
        // this through sample_channel; only override when a key is actually held
        // so the stick still works when no arrow is pressed.
        for (usize channel = 0; channel < inputs->analog.size(); ++channel) {
            if (game.analog[channel].control != rom::AnalogControl::Curving) {
                continue;
            }
            if (left != right) {  // one, not both
                inputs->analog[channel] = left ? 0xff : 0x00;
            }
        }
    }

    // Power Sled. Entry and Call are IN1 buttons 1 and 2, which the generic
    // player-one mapping already covers. Its two cabinet-only buttons are not:
    // Cancel Error on IN0 0x80 and Cancel Network Check on IN3 0x02, the latter
    // letting a lone unit give up waiting for the rest of the linked set.
    if (game.name == "powsled" || game.parent == "powsled") {
        constexpr u8 kCancelError   = 0x80;  // IN0
        constexpr u8 kCancelNetwork = 0x02;  // IN3

        constexpr u8 kP2Entry       = 0x04;  // IN1, button 3
        constexpr u8 kP2Call        = 0x08;  // IN1, button 4

        bool cancel_error = false, cancel_network = false;
        bool p2_entry = false, p2_call = false;
        if (keys != nullptr) {
            const auto down = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            cancel_error   = down(SDL_SCANCODE_B);
            cancel_network = down(SDL_SCANCODE_N);
            p2_entry       = down(SDL_SCANCODE_C);
            p2_call        = down(SDL_SCANCODE_V);
        }
        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }
            cancel_error   |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            cancel_network |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            p2_entry       |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_WEST);
            p2_call        |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_NORTH);
        }
        if (cancel_error)   inputs->in0 &= static_cast<u8>(~kCancelError);
        if (cancel_network) inputs->in3 &= static_cast<u8>(~kCancelNetwork);

        // The shoulders are the cancels here, not the generic buttons 3 and 4,
        // so P2 Entry/Call follow only X/Y and C/V.
        if (!p2_entry) inputs->in1 |= kP2Entry;
        if (!p2_call)  inputs->in1 |= kP2Call;
    }

    // Virtual On twin-stick layout. port map puts the whole cabinet on player one,
    // split across two ports:
    //   IN1: 0x01 Left Shot, 0x02 Left Dash, and the LEFT stick's four directions
    //   IN2: 0x01 Right Shot, 0x02 Right Dash, and the RIGHT stick's directions
    // One pad drives both levers: the analog sticks are the two levers, 
    // the d-pad and face buttons double the left and right levers respectively 
    // for the custom twin-stick feel, the bumpers are the dash (boost) buttons
    // and the triggers are the shot buttons.
    if (is_von) {
        u8 in1 = 0;  // bits to press (active high here; folded in active-low below)
        u8 in2 = 0;

        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }

            // Left lever: left analog stick, doubled by the d-pad.
            in1 |= stick_bits(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX),
                              SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY));
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))    in1 |= kUp;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  in1 |= kDown;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  in1 |= kLeft;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) in1 |= kRight;

            // Right lever: right analog stick, doubled by the face buttons
            // (Y/A up/down, X/B left/right) to account for the custom twin-stick.
            {
                const auto lx = static_cast<u32>(Config::PadAxisRole::LeverX);
                const auto ly = static_cast<u32>(Config::PadAxisRole::LeverY);
                const s32  bx = m_pad_axes[pad.player][lx];
                const s32  by = m_pad_axes[pad.player][ly];
                in2 |= stick_bits(
                    SDL_GetGamepadAxis(pad.handle, bx >= 0 ? static_cast<SDL_GamepadAxis>(bx)
                                                           : SDL_GAMEPAD_AXIS_RIGHTX),
                    SDL_GetGamepadAxis(pad.handle, by >= 0 ? static_cast<SDL_GamepadAxis>(by)
                                                           : SDL_GAMEPAD_AXIS_RIGHTY));
            }
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_NORTH)) in2 |= kUp;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_SOUTH)) in2 |= kDown;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_WEST))  in2 |= kLeft;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_EAST))  in2 |= kRight;

            // Shots on the triggers, dashes (boost) on the bumpers.
            if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kPedalFloor) {
                in1 |= kButton1;  // Left Shot
            }
            if (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kPedalFloor) {
                in2 |= kButton1;  // Right Shot (IN2 0x01)
            }
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
                in1 |= kButton2;  // Left Dash
            }
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
                in2 |= kButton2;  // Right Dash (IN2 0x02)
            }

            // Start and coin, as for any other pad.
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_START)) {
                inputs->in0 &= static_cast<u8>(~kStart1);
            }
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_BACK)) {
                inputs->in0 &= static_cast<u8>(~kCoin1);
            }
        }

        // Keyboard twin-stick (one-player cabinet, so WASD is free for the left
        // lever): WASD left lever, Q/E left shot/dash; arrows right lever,
        // Right Shift/Right Ctrl right shot/dash.
        if (keys != nullptr) {
            const auto kd = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            if (kd(SDL_SCANCODE_W)) in1 |= kUp;
            if (kd(SDL_SCANCODE_S)) in1 |= kDown;
            if (kd(SDL_SCANCODE_A)) in1 |= kLeft;
            if (kd(SDL_SCANCODE_D)) in1 |= kRight;
            if (kd(SDL_SCANCODE_Q)) in1 |= kButton1;  // Left Shot
            if (kd(SDL_SCANCODE_E)) in1 |= kButton2;  // Left Dash
            if (kd(SDL_SCANCODE_UP))    in2 |= kUp;
            if (kd(SDL_SCANCODE_DOWN))  in2 |= kDown;
            if (kd(SDL_SCANCODE_LEFT))  in2 |= kLeft;
            if (kd(SDL_SCANCODE_RIGHT)) in2 |= kRight;
            if (kd(SDL_SCANCODE_RSHIFT)) in2 |= kButton1;  // Right Shot
            if (kd(SDL_SCANCODE_RCTRL))  in2 |= kButton2;  // Right Dash
        }

        // Rebuild both ports from the twin-stick state, dropping the stray bits
        // the generic keyboard/pad pass wrote (they do not match this layout).
        inputs->in1 = static_cast<u8>(~in1);
        inputs->in2 = static_cast<u8>(~in2);
    }

    // Desert Tank layout. MAME's `desert` port map (see model2o_state::desert):
    //   analog: ch0 STEER (paddle), ch1 ACCEL (pedal), ch2 turret elevation
    //           (IPT_AD_STICK_Y);
    //   IN0: 0x20 VR1, 0x40 VR2, 0x80 VR3 (the three coloured view buttons);
    //   IN1: 0x01 Shift (PORT_TOGGLE, forward/reverse), 0x10 Machine Gun,
    //        0x20 Cannon.
    // Steer (ch0 -> left stick X), turret elevation (ch2 sticky -> left stick Y)
    // and accel (ch1 -> right trigger) all come through the generic analog
    // sampler; this block owns the digital buttons and the shift toggle. The
    // d-pad doubles the two left-stick axes and the three views sit on the face
    // buttons (B/Y/X = VR1/VR2/VR3). One pad drives the whole cabinet.
    //
    // Keyboard: arrows steer (L/R) and elevate the turret (up/down), Left Shift
    // is gas, Z/X are the guns, B/N/M are the views and Space taps the shift.
    if (is_desert) {
        // Steer (ch0) and turret elevation (ch2, sticky) by control, not index.
        int steer_ch  = -1;
        int turret_ch = -1;
        for (usize ch = 0; ch < game.analog.size(); ++ch) {
            if (game.analog[ch].control == rom::AnalogControl::Steer)  steer_ch  = static_cast<int>(ch);
            if (game.analog[ch].control == rom::AnalogControl::StickY) turret_ch = static_cast<int>(ch);
        }

        // Drive an analog channel to one end of its declared travel, honouring
        // PORT_REVERSE. `high` picks the maximum end (before reverse).
        const auto slam = [&](int ch, bool high) {
            if (ch < 0) {
                return;
            }
            const rom::AnalogChannel& c = game.analog[static_cast<usize>(ch)];
            const bool at_max = c.reverse ? !high : high;
            inputs->analog[static_cast<usize>(ch)] = at_max ? c.maximum : c.minimum;
        };

        bool shift_now  = false;
        bool mg         = false;
        bool cannon     = false;
        u8   in0_clear  = 0;

        // Keyboard: Space taps the forward/reverse shift.
        if (keys != nullptr && static_cast<int>(SDL_SCANCODE_SPACE) < key_count) {
            shift_now |= keys[SDL_SCANCODE_SPACE];
        }

        for (const Pad& pad : m_pads) {
            if (!plays(pad) || pad.player != 0) {
                continue;
            }

            // Steer and turret elevation are both on the left stick (X and Y)

            // The d-pad doubles the two analog axes, slamming them to an extreme
            // while held (SDL's stick Y grows downward, so Up is the minimum end
            // and raises the turret the same way pushing the stick up does).
            const bool dl = pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            const bool dr = pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            const bool du = pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
            const bool dd = pad_button_held(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            if (dl != dr) slam(steer_ch, dr);
            if (du != dd) slam(turret_ch, dd);

            mg     |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            cannon |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);

            // Shift on A or the left trigger; either flips forward/reverse.
            shift_now |= pad_button_held(pad, SDL_GAMEPAD_BUTTON_SOUTH)
                      || SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kPedalFloor;

            // Views on the face buttons: B=VR1, Y=VR2, X=VR3.
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_EAST))  in0_clear |= 0x20;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_NORTH)) in0_clear |= 0x40;
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_WEST))  in0_clear |= 0x80;

            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_START)) {
                inputs->in0 &= static_cast<u8>(~kStart1);
            }
            if (pad_button_held(pad, SDL_GAMEPAD_BUTTON_BACK)) {
                inputs->in0 &= static_cast<u8>(~kCoin1);
            }
        }

        // The generic player-one keyboard put arrows and Z/X/C/V on IN1's
        // direction and low-nibble bits, none of which are desert's layout (its
        // IN1 is Shift 0x01, Machine Gun 0x10, Cannon 0x20). Release the whole
        // port and drive only the real bits from here.
        inputs->in1 = 0xff;

        // Keyboard guns: Z = Machine Gun, X = Cannon.
        if (keys != nullptr) {
            const auto down = [&](SDL_Scancode sc) {
                return static_cast<int>(sc) < key_count && keys[sc];
            };
            mg     |= down(SDL_SCANCODE_Z);
            cannon |= down(SDL_SCANCODE_X);

            // Keyboard analog: arrows steer (L/R) and elevate the turret (up/down),
            // Left Shift is the gas. Each drives its channel to an extreme while
            // held and leaves it at rest otherwise, so the pad axes still work
            // when no key is down. Up is the turret's minimum end, matching the
            // pad (SDL stick Y grows downward, so pushing up is the low value).
            const bool kb_left  = down(SDL_SCANCODE_LEFT);
            const bool kb_right = down(SDL_SCANCODE_RIGHT);
            const bool kb_up    = down(SDL_SCANCODE_UP);
            const bool kb_down  = down(SDL_SCANCODE_DOWN);
            if (kb_left != kb_right) slam(steer_ch, kb_right);
            if (kb_up != kb_down)    slam(turret_ch, kb_down);
            if (down(SDL_SCANCODE_LSHIFT)) {
                for (usize ch = 0; ch < game.analog.size(); ++ch) {
                    if (game.analog[ch].control == rom::AnalogControl::Accel) {
                        const rom::AnalogChannel& c = game.analog[ch];
                        inputs->analog[ch] = c.reverse ? c.minimum : c.maximum;
                    }
                }
            }
        }

        if (shift_now && !m_desert_shift_held) {
            m_desert_shift = !m_desert_shift;
        }
        m_desert_shift_held = shift_now;

        if (m_desert_shift) inputs->in1 &= static_cast<u8>(~0x01);  // Shift (reverse)
        if (mg)             inputs->in1 &= static_cast<u8>(~0x10);  // Machine Gun
        if (cannon)         inputs->in1 &= static_cast<u8>(~0x20);  // Cannon
        inputs->in0 &= static_cast<u8>(~in0_clear);                 // VR1/VR2/VR3
    }

    gather_lightguns(inputs, game);

    // Gear selector, one bit per position. F1-F4 and an H shifter's buttons pick
    // gears 1-4 (bits 1..4) and F5 picks neutral (bit 0); paddles and RB/LB step
    // through the gears. With nothing held the last gear stays, or neutral for
    // an H shifter.
    if (game.gearbox) {
        if (m_gear_game != game.name) {
            m_wheel_gear           = game.start_gear;
            m_gear_game            = game.name;
            m_shift_neutral_frames = 0;
        }

        u8 gears = 0;
        if (keys != nullptr) {
            static constexpr SDL_Scancode kGearKeys[4] = {
                SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
            };
            for (u32 gear = 0; gear < 4; ++gear) {
                if (static_cast<int>(kGearKeys[gear]) < key_count && keys[kGearKeys[gear]]) {
                    gears |= static_cast<u8>(1u << (gear + 1));  // bit 1..4 = gear 1..4
                    m_wheel_gear = gear + 1;  // stays put after release
                }
            }
            if (static_cast<int>(SDL_SCANCODE_F5) < key_count && keys[SDL_SCANCODE_F5]) {
                gears |= 0x01;  // bit 0 = neutral
                m_wheel_gear = 0;
            }
        }

        const int  wheel_buttons = m_wheel.handle != nullptr
                                     ? SDL_GetNumJoystickButtons(m_wheel.handle) : 0;
        const auto role_bound    = [&](Config::WheelRole role) {
            const s32 button = m_wheel_settings.buttons[static_cast<usize>(role)];
            return button >= 0 && button < wheel_buttons;
        };
        const auto role_held = [&](Config::WheelRole role) {
            return role_bound(role)
                && SDL_GetJoystickButton(m_wheel.handle,
                                         m_wheel_settings.buttons[static_cast<usize>(role)]);
        };

        // An H shifter: a button per gear, and maybe one for neutral.
        static constexpr Config::WheelRole kGearRoles[4] = {
            Config::WheelRole::Gear1, Config::WheelRole::Gear2,
            Config::WheelRole::Gear3, Config::WheelRole::Gear4,
        };
        bool h_shifter = false;
        for (u32 gear = 0; gear < 4; ++gear) {
            h_shifter |= role_bound(kGearRoles[gear]);
            if (role_held(kGearRoles[gear])) {
                gears |= static_cast<u8>(1u << (gear + 1));
                m_wheel_gear = gear + 1;
            }
        }
        if (role_held(Config::WheelRole::GearNeutral)) {
            gears |= 0x01;
            m_wheel_gear = 0;
        }
        if (gears != 0) {
            m_shift_neutral_frames = 0;  // a lever in a position needs no crossing
        }

        // Paddles and RB/LB step through the gate, sharing m_wheel_gear. A shift
        // between two gears can pass through neutral for shift_neutral_ms.
        constexpr double kFrameHz = 57.5245;
        const u32 crossing = static_cast<u32>(
            m_wheel_settings.shift_neutral_ms * kFrameHz / 1000.0 + 0.999);
        const auto step = [&](bool up, bool down, bool* up_held, bool* down_held) {
            const u32 from = m_wheel_gear;
            if (up && !*up_held && m_wheel_gear < 4) {
                ++m_wheel_gear;
            }
            if (down && !*down_held && m_wheel_gear > 0) {
                --m_wheel_gear;
            }
            *up_held   = up;
            *down_held = down;
            if (m_wheel_gear != from && from != 0 && m_wheel_gear != 0) {
                m_shift_neutral_frames = crossing;
            }
        };
        bool sequential = false;
        if (m_wheel.handle != nullptr) {
            sequential = true;
            step(role_held(Config::WheelRole::GearUp), role_held(Config::WheelRole::GearDown),
                 &m_gear_up_held, &m_gear_down_held);
        }
        if (pad_for(0) != nullptr) {
            sequential = true;
            step(pad_role_held(0, Config::PadRole::GearUp),
                 pad_role_held(0, Config::PadRole::GearDown),
                 &m_pad_gear_up_held, &m_pad_gear_down_held);
        }

        // A position held (a key, an H shifter's button) wins over the paddles.
        if (gears == 0) {
            if (h_shifter && m_wheel_settings.shifter_neutral) {
                // The H shifter's lever is in none of the gears: neutral, as
                // on the cabinet.
                gears                  = 0x01;
                m_wheel_gear           = 0;
                m_shift_neutral_frames = 0;
            } else if (sequential) {
                gears = static_cast<u8>(m_shift_neutral_frames > 0 ? 0x01 : 1u << m_wheel_gear);
            }
        }
        if (m_shift_neutral_frames > 0) {
            --m_shift_neutral_frames;
        }

        inputs->gears = gears;
    }

    // Two-button shifters (Indy 500, Manx TT family): Shift Up on IN1 0x10, Down
    // on 0x20. Held bits, no gear state, from keyboard (F1/F2), pad (RB/LB) or
    // the wheel's GearUp/GearDown roles.
    if (game.shift_buttons) {
        bool up   = false;
        bool down = false;

        if (keys != nullptr) {
            up   |= static_cast<int>(SDL_SCANCODE_F1) < key_count && keys[SDL_SCANCODE_F1];
            down |= static_cast<int>(SDL_SCANCODE_F2) < key_count && keys[SDL_SCANCODE_F2];
        }
        up   |= pad_role_held(0, Config::PadRole::GearUp);
        down |= pad_role_held(0, Config::PadRole::GearDown);
        if (m_wheel.handle != nullptr) {
            const int count = SDL_GetNumJoystickButtons(m_wheel.handle);
            const auto role_held = [&](Config::WheelRole role) {
                const s32 button = m_wheel_settings.buttons[static_cast<usize>(role)];
                return button >= 0 && button < count
                    && SDL_GetJoystickButton(m_wheel.handle, button);
            };
            up   |= role_held(Config::WheelRole::GearUp);
            down |= role_held(Config::WheelRole::GearDown);
        }

        if (up)   inputs->in1 &= static_cast<u8>(~0x10);
        if (down) inputs->in1 &= static_cast<u8>(~0x20);
    }
}

std::vector<std::string> Input::gamepad_names() const
{
    std::vector<std::string> names(kPlayers);
    for (const Pad& pad : m_pads) {
        if (pad.player < kPlayers && pad.handle != nullptr) {
            const char* name = SDL_GetGamepadName(pad.handle);
            names[pad.player] = name != nullptr ? name : "gamepad";
        }
    }
    return names;
}

usize Input::gun_count() const
{
#ifdef SM2_HAVE_LIGHTGUNS
    return m_guns ? m_guns->count() : 0;
#else
    return 0;
#endif
}

std::string Input::gun_name(usize index) const
{
#ifdef SM2_HAVE_LIGHTGUNS
    if (m_guns && index < m_guns->count()) {
        return m_guns->gun(index).name;
    }
#else
    (void)index;
#endif
    return {};
}

u16 Input::gun_take_last_pressed(usize index) const
{
#ifdef SM2_HAVE_LIGHTGUNS
    if (m_guns && index < m_guns->count()) {
        m_guns->poll();  // ensure fresh while the overlay is open
        return m_guns->take_last_pressed(index);
    }
#else
    (void)index;
#endif
    return 0;
}

void Input::print_bindings()
{
    std::printf("Gamepad, per player:\n");
    std::printf("  d-pad or left stick  stick / steering, and other centred axes\n");
    std::printf("  A B X Y              buttons 1 to 4 (also VR 1-4 on view titles)\n");
    std::printf("  L / R shoulder       buttons 3/4; also gear/shift down/up on racers\n");
    std::printf("  L / R trigger        brake / accelerate on driving titles\n");
    std::printf("  right stick          aim on gun titles (no mouse needed)\n");
    std::printf("  Start / Back         start / insert coin\n");
    std::printf("  Guide + L3 / R3      service / test (operator menus)\n");
    std::printf("  Guide + A            settings menu\n");
    std::printf("  Guide + X / Y        quick-save / quick-load state\n");
    std::printf("  Guide + Left / Right rewind / fast-forward (held)\n");
    std::printf("  Guide + LB           screenshot\n");
    std::printf("  Guide + Start        back to the games, or quit\n");
    std::printf("\nKeyboard:\n");
    std::printf("  5 6                  coin 1, coin 2\n");
    std::printf("  1 2                  start 1, start 2\n");
    std::printf("  9 0                  service, test\n");
    std::printf("  arrows Z X C V       player 1 stick and buttons\n");
    std::printf("  W A S D  G H J K     player 2 stick and buttons\n");
    std::printf("  arrows               steering and centred analog axes\n");
    std::printf("  Left Ctrl / Left Alt accelerate / brake (driving titles)\n");
    std::printf("  F1 F2                shift up / down (Indy 500, Manx TT family)\n");
    std::printf("  F1-F4  F5            gears 1 to 4, neutral (gate gearbox titles)\n");
    std::printf("  B N M ,              VR / view buttons 1 to 4 (titles that have them)\n");
    std::printf("  Space                Desert Tank forward/reverse shift\n");
    std::printf("  arrows + Left Ctrl   aim + fire on gun titles (no mouse needed)\n");
    std::printf("  Escape               quit\n");
    std::printf("  P                    pause\n");
    std::printf("  Tab (held)           fast-forward\n");
    std::printf("  Backspace (held)     rewind, when turned on\n");
    std::printf("  F6 F7                quick-save, quick-load state (more slots in F10)\n");
    std::printf("  F8                   toggle FPS counter\n");
    std::printf("  F10 F11 F12          settings menu, fullscreen, screenshot\n");
    std::printf("  (Escape to F12 are the defaults; the System tab rebinds them)\n");
    std::printf("\nVirtual On (keyboard): WASD left lever, arrows right lever,\n");
    std::printf("Q/E left shot/dash, Right Shift/Right Ctrl right shot/dash.\n");
    std::printf("\nThe first gamepad to connect is player 1. Gamepads and the keyboard\n");
    std::printf("are both live, so a second player can join on the keyboard. A mouse or\n");
    std::printf("a wheel, where present, is the preferred device for gun/driving titles.\n");
}

Input::ScriptedPress Input::scripted_press(u32 frame, u32 coin_frame, u8 start1_bit)
{
    // Each press is a pulse rather than a hold: the program looks for the
    // transition, so a permanently held coin counts once and a permanently held
    // button can be read as a hold instead of a tap. Twelve frames is about a fifth
    // of a second, roughly the shortest a person manages.
    constexpr u32 kPulse = 12;
    constexpr u32 kGap   = 30;
    constexpr u32 kStep  = kPulse + kGap;

    struct Event {
        u32 offset;  ///< Frames after the coin frame.
        u8  in0;
        u8  in1;
    };
    // Two coins because the default setting asks for two per credit, then a
    // run of start presses to walk through however many confirmation screens
    // stand between the coin and gameplay -- a fighting game needs only one
    // (the character highlighted at boot is already fine), but a menu-heavy
    // racer like Motor Raid asks for player, then mode, then course, each on
    // its own screen with its own dwell time before the next input reads.
    // Pressing start on a screen that does not need it is harmless: Model 2's
    // start button has no effect once play has begun. Start1's bit varies by
    // game (several PORT_MODIFY it away from 0x10), hence the parameter.
    const Event kScript[] = {
        {0 * kStep, kCoin1, 0},
        {1 * kStep, kCoin1, 0},
        {2 * kStep, start1_bit, 0},
        {2 * kStep + 120, 0, kButton1},
        {4 * kStep, start1_bit, 0},
        {6 * kStep, start1_bit, 0},
        {8 * kStep, start1_bit, 0},
    };

    ScriptedPress press;
    for (const Event& event : kScript) {
        const u32 begin = coin_frame + event.offset;
        if (frame >= begin && frame < begin + kPulse) {
            press.in0 |= event.in0;
            press.in1 |= event.in1;
        }
    }
    return press;
}

}  // namespace sm2::osd
