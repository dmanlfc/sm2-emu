//
// See comm_udp.h.

#include "hw/comm_udp.h"

#include "core/log.h"

#include <utility>

namespace sm2::hw {

UdpTransport::UdpTransport(const std::string& local_ip, u16 local_port,
                           std::string next_ip, u16 next_port)
    : m_next_ip(std::move(next_ip)), m_next_port(next_port)
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
        // Nothing to send to is not a failure: the tail of a chain that loops
        // back elsewhere legitimately has no next hop.
        return;
    }
    if (!m_socket.send_to(frame, m_next_ip, m_next_port)) {
        // A send that fails is the far side gone or the buffer wedged. Report it
        // through connected() so the board drops the link, matching how the
        // loopback signals a backed-up transmit.
        m_send_failed = true;
    }
}

void UdpTransport::drain()
{
    if (!m_socket.valid()) return;
    std::vector<u8> datagram;
    while (m_socket.recv_from(datagram)) {
        if (m_rx.size() >= kBacklog) {
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
