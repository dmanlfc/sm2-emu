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
#include "osd/input.h"

#include "core/log.h"
#include "hw/model2.h"

#include <algorithm>
#include <cstdio>

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

struct PadBinding {
    SDL_GamepadButton button;
    u8                bit;
};

/// Face buttons in SDL's positional order, so a pad reports the same physical
/// position whatever its labels say. The two shoulders double up on buttons three
/// and four, which is what a six-button arcade layout on a modern pad wants.
constexpr PadBinding kPlayerPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, kButton1},
    {SDL_GAMEPAD_BUTTON_EAST, kButton2},
    {SDL_GAMEPAD_BUTTON_WEST, kButton3},
    {SDL_GAMEPAD_BUTTON_NORTH, kButton4},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kButton3},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kButton4},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
};

/// Half travel. Wide enough that a resting stick stays quiet and the diagonal band
/// is comfortable to hold.
constexpr int kStickThreshold = 16384;

/// A pedal rests at zero, so it needs no centre band, only a floor under the noise.
constexpr int kPedalFloor = 1024;

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

/// Map a mouse position in the focused SDL window onto one lightgun axis.
///
/// The gun interface board is 10-bit and each title calibrates its own travel,
/// so a pointer at the edge of the window has to land on that title's own
/// minimum or maximum rather than on 0 or 0x3ff. Anything else puts the
/// crosshair in the wrong place and makes the offscreen test fire early.
[[nodiscard]] u16 mouse_to_gun(float position, int extent, const rom::LightgunAxis& axis)
{
    if (extent <= 1) {
        return axis.rest;
    }
    const float maximum  = static_cast<float>(extent - 1);
    const float clamped  = std::clamp(position, 0.0f, maximum);
    const float fraction = clamped / maximum;
    const float span     = static_cast<float>(axis.maximum - axis.minimum);
    return static_cast<u16>(static_cast<float>(axis.minimum) + fraction * span + 0.5f);
}

/// Mouse position in the focused window, and that window's size.
struct PointerState {
    float x      = 0.0f;
    float y      = 0.0f;
    int   width  = 0;
    int   height = 0;
};

[[nodiscard]] PointerState pointer_state()
{
    PointerState state;
    SDL_GetMouseState(&state.x, &state.y);
    if (SDL_Window* focus = SDL_GetMouseFocus(); focus != nullptr) {
        SDL_GetWindowSize(focus, &state.width, &state.height);
    }
    return state;
}

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
    // to be collected once at startup. A wheel enumerates as a joystick with no
    // gamepad mapping, which is exactly what add_gamepad would reject, so the two
    // lists are walked separately.
    int             count = 0;
    SDL_JoystickID* ids   = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int index = 0; index < count; ++index) {
            add_gamepad(ids[index]);
        }
        SDL_free(ids);
    }

    ids = SDL_GetJoysticks(&count);
    if (ids != nullptr) {
        for (int index = 0; index < count; ++index) {
            if (!SDL_IsGamepad(ids[index])) {
                add_wheel(ids[index]);
            }
        }
        SDL_free(ids);
    }

    if (m_pads.empty() && m_wheel.handle == nullptr) {
        SM2_INFO("no gamepad found; the keyboard covers both players");
    }
    return true;
}

void Input::shutdown()
{
    for (Pad& pad : m_pads) {
        if (pad.handle != nullptr) {
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
            // A joystick with a gamepad mapping arrives as GAMEPAD_ADDED too and
            // is handled there; only the unmapped ones (wheels) are ours here.
            if (!SDL_IsGamepad(event.jdevice.which)) {
                add_wheel(event.jdevice.which);
            }
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            remove_wheel(event.jdevice.which);
            break;
        default:
            break;
    }
}

u32 Input::first_free_player() const
{
    for (u32 player = 0; player < kPlayers; ++player) {
        const bool taken = std::any_of(m_pads.begin(), m_pads.end(), [player](const Pad& pad) {
            return pad.player == player;
        });
        if (!taken) {
            return player;
        }
    }
    return kPlayers;
}

void Input::add_gamepad(SDL_JoystickID id)
{
    if (std::any_of(m_pads.begin(), m_pads.end(),
                    [id](const Pad& pad) { return pad.id == id; })) {
        return;
    }
    if (!SDL_IsGamepad(id)) {
        // A joystick SDL has no mapping for. Reporting it is worth doing, because
        // the user's device is plugged in and doing nothing.
        SM2_INFO("joystick %u has no gamepad mapping and is ignored",
                 static_cast<unsigned>(id));
        return;
    }

    const u32 player = first_free_player();
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
                                        return pad.player == player && pad.handle != nullptr;
                                    });
    return match != m_pads.end() ? match->handle : nullptr;
}

