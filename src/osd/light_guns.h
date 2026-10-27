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
// Per-device light guns, one host device per player.
//
// SDL merges every physical mouse into one system pointer, so it cannot tell
// two light guns apart. This class reads each gun as its own device and sits
// beside the SDL input path, which still handles the keyboard, the pads and
// the single-mouse fallback.
//
// There are two backends. On Linux (evdev_gun.cpp) guns are evdev nodes found
// through libudev, by the ID_INPUT_GUN property or a light-gun model name. On
// Windows (rawinput_gun.cpp) guns are Raw Input mice with a light-gun product
// name. Both report buttons as Linux evdev key codes, so the bindings in the
// config work on either platform.
//
// The evdev technique follows Supermodel's evdev input system, but no code is
// taken from it: Supermodel is GPL-3.0 and this tree is BSD-3-Clause.
#pragma once

#include "core/types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>

namespace sm2::osd {

/// A set of absolute-positioning light guns.
class LightGuns {
public:
    /// The hardware never has more than two; the headroom just avoids surprises
    /// with hubs presenting extra nodes.
    static constexpr usize kMaxGuns = 8;

    /// One gun's current state: normalised position and the evdev key codes
    /// currently held. Buttons are tracked by their raw evdev code so any
    /// binding (set in the config) resolves without a fixed slot table.
    struct Gun {
        float x = 0.5f;  ///< 0..1 across the screen, centred until moved.
        float y = 0.5f;
        std::string name;

        /// Whether evdev key `code` (e.g. BTN_LEFT, BTN_1) is held.
        [[nodiscard]] bool held(u16 code) const {
            const auto it = pressed.find(code);
            return it != pressed.end() && it->second;
        }
        std::unordered_map<u16, bool> pressed;
    };

    LightGuns();
    ~LightGuns();

    LightGuns(const LightGuns&)            = delete;
    LightGuns& operator=(const LightGuns&) = delete;

    /// The guns to put first, as player 1 / player 2 (Config::gun_devices): any
    /// part of the device path, ignoring case. Call before init().
    void set_preferred_devices(const std::array<std::string, 2>& devices)
    {
        m_preferred = devices;
    }

    /// 0 or 1 when `path` contains preferred entry 0 or 1 (ignoring case), 2
    /// otherwise: the sort key that puts the pinned guns first.
    [[nodiscard]] static int preferred_rank(const std::array<std::string, 2>& preferred,
        std::string path)
    {
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
            };
        path = lower(path);
        for (int i = 0; i < 2; ++i) {
            if (!preferred[i].empty() && path.find(lower(preferred[i])) != std::string::npos) {
                return i;
            }
        }
        return 2;
    }

    /// Discover and open every light gun. Safe to call when no guns are
    /// present; count() is then zero and the caller uses its fallback.
    /// Returns false only if the platform's input layer will not start.
    bool init();

    void shutdown();

    /// Update each gun's state. Call once per frame.
    void poll();

    [[nodiscard]] usize count() const;
    [[nodiscard]] const Gun& gun(usize index) const;

    /// Whether gun `index` has a recoil / rumble motor (a force-feedback device).
    [[nodiscard]] bool has_recoil(usize index) const;

    /// Fire the recoil pulse on gun `index` at `strength` percent (0..100). A
    /// no-op for a gun without a motor, or when strength is zero.
    void fire_recoil(usize index, u32 strength);

    /// Most recent button press on gun `index` since the last call (0 if none),
    /// consumed on read, for the GUI bind capture.
    [[nodiscard]] u16 take_last_pressed(usize index);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::array<std::string, 2> m_preferred;
};

}  // namespace sm2::osd
