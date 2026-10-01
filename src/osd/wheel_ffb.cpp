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
#include "osd/wheel_ffb.h"

#include "core/log.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace sm2::osd {
namespace {

constexpr u16 kRumblePeriod = WheelForce::kRumblePeriod;

#ifdef __linux__
constexpr usize kLongBits = 8 * sizeof(unsigned long);

bool test_bit(const unsigned long* bits, int bit)
{
    return ((bits[static_cast<usize>(bit) / kLongBits] >> (static_cast<usize>(bit) % kLongBits)) & 1u) != 0;
}

/// Along the steering axis; the level's sign sets the direction. Matches what
/// SDL sends for a Cartesian {x, 0} direction.
constexpr u16 kSteerDirection = 0x4000;

bool write_ff(int fd, u16 code, s32 value)
{
    input_event event{};
    event.type  = EV_FF;
    event.code  = code;
    event.value = value;
    return ::write(fd, &event, sizeof event) == static_cast<ssize_t>(sizeof event);
}

/// Replay length 0 plays until stopped. Id -1 uploads a new effect; an existing
/// id updates it in place.
ff_effect constant_effect(int id, s16 level)
{
    ff_effect effect{};
    effect.type              = FF_CONSTANT;
    effect.id                = static_cast<s16>(id);
    effect.direction         = kSteerDirection;
    effect.u.constant.level  = level;
    return effect;
}

ff_effect sine_effect(int id, s16 magnitude, u16 period_ms = kRumblePeriod)
{
    ff_effect effect{};
    effect.type                 = FF_PERIODIC;
    effect.id                   = static_cast<s16>(id);
    effect.direction            = kSteerDirection;
    effect.u.periodic.waveform  = FF_SINE;
    effect.u.periodic.period    = period_ms;
    effect.u.periodic.magnitude = magnitude;
    return effect;
}
#endif

SDL_HapticEffect sdl_constant_effect(s16 level)
{
    SDL_HapticEffect effect{};
    effect.type                      = SDL_HAPTIC_CONSTANT;
    effect.constant.type             = SDL_HAPTIC_CONSTANT;
    effect.constant.length           = SDL_HAPTIC_INFINITY;
    // dir[0] = 0: the level's sign sets the direction.
    effect.constant.direction.type   = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = 0;
    effect.constant.level            = level;
    return effect;
}

SDL_HapticEffect sdl_sine_effect(s16 magnitude, u16 period_ms = kRumblePeriod)
{
    SDL_HapticEffect effect{};
    effect.type                      = SDL_HAPTIC_SINE;
    effect.periodic.type             = SDL_HAPTIC_SINE;
    effect.periodic.direction.type   = SDL_HAPTIC_CARTESIAN;
    effect.periodic.direction.dir[0] = 1;
    effect.periodic.period           = period_ms;
    effect.periodic.magnitude        = magnitude;
    effect.periodic.length           = SDL_HAPTIC_INFINITY;
    return effect;
}

}  // namespace

WheelForce::~WheelForce()
{
    close();
}

bool WheelForce::open(SDL_Joystick* joystick)
{
    close();

#ifdef __linux__
    // No path for HIDAPI or virtual joysticks; those use SDL haptics.
    if (const char* path = SDL_GetJoystickPath(joystick); path != nullptr && open_evdev(path)) {
        return true;
    }
#endif

    SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
    if (haptic == nullptr) {
        SM2_DEBUG("wheel FFB: not a haptic joystick (%s); searching haptic devices by name",
                  SDL_GetError());
        haptic = find_sdl_haptic(joystick);
    }
    return haptic != nullptr && open_sdl(haptic);
}

void WheelForce::close()
{
#ifdef __linux__
    if (m_fd >= 0) {
        for (const int effect : {m_rumble_effect, m_force_effect}) {
            if (effect >= 0) {
                write_ff(m_fd, static_cast<u16>(effect), 0);
                ioctl(m_fd, EVIOCRMFF, effect);
            }
        }
        set_autocenter(50);  // restore autocentre
        ::close(m_fd);
        m_fd = -1;
    }
#endif
    if (m_haptic != nullptr) {
        for (const int effect : {m_rumble_effect, m_force_effect}) {
            if (effect >= 0) {
                SDL_StopHapticEffect(m_haptic, effect);
                SDL_DestroyHapticEffect(m_haptic, effect);
            }
        }
        SDL_StopHapticEffects(m_haptic);
        set_autocenter(50);
        SDL_CloseHaptic(m_haptic);
        m_haptic = nullptr;
    }
    m_force_effect   = -1;
    m_rumble_effect  = -1;
    m_has_autocenter = false;
    m_backend        = "none";
}

