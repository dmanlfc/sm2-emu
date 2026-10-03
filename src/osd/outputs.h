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
//
// Cabinet outputs: lamps and drive-board bytes, published for MAMEHooker,
// DOFLinx and similar tools in MAME's two formats.
//
// Network: a TCP server (port 8000 by default) sending "name = value\r" lines,
// with "mame_start = <set>", "mame_stop = 1" and "pause = 0/1". When a game
// starts, it is also announced once the way Supermodel does,
// "mame_start = <set>\rtcp = <port>\r" over UDP to 127.0.0.1:8001:
// BackForceFeeder waits for that before it connects.
//
// Windows: a hidden "MAMEOutput" window answering MAME's registered window
// messages, so clients that speak the Windows protocol work unchanged.
//
// Every game publishes lamp0..lamp5 (MAME's names for the lamp latch's bits
// 2-7), RawLamps and RawDrive, plus LampStart/LampView1-4/LampLeader where the
// bit meanings are known. Values are sent only when they change.

#pragma once

#include "core/types.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sm2::osd {

class Outputs {
public:
    Outputs();
    ~Outputs();

    Outputs(const Outputs&)            = delete;
    Outputs& operator=(const Outputs&) = delete;

    /// Enable or disable each backend. Cheap to call every frame. announce_port
    /// is the UDP port for the start announcement, 0 for none.
    void configure(bool network, u16 port, u16 announce_port, bool windows);

    /// The set being run, or an empty string for none. A change sends
    /// mame_stop for the old set and mame_start for the new one.
    void set_game(const std::string& name, const std::string& parent);

    void set_paused(bool paused);

    /// This frame's lamp latch and drive-board bytes.
    void update(u8 lamp_latch, std::span<const u8> drive_writes);

    /// Accept clients and flush queued messages. Call once per frame.
    void poll();

    /// Where the network outputs stand, for the settings.
    struct NetworkStatus {
        bool        enabled   = false;
        bool        listening = false;
        usize       clients   = 0;
        std::string error;  ///< Why it is not listening; empty when it is.
    };
    [[nodiscard]] NetworkStatus network_status() const;

    /// One published output, exposed for the backends.
    struct Item {
        std::string name;
        s32         value = 0;
        bool        known = false;  ///< False until the first value is set.
    };

    class Backend;

private:
    void set(usize index, s32 value);
    void build_items(const std::string& parent);

    std::string m_game;
    bool        m_paused = false;

    std::vector<Item> m_items;

    /// Which lamp-latch bit feeds each item, or -1 for RawDrive.
    std::vector<int> m_bits;

    std::unique_ptr<Backend> m_network;
    std::unique_ptr<Backend> m_windows;
    u16                      m_port          = 0;
    u16                      m_announce_port = 0;
};

}  // namespace sm2::osd
