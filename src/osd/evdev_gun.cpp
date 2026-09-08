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
// See evdev_gun.h.

#include "osd/evdev_gun.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <fcntl.h>
#include <libudev.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sm2::osd {
namespace {

/// Case-insensitive substring test.
[[nodiscard]] bool contains_ci(const char* haystack, const char* needle)
{
    if (haystack == nullptr) {
        return false;
    }
    std::string h(haystack);
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(needle) != std::string::npos;
}

/// Whether a udev input device looks like a light gun. True if the light-gun
/// udev rule tagged it ID_INPUT_GUN, or its name/model names it one (the Sinden
/// presents as an absolute mouse with no gun rule, so the name is all we have).
[[nodiscard]] bool is_gun_device(udev_device* dev)
{
    if (const char* gun = udev_device_get_property_value(dev, "ID_INPUT_GUN");
        gun != nullptr && std::strcmp(gun, "1") == 0) {
        return true;
    }
    for (const char* key : {"ID_MODEL", "ID_MODEL_ENC", "NAME"}) {
        const char* value = udev_device_get_property_value(dev, key);
        if (contains_ci(value, "lightgun") || contains_ci(value, "light gun")) {
            return true;
        }
    }
    return false;
}

/// A key shared by every input node of one physical device, so a gun's several
/// nodes collapse to one. The parent USB device's syspath is common to all its
/// nodes; fall back to ID_PATH then the node for a device with no USB parent.
[[nodiscard]] std::string physical_device_key(udev_device* dev)
{
    if (udev_device* usb =
            udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
        usb != nullptr) {
        if (const char* syspath = udev_device_get_syspath(usb); syspath != nullptr) {
            return syspath;
        }
    }
    if (const char* path = udev_device_get_property_value(dev, "ID_PATH");
        path != nullptr) {
        return path;
    }
    if (const char* node = udev_device_get_devnode(dev); node != nullptr) {
        return node;
    }
    return {};
}

}  // namespace

EvdevGuns::~EvdevGuns()
{
    shutdown();
}

bool EvdevGuns::init()
{
    udev* ctx = udev_new();
    if (ctx == nullptr) {
        SM2_WARN("evdev: udev_new failed; light guns via evdev unavailable");
        return false;
    }

    // A single physical gun exposes several input nodes (the Sinden shows both a
    // "SindenLightgun Mouse" node and a "Sinden Lightgun" node, both tagged
    // ID_INPUT_GUN), so opening every node would present one gun as two players.
    // Group nodes by physical device and keep one per device, preferring the
    // dedicated light-gun node (prio 0) over the "Mouse" node -- the Sinden's
    // calibration handshake only works through the former.
    struct Candidate {
        std::string node;
        std::string phys;  // physical_device_key: one per physical gun.
        int         prio = 1;
    };
    std::vector<Candidate> candidates;
    if (udev_enumerate* enumerate = udev_enumerate_new(ctx); enumerate != nullptr) {
        udev_enumerate_add_match_subsystem(enumerate, "input");
        udev_enumerate_scan_devices(enumerate);

        for (udev_list_entry* item = udev_enumerate_get_list_entry(enumerate);
             item != nullptr; item = udev_list_entry_get_next(item)) {
            udev_device* dev =
                udev_device_new_from_syspath(ctx, udev_list_entry_get_name(item));
            if (dev == nullptr) {
                continue;
            }
            const char* node = udev_device_get_devnode(dev);
            if (node != nullptr && is_gun_device(dev)) {
                // Prefer a dedicated light-gun node over the companion "Mouse".
                bool named_lightgun = false;
                bool named_mouse    = false;
                for (const char* key : {"NAME", "ID_MODEL", "ID_MODEL_ENC"}) {
                    const char* v = udev_device_get_property_value(dev, key);
                    named_lightgun |= contains_ci(v, "lightgun")
                                      || contains_ci(v, "light gun");
                    named_mouse    |= contains_ci(v, "mouse");
                }
                const int prio = (named_lightgun && !named_mouse) ? 0 : 1;
                candidates.push_back({node, physical_device_key(dev), prio});
            }
            udev_device_unref(dev);
        }
        udev_enumerate_unref(enumerate);
    }
    udev_unref(ctx);

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.phys != b.phys) return a.phys < b.phys;
                  if (a.prio != b.prio) return a.prio < b.prio;
                  return a.node < b.node;
              });
    std::string last_phys;
    for (const Candidate& c : candidates) {
        if (m_guns.size() >= kMaxGuns) {
            break;
        }
        if (c.phys == last_phys) {
            continue;  // another node of a device already opened
        }
        // open_gun rejects a node with no absolute axis; only count the device
        // as taken once a node actually opened, so a non-aiming node does not
        // shadow the sibling that carries the aim.
        if (open_gun(c.node)) {
            last_phys = c.phys;
        }
    }

    if (!m_guns.empty()) {
        SM2_INFO("evdev: opened %zu light gun(s)", m_guns.size());
    }
    return true;
}

