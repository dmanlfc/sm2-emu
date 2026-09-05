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
// Per-device light guns, read straight from Linux evdev.
//
// SDL merges every physical mouse into one system pointer, so it cannot tell
// two light guns apart. This reads absolute-positioning guns from their
// /dev/input/eventN nodes instead, one host device per player, and sits beside
// the SDL input path (which still handles the keyboard, pads and the
// single-mouse fallback). Guns are found through libudev by the ID_INPUT_GUN
// property or a light-gun model name (e.g. the Sinden, which has no gun rule),
// and ordered by node so numbering is stable across runs.
//
// Linux only, and only compiled when libudev is present (SM2_HAVE_EVDEV). The
// technique follows Supermodel's evdev input system, but no code is taken from
// it: Supermodel is GPL-3.0 and this tree is BSD-3-Clause.
#pragma once

#include "core/types.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace sm2::osd {

/// A set of absolute-positioning light guns opened from evdev.
class EvdevGuns {
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

    EvdevGuns() = default;
    ~EvdevGuns();

    EvdevGuns(const EvdevGuns&)            = delete;
    EvdevGuns& operator=(const EvdevGuns&) = delete;

    /// Discover and open every light gun (by ID_INPUT_GUN or model name). Safe
    /// to call when no guns are present; count() is then zero and the caller
    /// uses its fallback.
    /// Returns false only if libudev itself will not start.
    bool init();

    void shutdown();

    /// Drain each gun's event queue and update its state. Call once per frame.
    void poll();

    [[nodiscard]] usize count() const { return m_guns.size(); }
    [[nodiscard]] const Gun& gun(usize index) const { return m_guns[index].state; }

    /// Whether gun `index` has a recoil / rumble motor (a force-feedback device).
    [[nodiscard]] bool has_recoil(usize index) const;

    /// Fire the recoil pulse on gun `index` at `strength` percent (0..100). A
    /// no-op for a gun without a motor, or when strength is zero.
    void fire_recoil(usize index, u32 strength);

    /// Most recent button press on gun `index` since the last call (0 if none),
    /// consumed on read, for the GUI bind capture.
    [[nodiscard]] u16 take_last_pressed(usize index);

private:
    struct Device {
        int         fd = -1;
        std::array<int, 2> raw_min{};    ///< ABS_X, ABS_Y minimums.
        std::array<int, 2> raw_range{};  ///< ABS_X, ABS_Y ranges (max - min).
        std::array<int, 2> raw{};        ///< latest raw ABS values.
        Gun         state;
        /// Force feedback: the uploaded rumble effect id, or -1 if the device
        /// has no motor or the effect could not be created. Set at strength 0
        /// so fire_recoil can re-upload at the requested level on demand.
        int  ff_effect = -1;
        u32  ff_strength = 0;  ///< strength the current effect was built at.
        u16  last_pressed = 0;  ///< most recent EV_KEY press, for GUI capture.
    };

    bool open_gun(const std::string& node);
    void read_device(Device& device);

    std::vector<Device> m_guns;
};

}  // namespace sm2::osd
