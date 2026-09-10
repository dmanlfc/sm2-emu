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
// A CommTransport carrying link-board frames over a LAN by UDP. The board is a
// ring: receive from the previous cabinet, send to the next. A frame maps
// one-to-one onto a datagram; the ring protocol re-sends state every frame and
// tolerates a lost datagram, so UDP fits and avoids a TCP ring's head-of-line
// stall when a cabinet boots late. Driven from vblank(), so it never blocks.
#pragma once

#include "core/net.h"
#include "core/types.h"
#include "hw/comm_transport.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class UdpTransport final : public CommTransport {
public:
    /// Received datagrams held before the oldest is dropped; a deeper backlog is
    /// the far end racing ahead, and the stale frames are worthless.
    static constexpr std::size_t kBacklog = 64;

    /// Bind local_ip:local_port to receive on and remember where to send. Empty
    /// local_ip listens on every interface; empty next_ip means no forwarding
    /// (tail of a chain). On failure ok() is false and the transport is inert.
    UdpTransport(const std::string& local_ip, u16 local_port,
                 std::string next_ip, u16 next_port);

    [[nodiscard]] bool               ok() const { return m_socket.valid(); }
    [[nodiscard]] const std::string& error() const { return m_socket.last_error(); }

    void                           send(std::span<const u8> frame) override;
    std::optional<std::vector<u8>> recv() override;
    [[nodiscard]] bool             connected() const override;
    void                           reset() override;

private:
    void drain();  ///< Pull every waiting datagram off the socket into m_rx.

    net::UdpSocket m_socket;
    std::string    m_next_ip;
    u16            m_next_port = 0;

    std::deque<std::vector<u8>> m_rx;
    bool                        m_send_failed = false;
};

}  // namespace sm2::hw