bool WheelForce::open_evdev(const char* path)
{
#ifdef __linux__
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        SM2_DEBUG("wheel FFB: cannot open %s for writing: %s", path, std::strerror(errno));
        return false;
    }

    unsigned long ff_bits[(FF_CNT + kLongBits - 1) / kLongBits] = {};
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof ff_bits), ff_bits) < 0 || !test_bit(ff_bits, FF_CONSTANT)) {
        SM2_DEBUG("wheel FFB: %s has no constant-force effect", path);
        ::close(fd);
        return false;
    }

    ff_effect force = constant_effect(-1, 0);
    if (ioctl(fd, EVIOCSFF, &force) < 0) {
        SM2_INFO("wheel FFB: could not upload the force effect to %s: %s", path, std::strerror(errno));
        ::close(fd);
        return false;
    }

    m_fd             = fd;
    m_backend        = "evdev";
    m_force_effect   = force.id;
    m_has_autocenter = test_bit(ff_bits, FF_AUTOCENTER);
    if (test_bit(ff_bits, FF_GAIN)) {
        write_ff(fd, FF_GAIN, 0xffff);
    }
    write_ff(fd, static_cast<u16>(force.id), 1);

    if (test_bit(ff_bits, FF_PERIODIC) && test_bit(ff_bits, FF_SINE)) {
        ff_effect rumble = sine_effect(-1, 0);
        if (ioctl(fd, EVIOCSFF, &rumble) >= 0) {
            m_rumble_effect = rumble.id;
            write_ff(fd, static_cast<u16>(rumble.id), 1);
        }
    }

    SM2_DEBUG("wheel FFB: evdev %s, constant/sine 1/%d", path, m_rumble_effect >= 0 ? 1 : 0);
    return true;
#else
    (void)path;
    return false;
#endif
}

bool WheelForce::open_sdl(SDL_Haptic* haptic)
{
    if ((SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_CONSTANT) == 0) {
        SM2_DEBUG("wheel FFB: SDL haptic device lacks a constant-force effect");
        SDL_CloseHaptic(haptic);
        return false;
    }

    const SDL_HapticEffect force = sdl_constant_effect(0);
    const int force_effect = SDL_CreateHapticEffect(haptic, &force);
    if (force_effect < 0) {
        SM2_INFO("wheel FFB: could not create the force effect: %s", SDL_GetError());
        SDL_CloseHaptic(haptic);
        return false;
    }

    m_haptic       = haptic;
    m_backend      = "SDL haptic";
    m_force_effect = force_effect;
    SDL_SetHapticGain(haptic, 100);
    SDL_RunHapticEffect(haptic, force_effect, SDL_HAPTIC_INFINITY);

    if ((SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_SINE) != 0) {
        const SDL_HapticEffect rumble = sdl_sine_effect(0);
        m_rumble_effect = SDL_CreateHapticEffect(haptic, &rumble);
        if (m_rumble_effect >= 0) {
            SDL_RunHapticEffect(haptic, m_rumble_effect, SDL_HAPTIC_INFINITY);
        }
    }

    SM2_DEBUG("wheel FFB: SDL haptic, %d simultaneous effect(s), constant/sine 1/%d",
              SDL_GetMaxHapticEffectsPlaying(haptic), m_rumble_effect >= 0 ? 1 : 0);
    return true;
}

SDL_Haptic* WheelForce::find_sdl_haptic(SDL_Joystick* joystick)
{
    const char* name = SDL_GetJoystickName(joystick);
    if (name == nullptr) {
        return nullptr;
    }

    int           count = 0;
    SDL_HapticID* ids   = SDL_GetHaptics(&count);
    if (ids == nullptr) {
        return nullptr;
    }

    SDL_HapticID match   = 0;
    int          matches = 0;
    for (int index = 0; index < count; ++index) {
        const char* haptic_name = SDL_GetHapticNameForID(ids[index]);
        if (haptic_name != nullptr && SDL_strcmp(haptic_name, name) == 0) {
            match = ids[index];
            ++matches;
        }
    }
    SDL_free(ids);

    // SDL can only match a haptic device to a joystick by name. With two
    // identical wheels we cannot tell which is which, so use neither.
    if (matches != 1) {
        SM2_DEBUG("wheel FFB: %d haptic device(s) named \"%s\"", matches, name);
        return nullptr;
    }
    return SDL_OpenHaptic(match);
}

void WheelForce::set_autocenter(int percent)
{
#ifdef __linux__
    if (m_fd >= 0) {
        if (m_has_autocenter) {
            write_ff(m_fd, FF_AUTOCENTER, static_cast<s32>(0xffffu * static_cast<u32>(percent) / 100));
        }
        return;
    }
#endif
    if (m_haptic != nullptr) {
        SDL_SetHapticAutocenter(m_haptic, percent);
    }
}

void WheelForce::set_force(s16 level)
{
    if (m_force_effect < 0) {
        return;
    }
#ifdef __linux__
    if (m_fd >= 0) {
        ff_effect effect = constant_effect(m_force_effect, level);
        ioctl(m_fd, EVIOCSFF, &effect);
        return;
    }
#endif
    SDL_HapticEffect effect = sdl_constant_effect(level);
    SDL_UpdateHapticEffect(m_haptic, m_force_effect, &effect);
}

void WheelForce::set_rumble(s16 magnitude, u16 period_ms)
{
    if (m_rumble_effect < 0) {
        return;
    }
#ifdef __linux__
    if (m_fd >= 0) {
        ff_effect effect = sine_effect(m_rumble_effect, magnitude, period_ms);
        ioctl(m_fd, EVIOCSFF, &effect);
        return;
    }
#endif
    SDL_HapticEffect effect = sdl_sine_effect(magnitude, period_ms);
    SDL_UpdateHapticEffect(m_haptic, m_rumble_effect, &effect);
}

}  // namespace sm2::osd
