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

Input::~Input()
{
    shutdown();
}

bool Input::init()
{
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SM2_ERROR("SDL_InitSubSystem(GAMEPAD) failed: %s", SDL_GetError());
        return false;
    }
    m_started = true;

    // Pads already plugged in do not generate connection events, so they have to
    // be collected once at startup.
    int             count = 0;
    SDL_JoystickID* ids   = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int index = 0; index < count; ++index) {
            add_gamepad(ids[index]);
        }
        SDL_free(ids);
    }

    if (m_pads.empty()) {
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
    if (m_started) {
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

void Input::poll(hw::Inputs* inputs) const
{
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

    inputs->in0 = ports[0];
    inputs->in1 = ports[1];
    inputs->in2 = ports[2];

    // Analogue channels. Which channel a game reads is a per-game wiring matter and
    // none of the analogue games are in the database yet, so this is the obvious
    // arrangement for a driving cabinet and is unverified. Player 1's pad only:
    // no Model 2 game has two analogue stations.
    inputs->analog.fill(0);
    const auto primary = std::find_if(m_pads.begin(), m_pads.end(),
                                      [](const Pad& pad) { return pad.player == 0; });
    if (primary != m_pads.end() && primary->handle != nullptr) {
        inputs->analog[0] =
            axis_to_centred(SDL_GetGamepadAxis(primary->handle, SDL_GAMEPAD_AXIS_LEFTX));
        inputs->analog[1] = axis_to_pedal(
            SDL_GetGamepadAxis(primary->handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        inputs->analog[2] = axis_to_pedal(
            SDL_GetGamepadAxis(primary->handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    } else {
        // A wheel at rest is centred, not at one end.
        inputs->analog[0] = 0x80;
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

Input::ScriptedPress Input::scripted_press(u32 frame, u32 coin_frame)
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
    // Two coins because the default setting asks for two per credit, then start,
    // then a button to confirm the highlighted character. The wait before the
    // confirmation lets the selection screen appear.
    static constexpr Event kScript[] = {
        {0 * kStep, kCoin1, 0},
        {1 * kStep, kCoin1, 0},
        {2 * kStep, kStart1, 0},
        {2 * kStep + 120, 0, kButton1},
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