void Input::add_wheel(SDL_JoystickID id)
{
    if (m_wheel.handle != nullptr) {
        return;  // One wheel, driving player one, is all a Model 2 cabinet wires.
    }

    // Only take a device SDL classifies as a wheel. Anything else that lacks a
    // gamepad mapping -- a flightstick, an arcade panel, a controller SDL has no
    // profile for -- must not be pressed into service as a steering wheel. A
    // device that reports UNKNOWN is allowed through, since some wheels do, but a
    // positively-different type is refused.
    const SDL_JoystickType type = SDL_GetJoystickTypeForID(id);
    if (type != SDL_JOYSTICK_TYPE_WHEEL && type != SDL_JOYSTICK_TYPE_UNKNOWN) {
        return;
    }

    SDL_Joystick* handle = SDL_OpenJoystick(id);
    if (handle == nullptr) {
        SM2_WARN("could not open joystick %u: %s", static_cast<unsigned>(id),
                 SDL_GetError());
        return;
    }

    m_wheel        = Wheel{};
    m_wheel.handle = handle;
    m_wheel.id     = id;

    // Axis roles: use the calibrated values when set, else auto-detect. Steering
    // self-centres (rests mid-travel) while a pedal rests hard at one end; axis 0
    // is steering on every wheel that exists, so it anchors the search and the
    // other axes resting at an extreme are pedals, taken in order accel then
    // brake. A wheel whose layout defeats this is corrected in the GUI.
    const int axes = SDL_GetNumJoystickAxes(handle);
    if (m_wheel_settings.steer_axis >= 0 || m_wheel_settings.accel_axis >= 0
        || m_wheel_settings.brake_axis >= 0) {
        m_wheel.steer_axis = m_wheel_settings.steer_axis;
        m_wheel.accel_axis = m_wheel_settings.accel_axis;
        m_wheel.brake_axis = m_wheel_settings.brake_axis;
    } else {
        m_wheel.steer_axis = axes > 0 ? 0 : -1;
        int assigned_pedals = 0;
        for (int axis = 1; axis < axes; ++axis) {
            const int rest = static_cast<int>(SDL_GetJoystickAxis(handle, axis));
            const bool at_extreme = rest < -16384 || rest > 16384;
            if (!at_extreme) {
                continue;
            }
            if (assigned_pedals == 0) {
                m_wheel.accel_axis = axis;
            } else if (assigned_pedals == 1) {
                m_wheel.brake_axis = axis;
            }
            ++assigned_pedals;
        }
        if (assigned_pedals == 0) {
            m_wheel.accel_axis = axes > 1 ? 1 : -1;
            m_wheel.brake_axis = axes > 2 ? 2 : -1;
        }
    }

    const char* name = SDL_GetJoystickName(handle);
    SM2_INFO("wheel: %s (%d axes; steer %d accel %d brake %d)",
             name != nullptr ? name : "steering wheel", axes,
             m_wheel.steer_axis, m_wheel.accel_axis, m_wheel.brake_axis);

    // Force feedback is a constant force we aim ourselves each frame (see
    // update_force_feedback): the wheel's driver ignores FF_SPRING but honours
    // FF_CONSTANT, so the centring pull is computed from the wheel angle rather
    // than programmed as a spring. Uploaded once at zero level and left running.
    if (m_wheel_settings.ffb) {
        SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(handle);
        if (haptic == nullptr) {
            SM2_INFO("wheel has no force feedback: %s", SDL_GetError());
        } else if ((SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_CONSTANT) == 0) {
            SM2_INFO("wheel force feedback lacks a constant-force effect; leaving it limp");
            SDL_CloseHaptic(haptic);
        } else {
            m_wheel.haptic = haptic;
            // Turn off the wheel's built-in autocentre and set full gain, or the
            // device fights or scales our own force. (Supermodel does the same.)
            SDL_SetHapticAutocenter(haptic, 0);
            SDL_SetHapticGain(haptic, 100);

            SDL_HapticEffect effect{};
            effect.type                 = SDL_HAPTIC_CONSTANT;
            effect.constant.type        = SDL_HAPTIC_CONSTANT;
            effect.constant.length      = SDL_HAPTIC_INFINITY;
            // Cartesian direction with dir[0]=0: the sign of the level alone
            // chooses which way the force pulls. This is the encoding Logitech's
            // evdev FF honours; a steering-axis direction pulled hard one way.
            effect.constant.direction.type  = SDL_HAPTIC_CARTESIAN;
            effect.constant.direction.dir[0] = 0;
            effect.constant.level            = 0;
            m_wheel.force_effect = SDL_CreateHapticEffect(haptic, &effect);
            if (m_wheel.force_effect < 0) {
                SM2_INFO("could not create the force effect: %s", SDL_GetError());
            } else {
                SDL_RunHapticEffect(haptic, m_wheel.force_effect, SDL_HAPTIC_INFINITY);
            }

            // A periodic sine effect for the actual vibration/rumble, run
            // alongside the constant force. The wheel hardware oscillates it at a
            // real vibration frequency, so an impact or the game's road-buzz is
            // *felt* as a buzz through the rim rather than as a steady push a
            // once-per-frame constant force would produce. Magnitude is set per
            // frame in update_force_feedback; starts at zero.
            if ((SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_SINE) != 0) {
                SDL_HapticEffect rumble{};
                rumble.type              = SDL_HAPTIC_SINE;
                rumble.periodic.type     = SDL_HAPTIC_SINE;
                rumble.periodic.direction.type   = SDL_HAPTIC_CARTESIAN;
                rumble.periodic.direction.dir[0] = 1;
                rumble.periodic.period   = 20;   // ms -> ~50 Hz, a punchy road buzz
                rumble.periodic.magnitude = 0;
                rumble.periodic.length   = SDL_HAPTIC_INFINITY;
                m_wheel.rumble_effect = SDL_CreateHapticEffect(haptic, &rumble);
                if (m_wheel.rumble_effect >= 0) {
                    SDL_RunHapticEffect(haptic, m_wheel.rumble_effect, SDL_HAPTIC_INFINITY);
                }
            }
            SM2_DEBUG("wheel FFB: %d simultaneous effect(s), constant/sine %d/%d",
                      SDL_GetMaxHapticEffectsPlaying(haptic),
                      (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_CONSTANT) ? 1 : 0,
                      (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_SINE) ? 1 : 0);
        }
    }
}

