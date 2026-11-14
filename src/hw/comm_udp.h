// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// A CommTransport carrying link-board frames over UDP, one frame per datagram:
// receive from the previous cabinet in the ring, send to the next. The ring
// protocol re-sends state every frame and tolerates loss, so UDP avoids TCP's
// head-of-line stall when a cabinet boots late. Never blocks.
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
                 std::string next_ip, u16 next_port, std::size_t backlog = kBacklog);

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
    std::size_t                 m_backlog = kBacklog;
    bool                        m_send_failed = false;
};

}  // namespace sm2::hw