bool EvdevGuns::open_gun(const std::string& node)
{
    // Prefer read-write so a gun with a recoil motor can be driven; fall back to
    // read-only (input still works, just no recoil) if write access is denied.
    int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    bool writable = fd >= 0;
    if (fd < 0) {
        fd = open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        SM2_WARN("evdev: cannot open %s", node.c_str());
        return false;
    }

    // A gun must report an absolute position. A relative mouse mis-tagged, or a
    // keyboard node of the same composite device, has an empty abs range and is
    // of no use here.
    input_absinfo abs_x{};
    input_absinfo abs_y{};
    if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) < 0
        || abs_x.maximum <= abs_x.minimum || abs_y.maximum <= abs_y.minimum) {
        close(fd);
        return false;
    }

    Device device;
    device.fd           = fd;
    device.raw_min[0]   = abs_x.minimum;
    device.raw_range[0] = abs_x.maximum - abs_x.minimum;
    device.raw_min[1]   = abs_y.minimum;
    device.raw_range[1] = abs_y.maximum - abs_y.minimum;
    // Start centred, not at the corner, so an untouched gun does not read as
    // pointing off screen before it has been moved.
    device.raw[0] = abs_x.minimum + device.raw_range[0] / 2;
    device.raw[1] = abs_y.minimum + device.raw_range[1] / 2;

    char name[256];
    if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0) {
        name[sizeof name - 1] = '\0';
        device.state.name = name;
    } else {
        device.state.name = node;
    }

    // Recoil: if the device is writable and advertises a rumble force-feedback
    // effect, upload one at zero strength now. fire_recoil re-uploads it at the
    // requested level and plays it. The effect id stays -1 for a gun without a
    // motor, which makes has_recoil() false.
    if (writable) {
        unsigned long ff_bits[(FF_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof ff_bits), ff_bits) >= 0) {
            const bool has_rumble =
                (ff_bits[FF_RUMBLE / (8 * sizeof(long))] >> (FF_RUMBLE % (8 * sizeof(long)))) & 1u;
            if (has_rumble) {
                ff_effect effect{};
                effect.type                      = FF_RUMBLE;
                effect.id                        = -1;
                effect.u.rumble.strong_magnitude = 0;
                effect.u.rumble.weak_magnitude   = 0;
                effect.replay.length             = 120;  // ms
                if (ioctl(fd, EVIOCSFF, &effect) >= 0) {
                    device.ff_effect = effect.id;
                }
            }
        }
    }

    m_guns.push_back(std::move(device));
    return true;
}

bool EvdevGuns::has_recoil(usize index) const
{
    return index < m_guns.size() && m_guns[index].ff_effect >= 0;
}

void EvdevGuns::fire_recoil(usize index, u32 strength)
{
    if (index >= m_guns.size() || strength == 0) {
        return;
    }
    Device& device = m_guns[index];
    if (device.fd < 0 || device.ff_effect < 0) {
        return;
    }

    // Re-upload the effect at the requested magnitude if it changed, keeping the
    // same effect id (the kernel updates it in place).
    const u32 pct = std::min(strength, 100u);
    if (pct != device.ff_strength) {
        ff_effect effect{};
        effect.type                      = FF_RUMBLE;
        effect.id                        = device.ff_effect;
        const u16 mag                    = static_cast<u16>(0xffff * pct / 100);
        effect.u.rumble.strong_magnitude = mag;
        effect.u.rumble.weak_magnitude   = mag;
        effect.replay.length             = 120;
        if (ioctl(device.fd, EVIOCSFF, &effect) < 0) {
            return;
        }
        device.ff_strength = pct;
    }

    // Play one iteration.
    input_event play{};
    play.type  = EV_FF;
    play.code  = static_cast<u16>(device.ff_effect);
    play.value = 1;
    if (write(device.fd, &play, sizeof play) < 0) {
        // Non-fatal: the shot still registers, just without recoil.
    }
}

u16 EvdevGuns::take_last_pressed(usize index)
{
    if (index >= m_guns.size()) {
        return 0;
    }
    const u16 code = m_guns[index].last_pressed;
    m_guns[index].last_pressed = 0;
    return code;
}

void EvdevGuns::shutdown()
{
    for (Device& device : m_guns) {
        if (device.fd >= 0) {
            close(device.fd);
        }
    }
    m_guns.clear();
}

void EvdevGuns::poll()
{
    for (Device& device : m_guns) {
        read_device(device);
    }
}

void EvdevGuns::read_device(Device& device)
{
    if (device.fd < 0) {
        return;
    }

    // Drain the queue: one read() returns a batch of events; loop until EAGAIN.
    input_event events[64];
    for (;;) {
        const ssize_t len = read(device.fd, events, sizeof events);
        if (len <= 0) {
            break;  // EAGAIN (nothing pending) or a closed device.
        }
        const usize count = static_cast<usize>(len) / sizeof(input_event);
        for (usize i = 0; i < count; ++i) {
            const input_event& event = events[i];
            if (event.type == EV_ABS) {
                if (event.code == ABS_X) {
                    device.raw[0] = event.value;
                } else if (event.code == ABS_Y) {
                    device.raw[1] = event.value;
                }
            } else if (event.type == EV_KEY) {
                if (event.value != 0 && !device.state.pressed[event.code]) {
                    device.last_pressed = event.code;  // rising edge
                }
                device.state.pressed[event.code] = event.value != 0;
            }
        }
    }

    const auto norm = [&](int axis) -> float {
        const int range = device.raw_range[static_cast<usize>(axis)];
        if (range <= 0) {
            return 0.5f;
        }
        const float f = static_cast<float>(device.raw[static_cast<usize>(axis)]
                                           - device.raw_min[static_cast<usize>(axis)])
                        / static_cast<float>(range);
        return std::clamp(f, 0.0f, 1.0f);
    };
    device.state.x = norm(0);
    device.state.y = norm(1);
}

}  // namespace sm2::osd