void Input::remove_wheel(SDL_JoystickID id)
{
    if (m_wheel.handle == nullptr || m_wheel.id != id) {
        return;
    }
    if (m_wheel.haptic != nullptr) {
        SDL_CloseHaptic(m_wheel.haptic);  // Frees its effects too.
    }
    SDL_CloseJoystick(m_wheel.handle);
    m_wheel = Wheel{};
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

    const s16 raw = SDL_GetJoystickAxis(m_wheel.handle, axis);

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

    // A pedal. The physical pedal gives fraction 0 released, 1 pressed. A wheel
    // whose pedal reads the other way is corrected by the user's invert flag.
    const bool invert =
        (channel.control == rom::AnalogControl::Brake) ? m_wheel_settings.brake_invert
                                                       : m_wheel_settings.accel_invert;
    if (invert) {
        fraction = 1.0f - fraction;
    }
    fraction = std::clamp(fraction, 0.0f, 1.0f);

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

u8 Input::sample_channel(const rom::AnalogChannel& channel) const
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

    float fraction = 0.0f;
    switch (axis) {
        case HostAxis::PadLeftX:
        case HostAxis::Pad2LeftX:
            fraction = static_cast<float>(
                           SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) + 32768)
                     / 65535.0f;
            break;
        case HostAxis::PadLeftY:
        case HostAxis::Pad2LeftY:
            fraction = static_cast<float>(
                           SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) + 32768)
                     / 65535.0f;
            break;
        case HostAxis::RightTrigger:
        case HostAxis::LeftTrigger: {
            const SDL_GamepadAxis which = axis == HostAxis::RightTrigger
                                              ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                              : SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
            const int travelled = std::max(0, static_cast<int>(
                                                  SDL_GetGamepadAxis(pad, which))
                                                  - kPedalFloor);
            fraction = static_cast<float>(travelled) / static_cast<float>(32767 - kPedalFloor);
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

int Input::wheel_axis_count() const
{
    return m_wheel.handle != nullptr ? SDL_GetNumJoystickAxes(m_wheel.handle) : 0;
}

void Input::wheel_axis_baseline(s16* out, int count) const
{
    for (int axis = 0; axis < count; ++axis) {
        out[axis] = m_wheel.handle != nullptr
                        ? SDL_GetJoystickAxis(m_wheel.handle, axis)
                        : 0;
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
        const int delta = static_cast<int>(SDL_GetJoystickAxis(m_wheel.handle, axis))
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

void Input::update_force_feedback(const rom::GameSpec& game, u8 drive_force)
{
    if (m_wheel.haptic == nullptr || m_wheel.force_effect < 0 || m_wheel.steer_axis < 0) {
        return;
    }

    // Real force feedback, decoded from the command byte the game streams to its
    // drive board. Daytona's board (like the Model 2/3 family) encodes an effect
    // in the high nibble and a strength 0..15 in the low nibble. Learnt against
    // the wheel position while driving:
    //   0x5x  constant force to the RIGHT   (seen only with the wheel turned right)
    //   0x6x  constant force to the LEFT    (seen only with the wheel turned left)
    //   0x3x  centring spring, strength = level (board recentres from position)
    //   0x2x  a weaker force/friction, treated like a light centring
    //   0x1x  no effect;  0x0x / 0x7x  boot/handshake
    // The board's own motor sign convention is unknown, so the mapping to SDL's
    // cartesian level was chosen to match the previous working spring: a
    // negative level pushes the wheel right, a positive level pushes it left.
    int level  = 0;
    int rumble = 0;   // periodic-effect magnitude, felt as a buzz not a push
    if (game.has_steering() && m_wheel_settings.ffb) {
        const int ceiling = static_cast<int>(
            std::clamp(m_wheel_settings.strength, 0u, 100u) * 32767 / 100);
        const int cmd   = drive_force & 0xf0;
        const int steps = drive_force & 0x0f;          // 0..15
        const int mag   = steps * ceiling / 15;

        const int deflection = static_cast<int>(
            SDL_GetJoystickAxis(m_wheel.handle, m_wheel.steer_axis));  // -32768..32767

        // A baseline centring spring is always present on a drive-board game, so
        // the wheel self-centres from the moment the emulator launches -- through
        // the attract screens and menus, not only once the game streams its own
        // centring command. Firm from a small deadzone, capped short of full lock.
        int baseline = 0;
        {
            constexpr int kDeadzone = 1500;
            const int m = std::abs(deflection);
            if (m > kDeadzone) {
                const int span = std::min(m - kDeadzone, 14000);
                baseline = (ceiling * 3 / 4) * span / 14000;
                baseline = deflection < 0 ? -baseline : baseline;
            }
        }

        switch (cmd) {
            case 0x50:  // constant force right -> push wheel right (negative)
            case 0x60:  // constant force left  -> push wheel left  (positive)
            {
                const int dir = (cmd == 0x50) ? -1 : 1;
                // A crash holds a strong one-direction force for many frames. The
                // cabinet's heavy geared wheel barely moved under it; a free PC
                // wheel spins to the stop. So a *sustained* one-direction push is
                // held full for a brief kick, then decayed hard.
                // Every directional force command (0x5x/0x6x) is a scrub/impact
                // event the drive board would have felt as a jolt through the
                // rim. Feed its strength to the periodic effect so it is felt as
                // a vibration, not only as a push. A direction flip (the rapid
                // left/right road-buzz pattern) makes it strongest.
                rumble = mag;
                if (dir == m_wheel.constant_dir) {
                    m_wheel.constant_hold++;
                } else {
                    rumble = mag * 3 / 2;   // a flip is a sharper jolt
                    m_wheel.constant_hold = 0;
                    m_wheel.constant_dir  = dir;
                }
                constexpr int kFullFrames = 8;   // ~0.14 s of full kick
                int scaled = mag;
                if (m_wheel.constant_hold > kFullFrames) {
                    const int over = std::min(m_wheel.constant_hold - kFullFrames, 18);
                    scaled = mag - (mag * 9 / 10) * over / 18;
                }
                // The directional force adds to the baseline centring, and the
                // faster the game toggles it the more it decays into rumble.
                level = baseline + dir * scaled;
                break;
            }
            case 0x20:  // lighter centring
            case 0x30:  // main centring spring
            default: {
                m_wheel.constant_hold = 0;
                m_wheel.constant_dir  = 0;
                // The game's own centring command deepens the baseline spring by
                // its commanded strength; no-effect/boot codes leave the baseline.
                int deepen = 0;
                if (cmd == 0x20 || cmd == 0x30) {
                    constexpr int kDeadzone = 1500;
                    const int m = std::abs(deflection);
                    if (m > kDeadzone) {
                        const int span = std::min(m - kDeadzone, 14000);
                        deepen = mag * span / 14000;
                        deepen = deflection < 0 ? -deepen : deepen;
                    }
                }
                level = baseline + deepen;
                break;
            }
        }
        // The game mostly commands low levels (2..7 of 15), so raw forces sit
        // well under the ceiling. A gain lifts the mid range into something felt;
        // the clamp still protects the top end.
        level = level * 5 / 3;
        level = std::clamp(level, -ceiling, ceiling);
    }

    // Reprogram only on a meaningful change: reuploading every frame makes the
    // Boost the impact rumble so a hit is clearly felt.
    rumble *= 2;

    // Daytona streams no continuous road/engine buzz -- its command stream is
    // centring plus directional jolts -- so a light engine rumble is synthesised
    // from the throttle, giving constant feel while driving that rises with the
    // gas. This is a feel, not replayed game data, with its own on/off + strength.
    // Kept deliberately subtle: even at full strength it is a fraction of the
    // device maximum, so it reads as an engine hum rather than a jackhammer.
    if (game.has_steering() && m_wheel_settings.ffb && m_wheel_settings.rumble
        && m_wheel.accel_axis >= 0) {
        // Full strength maps to ~12% of the device max at full throttle; the
        // G923's motor is strong, so even a small sine magnitude is plenty.
        const int rmax = static_cast<int>(
            std::clamp(m_wheel_settings.rumble_strength, 0u, 100u) * 4000 / 100);
        const int accel_raw = static_cast<int>(
            SDL_GetJoystickAxis(m_wheel.handle, m_wheel.accel_axis));  // -32768..32767
        int throttle = accel_raw + 32768;  // 0..65535, pedal released..pressed
        if (m_wheel_settings.accel_invert) {
            throttle = 65535 - throttle;
        }
        // A faint idle hum (a fifth of the range) rising to the full engine level.
        const int engine = rmax / 5 + (rmax * 4 / 5) * throttle / 65535;
        rumble = std::max(rumble, engine);
    }

    // Smooth it: a trigger is a single-frame spike, so decay the running
    // magnitude and hold it a few frames into a sustained felt vibration.
    int rumble_now = std::max(rumble, (m_wheel.rumble_mag < 0 ? 0 : m_wheel.rumble_mag) * 4 / 5);
    rumble_now     = std::clamp(rumble_now, 0, 32767);

    // Update the constant force (the push/centring) when it changes.
    if (level != m_wheel.force_level) {
        m_wheel.force_level = level;
        SDL_HapticEffect effect{};
        effect.type                      = SDL_HAPTIC_CONSTANT;
        effect.constant.type             = SDL_HAPTIC_CONSTANT;
        effect.constant.length           = SDL_HAPTIC_INFINITY;
        effect.constant.direction.type   = SDL_HAPTIC_CARTESIAN;
        effect.constant.direction.dir[0] = 0;
        effect.constant.level            = static_cast<s16>(level);
        SDL_UpdateHapticEffect(m_wheel.haptic, m_wheel.force_effect, &effect);
        SDL_RunHapticEffect(m_wheel.haptic, m_wheel.force_effect, SDL_HAPTIC_INFINITY);
    }

    // Update the periodic rumble (the vibration) when it changes.
    if (m_wheel.rumble_effect >= 0 && rumble_now != m_wheel.rumble_mag) {
        m_wheel.rumble_mag = rumble_now;
        SDL_HapticEffect rmb{};
        rmb.type                     = SDL_HAPTIC_SINE;
        rmb.periodic.type            = SDL_HAPTIC_SINE;
        rmb.periodic.direction.type  = SDL_HAPTIC_CARTESIAN;
        rmb.periodic.direction.dir[0] = 1;
        rmb.periodic.period          = 20;   // ~50 Hz, a punchier buzz
        rmb.periodic.magnitude       = static_cast<s16>(rumble_now);
        rmb.periodic.length          = SDL_HAPTIC_INFINITY;
        SDL_UpdateHapticEffect(m_wheel.haptic, m_wheel.rumble_effect, &rmb);
        SDL_RunHapticEffect(m_wheel.haptic, m_wheel.rumble_effect, SDL_HAPTIC_INFINITY);
    }

    SM2_DEBUG("ffb: cmd=0x%02x level=%d rumble=%d", drive_force, level, rumble_now);
}

void Input::gather_lightguns(hw::Inputs* inputs, const rom::GameSpec& game) const
{
    const rom::LightgunSpec& spec = game.lightgun;
    if (!spec.present) {
        return;
    }

    // One pointer for both players: there is no per-gun host device yet, so
    // player 2's gun follows the mouse as well rather than sitting at a corner
    // where the offscreen test would fire continuously.
    const PointerState pointer = pointer_state();
    inputs->gun_p1x = mouse_to_gun(pointer.x, pointer.width, spec.p1x);
    inputs->gun_p1y = mouse_to_gun(pointer.y, pointer.height, spec.p1y);
    inputs->gun_p2x = mouse_to_gun(pointer.x, pointer.width, spec.p2x);
    inputs->gun_p2y = mouse_to_gun(pointer.y, pointer.height, spec.p2y);
}

void Input::poll(hw::Inputs* inputs) const
{
    poll(inputs, rom::GameSpec{});
}

void Input::poll(hw::Inputs* inputs, const rom::GameSpec& game) const
{
    const rom::InputFlags game_inputs = game.inputs;
    (void)game_inputs;

    u8 ports[1 + kPlayers] = {0xff, 0xff, 0xff};

    int         key_count = 0;
    const bool* keys      = SDL_GetKeyboardState(&key_count);
    if (keys != nullptr) {
        gather_keys(&ports[0], keys, key_count, kOperatorKeys);
        gather_keys(&ports[1], keys, key_count, kPlayerOneKeys);
        gather_keys(&ports[2], keys, key_count, kPlayerTwoKeys);
    }

    for (const Pad& pad : m_pads) {
        if (pad.handle == nullptr || pad.player >= kPlayers) {
            continue;
        }
        u8& port = ports[1 + pad.player];

        for (const PadBinding& binding : kPlayerPadButtons) {
            if (SDL_GetGamepadButton(pad.handle, binding.button)) {
                port &= static_cast<u8>(~binding.bit);
            }
        }

        // The left stick drives the same four switches as the d-pad, so either works
        // and holding both is harmless.
        port &= static_cast<u8>(
            ~stick_bits(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX),
                        SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY)));

        // Start and coin, so a player can put themselves into the game without
        // reaching for the keyboard. Coin 2 belongs to player 2's slot.
        if (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_START)) {
            ports[0] &= static_cast<u8>(~(pad.player == 0 ? kStart1 : kStart2));
        }
        if (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_BACK)) {
            ports[0] &= static_cast<u8>(~(pad.player == 0 ? kCoin1 : kCoin2));
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
    for (usize channel = 0; channel < inputs->analog.size(); ++channel) {
        const rom::AnalogChannel& wiring = game.analog[channel];
        inputs->analog[channel] = wiring.control == rom::AnalogControl::None
                                      ? 0x00
                                      : sample_channel(wiring);
    }


    gather_lightguns(inputs, game);

    // Gear selector. The shifter's positions are exposed as one bit each and the
    // machine turns them into the code its program expects. Nothing held means
    // "hold the last gear", which is what the real gate does between positions.
    //
    // F1 to F5 rather than the number row, which the operator controls already
    // own.
    if (game.gearbox) {
        u8 gears = 0;
        if (keys != nullptr) {
            static constexpr SDL_Scancode kGearKeys[5] = {
                SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3,
                SDL_SCANCODE_F4, SDL_SCANCODE_F5,
            };
            for (u32 gear = 0; gear < 5; ++gear) {
                if (static_cast<int>(kGearKeys[gear]) < key_count && keys[kGearKeys[gear]]) {
                    gears |= static_cast<u8>(1u << gear);
                }
            }
        }

        // Paddle shifters step through the same five positions sequentially.
        // Edge-detected so one press is one shift, and the wheel remembers its
        // gear between frames; when a paddle set it, that position wins over an
        // idle keyboard. Gears 0..3 are 1st..4th; position 4 (reverse) is only
        // reachable from the keyboard, since a paddle sequence should not fall
        // into reverse.
        if (m_wheel.handle != nullptr) {
            const int count = SDL_GetNumJoystickButtons(m_wheel.handle);
            const auto role_held = [&](Config::WheelRole role) {
                const s32 button = m_wheel_settings.buttons[static_cast<usize>(role)];
                return button >= 0 && button < count
                    && SDL_GetJoystickButton(m_wheel.handle, button);
            };
            const bool up   = role_held(Config::WheelRole::GearUp);
            const bool down = role_held(Config::WheelRole::GearDown);
            bool shifted = false;
            // The gate's five positions are neutral, 1, 2, 3, 4 (MAME's "GEARS"
            // port: bit 0 = N, bits 1..4 = the gears). So m_wheel_gear is the gear
            // number: 0 = neutral, up to 4. Shifting up from neutral engages 1st;
            // shifting down from 1st returns to neutral.
            if (up && !m_gear_up_held && m_wheel_gear < 4) {
                ++m_wheel_gear;
                shifted = true;
            }
            if (down && !m_gear_down_held && m_wheel_gear > 0) {
                --m_wheel_gear;
                shifted = true;
            }
            m_gear_up_held   = up;
            m_gear_down_held = down;
            if (shifted || gears == 0) {
                gears = static_cast<u8>(1u << m_wheel_gear);
            }
        }

        inputs->gears = gears;
    }

    // Games that shift with two momentary buttons rather than a gate (Indy 500,
    // Manx TT and family) put Shift Up on IN1 0x10 and Shift Down on IN1 0x20.
    // The GearUp/GearDown wheel roles press those bits directly; no gear state.
    if (game.shift_buttons && m_wheel.handle != nullptr) {
        const int count = SDL_GetNumJoystickButtons(m_wheel.handle);
        const auto role_held = [&](Config::WheelRole role) {
            const s32 button = m_wheel_settings.buttons[static_cast<usize>(role)];
            return button >= 0 && button < count
                && SDL_GetJoystickButton(m_wheel.handle, button);
        };
        if (role_held(Config::WheelRole::GearUp)) {
            inputs->in1 &= static_cast<u8>(~0x10);
        }
        if (role_held(Config::WheelRole::GearDown)) {
            inputs->in1 &= static_cast<u8>(~0x20);
        }
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

void Input::print_bindings()
{
    std::printf("Gamepad, per player:\n");
    std::printf("  d-pad or left stick  stick\n");
    std::printf("  A B X Y              buttons 1 to 4\n");
    std::printf("  L / R shoulder       buttons 3 and 4\n");
    std::printf("  Start                start\n");
    std::printf("  Back                 insert coin\n");
    std::printf("\nKeyboard:\n");
    std::printf("  5 6                  coin 1, coin 2\n");
    std::printf("  1 2                  start 1, start 2\n");
    std::printf("  9 0                  service, test\n");
    std::printf("  arrows Z X C V       player 1 stick and buttons\n");
    std::printf("  W A S D  G H J K     player 2 stick and buttons\n");
    std::printf("  Escape               quit\n");
    std::printf("\nThe first gamepad to connect is player 1. Gamepads and the keyboard\n");
    std::printf("are both live, so a second player can join on the keyboard.\n");
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
