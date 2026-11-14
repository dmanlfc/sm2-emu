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
// See comm_udp.h.

#include "hw/comm_udp.h"

#include "core/log.h"

#include <utility>

namespace sm2::hw {

UdpTransport::UdpTransport(const std::string& local_ip, u16 local_port,
                           std::string next_ip, u16 next_port, std::size_t backlog)
    : m_next_ip(std::move(next_ip)), m_next_port(next_port), m_backlog(backlog)
{
    if (!m_socket.open(local_ip, local_port)) {
        SM2_WARN("m2comm: link socket failed: %s", m_socket.last_error().c_str());
        return;
    }
    SM2_INFO("m2comm: link listening on %s:%u, sending to %s:%u",
             local_ip.empty() ? "*" : local_ip.c_str(), local_port,
             m_next_ip.empty() ? "(none)" : m_next_ip.c_str(), m_next_port);
}

void UdpTransport::send(std::span<const u8> frame)
{
    if (!m_socket.valid() || m_next_ip.empty()) {
        return;  // no next hop (tail of a chain) is not a failure
    }
    if (!m_socket.send_to(frame, m_next_ip, m_next_port)) {
        m_send_failed = true;  // far side gone or buffer wedged; drops the link
    }
}

void UdpTransport::drain()
{
    if (!m_socket.valid()) return;
    std::vector<u8> datagram;
    while (m_socket.recv_from(datagram)) {
        if (m_rx.size() >= m_backlog) {
            m_rx.pop_front();  // drop the stalest; a late frame is worthless
        }
        m_rx.push_back(std::move(datagram));
        datagram = {};
    }
}

std::optional<std::vector<u8>> UdpTransport::recv()
{
    if (m_rx.empty()) {
        drain();
    }
    if (m_rx.empty()) {
        return std::nullopt;
    }
    std::vector<u8> front = std::move(m_rx.front());
    m_rx.pop_front();
    return front;
}

bool UdpTransport::connected() const
{
    return m_socket.valid() && !m_send_failed;
}

void UdpTransport::reset()
{
    m_rx.clear();
    m_send_failed = false;
}

}  // namespace sm2::hw
