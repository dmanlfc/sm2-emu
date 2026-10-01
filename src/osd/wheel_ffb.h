//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Wheel force feedback: a constant force set each frame, plus an optional sine
// effect for vibration.
//
// SDL3 will not open haptics on a joystick that has a gamepad mapping, and
// frontends such as Batocera give wheels one. So on Linux the wheel's evdev
// node is driven directly. Otherwise SDL haptics are used: from the joystick,
// or failing that, the haptic device with the same name.
#pragma once

#include "core/types.h"

#include <SDL3/SDL.h>

namespace sm2::osd {

class WheelForce {
public:
    WheelForce() = default;
    ~WheelForce();

    WheelForce(const WheelForce&)            = delete;
    WheelForce& operator=(const WheelForce&) = delete;

    /// Open the wheel's force feedback and start the effects at zero.
    /// Returns false if the wheel has no constant-force effect.
    [[nodiscard]] bool open(SDL_Joystick* joystick);

    /// Remove the effects and restore the device's autocentre.
    void close();

    /// True if a sine effect is available for set_rumble.
    [[nodiscard]] bool has_rumble() const { return m_rumble_effect >= 0; }

    /// Backend in use, for logging.
    [[nodiscard]] const char* backend() const { return m_backend; }

    /// Device autocentre strength, 0..100.
    void set_autocenter(int percent);
    /// Signed constant force; the sign sets the direction.
    void set_force(s16 level);
    /// Default sine period in ms (~50 Hz), felt as a buzz.
    static constexpr u16 kRumblePeriod = 20;

    /// Sine magnitude, 0..32767, and its period in ms.
    void set_rumble(s16 magnitude, u16 period_ms = kRumblePeriod);

private:
    bool open_evdev(const char* path);
    bool open_sdl(SDL_Haptic* haptic);
    [[nodiscard]] static SDL_Haptic* find_sdl_haptic(SDL_Joystick* joystick);

    const char* m_backend = "none";

    // Linux evdev path.
    int  m_fd             = -1;
    bool m_has_autocenter = false;

    // SDL path.
    SDL_Haptic* m_haptic = nullptr;

    /// Effect ids for the open backend, or -1.
    int m_force_effect  = -1;
    int m_rumble_effect = -1;
};

}  // namespace sm2::osd
